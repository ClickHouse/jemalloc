#ifndef JEMALLOC_INTERNAL_CLICKHOUSE_H
#define JEMALLOC_INTERNAL_CLICKHOUSE_H

#include "jemalloc/internal/atomic.h"

/// Extensions in ClickHouse's fork of jemalloc. For reliably tracking and limiting memory usage.

/// Context:
///
/// We'd like to avoid OOMing [1] the current process as much as possible. To that end, we'd like to
/// reliably enforce a ~hard limit on the process's resident memory, past which allocations fail.
/// ClickHouse can then handle failed allocations gracefully by canceling the query or shrinking
/// caches and retrying the allocation.
///
/// To make that happen we need two things:
///  1. An estimate of the resident memory amount that is updated synchronously and is cheap to
///     access, so that we can afford to compare it to the limit on each allocation [2] [3].
///  2. An option to fail allocation if it would increase resident memory above the limit [4] [5].
///
/// Both things can't be implemented well outside jemalloc.
///
/// This header is the API for these 2 things.
/// It's designed to require the minimum amount of changes to jemalloc.
///
/// Enforcement of the limit is opt-in per malloc call (or posix_memalign etc), to avoid breaking
/// parts of the code that are not equipped to handle allocation failures (e.g. code in third-partly
/// libraries). But the tracking of resident memory covers all jemalloc allocations.
///
/// Footnotes:
/// [1] Specifically, avoid triggering OOM killer, and avoid the thing where Linux virtual memory
///     subsystem's performance falls off a cliff when it's very low on free memory.
/// [2] Existing jemalloc stats are not cheap to access (they iterate over all arenas).
///     RSS reported by the OS is not cheap to access.
/// [3] We could use some hybrid scheme where the RSS estimate is updated approximately during
///     malloc/free and asynchrnously corrected by periodically querying the RSS from the OS
///     or from jemalloc. But it seems ~impossible to avoid the race condition between the
///     asynchronous correction and concurrent malloc/free calls. E.g. this scenario:
///     bg thread gets RSS stat from OS/jemalloc and hesitates for a bit, then a big allocation
///     happens and updates our RSS estimate, then the bg thread overwrites our RSS estimate with
///     the slightly-stale value that doesn't include the big allocation. Perhaps some scheme like
///     this can be made to work well in practice using some tricks, but it seems worse than what
///     we're doing here.
/// [4] We could live without this feature by requiring the free memory amount to be bigger than the
///     allocation size - this can be checked outside jemalloc, before the allocation. But it may be
///     too restrictive for big allocations. E.g. suppose we repeatedly allocate and deallocate
///     a huge block of memory, changing active memory amount between 40% and 80% of RAM size,
///     back and forth; normally jemalloc would just keep reusing the same block of memory for it,
///     without any syscalls; but if we require enough *free* memory to fit the allocation, the
///     second allocation will fail (because resident memory is still 80%, 40% of which is dirty),
///     and we'll either unnecessarily fail the query or unnecessarily wait for purging
///     (delayed madvise(MADV_DONTNEED) call inside jemalloc, changing pages' state from
///     "dirty" to "retained").
/// [5] Can we use extent hooks (ehooks.h) or other hooks (hook.h) for this? Doesn't seem so:
///     jemalloc doesn't call any hooks when pages change state from "retained" to "active".
///     Or between "dirty" and "active" (which doesn't affect RSS, but clickhouse may want to
///     know how much memory can be purged).

/// This currently doesn't support HPA (huge page allocator) because HPA doesn't seem useful in its
/// current state (only works for allocations smaller than a hugepage, default "hugepage" size is
/// 64 KiB for some reason, stats are not propagated to stats.active/dirty/retained, extents in
/// small extent cache are not counted by any of the stats), and it would require the most code
/// changes.

typedef struct je_clickhouse_tls_s je_clickhouse_tls_t;
struct je_clickhouse_tls_s {
    /// If use_thread_local_stats == true, these counters are increased/decreased when pages
    /// change state between active/dirty/neither.
    /// Indended use is to subtract the values before and after an alloc/free call.
    /// Absolute values are not meaningful.
	int64_t active_bytes_delta; // total size of active pages (i.e. containing any live allocations)
    int64_t dirty_bytes_delta; // total size of dirty regions (i.e. purgable but not purged)

    /// If true, jemalloc will be updating the counters in je_clickhouse_tls only.
    /// If false, jemalloc will be updating the global counters only
    /// (je_clickhouse_resident_bytes and je_clickhouse_active_bytes).
    /// Whoever sets it to true is responsible for updating the global counters after the
    /// malloc/free/etc call as needed.
    bool use_thread_local_stats;

    /// If true, allocation will succeed only if it can live entirely within already-active or dirty
    /// pages. I.e. it shouldn't increase the process's resident set size.
    bool do_not_increase_rss;
};

extern __thread JEMALLOC_TLS_MODEL je_clickhouse_tls_t je_clickhouse_tls;

/// Global memory usage counters. "Resident" means active+dirty, it's a good estimate of RSS.
///
/// The same information is available through mallctl "stats.active"/"stats.dirty", but these
/// counters are updated synchronously and are cheap to read.
///
/// If use_thread_local_stats == false, these counters are updated by jemalloc
/// (increased/decreased when pages change state). Useful for allocations not instrumented
/// by the user (e.g. from third-party libraries or during initialization).
///
/// If use_thread_local_stats == true, the user is responsible for updating these counters
/// (presumably using the information from je_clickhouse_tls).
/// Why put this burden on the user instead of always updating the global atomics from jemalloc?
///  - This allows the user to implement an optimization where the stat updates are cached
///    thread-locally for up to N bytes before being flushed to the global atomic. [1]
///  - This avoids the race condition when pages change state from dirty to active.
///    With use_thread_local_stats == false, this causes 2 or 3 separate updates to
///    je_clickhouse_resident_bytes: it's first decreased when a dirty extent is extracted, then
///    increased when (part of) the extent is marked as active [2].
///    If someone reads the counter between these two operations, they'll see an incorrectly low
///    value. Avoiding this within jemalloc would require more (and more fragile) code changes,
///    so we solve it only for instrumented alloc/free calls, using the thread-local counters
///    (where the increments/decrements will cancel out before updating the atomic).
///  - User code may speculatively update this before allocation to avoid going over the memory
///    limit if multiple big allocations are attempted in parallel. See intended usage below.
///
/// Intended usage:
///     /// Speculatively reserve memory for the allocation.
///     int64_t resident = atomic_fetch_add_zd(&je_clickhouse_resident_bytes, size);
///     prev_active = je_clickhouse_tls.active_bytes_delta;
///     prev_dirty = je_clickhouse_tls.dirty_bytes_delta;
///     je_clickhouse_tls.do_not_increase_rss = resident + size > memory_limit;
///     je_clickhouse_tls.use_thread_local_stats = true;
///
///     void *ptr = malloc(size);
///
///     je_clickhouse_tls.use_thread_local_stats = false;
///     je_clickhouse_tls.do_not_increase_rss = false;
///     je_clickhouse_resident_bytes +=
///       (je_clickhouse_tls.active_bytes_delta - prev_active)
///       + (je_clickhouse_tls.dirty_bytes_delta - prev_dirty)
///       - (int64_t)size;
///     je_clickhouse_active_bytes += je_clickhouse_tls.active_bytes_delta - prev_active;
///
/// Footnotes:
///  [1] Why not do the same optimization inside jemalloc? Because it would require extra code in
///      jemalloc to flush the cache when a thread is destroyed. While on ClickHouse side such code
///      already exists.
///  [2] And possibly increased in between, if the extent is split and part of it becomes dirty
///      again.
extern atomic_zd_t je_clickhouse_resident_bytes;
extern atomic_zd_t je_clickhouse_active_bytes;

#endif /* JEMALLOC_INTERNAL_CLICKHOUSE_H */
