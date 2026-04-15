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
#include <sys/mman.h>

#define TCACHE_BT_FRAMES 8
#define TCACHE_BT_TABLE_SIZE (1 << 16)  /* 65536, must be power of 2 */
#define TCACHE_BT_TABLE_MASK (TCACHE_BT_TABLE_SIZE - 1)

typedef struct {
	void *ptr;
	void *frames[TCACHE_BT_FRAMES];
	int nframes;
} tcache_bt_entry_t;
/* 8 + 64 + 4 = 76 bytes per entry, 65536 entries = ~5MB per thread */

static __thread tcache_bt_entry_t *tcache_bt_table;

static void
tcache_bt_ensure_table(void) {
	if (likely(tcache_bt_table != NULL)) {
		return;
	}
	/* Use mmap to avoid re-entering jemalloc. */
	size_t sz = TCACHE_BT_TABLE_SIZE * sizeof(tcache_bt_entry_t);
	tcache_bt_table = (tcache_bt_entry_t *)mmap(NULL, sz,
	    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

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
	tcache_bt_ensure_table();
	if (tcache_bt_table == MAP_FAILED) return;
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
	if (tcache_bt_table == NULL || tcache_bt_table == MAP_FAILED)
		return NULL;
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
	if (tcache_bt_table == NULL || tcache_bt_table == MAP_FAILED)
		return;
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
 * Record backtrace on every tcache push. Inlineable by LTO — just a
 * hash table insert, no scan, no barrier effect.
 */
void
tcache_debug_bt_record(void *ptr) {
	tcache_bt_record(ptr);
}

/*
 * Remove backtrace record on tcache pop. Inlineable by LTO.
 */
void
tcache_debug_on_pop(void *ptr) {
	if (ptr != NULL) {
		tcache_bt_remove(ptr);
	}
}

/*
 * Called during tcache flush to scan for duplicates.
 * This runs in tcache.c (not inlined into callers), so it won't
 * affect LTO optimization of the push/pop fast paths.
 */
void
tcache_debug_check_flush(void **ptrs, unsigned nflush) {
	for (unsigned i = 0; i < nflush; i++) {
		for (unsigned j = i + 1; j < nflush; j++) {
			if (ptrs[i] == ptrs[j]) {
				char buf[256];
				malloc_snprintf(buf, sizeof(buf),
				    "tcache duplicate in flush: ptr %p "
				    "at positions %u and %u "
				    "(nflush %u)\n",
				    ptrs[i], i, j, nflush);
				malloc_write(buf);

				/* Print first push backtrace. */
				tcache_bt_entry_t *orig =
				    tcache_bt_find(ptrs[i]);
				if (orig != NULL) {
					tcache_bt_print("push", orig);
				} else {
					malloc_write(
					    "  (backtrace not found)\n");
				}

				safety_check_fail(
				    "tcache duplicate: ptr %p\n",
				    ptrs[i]);
				return;
			}
		}
	}
}
