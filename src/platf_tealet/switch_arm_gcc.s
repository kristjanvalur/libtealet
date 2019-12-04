	.arch armv6
	.eabi_attribute 28, 1
	.eabi_attribute 20, 1
	.eabi_attribute 21, 1
	.eabi_attribute 23, 3
	.eabi_attribute 24, 1
	.eabi_attribute 25, 1
	.eabi_attribute 26, 2
	.eabi_attribute 30, 1
	.eabi_attribute 34, 1
	.eabi_attribute 18, 4
	.file	"test.c"
	.text
	.align	2
	.global	tealet_slp_switch
	.arch armv6
	.syntax unified
	.arm
	.fpu vfp
	.type	tealet_slp_switch, %function
tealet_slp_switch:
	@ args = 0, pretend = 0, frame = 0
	@ frame_needed = 0, uses_anonymous_args = 0
	push	{r3, r4, r5, r6, r7, r8, r9, r10, fp, lr}
	vpush.64	{d8, d9, d10, d11, d12, d13, d14, d15}
	mov	r3, r0
	mov	fp, r1
	.syntax divided
@ 47 "switch_arm_gcc.h" 1
	mov r0, sp
@ 0 "" 2
	.arm
	.syntax unified
	mov	r4, r2
	mov	r1, r2
	blx	r3
	tst	r0, #1
	bne	.L1
	.syntax divided
@ 53 "switch_arm_gcc.h" 1
	mov sp, r0
@ 0 "" 2
	.arm
	.syntax unified
	mov	r1, r4
	blx	fp
.L1:
	vldm	sp!, {d8-d15}
	pop	{r3, r4, r5, r6, r7, r8, r9, r10, fp, pc}
	.size	tealet_slp_switch, .-tealet_slp_switch
	.ident	"GCC: (Raspbian 8.3.0-6+rpi1) 8.3.0"
	.section	.note.GNU-stack,"",%progbits
