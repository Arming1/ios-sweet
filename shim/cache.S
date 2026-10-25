/*
 * mmu_off_asm(sctlr_restore): the core of mmu_disable() (mmu.c).
 *
 * While the MMU is on, the C stack and .data/.bss are Normal WB, so dirty lines
 * must reach memory before the caches go off. Once the first clean is done,
 * nothing may be written to memory until SCTLR_EL1 is restored: a new dirty
 * line would never be written back with the caches off. So this is a leaf
 * routine: registers only, no stack, no calls, no stores.
 *
 *   x0 = SCTLR_EL1 value to restore (the ABL state saved by mmu_enable)
 */

/*
 * Clean and invalidate every data/unified cache level by set/way, the Linux
 * __dcache_flush_all sequence: levels 0 .. LoC-1, CCSIDR in the 32-bit format
 * (FEAT_CCIDX is not used). Clobbers x0-x11, x13, x14; x12 is kept.
 */
	.macro	clean_inv_all
	mrs	x0, clidr_el1
	ubfx	x3, x0, #24, #3		/* LoC (bits 26:24): levels 0 .. LoC-1 */
	cbz	x3, 9f
	mov	x10, #0			/* cache level, 0-based */
1:	add	x1, x10, x10, lsl #1	/* 3 * level: Ctype field position in CLIDR */
	lsr	x1, x0, x1
	and	x1, x1, #7
	cmp	x1, #2			/* 2: data or unified; 0/1: no data cache, skip */
	b.lt	8f
	lsl	x13, x10, #1		/* CSSELR: level in bits 3:1, InD = 0 */
	msr	csselr_el1, x13
	isb
	mrs	x1, ccsidr_el1
	and	x2, x1, #7		/* line size = 2^(LineSize + 4) bytes */
	add	x2, x2, #4		/* x2 = log2(line bytes) = set field shift */
	ubfx	x4, x1, #3, #10		/* x4 = associativity - 1 */
	ubfx	x7, x1, #13, #15	/* x7 = number of sets - 1 */
	clz	w5, w4			/* way field shift = clz(ways - 1) */
	mov	x8, x7			/* set counter */
2:	mov	x9, x4			/* way counter */
3:	lsl	x6, x9, x5
	orr	x6, x6, x13
	lsl	x14, x8, x2
	orr	x6, x6, x14
	dc	cisw, x6
	subs	x9, x9, #1
	b.ge	3b
	subs	x8, x8, #1
	b.ge	2b
8:	add	x10, x10, #1
	cmp	x3, x10
	b.gt	1b
	msr	csselr_el1, xzr
9:	dsb	sy
	isb
	.endm

	.text
	.globl	mmu_off_asm
	.type	mmu_off_asm, %function
mmu_off_asm:
	mov	x12, x0			/* SCTLR value to restore */
	dsb	sy
	clean_inv_all			/* (a)-(c): dirty lines to memory, then dsb */
	msr	sctlr_el1, x12		/* (d): MMU and caches off; no memory access before this */
	isb
	ic	iallu			/* (e): I-cache and TLB, now with the MMU off */
	tlbi	vmalle1
	dsb	sy
	isb
	clean_inv_all			/* (f): drop lines allocated before the switch */
	dsb	sy
	isb
	ret
	.size	mmu_off_asm, . - mmu_off_asm
