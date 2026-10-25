#pragma once
#include <stdint.h>

#define FB_PHYS	0xe2a00000UL

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
};

/*
 * Must be called before mmu_enable: the 16K pages of the image in [lo, hi)
 * (rounded out to pages) become Normal non-cacheable (DMA buffers) instead
 * of Normal write-back. A window of 0/0 (the default) changes nothing.
 */
void mmu_set_nc_window(uintptr_t lo, uintptr_t hi);
int mmu_enable(struct mmu_info *info);
void mmu_disable(const struct mmu_info *info);
uint64_t mmu_probe(uintptr_t va);

/*
 * Data cache maintenance by VA (VA == PA; also correct with the MMU off): every
 * line overlapping [start, end), then dsb sy. inval discards the lines, dirty
 * data included. clean_inval writes dirty data back, then discards the lines.
 */
void dcache_inval_range(uintptr_t start, uintptr_t end);
void dcache_clean_inval_range(uintptr_t start, uintptr_t end);
