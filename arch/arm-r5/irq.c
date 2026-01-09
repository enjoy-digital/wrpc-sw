/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2021 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Author: Adam Wujek
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include "irq.h"
#include <stdint.h>
#include "pp-printf.h"

/* CPSR bits */
#define R5_CPSR_FIQ (1<<6)
#define R5_CPSR_IRQ (1<<7)

/* GICv1 */
#define RCPU_GIC 0xf9000000
#define ICDDCR	   *((uint32_t *)(RCPU_GIC  + 0x000)) //Distributor Control
#define ICDISER(n) *(((uint32_t *)(RCPU_GIC + 0x100)) + n) //Int. set enable
#define ICDICER(n) *(((uint32_t *)(RCPU_GIC + 0x180)) + n) //Int. Clear Enable
#define ICDABR(n)  *(((uint32_t *)(RCPU_GIC + 0x300)) + n) //Active bit
#define ICDIPR(n)  *(((uint32_t *)(RCPU_GIC + 0x400)) + n) //Int. Priority
#define ICDIPTR(n) *(((uint32_t *)(RCPU_GIC + 0x800)) + n) //Int. Proc Target
#define ICDICFR(n) *(((uint32_t *)(RCPU_GIC + 0xC00)) + n) //Int. Configuration
#define ICCICR	   *((uint32_t *)(RCPU_GIC + 0x1000)) //CPU interface control
#define ICCPMR	   *((uint32_t *)(RCPU_GIC + 0x1004)) //Priority mask
#define ICCIAR	   *((uint32_t *)(RCPU_GIC + 0x100C)) //Int. Acknoledge
#define ICCEOIR	   *((uint32_t *)(RCPU_GIC + 0x1010)) //End of Interrupt
#define ICCRPR	   *((uint32_t *)(RCPU_GIC + 0x1014)) //Running priority
#define ICCHPIR	   *((uint32_t *)(RCPU_GIC + 0x1018)) //Highest pending int

#define PL_IRQ 121

void
init_irq(void)
{
    /* Disable GIC distributor */
    ICDDCR = 0;

    /* Driver IRQ from GIV (disable passthrough) */
    ICCICR = 0x3;

    /* Set GIC priority mask (to 0xff, the lowest) */
    ICCPMR = 0xff;

    /* Disable interrupt */
    ICDIPTR(PL_IRQ / 4) &= ~(0x3 << (8 * (PL_IRQ & 0x3)));
    ICDICER(PL_IRQ / 32) = 1 << (PL_IRQ & 0x1f);

    /* Set sensitivity to level (00) */
    ICDICFR(PL_IRQ / 16) &= ~(0x3 << (2 * (PL_IRQ & 0x0f)));
    ICDICFR(PL_IRQ / 16) |= 0 << (2 * (PL_IRQ & 0x0f));

    /* Set priority */
    ICDIPR(PL_IRQ / 4) &= ~(0xff << (8 * (PL_IRQ & 0x03)));
    ICDIPR(PL_IRQ / 4) |= 0xf0 << (8 * (PL_IRQ & 0x03));

    /* Target CPU #0 */
    ICDIPTR(PL_IRQ / 4) |= 1 << (8 * (PL_IRQ & 0x3));

    /* Enable */
    ICDISER(PL_IRQ / 32) = 1 << (PL_IRQ & 0x1f);

    /* Enable distributor */
    ICDDCR = 1;
}

void disable_irq(void)
{
	asm volatile ("cpsid i");
}

void enable_irq(void)
{
	asm volatile ("cpsie i");
}

void clear_irq(void)
{
	unsigned iack = ICCIAR;
	ICCEOIR = iack;
}

extern void DataAbortInterrupt(unsigned pc);

void DataAbortInterrupt(unsigned pc)
{
    unsigned dfsr, adfsr, dfar;

    asm volatile ("MRC p15,0,%0,c5,c0,0" : "=r"(dfsr));
    asm volatile ("MRC p15,0,%0,c5,c1,0" : "=r"(adfsr));
    asm volatile ("MRC p15,0,%0,c6,c0,0" : "=r"(dfar));
    pp_printf("data abort pc=%08x addr=%08x dfsr=%08x adfsr=%08x\n",
		pc, dfar, dfsr, adfsr);
}

#define cpu_isb() asm volatile ("isb")

static void
write_mpu_addr(uint32_t addr)
{
  asm volatile ("MCR p15,0,%0,c6,c1,0" : : "r"(addr));
}

static void
write_mpu_size(uint32_t val)
{
  asm volatile ("MCR p15,0,%0,c6,c1,2" : : "r"(val));
}

static void
write_mpu_acc(uint32_t val)
{
  asm volatile ("MCR p15,0,%0,c6,c1,4" : : "r"(val));
}

static void
write_mpu_num(uint32_t val)
{
  asm volatile ("MCR p15,0,%0,c6,c2,0" : : "r"(val));
  cpu_isb();
}

extern void _init_mpu(void);

/* Permissions: AP=3 (RW for all) */
#define ACC_NC 0x310 /* TEX=010 S=0 C=0 B=0 (non-shareable device) */
#define ACC_WB 0x30b /* TEX=001 S=0 C=1 B=1 (normal, write-back, write-alloc) */

/* Called by crt0.S to setup MPU */
void
_init_mpu(void)
{
  unsigned n = 0;

  /* 0: 0 - 2GB (TCM + ddram).
     Note: region attributes are ignored for TCM (cf DDI0460D p4-54) */
  write_mpu_num(n++);
  write_mpu_addr(0x00000000);
  write_mpu_acc(ACC_WB);
  write_mpu_size((30 << 1) | 1);

  /* 1: 0x80000000 2GB + 1GB (PL) */
  write_mpu_num(n++);
  write_mpu_addr(0x80000000);
  write_mpu_acc(ACC_NC);
  write_mpu_size((29 << 1) | 1);

#if 0
  /* 2: 0xC0000000 + 512M (qspi) */
  write_mpu_num(n++);
  write_mpu_addr(0xC0000000);
  write_mpu_acc(ACC_NC);
  write_mpu_size((28 << 1) | 1);

  /* 3: 0xE0000000 + 256M (PCIe) */
  write_mpu_num(n++);
  write_mpu_addr(0xE0000000);
  write_mpu_acc(ACC_NC);
  write_mpu_size((27 << 1) | 1);
#endif

  /* 5: 0xF9000000 + 8K (GIC) */
  write_mpu_num(n++);
  write_mpu_addr(0xf9000000);
  write_mpu_acc(ACC_NC);
  write_mpu_size((12 << 1) | 1);

  /* 6: 0xFD000000 + 32M (FPS + upper LPS) */
  write_mpu_num(n++);
  write_mpu_addr(0xFD000000);
  write_mpu_acc(ACC_NC);
  write_mpu_size((24 << 1) | 1);

  /* 8: 0xFF000000 + 16M (lower LPS + TCM) */
  write_mpu_num(n++);
  write_mpu_addr(0xFF000000);
  write_mpu_acc(ACC_NC);
  write_mpu_size((23 << 1) | 1);

  /* 9: 0xFFFC0000 + 256K (OCM) */
  write_mpu_num(n++);
  write_mpu_addr(0xFFFC0000);
  write_mpu_acc(ACC_WB);
  write_mpu_size((17 << 1) | 1);

  /* Disable remaing entries */
  while (n < 16) {
    write_mpu_num(n++);
    write_mpu_size(0);
  }
}
