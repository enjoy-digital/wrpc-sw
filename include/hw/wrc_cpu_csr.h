#ifndef __CHEBY__WRC_CPU_CSR__H__
#define __CHEBY__WRC_CPU_CSR__H__

#define WRC_CPU_CSR_SIZE 148 /* 0x94 */

/* REG RESET */
#define WRC_CPU_CSR_RESET 0x0UL

/* REG UADDR */
#define WRC_CPU_CSR_UADDR 0x4UL

/* The address to read/write from/to is specified in the UADDR register. */
#define WRC_CPU_CSR_UDATA 0x8UL

/* REG DBG_STATUS */
#define WRC_CPU_CSR_DBG_STATUS 0x80UL

/* REG DBG_FORCE */
#define WRC_CPU_CSR_DBG_FORCE 0x84UL

/* REG DBG_INSN_READY */
#define WRC_CPU_CSR_DBG_INSN_READY 0x88UL

/* REG DBG_CORE0_INSN */
#define WRC_CPU_CSR_DBG_CORE0_INSN 0x8cUL

/* REG DBG_CORE0_MBX */
#define WRC_CPU_CSR_DBG_CORE0_MBX 0x90UL

#ifndef __ASSEMBLER__
struct wrc_cpu_csr {
  /* [0x0]: REG (rw) */
  uint8_t RESET;

  /* padding to: 4 Bytes */
  uint8_t __padding_0[3];

  /* [0x4]: REG (rw) */
  uint32_t UADDR;

  /* [0x8]: REG (rw) The address to read/write from/to is specified in the UADDR register. */
  uint32_t UDATA;

  /* padding to: 128 Bytes */
  uint32_t __padding_1[29];

  /* [0x80]: REG (ro) */
  uint8_t DBG_STATUS;

  /* padding to: 132 Bytes */
  uint8_t __padding_2[3];

  /* [0x84]: REG (rw) */
  uint8_t DBG_FORCE;

  /* padding to: 136 Bytes */
  uint8_t __padding_3[3];

  /* [0x88]: REG (ro) */
  uint8_t DBG_INSN_READY;

  /* padding to: 140 Bytes */
  uint8_t __padding_4[3];

  /* [0x8c]: REG (rw) */
  uint32_t DBG_CORE0_INSN;

  /* [0x90]: REG (rw) */
  uint32_t DBG_CORE0_MBX;
};
#endif /* !__ASSEMBLER__*/

#endif /* __CHEBY__WRC_CPU_CSR__H__ */
