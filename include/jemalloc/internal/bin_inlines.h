#ifndef JEMALLOC_INTERNAL_BIN_INLINES_H
#define JEMALLOC_INTERNAL_BIN_INLINES_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/bin_info.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/div.h"
#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/sc.h"

/*
 * The dalloc bin info contains just the information that the common paths need
 * during tcache flushes.  By force-inlining these paths, and using local copies
 * of data (so that the compiler knows it's constant), we avoid a whole bunch of
 * redundant loads and stores by leaving this information in registers.
 */
typedef struct bin_dalloc_locked_info_s bin_dalloc_locked_info_t;
struct bin_dalloc_locked_info_s {
	div_info_t div_info;
	uint32_t   nregs;
	uint64_t   ndalloc;
};

/* Find the region index of a pointer within a slab. */
JEMALLOC_ALWAYS_INLINE size_t
bin_slab_regind_impl(
    div_info_t *div_info, szind_t binind, edata_t *slab, const void *ptr) {
	size_t diff, regind;

	/* Freeing a pointer outside the slab can cause assertion failure. */
	assert((uintptr_t)ptr >= (uintptr_t)edata_addr_get(slab));
	assert((uintptr_t)ptr < (uintptr_t)edata_past_get(slab));
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - (uintptr_t)edata_addr_get(slab))
	        % (uintptr_t)bin_infos[binind].reg_size
	    == 0);

	diff = (size_t)((uintptr_t)ptr - (uintptr_t)edata_addr_get(slab));

	/* Avoid doing division with a variable divisor. */
	regind = div_compute(div_info, diff);
	assert(regind < bin_infos[binind].nregs);
	return regind;
}

JEMALLOC_ALWAYS_INLINE size_t
bin_slab_regind(bin_dalloc_locked_info_t *info, szind_t binind,
    edata_t *slab, const void *ptr) {
	size_t regind = bin_slab_regind_impl(
	    &info->div_info, binind, slab, ptr);
	return regind;
}

JEMALLOC_ALWAYS_INLINE void
bin_dalloc_locked_begin(
    bin_dalloc_locked_info_t *info, szind_t binind) {
	info->div_info = arena_binind_div_info[binind];
	info->nregs = bin_infos[binind].nregs;
	info->ndalloc = 0;
}

/*
 * Does the deallocation work associated with freeing a single pointer (a
 * "step") in between a bin_dalloc_locked begin and end call.
 *
 * Returns true if arena_slab_dalloc must be called on slab.  Doesn't do
 * stats updates, which happen during finish (this lets running counts get left
 * in a register).
 */
JEMALLOC_ALWAYS_INLINE bool
bin_dalloc_locked_step(tsdn_t *tsdn, bool is_auto, bin_t *bin,
    bin_dalloc_locked_info_t *info, szind_t binind, edata_t *slab,
    void *ptr) {
	const bin_info_t *bin_info = &bin_infos[binind];
	size_t            regind = bin_slab_regind(info, binind, slab, ptr);
	slab_data_t      *slab_data = edata_slab_data_get(slab);

	assert(edata_nfree_get(slab) < bin_info->nregs);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(slab_data->bitmap, &bin_info->bitmap_info, regind));

	/* Debug: snapshot bitmap group before unset. */
	size_t goff_dbg = regind >> LG_BITMAP_GROUP_NBITS;
	bitmap_t before_dbg = *(volatile bitmap_t *)&slab_data->bitmap[goff_dbg];

	bitmap_unset(slab_data->bitmap, &bin_info->bitmap_info, regind);

	/* Debug: verify the bit was actually flipped. */
	bitmap_t after_dbg = *(volatile bitmap_t *)&slab_data->bitmap[goff_dbg];
	bitmap_t expected_bit = ZU(1) << (regind & BITMAP_GROUP_NBITS_MASK);
	if (unlikely((before_dbg | expected_bit) != after_dbg)) {
		safety_check_fail(
		    "bitmap_unset lost: binind %u regind %zu "
		    "goff %zu before %lx after %lx expected_bit %lx\n",
		    binind, regind, goff_dbg,
		    (unsigned long)before_dbg,
		    (unsigned long)after_dbg,
		    (unsigned long)expected_bit);
	}

	edata_nfree_inc(slab);

	/* Debug: verify nfree/bitmap consistency after free. */
	{
		unsigned actual_free = 0;
		unsigned ngroups =
#ifdef BITMAP_USE_TREE
		    bin_info->bitmap_info.levels[
		        bin_info->bitmap_info.nlevels].group_offset;
#else
		    bin_info->bitmap_info.ngroups;
#endif
		for (unsigned gi = 0; gi < ngroups; gi++) {
			actual_free += popcount_lu(slab_data->bitmap[gi]);
		}
		if (unlikely(actual_free != edata_nfree_get(slab))) {
			safety_check_fail(
			    "bin_dalloc_locked_step: post-free "
			    "nfree/bitmap mismatch for binind %u "
			    "regind %zu: nfree=%u actual=%u\n",
			    binind, regind,
			    edata_nfree_get(slab), actual_free);
		}
	}

	if (config_stats) {
		info->ndalloc++;
	}

	unsigned nfree = edata_nfree_get(slab);
	if (nfree == bin_info->nregs) {
		bin_dalloc_locked_handle_newly_empty(
		    tsdn, is_auto, slab, bin);
		return true;
	} else if (nfree == 1 && slab != bin->slabcur) {
		bin_dalloc_locked_handle_newly_nonempty(
		    tsdn, is_auto, slab, bin);
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE void
bin_dalloc_locked_finish(tsdn_t *tsdn, bin_t *bin,
    bin_dalloc_locked_info_t *info) {
	if (config_stats) {
		bin->stats.ndalloc += info->ndalloc;
		assert(bin->stats.curregs >= (size_t)info->ndalloc);
		bin->stats.curregs -= (size_t)info->ndalloc;
	}
}

#endif /* JEMALLOC_INTERNAL_BIN_INLINES_H */
