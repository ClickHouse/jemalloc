#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

static safety_check_abort_hook_t safety_check_abort;

void
safety_check_fail_sized_dealloc(bool current_dealloc, const void *ptr,
    size_t true_size, size_t input_size) {
	char *src = current_dealloc
	    ? "the current pointer being freed"
	    : "in thread cache, possibly from previous deallocations";
	char *suggest_debug_build = config_debug ? "" : " --enable-debug or";

	safety_check_fail(
	    "<jemalloc>: size mismatch detected (true size %zu "
	    "vs input size %zu), likely caused by application sized "
	    "deallocation bugs (source address: %p, %s). Suggest building with"
	    "%s address sanitizer for debugging. Abort.\n",
	    true_size, input_size, ptr, src, suggest_debug_build);
}

void
safety_check_set_abort(safety_check_abort_hook_t abort_fn) {
	safety_check_abort = abort_fn;
}

/*
 * In addition to malloc_write, also embed hint msg in the abort function name
 * because there are cases only logging crash stack traces.
 */
static void
safety_check_detected_heap_corruption___run_address_sanitizer_build_to_debug(
    const char *buf) {
	if (safety_check_abort == NULL) {
		malloc_write(buf);
		abort();
	} else {
		safety_check_abort(buf);
	}
}

void
safety_check_fail(const char *format, ...) {
	char buf[MALLOC_PRINTF_BUFSIZE];

	va_list ap;
	va_start(ap, format);
	malloc_vsnprintf(buf, MALLOC_PRINTF_BUFSIZE, format, ap);
	va_end(ap);

	safety_check_detected_heap_corruption___run_address_sanitizer_build_to_debug(
	    buf);
}

/*
 * Debug: per-pointer backtrace tracker for tcache pushes.
 * When a duplicate is detected, prints both the current and original
 * stack traces.
 */
#include <execinfo.h>

#define TCACHE_BT_FRAMES 15
#define TCACHE_BT_TABLE_SIZE 16384  /* must be power of 2 */
#define TCACHE_BT_TABLE_MASK (TCACHE_BT_TABLE_SIZE - 1)

typedef struct {
	void *ptr;
	void *frames[TCACHE_BT_FRAMES];
	int nframes;
} tcache_bt_entry_t;

static __thread tcache_bt_entry_t tcache_bt_table[TCACHE_BT_TABLE_SIZE];

static unsigned
tcache_bt_hash(void *ptr) {
	uintptr_t v = (uintptr_t)ptr;
	v ^= v >> 16;
	v *= 0x45d9f3b;
	v ^= v >> 16;
	return (unsigned)(v & TCACHE_BT_TABLE_MASK);
}

static void
tcache_bt_record(void *ptr) {
	unsigned idx = tcache_bt_hash(ptr);
	for (unsigned i = 0; i < 64; i++) {
		unsigned slot = (idx + i) & TCACHE_BT_TABLE_MASK;
		if (tcache_bt_table[slot].ptr == NULL
		    || tcache_bt_table[slot].ptr == ptr) {
			tcache_bt_table[slot].ptr = ptr;
			tcache_bt_table[slot].nframes = backtrace(
			    tcache_bt_table[slot].frames, TCACHE_BT_FRAMES);
			return;
		}
	}
}

static tcache_bt_entry_t *
tcache_bt_find(void *ptr) {
	unsigned idx = tcache_bt_hash(ptr);
	for (unsigned i = 0; i < 64; i++) {
		unsigned slot = (idx + i) & TCACHE_BT_TABLE_MASK;
		if (tcache_bt_table[slot].ptr == ptr) {
			return &tcache_bt_table[slot];
		}
		if (tcache_bt_table[slot].ptr == NULL) {
			return NULL;
		}
	}
	return NULL;
}

static void
tcache_bt_remove(void *ptr) {
	unsigned idx = tcache_bt_hash(ptr);
	for (unsigned i = 0; i < 64; i++) {
		unsigned slot = (idx + i) & TCACHE_BT_TABLE_MASK;
		if (tcache_bt_table[slot].ptr == ptr) {
			tcache_bt_table[slot].ptr = NULL;
			return;
		}
		if (tcache_bt_table[slot].ptr == NULL) {
			return;
		}
	}
}

static void
tcache_bt_print(const char *label, tcache_bt_entry_t *entry) {
	char buf[256];
	malloc_snprintf(buf, sizeof(buf),
	    "  %s backtrace (%d frames):\n", label, entry->nframes);
	malloc_write(buf);
	for (int i = 0; i < entry->nframes; i++) {
		malloc_snprintf(buf, sizeof(buf),
		    "    #%d: %p\n", i, entry->frames[i]);
		malloc_write(buf);
	}
}

/*
 * Called after every tcache push to record the backtrace and check
 * for duplicates.
 */
JEMALLOC_NOINLINE void
tcache_debug_check_bin_after_push(void **stack_head, unsigned ncached,
    void *ptr) {
	/* Check for duplicate in the bin. */
	for (unsigned i = 1; i < ncached; i++) {
		if (stack_head[i] == ptr) {
			char buf[256];
			malloc_snprintf(buf, sizeof(buf),
			    "tcache duplicate on push: ptr %p at "
			    "position %u (ncached %u)\n",
			    ptr, i, ncached);
			malloc_write(buf);

			/* Print the original push backtrace. */
			tcache_bt_entry_t *orig = tcache_bt_find(ptr);
			if (orig != NULL) {
				tcache_bt_print("ORIGINAL push", orig);
			} else {
				malloc_write("  (original backtrace "
				    "not found)\n");
			}

			/* Print current backtrace. */
			tcache_bt_entry_t current;
			current.nframes = backtrace(
			    current.frames, TCACHE_BT_FRAMES);
			tcache_bt_print("DUPLICATE push", &current);

			safety_check_fail(
			    "tcache duplicate detected: ptr %p\n", ptr);
			return;
		}
	}

	/* No duplicate — record this push. */
	tcache_bt_record(ptr);
}

/*
 * Called when a pointer is popped from tcache (allocation).
 * Removes the backtrace record so it can be re-recorded on next push.
 */
JEMALLOC_NOINLINE void
tcache_debug_on_pop(void *ptr) {
	if (ptr != NULL) {
		tcache_bt_remove(ptr);
	}
}
