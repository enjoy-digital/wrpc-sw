#ifndef __CHEBY__SPLL_HOST_MAP__H__
#define __CHEBY__SPLL_HOST_MAP__H__

#define SPLL_HOST_MAP_SIZE 12 /* 0xc */

/* Fifo output (data) */
#define SPLL_HOST_MAP_DFR_HOST_R0 0x0UL

/* Fifo status */
#define SPLL_HOST_MAP_DFR_HOST_CSR 0x8UL
#define SPLL_HOST_MAP_DFR_HOST_CSR_USEDW_MASK 0x1fffUL
#define SPLL_HOST_MAP_DFR_HOST_CSR_USEDW_SHIFT 0
#define SPLL_HOST_MAP_DFR_HOST_CSR_EMPTY 0x20000UL
#define SPLL_HOST_MAP_DFR_HOST_CSR_EMPTY_MASK 0x20000UL
#define SPLL_HOST_MAP_DFR_HOST_CSR_EMPTY_SHIFT 17

#ifndef __ASSEMBLER__
struct spll_host_map {
  /* [0x0]: REG (ro) Fifo output (data) */
  uint32_t DFR_HOST_R0;

  /* padding to: 8 Bytes */
  uint32_t __padding_0[1];

  /* [0x8]: REG (ro) Fifo status */
  uint32_t DFR_HOST_CSR;
};
#endif /* !__ASSEMBLER__*/

#endif /* __CHEBY__SPLL_HOST_MAP__H__ */
