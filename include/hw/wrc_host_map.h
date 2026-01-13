#ifndef __CHEBY__WRC_HOST_MAP__H__
#define __CHEBY__WRC_HOST_MAP__H__


#include "spll_host_map.h"
#include "vuart_host_map.h"
#include "wrc_cpu_csr.h"
#define WRC_HOST_MAP_SIZE 3072 /* 0xc00 = 3KB */

/* REG spll */
#define WRC_HOST_MAP_SPLL 0x270UL
#define ADDR_MASK_WRC_HOST_MAP_SPLL 0xff0UL
#define ADDR_FMASK_WRC_HOST_MAP_SPLL 0xff0UL
#define WRC_HOST_MAP_SPLL_SIZE 16 /* 0x10 */

/* REG vuart */
#define WRC_HOST_MAP_VUART 0x510UL
#define ADDR_MASK_WRC_HOST_MAP_VUART 0xff8UL
#define ADDR_FMASK_WRC_HOST_MAP_VUART 0xff8UL
#define WRC_HOST_MAP_VUART_SIZE 8 /* 0x8 */

/* REG wdiags */
#define WRC_HOST_MAP_WDIAGS 0x900UL
#define ADDR_MASK_WRC_HOST_MAP_WDIAGS 0xf00UL
#define ADDR_FMASK_WRC_HOST_MAP_WDIAGS 0xf00UL
#define WRC_HOST_MAP_WDIAGS_SIZE 256 /* 0x100 */

/* REG cpu */
#define WRC_HOST_MAP_CPU 0xb00UL
#define ADDR_MASK_WRC_HOST_MAP_CPU 0xf00UL
#define ADDR_FMASK_WRC_HOST_MAP_CPU 0xf00UL
#define WRC_HOST_MAP_CPU_SIZE 256 /* 0x100 */

#ifndef __ASSEMBLER__
struct wrc_host_map {

  /* padding to: 624 Bytes */
  uint32_t __padding_0[156];
  /* [0x270]: SUBMAP */
  struct spll_host_map spll;

  /* padding to: 1296 Bytes */
  uint32_t __padding_1[165];

  /* [0x510]: SUBMAP */
  struct vuart_host_map vuart;

  /* padding to: 2304 Bytes */
  uint32_t __padding_2[250];

  /* [0x900]: SUBMAP */
  uint32_t wdiags[64];

  /* padding to: 2816 Bytes */
  uint32_t __padding_3[64];

  /* [0xb00]: SUBMAP */
  struct wrc_cpu_csr cpu;

  /* padding to: 3072 Bytes */
  uint32_t __padding_4[27];
};
#endif /* !__ASSEMBLER__*/

#endif /* __CHEBY__WRC_HOST_MAP__H__ */
