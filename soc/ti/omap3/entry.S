/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * First code of the image. The bootloader jumps to the first byte of the
 * binary wherever it loaded it; this stub is position-independent, moves
 * the image to its link address when needed and enters the kernel there.
 * The layout matches a zImage header so loaders that check the magic
 * accept the image.
 *
 * The bootloader may have left the data cache on, holding parts of the
 * image: clean it to memory and turn the caches and MMU off first.
 *
 * The source and destination of the move may overlap the stub itself, so
 * the copy routine first places itself above both ranges and runs there.
 */

#include <zephyr/toolchain.h>

_ASM_FILE_PROLOGUE

GTEXT(z_omap3_entry)
GDATA(omap3_load_addr)

#define SCTLR_M (1 << 0)
#define SCTLR_C (1 << 2)
#define SCTLR_I (1 << 12)

	.section .omap3_entry, "ax"
	.arm
	.balign 4
z_omap3_entry:
	b	1f
	.rept	8
	.word	0
	.endr
	.word	0x016f2818		/* zImage magic, offset 0x24 */
	.word	z_omap3_entry		/* start address */
	.word	z_omap3_image_end	/* end address */

1:
	cpsid	aif

	mrc	p15, 0, r3, c1, c0, 0	/* SCTLR */
	tst	r3, #SCTLR_C
	beq	caches_off

	/* Clean and invalidate all data cache levels by set/way */
	mrc	p15, 1, r6, c0, c0, 1	/* CLIDR */
	ands	r7, r6, #0x07000000
	lsr	r7, r7, #23		/* level of coherency * 2 */
	beq	clean_done
	mov	r10, #0			/* cache level * 2 */
level_loop:
	add	r2, r10, r10, lsr #1
	lsr	r1, r6, r2
	and	r1, r1, #7		/* cache type at this level */
	cmp	r1, #2
	blt	next_level		/* no data cache */
	mcr	p15, 2, r10, c0, c0, 0	/* CSSELR */
	isb
	mrc	p15, 1, r1, c0, c0, 0	/* CCSIDR */
	and	r2, r1, #7
	add	r2, r2, #4		/* log2 of the line length */
	movw	r4, #0x3ff
	ands	r4, r4, r1, lsr #3	/* highest way number */
	clz	r5, r4			/* way field position */
	movw	r8, #0x7fff
	ands	r8, r8, r1, lsr #13	/* highest set number */
set_loop:
	mov	r9, r4
way_loop:
	orr	r11, r10, r9, lsl r5
	orr	r11, r11, r8, lsl r2
	mcr	p15, 0, r11, c7, c14, 2	/* DCCISW */
	subs	r9, r9, #1
	bge	way_loop
	subs	r8, r8, #1
	bge	set_loop
next_level:
	add	r10, r10, #2
	cmp	r7, r10
	bgt	level_loop
clean_done:
	mov	r10, #0
	mcr	p15, 2, r10, c0, c0, 0	/* CSSELR back to level 1 */
	dsb
	isb

caches_off:
	bic	r3, r3, #(SCTLR_M | SCTLR_C)
	bic	r3, r3, #SCTLR_I
	mcr	p15, 0, r3, c1, c0, 0
	isb
	mov	r3, #0
	mcr	p15, 0, r3, c7, c5, 0	/* invalidate the instruction cache */
	mcr	p15, 0, r3, c8, c7, 0	/* invalidate the TLBs */
	dsb
	isb

	adr	r0, z_omap3_entry	/* source: where the image runs now */
	ldr	r1, =z_omap3_entry	/* destination: where it is linked */
	ldr	r2, =z_omap3_image_end
	sub	r2, r2, r1
	add	r2, r2, #3
	bic	r2, r2, #3		/* bytes to move */
	mov	r4, r0

	/* Scratch space past the end of both ranges: copy routine, stack */
	cmp	r0, r1
	movhi	r5, r0
	movls	r5, r1
	add	r5, r5, r2
	add	sp, r5, #0x2000

#if defined(CONFIG_SOC_OMAP3_BOOT_STAGES)
	push	{r0-r5}
	mov	r0, #0
	mov	r1, #0xff0000
	bl	omap3_boot_stage	/* runs where the image was loaded */
	pop	{r0-r5}
#endif

	cmp	r0, r1
	beq	enter

	adr	r6, copy_start
	adr	r7, copy_end
	mov	r8, r5
2:	ldr	r3, [r6], #4
	str	r3, [r8], #4
	cmp	r6, r7
	blo	2b
	mov	r3, #0
	mcr	p15, 0, r3, c7, c5, 0	/* invalidate the instruction cache */
	dsb
	isb
	bx	r5

copy_start:
	cmp	r0, r1
	bhi	3f

	/* Destination above source: copy backwards */
	add	r0, r0, r2
	add	r1, r1, r2
4:	ldr	r3, [r0, #-4]!
	str	r3, [r1, #-4]!
	subs	r2, r2, #4
	bne	4b
	b	5f

	/* Destination below source: copy forwards */
3:	ldr	r3, [r0], #4
	str	r3, [r1], #4
	subs	r2, r2, #4
	bne	3b

5:	mov	r3, #0
	mcr	p15, 0, r3, c7, c5, 0	/* invalidate the instruction cache */
	dsb
	isb

#if defined(CONFIG_SOC_OMAP3_BOOT_STAGES)
	mov	r0, #1
	ldr	r1, =0xff8000
	ldr	r3, =omap3_boot_stage	/* at its link address now */
	blx	r3
#endif

enter:
	ldr	r1, =omap3_load_addr
	str	r4, [r1]
	ldr	pc, =__start

	.ltorg
copy_end:
