	.section .vector_table, "x"
	.global _reset_vector
_reset_vector:
	b _boot
	b . /* 0x4  Undefined Instruction */
	b . /* 0x8  Software Interrupt */
	b . /* 0xC  Prefetch Abort */
	b .	/* _data_abort_handler *//* 0x10 Data Abort */
	b . /* 0x14 Reserved */
	b	_irq_handler /* 0x18 IRQ */
	b . /* 0x1C FIQ */


	.section .text
d1:	.word 0x1

_boot:
	/* Initialize processor registers to 0 */
	mov	r0,#0
	mov	r1,#0
	mov	r2,#0
	mov	r3,#0
	mov	r4,#0
	mov	r5,#0
	mov	r6,#0
	mov	r7,#0
	mov	r8,#0
	mov	r9,#0
	mov	r10,#0
	mov	r11,#0
	mov	r12,#0

	/* Initialize stack pointer and banked registers for various mode */
	mrs	r0, cpsr		/* get the current PSR */
	mvn	r1, #0x1f		/* set up the irq stack pointer */
	and	r0, r1, r0

	orr	r2, r0, #0x12		/* IRQ mode */
	msr	cpsr, r2
	ldr	r13,=__irq_stack	/* IRQ stack pointer */
	mov 	r14,#0

	/* TODO: other modes */

	orr	r2, r0, #0x1F		/* SYS mode */
	msr	cpsr, r2
	ldr	r13,=__stack		/* SYS stack pointer */
	mov 	r14,#0

	/*
	* Enable access to VFP by enabling access to Coprocessors 10 and 11.
	* Enables Full Access i.e. in both privileged and non privileged modes
	*/
	mrc     p15, 0, r0, c1, c0, 2      	/* Read Coprocessor Access Control Register (CPACR) */
        orr     r0, r0, #(0xF << 20)       	/* Enable access to CP 10 & 11 */
        mcr     p15, 0, r0, c1, c0, 2      	/* Write Coprocessor Access Control Register (CPACR) */
        isb

	/* enable fpu access  */
	vmrs	r3, FPEXC
	orr	r1, r3, #(1<<30)
	vmsr	FPEXC, r1

	/* clear the floating point register*/
	mov	r1,#0
	vmov	d0,r1,r1
	vmov	d1,r1,r1
	vmov	d2,r1,r1
	vmov	d3,r1,r1
	vmov	d4,r1,r1
	vmov	d5,r1,r1
	vmov	d6,r1,r1
	vmov	d7,r1,r1
	vmov	d8,r1,r1
	vmov	d9,r1,r1
	vmov	d10,r1,r1
	vmov	d11,r1,r1
	vmov	d12,r1,r1
	vmov	d13,r1,r1
	vmov	d14,r1,r1
	vmov	d15,r1,r1

#ifdef __SOFTFP__
	/* Disable the FPU if SOFTFP is defined*/
	vmsr	FPEXC,r3
#endif

	/* Disable MPU and caches */
        mrc     p15, 0, r0, c1, c0, 0   /* Read CP15 Control Register SCLTR */
        bic     r0, r0, #0x05           /* Disable MPU (M bit) and data cache (C bit) */
        bic     r0, r0, #0x1000         /* Disable instruction cache (I bit) */
#if 0
	orr	r0, r0, #0x2000		/* Enable HIVECS */
#endif
        dsb                             /* Ensure all previous loads/stores have completed */
        mcr     p15, 0, r0, c1, c0, 0   /* Write CP15 Control Register */
        isb                             /* Ensure subsequent insts execute wrt new MPU settings */

	/* Disable Branch prediction, TCM ECC checks */
        mrc     p15, 0, r0, c1, c0, 1       	/* Read ACTLR */
        orr     r0, r0, #(0x1 << 17)        	/* Enable RSDIS bit 17 to disable the return stack */
        orr     r0, r0, #(0x1 << 16)        	/* Clear BP bit 15 and set BP bit 16:*/
        bic     r0, r0, #(0x1 << 15)        	/* Branch always not taken and history table updates disabled*/
        orr     r0, r0, #(0x1 << 27)		/* Enable B1TCM ECC check */
        orr     r0, r0, #(0x1 << 26)		/* Enable B0TCM ECC check */
        orr     r0, r0, #(0x1 << 25)		/* Enable ATCM ECC check */
	bic	r0, r0, #(0x1 << 5)		/* Generate abort on parity errors, with [5:3]=b 000*/
	bic 	r0, r0, #(0x1 << 4)
	bic	r0, r0, #(0x1 << 3)
        mcr     p15, 0, r0, c1, c0, 1       	/* Write ACTLR*/
	dsb				    	/* Complete all outstanding explicit memory operations*/

	/* Invalidate caches */
	mov	r0,#0				/* r0 = 0  */
	dsb
	mcr	p15, 0, r0, c7, c5, 0		/* invalidate icache */
	mcr 	p15, 0, r0, c15, c5, 0      	/* Invalidate entire data cache*/
	isb

	bl 	_init_mpu		/* Initialize MPU */

#if 0
	/* Copy vectors */
	ldr	r0,=_vector_table
	ldr	r1,=0xffff0000
	add	r4, r0, #0x40	/* 16 words */
0:
	ldr	r3, [r0], #4
	str	r3, [r1], #4
	cmp	r0, r4
	bls	0b
#endif

	/* Enable Branch prediction */
	mrc     p15, 0, r0, c1, c0, 1       /* Read ACTLR*/
        bic     r0, r0, #(0x1 << 17)        /* Clear RSDIS bit 17 to enable return stack*/
        bic     r0, r0, #(0x1 << 16)        /* Clear BP bit 15 and BP bit 16:*/
        bic     r0, r0, #(0x1 << 15)        /* Normal operation, BP is taken from the global history table.*/
        orr	r0, r0, #(0x1 << 14)	    /* Disable DBWR for errata 780125 */
	mcr     p15, 0, r0, c1, c0, 1       /* Write ACTLR*/

	/* Enable MPU, icache and dcache */
	mrc 	p15,0,r1,c1,c0,0
	ldr	r0, =0x1005
	orr 	r1,r1,r0
	dsb
	mcr	p15,0,r1,c1,c0,0		/* Enable cache  */
	isb					/* isb	flush prefetch buffer */

#if 0
	/* enable asynchronous abort exception */
	mrs	r0, cpsr
	bic	r0, r0, #0x100
	msr	cpsr_xsf, r0
#endif

#if 0
	/* More CPU initialization (see cpu_init.S) */
	bl	__cpu_init

	bl	map_tcm
#endif

	/* Clear BSS */
	mov	r0, #0

	ldr	r1,=__bss_start__
	ldr	r2,=__bss_end__
0:
	cmp	r1,r2
	bge	1f
	str	r0, [r1], #4
	b	0b

1:

	/* Increment d1 */
	adr r3, d1
	ldr r1, [r3]
	add r1, r1, #1
	str r1, [r3]

	bl	main			/* Jump to main C code */


.text
_irq_handler:
	stmdb	sp!,{r0-r3,r12,lr}
	bl	spll_irq_entry			/* IRQ vector */
	ldmia	sp!,{r0-r3,r12,lr}
	subs	pc, lr, #4			/* adjust return */

_data_abort_handler:
	stmdb	sp!,{r0-r3,r12,lr}
	sub     r0, lr, #8
	bl	DataAbortInterrupt
	ldmia	sp!,{r0-r3,r12,lr}
	subs	pc, lr, #8			/* adjust return */
