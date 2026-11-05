#include "test/jemalloc_test.h"

#ifdef JEMALLOC_HAVE_SCHED_SETAFFINITY
#include <sched.h>
#endif

/*
 * Under percpu_arena, binding a thread to a manual arena (an index at or above
 * the per-CPU auto range) is one-way: percpu never reclaims it (see
 * arena_choose_impl). Setting thread.arena back to an index in the auto range
 * resumes per-CPU selection instead of failing with EPERM.
 */
TEST_BEGIN(test_thread_arena_resume_percpu) {
	test_skip_if(!have_percpu_arena
	    || !PERCPU_ARENA_ENABLED(opt_percpu_arena));

	unsigned limit = percpu_arena_ind_limit(opt_percpu_arena);
	/* Bypass the tcache so every allocation and free hits the arena. */
	const int flags = MALLOCX_TCACHE_NONE;

	void *warm = mallocx(1, 0);
	expect_ptr_not_null(warm, "Unexpected mallocx() failure");
	dallocx(warm, 0);

	unsigned cur;
	size_t sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_lt(cur, limit, "Thread should start on a per-CPU arena");

	unsigned manual;
	sz = sizeof(manual);
	expect_d_eq(mallctl("arenas.create", (void *)&manual, &sz, NULL, 0), 0,
	    "Unexpected arenas.create() failure");
	expect_u_ge(manual, limit, "A manual arena is outside the per-CPU range");

	unsigned old;
	sz = sizeof(old);
	expect_d_eq(mallctl("thread.arena", (void *)&old, &sz, (void *)&manual,
	    sizeof(manual)), 0, "Binding to a manual arena should be allowed");
	sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_eq(cur, manual, "Thread should be bound to the manual arena");

	void *p_manual = mallocx(1024, flags);
	expect_ptr_not_null(p_manual, "Unexpected mallocx() failure");
	unsigned found;
	sz = sizeof(found);
	expect_d_eq(mallctl("arenas.lookup", (void *)&found, &sz,
	    (void *)&p_manual, sizeof(p_manual)), 0,
	    "Unexpected arenas.lookup() failure");
	expect_u_eq(found, manual, "Allocation should come from the manual arena");

	void *scratch = mallocx(1024, flags);
	expect_ptr_not_null(scratch, "Unexpected mallocx() failure");
	dallocx(scratch, flags);

	unsigned resume = 0;
	expect_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&resume,
	    sizeof(resume)), 0, "Should resume per-CPU selection, not fail");
	sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_lt(cur, limit, "Thread should be back on a per-CPU arena");

	void *p_percpu = mallocx(1024, flags);
	expect_ptr_not_null(p_percpu, "Unexpected mallocx() failure");
	sz = sizeof(found);
	expect_d_eq(mallctl("arenas.lookup", (void *)&found, &sz,
	    (void *)&p_percpu, sizeof(p_percpu)), 0,
	    "Unexpected arenas.lookup() failure");
	expect_u_lt(found, limit, "Allocation should come from a per-CPU arena");
	dallocx(p_percpu, flags);

	/* Free the manual-arena region while bound to a different arena. */
	dallocx(p_manual, flags);
}
TEST_END

/*
 * Resuming per-CPU selection while running on a CPU id at or above the
 * reserved arena range (possible only with a masked/lying cpu/possible file)
 * must clamp, not index past the range.
 */
TEST_BEGIN(test_thread_arena_resume_percpu_clamped) {
	test_skip_if(!have_percpu_arena
	    || !PERCPU_ARENA_ENABLED(opt_percpu_arena));
#ifdef JEMALLOC_HAVE_SCHED_SETAFFINITY
	cpu_set_t old_set;
	expect_d_eq(sched_getaffinity(0, sizeof(old_set), &old_set), 0,
	    "Unexpected sched_getaffinity() failure");
	/* Pin to a CPU id above the shrunken range below, so the raw index
	 * differs from the clamped one. */
	int cpu = -1;
	for (int i = (int)(sizeof(old_set) * 8); i-- > 1;) {
		if (CPU_ISSET(i, &old_set)) {
			cpu = i;
			break;
		}
	}
	test_skip_if(cpu < 0);
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	expect_d_eq(sched_setaffinity(0, sizeof(set), &set), 0,
	    "Unexpected sched_setaffinity() failure");

	/* Pretend the reserved per-CPU range is a single arena. */
	unsigned ncpus_old = ncpus;
	unsigned ncpus_possible_old = ncpus_possible;
	ncpus = 1;
	ncpus_possible = 1;

	unsigned resume = 0;
	expect_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&resume,
	    sizeof(resume)), 0, "Should resume per-CPU selection, not fail");
	unsigned cur;
	size_t sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_eq(cur, 0, "CPU id above the range must be clamped");

	ncpus = ncpus_old;
	ncpus_possible = ncpus_possible_old;
	sched_setaffinity(0, sizeof(old_set), &old_set);
#else
	test_skip("sched_setaffinity() not available");
#endif
}
TEST_END

int
main(void) {
	return test(
	    test_thread_arena_resume_percpu,
	    test_thread_arena_resume_percpu_clamped);
}
