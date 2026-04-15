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
 * Debug: scan the tcache bin for duplicate pointers after a push.
 * noinline so LTO cannot optimize through this call — it must remain
 * an opaque barrier to prevent the optimizer from reordering/merging
 * the tcache push with surrounding code.
 */
JEMALLOC_NOINLINE void
tcache_debug_check_bin_after_push(void **stack_head, unsigned ncached,
    void *ptr) {
	for (unsigned i = 1; i < ncached; i++) {
		if (stack_head[i] == ptr) {
			safety_check_fail(
			    "tcache duplicate detected on push: "
			    "ptr %p already at position %u "
			    "(ncached %u)\n",
			    ptr, i, ncached);
			return;
		}
	}
}
