/*
 * Position-independent entry for a NOLO-loaded test image.
 * Laid out like a zImage header so loaders that look for the magic accept it.
 */
	.syntax unified
	.arm
	.section .text.start, "ax"
	.global _start
_start:
	.rept 8
	mov	r0, r0
	.endr
	b	1f
	.word	0x016f2818		/* zImage magic at offset 0x24 */
	.word	0			/* load address hint */
	.word	_end - _start		/* image size */
1:
	cpsid	if
	adr	r4, _start
	add	sp, r4, #0x100000	/* 1 MiB above the load address */
	bic	sp, sp, #7
	mov	r0, r4
	bl	smoke_main
2:	wfi
	b	2b
