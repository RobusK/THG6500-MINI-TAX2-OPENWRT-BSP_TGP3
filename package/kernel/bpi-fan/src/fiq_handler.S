/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FIQ handler for the GPIO19 fan PWM. Copied over the kernel's vector_fiq
 * stub, so it must be position independent (no literal pools).
 *
 * Banked FIQ registers, set up once on CPU1 (sp_fiq is left alone - Linux
 * keeps its per-CPU FIQ stack pointer there):
 *   r8  = GPIO0-31 data register (VA)
 *   r9  = SP804 timer1 base (VA)
 *   r10 = params (lowmem, never faults):
 *           [0] high ticks  [1] low ticks  [2] pin level (0 / bit 19)
 *           [3] FIQ count
 *   r11, r12 = scratch
 *
 * State lives in memory, not in banked registers, so the Linux IRQ fallback
 * (used when Linux's own GICC_IAR read happens to acknowledge this
 * interrupt first) can do exactly the same step.
 *
 * The timer runs periodic; its background load is set one period ahead,
 * so edge timing comes from hardware reload, not from FIQ latency.
 */
	.syntax unified
	.arm
	.text
	.align	2
	.global	bpi_fiq_start
	.global	bpi_fiq_end
bpi_fiq_start:
	ldr	r11, [r9, #0x10]	@ TIMER1 RIS
	tst	r11, #1
	subseq	pc, lr, #4		@ spurious: nothing to do
	str	r11, [r9, #0x0c]	@ INTCLR (any value)
	ldr	r12, [r10, #8]
	eor	r12, r12, #0x80000	@ toggle tracked level (bit 19)
	str	r12, [r10, #8]
	ldr	r11, [r8]
	bic	r11, r11, #0x80000
	orr	r11, r11, r12		@ force our level, keep other pins
	str	r11, [r8]
	cmp	r12, #0
	ldreq	r11, [r10, #0]		@ now low  -> next period is high
	ldrne	r11, [r10, #4]		@ now high -> next period is low
	str	r11, [r9, #0x18]	@ BGLOAD, applied at next reload
	ldr	r12, [r10, #12]
	add	r12, r12, #1
	str	r12, [r10, #12]
	dsb				@ INTCLR must land before FIQs reopen
	subs	pc, lr, #4
bpi_fiq_end:
