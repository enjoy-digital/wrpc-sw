/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2019 CERN (home.cern)
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/time.h>
#include <stdbool.h>
#include <time.h>
#include <limits.h>
#include <termios.h>
#include <signal.h>
#include <stddef.h>
#include <elf.h>

#ifdef SUPPORT_CERN_VMEBRIDGE
#include <libvmebus.h>
#endif

#ifdef SUPPORT_ERTM
#include "libertm.h"
#endif

#include "spll_debug.h"

#include "hw/wrc_cpu_csr.h"
#include "hw/wrc_syscon_regs.h"
#include "hw/wb_uart.h"
#include "hw/spll_host_map.h"
#include "hw/wrc_host_map.h"
#include "hw/wrc_diags_regs.h"
#include "hw/endpoint_regs.h"

#ifdef CONFIG_TARGET_WR_SWITCH
/* Not all features are available when building for a switch */
#define SUPPORT_WRS 1
#endif

#ifdef SUPPORT_WRS
	#define BASE_FPGA		0x10000000
	#define OFFSET_CPU_CSR  	0x00010800
	#define SIZE_FPGA 		0x20000
	#define OFFSET_UART 		0x00010000
	#define OFFSET_SOFTPLL  	0x00010170
#else
	/* From include/boards.h */
	#define OFFSET_ENDPOINT		0x100
	#define OFFSET_SOFTPLL		WRC_HOST_MAP_SPLL
	#define OFFSET_SYSCON		0x400
	#define OFFSET_UART		0x500
	#define OFFSET_WDIAGS		0x900
	#define OFFSET_CPU_CSR		0xb00

#endif

#define WRPCV4_BASE_SYSCON 0x20400

#define VUART_EOL 13
#define VUART_CMD_USLEEP 1000000
#define VUART_CMD_PROMPT "wrc#"

static const char *progname;

struct tool_base {
	const char *name;
        const char *short_help;
	int (*run)(int argc, char *argv[]);
	void (*help)(void);
};

struct board {
	const char *name;
	int (*init)(struct board *board, int *argc, char *argv[]);
	int (*fini)(struct board *board);
	void (*help)(void);

	/* Read/Write to the ptp core register at OFF */
	uint32_t (*readl)(struct board *board, unsigned off);
	void (*writel)(struct board *board, unsigned off, uint32_t v);

	/* Map 4KB of the local bus at address ADDR, for ZynqUS+ only.
	   The previous map (if present) is invalidated.
	   TODO: handle multiple map ?  handle other processor ?  */
	void *(*map)(struct board *board, unsigned addr);
	void (*unmap)(struct board *board);
};

static struct board *board;

static const struct tool_base *tools[];

static int verbose;
static int flag_check;

/* How much memory to map  */
static unsigned map_size = 4 << 10;

static void remove_arg1(int *argc, char *argv[])
{
	for (unsigned i = 2; i < *argc; i++)
		argv[i - 1] = argv[i];
	(*argc)--;
}

/* Any board which can be mapped into memory.  */
struct board_mem {
	struct board parent;
	volatile void *base;
	int is_be;
	/* Virtual address of the PTP core */
	void *map_addr;
	unsigned map_length;
};

struct board_pci {
	struct board_mem parent;
	const char *resource_file;
	uint64_t offset;
};

struct board_ertm14 {
	struct board parent;
	const char* uart_dev;
	struct ertm_status *handle;
};

struct pci_slot {
	unsigned domain;
	unsigned bus;
	unsigned slot;
	unsigned func;

	unsigned bar;
};

struct board_host {
	struct board_mem parent;
};

struct board_wrs{
	struct board_mem parent;
};

static int parse_pci_slot(struct pci_slot *res, const char *s)
{
	char *e;

	res->domain = strtoul (s, &e, 16);
	if (*e != ':') {
		fprintf (stderr, "missing pci device id in '%s'\n", s);
		return -1;
	}
	res->bus = strtoul (e + 1, &e, 16);
	if (*e == ':')
		res->slot = strtoul (e + 1, &e, 16);
	else {
		res->slot = res->bus;
		res->bus = res->domain;
		res->domain = 0;
	}
	if (*e == '.')
		res->func = strtoul (e + 1, &e, 16);
	else
		res->func = 0;
	if (*e == '@')
		res->bar = strtoul (e + 1, &e, 10);
	else
		res->bar = 0;
	if (*e != 0) {
		fprintf (stderr, "incorrect pci slot format in '%s'\n", s);
		return -1;
	}
	return 0;
}

static void *pci_map_resource(const char *resource_file,
			      unsigned off, unsigned len)
{
	int fd;
	void *res;

	fd = open(resource_file, O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "cannot open resource file '%s': %s\n",
			resource_file, strerror(errno));
		return NULL;
	}

	res = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
	if (res == MAP_FAILED) {
		fprintf(stderr, "cannot map resource file '%s': %s\n",
			resource_file, strerror(errno));
		close(fd);
		return NULL;
	}
	close(fd);

	return res;
}

static int board_pci_common_open(struct board_pci *board)
{
	unsigned pg = getpagesize();
	unsigned pa_offset;

	/* offset is page aligned */
	pa_offset = board->offset & ~(pg - 1);

	board->parent.map_addr =
		pci_map_resource(board->resource_file, pa_offset, map_size);
	if (board->parent.map_addr == NULL)
		return -1;

	board->parent.map_length = map_size;
	board->parent.base =
		board->parent.map_addr + (board->offset - pa_offset);

	board->parent.is_be = 0; /* default set to little endian */

	return 0;
}

/* Parse PCI board identifier (either resource file or slot).  */

static int parse_pci_board(struct board_pci *board, int *argc, char *argv[])
{
	/* Args: -f file, -o offset */
	if (*argc > 2 && !strcmp (argv[1], "-f")) {
		remove_arg1(argc, argv);
		board->resource_file = argv[1];
		remove_arg1(argc, argv);
	}
	else if (*argc > 2 && !strcmp(argv[1], "-s")) {
		static char pci_file[64];
		struct pci_slot slot;
		remove_arg1(argc, argv);
		if (parse_pci_slot(&slot, argv[1]) < 0)
			return -1;
		remove_arg1(argc, argv);
		snprintf (pci_file, sizeof(pci_file),
			  "/sys/bus/pci/devices/%04x:%02x:%02x.%x/resource%u",
			  slot.domain, slot.bus, slot.slot, slot.func,
			  slot.bar);
		board->resource_file = pci_file;
	}
	else {
		fprintf(stderr, "missing '-f resource-file' or '-s [dom:]bus:slot[.fn][@bar]' for pci\n");
		return -1;
	}

        return 0;
}

static int board_pci_init(struct board *board_base,
			  int *argc, char *argv[])
{
	struct board_pci *board = (struct board_pci *)board_base;

        if (parse_pci_board(board, argc, argv) < 0)
                return -1;

	if (*argc > 2 && !strcmp (argv[1], "-o")) {
		char *e;
		remove_arg1(argc, argv);
		board->offset = strtoul(argv[1], &e, 0);
		if (*e != 0) {
			fprintf (stderr, "bad offset '%s'\n", argv[1]);
			return -1;
		}
		remove_arg1(argc, argv);
	}

	if (board_pci_common_open(board) < 0)
		return -1;
	return 0;
}


static int board_pci_fini(struct board *base_board)
{
	struct board_pci *board = (struct board_pci *)base_board;
	munmap(board->parent.map_addr, board->parent.map_length);

	return 0;
}

static void board_pci_help(void)
{
        printf("Generic PCI board\n");
        printf(" -f resource-file\n");
        printf(" -s [domain:]bus:slot[.func][@bar]\n");
        printf(" -o offset\n");
        printf("One of -f or -s is required to identify the board\n");
}

#ifdef SUPPORT_ERTM
static void board_ertm14_help(void)
{
        printf("eRTM14/15 board\n");
        printf(" -e USB/UART device \n");
        printf("Note only the SoftPLL recorder tool is available. Others will not work.\n");
}

static int board_ertm14_fini(struct board *base_board)
{
	struct board_ertm14 *board = (struct board_ertm14 *)base_board;

	if(board->handle)
		ertm_exit(board->handle);

	return 0;
}


static int board_ertm14_init(struct board *board_base,
			  int *argc, char *argv[])
{
	struct board_ertm14 *board = (struct board_ertm14 *)board_base;
	printf("bi");

	/* Args: -f file, -o offset */
	if (*argc > 2 && !strcmp (argv[1], "-e")) {
		remove_arg1(argc, argv);
		board->uart_dev = argv[1];
		remove_arg1(argc, argv);
	}

	if( !board->uart_dev )
	{
		fprintf(stderr, "ertm14: USB/UART device expected (-e option, check help).\n");
		return -1;
	}

	struct ertm_status *handle = ertm_init(board->uart_dev);

	if (handle == NULL)
	{
		fprintf(stderr, "ertm14: could not open %s\n", board->uart_dev);
		return -1;
	}

	board->handle = handle;

	return 0;
}

static struct board_ertm14 board_ertm14 =
{
	{
		"ertm14",
		board_ertm14_init,
		board_ertm14_fini,
		board_ertm14_help,
		NULL,
		NULL
	},
	NULL,
	NULL
};
#endif /* SUPPORT_ERTM */

static uint32_t mem_readl(struct board *base_board, unsigned reg)
{
	struct board_mem *board = (struct board_mem *)base_board;

	uint32_t r = *(volatile uint32_t *)(board->base + reg);

	if (board->is_be)
		return ntohl(r);
	else
		return r;
}

static void mem_writel(struct board *base_board, unsigned reg, uint32_t value)
{
	struct board_mem *board = (struct board_mem *)base_board;

	if (board->is_be)
		value = htonl(value);

	*(volatile uint32_t *)(board->base + reg ) = value;
}

static void board_host_help(void)
{
	printf("host (WRPC on an AXI bus)\n");
	printf(" -b BASE      address of wrpc (required)\n");
	printf(" -f /dev/mem  (default)\n");
}

static int board_host_fini(struct board *base_board)
{
	struct board_host *board = (struct board_host *)base_board;
	munmap(board->parent.map_addr, board->parent.map_length);
	return 0;
}

static int board_host_init(struct board *board_base,
			   int *argc, char *argv[])
{
	struct board_host *board = (struct board_host *)board_base;
	const char *mem_file = "/dev/mem";
	unsigned long base = 0;
	int fd;

	while (*argc != 1) {
		if (*argc > 2 && !strcmp (argv[1], "-b")) {
			char *e;
			remove_arg1(argc, argv);
			base = strtoul(argv[1], &e, 0);
			if (*e != 0) {
				fprintf (stderr, "bad base '%s'\n", argv[1]);
				return -1;
			}
			remove_arg1(argc, argv);
		}
		else if (*argc > 2 && !strcmp (argv[1], "-f")) {
			remove_arg1(argc, argv);
			mem_file = argv[1];
			remove_arg1(argc, argv);
		}
		else {
			/* Not handled by board, pass it to the tool */
			break;
		}
	}

	if (base == 0) {
		fprintf(stderr, "option '-b BASE' is required\n");
		return -1;
	}

	fd = open(mem_file, O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "cannot open resource file '%s': %s\n",
			mem_file, strerror(errno));
		return -1;
	}

	board->parent.map_addr = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
				      MAP_SHARED, fd, base);


	if (board->parent.map_addr == MAP_FAILED) {
		fprintf(stderr, "cannot map resource file '%s': %s\n",
			mem_file, strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);

	board->parent.map_length = map_size;
	board->parent.base = board->parent.map_addr;

	board->parent.is_be = 0;

	return 0;

}

#ifdef SUPPORT_WRS

static void board_wrs_help(void)
{
	printf("wrsv3 board\n");
}

static int board_wrs_fini(struct board *base_board)
{
	struct board_wrs *board = (struct board_wrs *)base_board;
	munmap(board->parent.map_addr, board->parent.map_length);
	return 0;
}

static int board_wrs_init(struct board *board_base,
			  int *argc, char *argv[])
{

	struct board_wrs *board = (struct board_wrs *)board_base;
	#ifdef CONFIG_TARGET_WR_SWITCH_V4
		printf("init wrsv4\n");
	#else
		printf("init wrsv3\n");
	#endif
	int fd;
	unsigned pg = getpagesize();

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "cannot open resource file '%s': %s\n",
			"/dev/mem", strerror(errno));
		return -1;
	}

	board->parent.map_addr = mmap(NULL, SIZE_FPGA, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd,
		       BASE_FPGA);


	if (board->parent.map_addr == MAP_FAILED) {
		fprintf(stderr, "cannot map resource file '%s': %s\n",
			"/dev/mem", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);

	board->parent.map_length = pg;
	board->parent.base = board->parent.map_addr;
	board->parent.is_be = 1;

	return 0;

}

//FIXME: update gateware so can use same readl/writel for wrsv3 and wrsv4
static uint32_t mem_wrsv3_readl(struct board *base_board, unsigned reg)
{
	struct board_mem *board = (struct board_mem *)base_board;

	uint32_t r = *(volatile uint32_t *)(board->base + reg);
	return r;
}

static void mem_wrsv3_writel(struct board *base_board, unsigned reg, uint32_t value)
{
	struct board_mem *board = (struct board_mem *)base_board;
	*(volatile uint32_t *)(board->base + reg ) = value;
}
#endif /* SUPPORT_WRS */

static struct board_host board_host =
{
	{
		{
			"host",
			board_host_init,
			board_host_fini,
			board_host_help,
			mem_readl,
			mem_writel
		},
		NULL,
		0,
		NULL,
		0
	},
};

#ifdef SUPPORT_WRS
static struct board_wrs board_wrs =
{
	{
		{
			"wrs",
			board_wrs_init,
			board_wrs_fini,
			board_wrs_help,
			mem_wrsv3_readl,
			mem_wrsv3_writel
		},
		NULL,
		0,
		NULL,
		0
	},
};

#endif /* SUPPORT_WRS */


static struct board_pci board_pci =
{
	{
		{
			"pci",
			board_pci_init,
			board_pci_fini,
			board_pci_help,
			mem_readl,
			mem_writel
		},
		NULL,
		0,
		NULL,
		0
	},
	NULL,
	0
};


static int board_spec_init(struct board *board_base,
			  int *argc, char *argv[])
{
	struct board_pci *board = (struct board_pci *)board_base;

        if (parse_pci_board(board, argc, argv) < 0)
                return -1;

	if (board_pci_common_open(board) < 0)
		return -1;
	return 0;
}

static void board_spec_help(void)
{
        printf("SPEC board (using the convention)\n");
        printf(" -f resource-file \n");
        printf(" -s [domain:]bus:slot[.func]\n");
        printf("One of -f or -s is required to identify the board\n");
        printf("(wrpc is at offset 0x1000)\n");
}

static struct board_pci board_spec =
{
	{
		{
			"spec",
			board_spec_init,
			board_pci_fini,
			board_spec_help,
			mem_readl,
			mem_writel
		},
		NULL,
		0,
		NULL,
		0
	},
	NULL,
	0x1000
};

#ifdef SUPPORT_CERN_VMEBRIDGE
struct board_cernvme {
        struct board_mem parent;
	uint32_t data_width; /**< default register size in bytes */
	uint32_t am; /**< VME address modifier to use */
	uint64_t addr; /**< physical base address */
        uint32_t offset;
        struct vme_mapping map;
};

static int cernvme_map_wrpc(struct board_cernvme *board)
{
        unsigned pg = getpagesize();

        memset(&board->map, 0, sizeof(struct vme_mapping));
        board->map.am = board->am;
        board->map.data_width = board->data_width;
        board->map.sizel = map_size;
        board->map.vme_addrl = board->addr | (board->offset & ~(pg - 1));

        board->parent.map_addr = vme_map(&board->map, 1);
        if (!board->parent.map_addr) {
                fprintf(stderr, "cannot map vme: %s\n", strerror(errno));
                return -1;
        }
        board->parent.map_length = map_size;
	board->parent.base = board->parent.map_addr + (board->offset & (pg - 1));

        board->parent.is_be = 1;

        return 0;
}

static unsigned cernvme_slot_to_addr(unsigned slot, unsigned verbose)
{
	struct vme_mapping map;
	unsigned char *map_addr;
	unsigned i;
	unsigned res;

        memset(&map, 0, sizeof(struct vme_mapping));
        map.am = 0x2f;
        map.data_width = 32;
        map.sizel = 0x80000;
        map.vme_addrl = slot << 19;

        map_addr = vme_map(&map, 1);
        if (!map_addr) {
                fprintf(stderr, "cannot map vme CS/CSR: %s\n", strerror(errno));
                return 0;
        }

	res = 0;

	for (i = 0; i < 4; i++) {
		unsigned adem = 0;
		unsigned ader = 0;
		unsigned j;

		for (j = 0; j < 4; j++) {
			adem <<= 8;
			adem |= map_addr[0x623 + (i * 0x10) + (j << 2)];
		}
		if (verbose > 1)
			printf("adem[%u] = %08x\n", i, adem);
		if (adem == 0)
			continue;
		adem &= ~0xffU;

		for (j = 0; j < 4; j++) {
			ader <<= 8;
			ader |= map_addr[0x7ff63 + (i * 0x10) + (j << 2)];
		}
		if (verbose > 1)
			printf("ader[%u] = %08x, ader & adem = %08x\n",
			       i, ader, ader & adem);
		res = ader & adem;
		if (res) {
			if (verbose)
				printf("VME slot %u: use ader %u, address: %08x\n", slot, i, res);
			break;
		}
	}

	vme_unmap(&map, 1);

	return res;
}

static void board_cernvme_default(struct board_cernvme *board)
{
	board->offset = 0;
	board->data_width = 32;
	board->am = 0x39;
	board->addr = ~0;
}

static int board_cernvme_init_common(struct board *board_base,
				     int *argc, char *argv[])
{
	struct board_cernvme *board = (struct board_cernvme *)board_base;

        while (*argc > 2) {
                if (argv[1][0] != '-')
                        break;

		if (!strcmp(argv[1], "-v")) {
			remove_arg1(argc, argv);
			verbose++;
		}
                else if (!strcmp(argv[1], "-a") || !strcmp(argv[1], "--address")) {
                        char *e;
                        remove_arg1(argc, argv);
                        board->addr = strtoul(argv[1], &e, 0);
                        if (*e != 0) {
                                fprintf(stderr, "invalid address '%s'\n", argv[1]);
                                return -1;
                        }
                        remove_arg1(argc, argv);
                }
                else if (!strcmp(argv[1], "-s") || !strcmp(argv[1], "--slot")) {
                        char *e;
                        unsigned slot;

                        remove_arg1(argc, argv);
                        slot = strtoul(argv[1], &e, 0);
                        if (*e != 0) {
                                fprintf(stderr, "invalid slot '%s'\n", argv[1]);
                                return -1;
                        }
			board->addr = cernvme_slot_to_addr(slot, verbose);
			if (!board->addr) {
				fprintf(stderr, "cannot find address for slot %u\n", slot);
				return -1;
			}
                        remove_arg1(argc, argv);
                }
                else if (!strcmp(argv[1], "-w")
                         || !strcmp(argv[1], "--data-width")) {
                        char *e;

                        remove_arg1(argc, argv);
                        board->data_width = strtoul(argv[1], &e, 0);
                        if (*e != 0) {
                                fprintf(stderr, "invalid data-width '%s'\n", argv[1]);
                                return -1;
                        }
                        if (!(board->data_width == 8
                              || board->data_width == 16
                              || board->data_width == 32)) {
                                fprintf(stderr, "invalid data-width %u\n",
                                        board->data_width);
                                return -1;
                        }
                        remove_arg1(argc, argv);
                }
                else if (!strcmp(argv[1], "-m")
                         || !strcmp(argv[1], "--am")) {
                        char *e;

                        remove_arg1(argc, argv);
                        board->am = strtoul(argv[1], &e, 0);
                        if (*e != 0) {
                                fprintf(stderr, "invalid address-modifier '%s'\n", argv[1]);
                                return -1;
                        }
                        remove_arg1(argc, argv);
                }
                else if (!strcmp (argv[1], "-o")
                         || !strcmp(argv[1], "--offset")) {
                        char *e;
                        remove_arg1(argc, argv);
                        board->offset = strtoul(argv[1], &e, 0);
                        if (*e != 0) {
                                fprintf (stderr, "bad offset '%s'\n", argv[1]);
                                return -1;
                        }
                        remove_arg1(argc, argv);
                }
                else {
			/* Option not handled by vme, pass it to the tool */
                        break;
		}

        }

        if (board->addr == ~0) {
                fprintf (stderr,
                         "vme address (-a) or vme slot (-s) required\n");
                return -1;
        }

        return cernvme_map_wrpc (board);
}

static int board_cernvme_init(struct board *board_base,
                              int *argc, char *argv[])
{
	struct board_cernvme *board = (struct board_cernvme *)board_base;
	int res;

	board_cernvme_default(board);
	res = board_cernvme_init_common(board_base, argc, argv);
	if (res < 0)
		return res;

	board->parent.is_be = 1;
	return 0;
}

static int board_cernvme_fini(struct board *base_board)
{
	struct board_cernvme *board = (struct board_cernvme *)base_board;
        vme_unmap(&board->map, 1);

	return 0;
}

static void board_cernvme_help_common(void)
{
        printf("VME board (using CERN-vme bridge)\n");
        printf(" -a, --address ADDR   board address\n");
        printf(" -s, --slot SLOT      board slot (use vme64x CS/CSR)\n");
        printf(" -w, --data-width WD  data width\n");
        printf(" -m, --am AM          address modified\n");
        printf(" -o, --offset OFF     offset\n");
        printf("One of -a or -s is required\n");
}

static void board_cernvme_help(void)
{
	printf("VME board (using CERN-vme bridge)\n");
	board_cernvme_help_common();
}

static struct board_cernvme board_cernvme =
{
	/* board_mem */
	{
		/* board */
		{
			"vme",
			board_cernvme_init,
			board_cernvme_fini,
			board_cernvme_help,
			mem_readl,
			mem_writel
		},
		NULL,
		0,
		NULL,
		0
	},
};

static int board_cernvme_le_init(struct board *board_base,
				  int *argc, char *argv[])
{
	struct board_cernvme *board = (struct board_cernvme *)board_base;
	int res;

	board_cernvme_default(board);
	res = board_cernvme_init_common(board_base, argc, argv);
	if (res < 0)
		return res;

	board->parent.is_be = 0;
	return 0;
}

static void board_cernvme_le_help(void)
{
	printf("VME little-endian board (using CERN-vme bridge)\n");
	board_cernvme_help_common();
}

static struct board_cernvme board_cernvme_le =
{
	{
		{
			"vme-le",
			board_cernvme_le_init,
			board_cernvme_fini,
			board_cernvme_le_help,
			mem_readl,
			mem_writel,
			NULL
		},
		NULL,
		0,
		NULL,
		0
	},
};

static int wren_vme_init(struct board *board_base,
			 int *argc, char *argv[])
{
	struct board_cernvme *board = (struct board_cernvme *)board_base;
	int res;

	board_cernvme_default(board);
	board->offset = 0x1000;

	res = board_cernvme_init_common(board_base, argc, argv);
	if (res < 0)
		return res;

	board->parent.is_be = 0;
	return 0;
}

static void *wren_vme_map(struct board *base_board, unsigned addr)
{
	/* Map the page with windows */
	struct board_cernvme *board = (struct board_cernvme *)base_board;
	struct vme_mapping vmap;
	uint32_t *vme_win;
	void *res;

        memset(&vmap, 0, sizeof(struct vme_mapping));
        vmap.am = board->map.am;
        vmap.data_width = board->map.data_width;
        vmap.sizel = 4096;
        vmap.vme_addrl = board->addr | 0x2000;

        vme_win = vme_map(&vmap, 1);
        if (!vme_win) {
                fprintf(stderr, "cannot map vme: %s\n", strerror(errno));
                return NULL;
        }

	for (unsigned i = 0; i < 8; i++)
		printf ("wren vme window %u: %08x\n",
			i, (unsigned)vme_win[(0x200 >> 2) + i]);

	/* Set window */
	vme_win[(0x200 >> 2) + 6] = addr;

	vme_unmap(&vmap, 0);

	/* Map the window */

        memset(&vmap, 0, sizeof(struct vme_mapping));
        vmap.am = board->map.am;
        vmap.data_width = board->map.data_width;
        vmap.sizel = 4096;
        vmap.vme_addrl = board->addr | 0x8000 | (0x1000 * 6);

        res = vme_map(&vmap, 1);
        if (!res) {
                fprintf(stderr, "cannot map vme: %s\n", strerror(errno));
                return NULL;
        }

	return res;
}

static struct board_cernvme board_wren_vme =
{
	/* board_mem */
	{
		/* board */
		{
			"wren-vme",
			wren_vme_init,
			board_cernvme_fini,
			board_cernvme_le_help,
			mem_readl,
			mem_writel,
			wren_vme_map
		},
		NULL,
		0,
		NULL,
		0
	},
};

static uint32_t wr2rf_readl(struct board *base_board, unsigned reg)
{
	struct board_mem *board = (struct board_mem *)base_board;
	volatile uint16_t *addr = (volatile uint16_t *)(board->base + reg);
	uint32_t l, h, res;

	/* A 16b VME bus with special circuitery to get an atomic 32b value */
	l = addr[0];
	h = addr[1];
	res = (l << 16) | h;
	return ntohl(res);
}

static void wr2rf_writel(struct board *base_board, unsigned reg, uint32_t val)
{
	struct board_mem *board = (struct board_mem *)base_board;
        volatile uint16_t *addr = (volatile uint16_t *)(board->base + reg);

        val = htonl(val);

        addr[1] = val & 0xffff;
	addr[0] = val >> 16;
}

static int board_wr2rf_init(struct board *board_base,
                            int *argc, char *argv[])
{
	struct board_cernvme *board = (struct board_cernvme *)board_base;

        unsigned vme_addr;

        if (*argc > 2
            && (!strcmp(argv[1], "-s") || !strcmp(argv[1], "--slot"))) {
                char *e;
                unsigned slot;

                remove_arg1(argc, argv);
                slot = strtoul(argv[1], &e, 0);
                if (*e != 0) {
                        fprintf(stderr, "invalid slot '%s'\n", argv[1]);
                        return -1;
                }
                vme_addr = slot << 19;
                remove_arg1(argc, argv);
        }
        else {
                fprintf(stderr, "missing slot number for wr2rf\n");
                return -1;
        }

	board->am = 0x39;
	board->data_width = 16;
	board->addr = vme_addr;
	board->offset = 0x2000;

        return cernvme_map_wrpc(board);
}

static void board_wr2rf_help(void)
{
        printf("wr2rf board (using CERN-vme bridge)\n");
        printf(" -s, --slot ADDR      board slot (512KB steps)\n");
}

static struct board_cernvme board_wr2rf =
{
	{
		{
			"wr2rf",
			board_wr2rf_init,
			board_cernvme_fini,
			board_wr2rf_help,
			wr2rf_readl,
			wr2rf_writel
		},
		NULL,
		0,
		NULL,
		0
	},
};
#endif

struct board_wren_pcie {
	struct board_mem parent;
	struct pci_slot slot;

	/* For map/unmap */
	void *soc_addr;
};

static int board_wren_pcie_init(struct board *board_base,
				int *argc, char *argv[])
{
	struct board_wren_pcie *board = (struct board_wren_pcie *)board_base;
	char pcie_file[64];

	if (*argc > 2 && !strcmp(argv[1], "-s")) {
		remove_arg1(argc, argv);
		if (parse_pci_slot(&board->slot, argv[1]) < 0)
			return -1;
		remove_arg1(argc, argv);
	}
	else {
		fprintf(stderr, "missing '-s [dom:]bus:slot[.fn][@bar]' for wren-pcie\n");
		return -1;
	}

	board->slot.bar = 1;

	snprintf (pcie_file, sizeof(pcie_file),
		  "/sys/bus/pci/devices/%04x:%02x:%02x.%x/resource%u",
		  board->slot.domain, board->slot.bus,
		  board->slot.slot, board->slot.func,
		  board->slot.bar);

	board->parent.map_addr = pci_map_resource(pcie_file, 0x1000, map_size);
	if (board->parent.map_addr == NULL)
		return -1;
	board->parent.base = board->parent.map_addr;
	board->parent.is_be = 0; /* default set to little endian */

	return 0;
}

struct xpcie_ingress {
  uint32_t capabilities;
  uint32_t status;
  uint32_t control;
  uint32_t pad_0c;

  uint32_t src_base_lo;
  uint32_t src_base_hi;
  uint32_t dst_base_lo;
  uint32_t dst_base_hi;
};

static void board_wren_pcie_unmap(struct board *base_board)
{
	struct board_wren_pcie *board = (struct board_wren_pcie *)base_board;

	if (board->soc_addr) {
		munmap(board->soc_addr, 0x1000);
		board->soc_addr = NULL;
	}
}

static void *board_wren_pcie_map(struct board *base_board, unsigned addr)
{
	char pcie_file[64];
	struct board_wren_pcie *board = (struct board_wren_pcie *)base_board;
	void *ingress_base;
	struct xpcie_ingress *ing;

	board_wren_pcie_unmap(base_board);

	/* Map xpcie bar (bar 0) to change ingress registers */
	snprintf (pcie_file, sizeof(pcie_file),
		  "/sys/bus/pci/devices/%04x:%02x:%02x.%x/resource%u",
		  board->slot.domain, board->slot.bus,
		  board->slot.slot, board->slot.func, 0);

	ingress_base = pci_map_resource(pcie_file, 0x8000, 0x1000);

	ing = ingress_base + 0x800;

	if (0) {
		for (unsigned i = 0; i < 8; i++) {
			printf ("ing %u: %08x %08x -> %08x %08x, ctrl: %08x (size: %uKB)\n", i,
				ing[i].src_base_hi, ing[i].src_base_lo,
				ing[i].dst_base_hi, ing[i].dst_base_lo,
				ing[i].control,
				(4 << ((ing[i].control >> 16) & 0x1f)));
		}
	}

	/* Modify ingress #3 used by bar 2 (was OCM) */
	ing[3].control = 0;
	ing[3].dst_base_hi = 0;
	ing[3].dst_base_lo = addr;
	ing[3].control = 1;

	munmap(ingress_base, 0x1000);

	/* And now map bar2 */
	snprintf (pcie_file, sizeof(pcie_file),
		  "/sys/bus/pci/devices/%04x:%02x:%02x.%x/resource%u",
		  board->slot.domain, board->slot.bus,
		  board->slot.slot, board->slot.func, 2);

	board->soc_addr = pci_map_resource(pcie_file, 0, 0x1000);
	return board->soc_addr;
}

static int board_wren_pcie_fini(struct board *base_board)
{
	struct board_wren_pcie *board = (struct board_wren_pcie *)base_board;
	munmap(board->parent.map_addr, board->parent.map_length);

	return 0;
}

static void board_wren_pcie_help(void)
{
        printf("WREN-pcie board\n");
        printf(" -s [domain:]bus:slot[.func]\n");
}

static struct board_pci board_wren_pcie =
{
	/* board_mem */
	{
		/* board */
		{
			"wren-pcie",
			board_wren_pcie_init,
			board_wren_pcie_fini,
			board_wren_pcie_help,
			mem_readl,
			mem_writel,
			board_wren_pcie_map,
			board_wren_pcie_unmap
		},
		NULL, 0, NULL, 0
	},
};

static struct board *boards[] = {
	&board_pci.parent.parent,
	&board_spec.parent.parent,
	&board_host.parent.parent,
#ifdef SUPPORT_ERTM
	&board_ertm14.parent,
#endif
#ifdef SUPPORT_CERN_VMEBRIDGE
	&board_cernvme.parent.parent,
	&board_cernvme_le.parent.parent,
	&board_wren_vme.parent.parent,
	&board_wr2rf.parent.parent,
#endif
	&board_wren_pcie.parent.parent,
#ifdef SUPPORT_WRS
        &board_wrs.parent.parent,
#endif
	NULL
};

static struct board *find_board(const char *name)
{
        struct board *b;

        for (unsigned i = 0; (b = boards[i]); i++)
                if (!strcmp(b->name, name))
                        return b;
        fprintf(stderr,
                "board '%s' is unknown, try %s board\n",
                name, progname);
        return NULL;
}

static int board_open(int *argc, char *argv[])
{
	if (*argc > 2 && !strcmp(argv[1], "-b")) {
                struct board *b;
		/* Board selection */
		remove_arg1(argc, argv);
                b = find_board(argv[1]);
                if (b == NULL)
                        return -1;
                board = b;
		remove_arg1(argc, argv);
	}
	else {
		/* Default is pci */
		board = &board_pci.parent.parent;
	}

	return board->init(board, argc, argv);
}


/* URV PART */
struct wrc_cpu {
	const char *name;
	void (*reset)(struct board *board, unsigned int rst);
	void (*writel)(struct board *board, unsigned int addr, uint32_t data);
	uint32_t (*readl)(struct board *board, unsigned int addr);
};

static void wrpc_v5_reset(struct board *board, unsigned int rst)
{
	board->writel (board, OFFSET_CPU_CSR + WRC_CPU_CSR_RESET, rst);
}

static void wrpc_v5_writel(struct board *board, unsigned int addr, uint32_t data)
{
	board->writel(board, OFFSET_CPU_CSR + WRC_CPU_CSR_UADDR, addr >> 2);
	board->writel(board, OFFSET_CPU_CSR + WRC_CPU_CSR_UDATA, data);
}

static uint32_t wrpc_v5_readl(struct board *board, unsigned int addr)
{
	board->writel(board, OFFSET_CPU_CSR + WRC_CPU_CSR_UADDR, addr >> 2);
	return board->readl(board, OFFSET_CPU_CSR + WRC_CPU_CSR_UDATA);
}


static void wrpc_v4_reset(struct board *board, unsigned int rst)
{
	board->writel (board, WRPCV4_BASE_SYSCON + 0, (rst << 28) |  0x0deadbee);
}

static void wrpc_v4_writel(struct board *board, unsigned int addr, uint32_t data)
{
	board->writel(board, addr, data);
}

static uint32_t wrpc_v4_readl(struct board *board, unsigned int addr)
{
	return board->readl(board, addr);
}

static const struct wrc_cpu wrpc_v5_cpu = {
	"wrpc-v5",
	wrpc_v5_reset,
	wrpc_v5_writel,
	wrpc_v5_readl
};

static const struct wrc_cpu wrpc_v4_cpu = {
	"wrpc-v4",
	wrpc_v4_reset,
	wrpc_v4_writel,
	wrpc_v4_readl
};

static int wrc_write_buf(const struct wrc_cpu *cpu,
			 struct board *board,
                         const unsigned char *buf,
                         unsigned len,
                         unsigned addr)
{
	if ((len & 0x03) != 0 || (addr & 0x03) != 0)
		abort();

	while (len > 0) {
		uint32_t v;


		/* Use BE.  */
		v = (buf[3] << 0)
			| (buf[2] << 8)
			| (buf[1] << 16)
			| (buf[0] << 24);
		cpu->writel(board, addr, v);

		if (verbose)
			printf ("Write %08x at %08x\n", v, addr);

                if (flag_check) {
                        uint32_t r;
                        r = cpu->readl(board, addr);
                        if (r != v) {
                                printf ("Error at %08x: "
                                        "read %08x instead of %08x\n",
                                        addr, r, v);
                                return -1;
                        }
                }

		len -= 4;
		addr += 4;
		buf += 4;
	}

        return 0;
}

static Elf32_Half read_elf_half (const Elf32_Half *v)
{
  const unsigned char *p = (const unsigned char *)v;
  return p[0] | (p[1] << 8);   /* LE  */
}

static Elf32_Word read_elf_word (const Elf32_Word *v)
{
  const unsigned char *p = (const unsigned char *)v;
  return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);   /* LE  */
}

#ifndef EM_RISCV
#define EM_RISCV	0xF3
#endif

struct wrc_elf_load_cb_data {
	const struct wrc_cpu *cpu;
	struct board *board;
};

static int wrc_elf_load_cb (void *data, unsigned char *buf,
			    unsigned len, unsigned vaddr)
{
	struct wrc_elf_load_cb_data *d = (struct wrc_elf_load_cb_data *)data;

	return wrc_write_buf(d->cpu, d-> board, buf, len, vaddr);
}

static int elf_dump_cb (void *data, unsigned char *buf,
			unsigned len, unsigned vaddr)
{
	for (unsigned i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf ("%08x:", vaddr + i);
		printf (" %02x", buf[i]);
		if (i % 16 == 15 || i == len - 1)
			printf("\n");
	}
	return 0;
}

static int elf_foreach_segment_fd(const char *filename, int fd, unsigned mach,
				  int (*cb)(void *data, unsigned char *buf,
					    unsigned len, unsigned vaddr),
				  void *data)
{
        Elf32_Ehdr ehdr;
        unsigned poff;
        unsigned pnum;
        //unsigned memsz;
        unsigned loff;
        unsigned i;

        if (lseek(fd, 0, SEEK_SET) != 0
            || read (fd, &ehdr, sizeof (ehdr)) != sizeof(ehdr)) {
                fprintf (stderr, "cannot read ELF header of %s\n", filename);
                return -1;
        }
        if (ehdr.e_ident[EI_MAG0] != ELFMAG0
            || ehdr.e_ident[EI_MAG1] != ELFMAG1
            || ehdr.e_ident[EI_MAG2] != ELFMAG2
            || ehdr.e_ident[EI_MAG3] != ELFMAG3) {
                fprintf (stderr, "file %s is not an ELF file\n", filename);
                return -1;
        }

        if (ehdr.e_ident[EI_CLASS] != ELFCLASS32
            || ehdr.e_ident[EI_DATA] != ELFDATA2LSB
            || ehdr.e_ident[EI_VERSION] != EV_CURRENT) {
                fprintf (stderr, "file %s is not expect ELF class\n", filename);
                return -1;
        }

        if (read_elf_half (&ehdr.e_type) != ET_EXEC
            || read_elf_word (&ehdr.e_version) != EV_CURRENT) {
                fprintf (stderr,
                         "file %s is not an executable\n", filename);
                return -1;
        }

	if (read_elf_half (&ehdr.e_machine) != mach) {
                fprintf (stderr,
                         "file %s has incorrect machine (0x%x)\n",
			 filename, mach);
                return -1;
        }

        if (read_elf_half (&ehdr.e_phentsize) != sizeof (Elf32_Phdr)) {
                fprintf (stderr, "file %s has bad phdr size\n", filename);
                return -1;
        }

        pnum = read_elf_half (&ehdr.e_phnum);

        if (read_elf_word (&ehdr.e_entry) != 0) {
                fprintf (stderr, "file %s entry point is not 0\n", filename);
                return -1;
        }

        poff = read_elf_word (&ehdr.e_phoff);

        loff = 0;
        for (i = 0; i < pnum; i++) {
                Elf32_Phdr phdr;
                unsigned sz;
                unsigned vaddr;
                unsigned char buf[4096];
                unsigned l, len;

                if (lseek (fd, poff + i * sizeof(Elf32_Phdr), SEEK_SET) < 0
                    || read (fd, &phdr, sizeof (phdr)) != sizeof(phdr)) {
                        fprintf(stderr,
                                "%s: cannot read program header\n", filename);
                        return -1;
                }
                if (read_elf_word (&phdr.p_type) != PT_LOAD)
                        continue;

                sz = read_elf_word (&phdr.p_filesz);
                if (sz == 0)
                        continue;
                //memsz = read_elf_word (&phdr.p_memsz);
                loff = read_elf_word (&phdr.p_offset);
                vaddr = read_elf_word (&phdr.p_vaddr);
                if (lseek (fd, loff, SEEK_SET) < 0) {
                        fprintf(stderr,
                                "%s: cannot seek to program content %i\n",
                                filename, i);
                        return -1;
                }

                sz = (sz + 3) & ~3;

                len = 0;
                while (len < sz) {
                        l = sz - len;
                        if (l > sizeof(buf))
                                l = sizeof(buf);
                        if (read (fd, buf, l) != l) {
                                fprintf(stderr,
                                        "%s: cannot read program\n", filename);
                                return -1;
                        }

                        if (cb (data, buf, l, vaddr + len) < 0)
                                return -1;
                        len += l;
                }

                /* TODO: do we want to clear until memsz ? */
        }
        return 0;
}

static int elf_foreach_segment(const char *filename, unsigned mach,
			       int (*cb)(void *data, unsigned char *buf,
					 unsigned len, unsigned vaddr),
			       void *data)
{
	int fd;
	int res;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s\n", filename);
		return -1;
	}

	res = elf_foreach_segment_fd(filename, fd, mach, cb, data);
	close (fd);

	return res;
}

static int wrc_load_firmware(const struct wrc_cpu *cpu, struct board *board, const char *filename)
{
	int fd;
	unsigned char hdr[4];
	unsigned char buf[1024];
	ssize_t res;
	unsigned addr;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s\n", filename);
		return -1;
	}

	res = read(fd, hdr, sizeof(hdr));
	if (res != sizeof(hdr)) {
		fprintf(stderr, "cannot read %s\n", filename);
		goto err_close;
	}

	if (hdr[0] == 0x7f
	    && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
		struct wrc_elf_load_cb_data cd;
		cd.cpu = cpu;
		cd.board = board;

                if (elf_foreach_segment_fd(filename, fd, EM_RISCV,
					   wrc_elf_load_cb, &cd) < 0)
                        goto err_close;
	}
        else {
                addr = 0;
                if (wrc_write_buf(cpu, board, hdr, sizeof(hdr), addr) != 0)
                        goto err_close;
                addr += sizeof (hdr);

                while (1) {
                        res = read(fd, buf, sizeof(buf));
                        if (res <= 0)
                                break;
                        if (wrc_write_buf(cpu, board, buf, res, addr) != 0)
                                goto err_close;
                        addr += res;
                }
                printf ("%u KB written\n", addr / 1024);
        }

	close(fd);
	return 0;

err_close:
	close(fd);
	return -1;
}

static int wrc_save_firmware(const struct wrc_cpu *cpu, struct board *board,
			     const char *filename)
{
	int fd;
	unsigned length = 0x20000;
	unsigned char buf[1024];
	ssize_t res;
	unsigned addr;

	fd = open(filename, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s\n", filename);
		return -1;
	}

	for (addr = 0; addr < length;) {
		unsigned l = length - addr;
		unsigned off;
		if (l > sizeof (buf))
			l = sizeof (buf);

		for (off = 0; off < l; off += 4) {
			unsigned int v;
			v = cpu->readl(board, addr);
			/* Use BE */
			buf[off + 0] = v >> 24;
			buf[off + 1] = v >> 16;
			buf[off + 2] = v >> 8;
			buf[off + 3] = v >> 0;

			addr += 4;
		}
		res = write(fd, buf, l);
		if (res != l) {
			fprintf(stderr, "write failure\n");
			close(fd);
			return -1;
		}
	}

	printf ("%u KB written to %s\n", length / 1024, filename);
	close(fd);
	return 0;
}

static void wrc_dump(const struct wrc_cpu *cpu, struct board *board, unsigned addr, unsigned len)
{
	unsigned off;

	off = 0;
	for (off = 0; off < len; off += 4) {
		uint32_t v;

		if ((off & 0x0f) == 0)
			printf ("%08x:", addr + off);

		v = cpu->readl(board, addr + off);
		printf (" %08x", v);
		if ((off & 0x0f) == 0xc)
			printf ("\n");
	}
	if ((off & 0x0f) != 0xc)
		printf ("\n");
}

/* Extract the basename of :param name: */
static const char *get_basename(const char *name)
{
	const char *res = name;

	for (res = name; *name; name++)
		if (*name == '/')
			res = name + 1;
	if (*res)
		return res;
	else
		return name;
}

static const struct tool_base *
find_tool(const char *name)
{
	const struct tool_base *tool;

	for (unsigned i = 0; (tool = tools[i]); i++) {
		if (strcmp (tool->name, name) == 0)
			return tool;
	}
	return NULL;
}

static int do_help(int argc, char *argv[])
{
	if (argc == 1) {
		printf ("usage: %s [command] [-b BOARD] [OPTIONS...]\n", progname);
		printf ("command is one of:\n");
		for (unsigned i = 0; tools[i]; i++)
			printf(" %-18s - %s\n", tools[i]->name, tools[i]->short_help);
	}
	else {
		for (unsigned i = 1; i < argc; i++) {
			const struct tool_base *tool;

			tool = find_tool(argv[i]);
			if (tool == NULL) {
				printf("tool '%s' does not exist\n", argv[i]);
				return 1;
			}
			if (tool->help == NULL)
				printf("%s\n", tool->short_help);
			else
				tool->help();
		}
	}
	return 0;
}

static int do_version(int argc, char *argv[])
{
	printf ("version 1.0\n");
	return 0;
}

static void help_load(void)
{
        printf("usage: %s load [-c CORE] BOARD-OPTIONS FILENAME\n", progname);
        printf("Load FILENAME into WR cpu and restart the code\n");
	printf("Option:\n"
	       " -c CORE   select wrpc core (wrpc-v5 or wrpc-v4)\n");
}

static int do_load(int argc, char *argv[])
{
	int c;
        int status;
	const char *filename;
	int reset;
	enum { CMD_LOAD, CMD_DUMP, CMD_SAVE } cmd;
	const struct wrc_cpu *cpu = &wrpc_v5_cpu;

	if (argc > 2 && !strcmp(argv[1], "-c")) {
		const char *core_name = argv[2];

		remove_arg1(&argc, argv);
		remove_arg1(&argc, argv);

		if (!strcmp(core_name, "wrpc-v5")) {
			cpu = &wrpc_v5_cpu;
			map_size = 4 << 10;
		}
		else if (!strcmp(core_name, "wrpc-v4")) {
			cpu = &wrpc_v4_cpu;
			map_size = 256 << 10;
		}
		else {
			printf("unknown core name '%s', try help load\n",
			       core_name);
			return 1;
		}
	}

	/* Decode board options and open the board. */
	if (board_open(&argc, argv) < 0)
		return 1;

	status = 0;
	reset = 1;

	cmd = CMD_LOAD;
	while ((c = getopt(argc, argv, "vdskr")) != -1) {
		switch (c) {
		case 'd':
			cmd = CMD_DUMP;
			break;
		case 's':
			cmd = CMD_SAVE;
			break;
		case 'v':
			verbose++;
			break;
                case 'k':
                        flag_check++;
                        break;
		case 'r':
			reset = 0;
			break;
		case '?':
                        printf("%s: unknown option, try -h\n", argv[0]);
                        exit(1);
		}
	}

	if (((cmd == CMD_LOAD || cmd == CMD_SAVE) && (optind != argc - 1))
	    || (cmd == CMD_DUMP && optind != argc)) {
		help_load();
		return 2;
	}
	filename = argv[optind];

	if (verbose)
		printf("load '%s' using board %s and core %s\n",
		       filename, board->name, cpu->name);

	/* Reset */
	if (reset) {
		if (verbose)
			printf("Reset the cpu\n");
		cpu->reset(board, 1);
	}

	switch (cmd) {
	case CMD_LOAD:
		/* Load */
		if (wrc_load_firmware (cpu, board, filename) < 0)
                        status = 1;
		break;
	case CMD_SAVE:
		/* Save */
		wrc_save_firmware (cpu, board, filename);
		break;
	case CMD_DUMP:
		/* TODO: specify offset and length */
		wrc_dump(cpu, board, 0, 0x200);
		break;
	}

	/* Start */
	if (reset) {
		if (verbose)
			printf("Un-reset the cpu\n");
		cpu->reset(board, 0);
	}

        board->fini(board);

	return status;
}

static void help_vuart(void)
{
	fprintf(stderr, "usage: %s vuart BOARD-OPTIONS [-k] [-c <cmd>] [-r] [-t <timeout>]\n", progname);
	fprintf(stderr, " -k keep terminal\n");
	fprintf(stderr, " -d debug\n");
	fprintf(stderr, " -c <cmd> execute command\n");
	fprintf(stderr, " -t <timeout> set a timeout to execute a command\n");
	fprintf(stderr, " -r do not expect stdin (may be useful for scripts),"
                        "    conflicts with -c\n");
}

static uint32_t vuart_readl(struct board *board, int reg)
{
	return board->readl(board, reg | OFFSET_UART);
}


static void vuart_writel(struct board *board, uint32_t value, int reg)
{
	board->writel(board, reg | OFFSET_UART, value);
}

static int wr_vuart_rx(struct board *board)
{
	int rdr = vuart_readl(board, UART_REG_HOST_RDR );
	return (rdr & UART_HOST_RDR_RDY) ? UART_HOST_RDR_DATA_R(rdr) : -1;
}

/**
 * It transmits a single byte
 * @param[in] vuart token from dev_map()
 */
static int wr_vuart_tx(struct board *board, char data)
{
	unsigned count;

	for (count = 0; count < 2000; count++) {
		int sr = vuart_readl(board, UART_REG_HOST_TDR);
		if (sr & UART_HOST_TDR_RDY) {
			vuart_writel(board, UART_HOST_TDR_DATA_W(data), UART_REG_HOST_TDR);
			return 0;
		}
		usleep (500);
	}

	return -1;
}

/**
 * It reads a number of bytes and it stores them in a given buffer
 * @param[in] vuart token from dev_map()
 * @param[out] buf destination for read bytes
 * @param[in] size numeber of bytes to read
 *
 * @return the number of read bytes
 */
static size_t wr_vuart_read(struct board *board, char *buf, size_t size)
{
	size_t s = size, n_rx = 0;
	int8_t c;

	while(s--) {
		c =  wr_vuart_rx(board);
		if(c < 0)
			return n_rx;
		*buf++ = c;
		n_rx ++;
	}
	return n_rx;
}

/**
 * It flush vuart buffer.
 *
 * @param[in] vuart token from dev_map()
 *
 */
static void wr_vuart_flush(struct board *board)
{
	char rx;

	while(wr_vuart_read(board,&rx,1) == 1) {}
}

/**
 * It writes a number of bytes from a given buffer
 * @param[in] vuart token from dev_map()
 * @param[in] buf buffer to write
 * @param[in] size numeber of bytes to write
 */
static int wr_vuart_write(struct board *board, char *buf, size_t size)
{
	while(size--)
		if (wr_vuart_tx(board, *buf++) < 0)
			return -1;
	return 0;
}

static void wrpc_vuart_set_tty_raw(struct termios *old_termios)
{
  	struct termios newkey;

	tcgetattr(STDIN_FILENO,old_termios);
	memcpy(&newkey, old_termios, sizeof(struct termios));
	newkey.c_cflag = B9600 | CS8 | CLOCAL | CREAD;
	newkey.c_iflag = IGNPAR;
	newkey.c_oflag = 0;
	newkey.c_lflag = ISIG;  /* Keep C-c, C-z, ... */
	tcflush(STDIN_FILENO, TCIFLUSH);
	tcsetattr(STDIN_FILENO,TCSANOW,&newkey);
}

static void wrpc_vuart_restore_tty(struct termios *old_termios)
{
	tcsetattr(STDIN_FILENO, TCSANOW, old_termios);
}

static time_t get_running_secs(void)
{
        struct timeval now;

        gettimeofday(&now, NULL);
        return now.tv_sec;
}

static void wrpc_vuart_term(struct board *board,
                            int keep_term,
                            unsigned timeout)
{
	struct termios oldkey;
	int need_exit = 0;
	fd_set fds;
	int ret;
	unsigned char tx;
	int rx;
        time_t start_time;

	fprintf(stderr, "[press C-a to exit]\n");

	if(!keep_term)
		wrpc_vuart_set_tty_raw(&oldkey);

        if (timeout)
                start_time = get_running_secs();

	while(!need_exit) {
		struct timeval tv = {0, 10000}; /* 10ms */

		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);

		/*
		 * Check if the STDIN has characters to read
		 * (what the user writes)
		 */
		ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
		switch (ret) {
		case -1:
			perror("select");
			break;
		case 0: /* timeout */
			break;
		default:
			if(!FD_ISSET(STDIN_FILENO, &fds))
				break;
			/* The user wrote something */
			do {
				ret = read(STDIN_FILENO, &tx, 1);
			} while (ret < 0 && errno == EINTR);
			if (ret != 1) {
				fprintf(stderr, "nothing to read. Port disconnected?\n");
				need_exit = 1; /* kill */
			}
			/* If the user character is C-a, then kill */
			if(tx == '\x01') {
				need_exit = 1;
				break;
			}

			if (wr_vuart_tx(board, tx) < 0) {
				fprintf(stderr, "sent character is not read\n");
			}
			break;
		}

		/* Print all the incoming characters */
		while((rx = wr_vuart_rx(board)) > 0) {
			putchar(rx);
		}
		fflush(stdout);

                if (timeout && get_running_secs() >= start_time + timeout)
                        break;
	}

	if(!keep_term)
		wrpc_vuart_restore_tty(&oldkey);
}

static void wrpc_vuart_only_read(struct board *board,
                            unsigned timeout)
{
	int need_exit = 0;
	int rx;
	time_t start_time;

	if (timeout)
		start_time = get_running_secs();

	while(!need_exit) {
		/* Print all the incoming characters */
		while((rx = wr_vuart_rx(board)) > 0) {
			putchar(rx);
		}
		fflush(stdout);
		usleep(10);

		if (timeout && get_running_secs() >= start_time + timeout)
			break;
	}
}

static void wrpc_vuart_debug(struct board *board)
{
	int print = 1;
	fd_set fds;
	int ret;

	while(1) {
		int rdr = vuart_readl(board, UART_REG_HOST_RDR );
		unsigned char c = rdr & 0xff;
		int rdy = rdr & UART_HOST_RDR_RDY;

		if (rdy || print) {
			printf("rdr: %08x (data:%02x '%c' rdy:%u count: %u)\n",
			       rdr,
			       c, c >= ' ' && c < 127 ? c : ' ',
			       rdy ? 1 : 0,
			       (rdr & UART_HOST_RDR_COUNT_MASK)
			         >> UART_HOST_RDR_COUNT_SHIFT);
			print = rdy;
			continue;
		}

		struct timeval tv = {0, 10000}; /* 10ms */
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);

		ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
		if (ret < 0) {
			perror("select");
			return;
		}
		if (ret == 0) {
			/* timeout */
			continue;
		}
		if (FD_ISSET(STDIN_FILENO, &fds)) {
			unsigned char tx;
			/* The user wrote something */
			do {
				ret = read(STDIN_FILENO, &tx, 1);
			} while (ret < 0 && errno == EINTR);

			int sr = vuart_readl(board, UART_REG_HOST_TDR);
			printf ("TDR: %08x ...", sr);
			if (sr & UART_HOST_TDR_RDY) {
				vuart_writel(board, UART_HOST_TDR_DATA_W(tx), UART_REG_HOST_TDR);
			}
			sr = vuart_readl(board, UART_REG_HOST_TDR);
			printf ("%08x\n", sr);
		}
	}
}

static void wrpc_vuart_command(struct board *board, char *command)
{
	//above is place for old and new port settings for keyboard teletype
	int cmd_len = 0;
	char *prompt = VUART_CMD_PROMPT;
	int i_prompt = 0;
	int i;
	int rx;

	/* Flush Vuart before sending command */
	wr_vuart_flush(board);
	/* Send command */
	cmd_len = strlen(command);
	wr_vuart_write(board, command, cmd_len);
	/* Flush command echo */
	wr_vuart_flush(board);
	/* Send end character */
	wr_vuart_tx(board, VUART_EOL);
	/* Wait for a while before reading command results */
	usleep(VUART_CMD_USLEEP);
	/* Discard characters until end of line control one */
	while((rx = wr_vuart_rx(board)) > 0) {
		if(rx == VUART_EOL)
			break;
	}

	while(1) {
		/* Print all the incoming characters */
		rx = wr_vuart_rx(board);
		if (rx < 0) {
			usleep(10);
			continue;
		}

		/* Prompt detection, skip characters */
		if (rx == prompt[i_prompt]) {
			i_prompt++;
			/* Prompt detected! */
			if(i_prompt == strlen(prompt))
				return;
		} else {
			/* Check if some previous characters have been skipped
			   by prompt detector code and print them */
			for(i = 0 ; i < i_prompt ; i++)
				putchar(prompt[i]);
			/* Reset prompt detector */
			i_prompt = 0;
			/* Print current character */
			putchar(rx);
			fflush(stdout);
		}
	}
}


static int do_vuart(int argc, char *argv[])
{
	int c;
	int keep_term = 0;
	char *cmd = NULL;
        unsigned timeout = 0;
	int read_only = 0;
	int debug = 0;

	if (board_open(&argc, argv) < 0)
		return 1;

	/* Parse specific args */
	while ((c = getopt (argc, argv, "c:kt:rd")) != -1) {
		switch (c) {
		case 'c':
			/* Enable command mode */
			cmd = optarg;
			break;
		case 'k':
			keep_term = 1;
			break;
                case 't':
                        timeout = atoi(optarg);
                        break;
		case 'r':
			read_only = 1;
                        break;
		case 'd':
			debug = 1;
			break;
		case '?':
			break;
		}
	}

	if ((cmd != NULL) + read_only + debug > 1) {
		perror("Only one of -r, -c, -d can be used\n");
		return 1;
	}

	if (read_only)
		wrpc_vuart_only_read(board, timeout);
	else if (cmd)
		wrpc_vuart_command(board, cmd);
	else if (debug)
		wrpc_vuart_debug(board);
	else
		wrpc_vuart_term(board, keep_term, timeout);

	board->fini(board);

	return 0;
}

#ifndef SUPPORT_WRS

static void help_info(void)
{
	printf("usage: %s info\n", progname);
	printf("display board info\n"
	       "also useful to check mapping\n");
}

static int do_info(int argc, char *argv[])
{
	unsigned hwfr;
	unsigned hwir;

	if (board_open(&argc, argv) < 0)
		return 1;

	hwfr = board->readl(board, OFFSET_SYSCON + offsetof(struct SYSC_WB, HWFR));
	printf ("hwfr=%08x:  "
		"memsize: %ukB,  storage: %u, storage sector size: %ukB\n",
		hwfr,
		(SYSC_HWFR_MEMSIZE_R(hwfr) + 1) * 16,
		SYSC_HWFR_STORAGE_TYPE_R(hwfr),
		SYSC_HWFR_STORAGE_SEC_R(hwfr));

	hwir = board->readl(board, OFFSET_SYSCON + offsetof(struct SYSC_WB, HWIR));
	printf ("hwir=%08x:  ", hwir);
        for (unsigned i = 0; i < 4; i++) {
                unsigned c = (hwir >> (24 - i * 8)) & 0xff;
                putchar (c >= 32 && c < 127 ? c : '.');
        }
        printf ("\n");

	board->fini(board);

	return 0;
}

static void help_mac(void)
{
	printf("usage: %s mac\n", progname);
	printf("display mac address\n");
}

static int do_mac(int argc, char *argv[])
{
	unsigned mach, macl;

	if (board_open(&argc, argv) < 0)
		return 1;

	mach = board->readl(board, OFFSET_ENDPOINT + EP_REG_MACH);
	macl = board->readl(board, OFFSET_ENDPOINT + EP_REG_MACL);
	printf ("mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
		(mach >> 8) & 0xff,
		(mach >> 0) & 0xff,
		(macl >> 24) & 0xff,
		(macl >> 16) & 0xff,
		(macl >> 8) & 0xff,
		(macl >> 0) & 0xff);

	board->fini(board);

	return 0;
}
#endif /* !defined(SUPPORT_WRS) */

static int do_board(int argc, char *argv[])
{
        struct board *b;

        if (argc > 1) {
                b = find_board(argv[1]);
                if (b == NULL)
                        return 1;
                b->help();
        }
        else {
                printf ("List of supported boards (or access methods):\n");
                for (unsigned i = 0; (b = boards[i]); i++)
                        printf(" %s\n", b->name);
        }
        return 0;
}

static void help_spll_recorder(void)
{
	fprintf(stderr, "SoftPLL debug/recorder tool. \n");
	fprintf(stderr, "This dumps the real-time SPLL traces (error values/DAC drive/events) into stdout for the purpose of further analysis/plotting. \n");

	fprintf(stderr, "Usage: %s spll-recorder [options]\n", progname);
	fprintf(stderr, "  -u USAMP  undersampling factor (only for ertm14)\n");
	fprintf(stderr, "  -d        debug output\n");
	fprintf(stderr, "  -b        binary output\n");
}

static const char *dbg_source_to_string(int src)
{
	static char str[16];

	switch (src)
	{
	case SPLL_DBG_SRC_HELPER:
		return "helper";
	case SPLL_DBG_SRC_MAIN:
		return "main";
	case SPLL_DBG_SRC_AUX(0):
		return "aux0";
	case SPLL_DBG_SRC_AUX(1):
		return "aux1";
	case SPLL_DBG_SRC_AUX(2):
		return "aux2";
	case SPLL_DBG_SRC_AUX(3):
		return "aux3";
	case SPLL_DBG_SRC_EXT:
		return "ext";
	case SPLL_DBG_SRC_RAW:
		return "raw";
	default:
		snprintf(str, sizeof(str), "%d", src);
		return str;
	}
}

static const char *dbg_signal_to_string(int src)
{
	static char str[16];

	switch (src)
	{
	case SPLL_DBG_SIGNAL_ERR:
		return "err";
	case SPLL_DBG_SIGNAL_Y:
		return "y";
	case SPLL_DBG_SIGNAL_PERIOD:
		return "period";
	case SPLL_DBG_SIGNAL_REF:
		return "ref";
	case SPLL_DBG_SIGNAL_TAG:
		return "tag";
	case SPLL_DBG_SIGNAL_SAMPLE_ID:
		return "sample";
	case SPLL_DBG_SIGNAL_TIME_MS:
		return "time_ms";
	case SPLL_DBG_SIGNAL_PHASE_CURRENT:
		return "ph_cur";
	case SPLL_DBG_SIGNAL_PHASE_TARGET:
		return "ph_targ";
	case SPLL_DBG_SIGNAL_SRC:
		return "source";
	default:
		snprintf(str, sizeof(str), "%d", src);
		return str;
	}
}

static const char *dbg_event_to_string(int src)
{
	static char str[16];

	switch (src)
	{
	case SPLL_DBG_EVT_GAIN_SWITCH:
		return "gain-switch";
	case SPLL_DBG_EVT_LOCK_ACQUIRED:
		return "lock-acquired";
	case SPLL_DBG_EVT_LOCK_LOSS:
		return "lock-lost";
	case SPLL_DBG_EVT_START:
		return "start";
	default:
		snprintf(str, sizeof(str), "%d", src);
		return str;
	}
}

static int32_t signext32(uint32_t in, int bit)
{
	uint32_t mask = ~((1 << bit) - 1);
	if (in & (1 << bit))
		return in | mask;
	else
		return in;
}

static int prev_src = -1;
static volatile int kill_acquisition = 0;

void spll_sighandler(int sig)
{
	fprintf(stderr, "Signal caught: %d\n", sig);
	kill_acquisition = 1;
}

void spll_dump_debug_data(const uint32_t *buf, size_t size)
{
	while (size--)
	{
		uint32_t x = *buf++;

		int sig = SPLL_DBG_EXTRACT_SIGNAL(x);
		int src = SPLL_DBG_EXTRACT_SOURCE(x);
		uint32_t value_raw = SPLL_DBG_EXTRACT_VALUE(x);
		int32_t value;

		switch (sig)
		{
		case SPLL_DBG_SIGNAL_ERR:
			value = signext32(value_raw, 23);
			break;
		default:
			value = value_raw;
		};

		if (prev_src != src)
		{
			printf("%s ", dbg_source_to_string(src));
			prev_src = src;
		}

		if (sig == SPLL_DBG_SIGNAL_EVENT)
			printf("event=%s ", dbg_event_to_string(value));
		else
			printf("%s=%d ", dbg_signal_to_string(sig), value);

		if (SPLL_DBG_IS_LAST_RECORD(x))
		{
			printf("\n");
			prev_src = -1;
		}
	}
}

#ifdef SUPPORT_ERTM
void spll_readout_ertm14(struct board_ertm14* board, int undersample )
{

	int r = ertm_configure_spll_debug_dump(board->handle, 1, undersample);
	if (r)
		perror("ertm_configure_spll_debug_dump()");

	for (;;)
	{
		uint32_t buf[16384];
		size_t buf_size = 16384;
		int r = ertm_read_spll_debug_data(board->handle, buf, &buf_size);
		if (r >= 0)
		{
			spll_dump_debug_data(buf, buf_size);
		}

		if (kill_acquisition)
			break;
	}

	r = ertm_configure_spll_debug_dump(board->handle, 0, 0);
	if (r)
		perror("ertm_configure_spll_debug_dump()");

	fprintf(stderr, "ertm14: stopping SPLL logging...\n");
}
#endif

/* Read spll FIFO status and return != 0 if the fifo is empty */
static unsigned spll_fifo_empty(struct board *board)
{
	uint32_t r = board->readl(board, WRC_HOST_MAP_SPLL + SPLL_HOST_MAP_DFR_HOST_CSR);
	if (0)
		fprintf(stderr, "spll csr: %08x\n", r);
	return r & SPLL_HOST_MAP_DFR_HOST_CSR_EMPTY;
}

static uint32_t spll_read_word(struct board *board)
{
	uint32_t r;

	r = board->readl(board, WRC_HOST_MAP_SPLL + SPLL_HOST_MAP_DFR_HOST_R0);

	if (0)
		fprintf(stderr, "spll r0:  %08x\n", r);
	return r;
}

static void spll_purge(struct board* board)
{
	// purge the SPLL debug FIFO
	while (!spll_fifo_empty(board)) {
		spll_read_word(board);
	}
}

static unsigned spll_read_direct(struct board* board, uint32_t *buf, unsigned len)
{
	size_t cnt = 0;
	const size_t max_record_size = 256;
	int got_a_full_record = 0;

	while (cnt < len) {
		if (spll_fifo_empty(board)) {
			if (got_a_full_record)
				break;
			usleep(100);
			continue;
		}

		uint32_t r = spll_read_word(board);
		buf[cnt++] = r;
		got_a_full_record = SPLL_DBG_IS_LAST_RECORD(r);
		if (got_a_full_record && cnt >= len - max_record_size)
			break;
	}

	return cnt;
}

static void spll_readout_direct(struct board* board)
{
	uint32_t buf[16384];

	spll_purge(board);

	for (;;) {
		unsigned cnt;

		cnt = spll_read_direct(board, buf, sizeof(buf)/sizeof(*buf));
		spll_dump_debug_data(buf, cnt);
	}
}

static void spll_readout_binary(struct board* board)
{
	uint32_t buf[16384];

	spll_purge(board);

	for (;;) {
		unsigned cnt;

		cnt = spll_read_direct(board, buf, sizeof(buf)/sizeof(*buf));
		fwrite(buf, 4, cnt, stdout);
	}
}

static void spll_debug_direct(struct board* board)
{
	unsigned csr_addr = OFFSET_SOFTPLL + SPLL_HOST_MAP_DFR_HOST_CSR;
	unsigned data_addr = OFFSET_SOFTPLL + SPLL_HOST_MAP_DFR_HOST_R0;

	while (1) {
		uint32_t r = board->readl(board, csr_addr);
		unsigned empty = (r & SPLL_HOST_MAP_DFR_HOST_CSR_EMPTY) != 0;
		printf ("csr (@0x%04x): %08x empty:%u\n",
			csr_addr, (unsigned)r, empty);
		if (empty)
			break;

		r = board->readl(board, data_addr);
		printf ("data (@0x%04x): %08x\n", data_addr, (unsigned)r);
	}
}

static int do_spll_recorder(int argc, char *argv[])
{
	int is_ertm;
	int c;
	int undersample __attribute__((unused)) = 20;
	int debug = 0;
	int binary = 0;

	if (board_open(&argc, argv) < 0)
		return 1;

	/* Parse specific args */
	while ((c = getopt (argc, argv, "u:hdb")) != -1) {
		switch (c) {
		case 'u':
			undersample = atoi(optarg);
			break;
		case 'h':
			help_spll_recorder();
			break;
		case 'd':
			debug = 1;
			break;
		case 'b':
			binary = 1;
			break;
		case '?':
		default:
			break;
		}
	}

	is_ertm = !strcmp( board->name, "ertm14" );
	if(is_ertm)
	{
#ifdef SUPPORT_ERTM
		signal(SIGINT, spll_sighandler);
		signal(SIGTERM, spll_sighandler);

		spll_readout_ertm14( (struct board_ertm14*) board, undersample );
#endif
	}
	else if (debug)
		spll_debug_direct(board);
	else if (binary)
		spll_readout_binary(board);
	else
		spll_readout_direct(board);

	board->fini(board);

	return 0;
}

static void help_spll_display(void)
{
	fprintf(stderr, "SoftPLL log display.\n");
	fprintf(stderr, "This reads binary dumps from spll-recorder -b\n");

	fprintf(stderr, "Usage: %s spll-display\n", progname);
}

static int do_spll_display(int argc, char *argv[])
{
	uint32_t buf[16384];
	size_t cnt;

	while (1) {
		cnt = fread(buf, 4, sizeof(buf)/4, stdin);
		if (cnt <= 0)
			break;
		spll_dump_debug_data(buf, cnt);
	}

	return 0;
}

#define GDB_PACKET_SIZE_MAX 2048

/**
 * struct gdb_packet - GDB packet
 * @data: message exchanged with GDB
 * @size: length in bytes
 */
struct gdb_packet {
	char data[GDB_PACKET_SIZE_MAX];
	size_t size;
};

static void gdb_packet_put_str(struct gdb_packet *out, const char *msg)
{
	out->size = snprintf(out->data, GDB_PACKET_SIZE_MAX, "%s", msg);
}

struct dbg_port;

typedef int (gdb_command_t)(struct dbg_port *dbg,
                            struct gdb_packet *out,
                            struct gdb_packet *in);

/**
 * struct dbg_port - descriptor to handle connection
 * @addr: Mock Turtle virtual address
 * @cpu: CPU index
 * @fd: socket file descriptor
 */
struct dbg_port {
	/* Cpu index */
	uint8_t cpu;
	/* The fd for the stub socket */
	int fd;
	/* Set if -t (terminal) option was present */
        unsigned flag_term;

	/* Debug access port */
	void *dap;

	gdb_command_t * const *cmds;
	size_t n_cmds;

	/* If not NULL, called after connect to stop the target */
	int (*post_connect_hook)(struct dbg_port *dbg);
};

/**
 * Read value from the Debug Port
 */
static uint32_t dbg_urv_readl(struct dbg_port *dbg, uint32_t reg)
{
        return board->readl(board, reg | OFFSET_CPU_CSR);
}

/**
 * Write value to the Debug Port
 */
static void dbg_urv_writel(struct dbg_port *dbg,
                       uint32_t reg, uint32_t val)
{
        board->writel(board, reg | OFFSET_CPU_CSR, val);
}

/**
 * Control CPU reset
 */
static void dbg_urv_set_cpu_reset(struct dbg_port *dbg, unsigned int rst)
{
	dbg_urv_writel (dbg, WRC_CPU_CSR_RESET, rst);
}

static uint32_t dbg_urv_get_cpu_reset(struct dbg_port *dbg)
{
	return dbg_urv_readl (dbg, WRC_CPU_CSR_RESET);
}

/**
 * Read mail-box
 * @dbg: debug port
 *
 * Return value read
 */
static uint32_t dbg_urv_read_mbx(struct dbg_port *dbg)
{
	uint32_t reg;

	reg = WRC_CPU_CSR_DBG_CORE0_MBX;
	reg += sizeof(uint32_t) * dbg->cpu;

	return dbg_urv_readl(dbg, reg);
}

/**
 * Write mail-box
 * @dbg: debug port
 * @val: value to write
 */
static void dbg_urv_write_mbx(struct dbg_port *dbg, uint32_t val)
{
	uint32_t reg;

	reg = WRC_CPU_CSR_DBG_CORE0_MBX;
	reg += sizeof(uint32_t) * dbg->cpu;

	dbg_urv_writel(dbg, reg, val);
}

/**
 * Execute one instructions
 * @dbg: debug port
 * @insn: instruction to execute
 */
static void dbg_urv_exec_insn(struct dbg_port *dbg, uint32_t insn)
{
	uint32_t reg;

	reg = WRC_CPU_CSR_DBG_CORE0_INSN;
	reg += sizeof(uint32_t) * dbg->cpu;

	dbg_urv_writel(dbg, reg, insn);
}

/**
 * Execute instruction to copy a register to the mail-box
 * @dbg: debug port
 * @reg: register index
 */
static void dbg_urv_exec_reg_to_mbx(struct dbg_port *dbg, uint32_t reg)
{
	dbg_urv_exec_insn(dbg, 0x7D001073 | (reg << 15));
}

/**
 * Execute instruction to the mail-box to a register
 * @dbg: debug port
 * @reg: register index
 */
static void dbg_urv_exec_mbx_to_reg(struct dbg_port *dbg, uint32_t reg)
{
	dbg_urv_exec_insn(dbg, 0x7D002073 | (reg << 7));
}

/**
 * Execute NOP instruction
 * @dbg: debug port
 */
static void dbg_urv_exec_nop(struct dbg_port *dbg)
{
	dbg_urv_exec_insn(dbg, 0x00000013);
}

/**
 * Check if MockTurtle CPU is in debug mode
 * @dbg: debug port
 *
 * Return true when it is in debug mode
 */
static bool dbg_urv_in_debug_mode(struct dbg_port *dbg)
{
	uint32_t status;

	status = dbg_urv_readl(dbg, WRC_CPU_CSR_DBG_STATUS);

	return ((status >> dbg->cpu) & 1);
}

/**
 * Set MockTurtle CPU in debug mode
 * @dbg: debug port
 *
 * Return 0 on success, -1 on error and errno is appropriately set
 */
static int dbg_urv_debug_mode_force_set(struct dbg_port *dbg)
{
	int retry;

	if (dbg_urv_in_debug_mode(dbg))
		return 0;
	dbg_urv_writel(dbg, WRC_CPU_CSR_DBG_FORCE, (1 << dbg->cpu));
	/* wait to debug to be ready max ~5s */
	retry = 5000;
	while (retry >= 0) {
		struct timespec ts = {0, 1000000};

		nanosleep(&ts, NULL);
		if (dbg_urv_in_debug_mode(dbg))
			break;
		retry--;
	}

	/* Remove the reset, otherwise the cpu won't be anymore in debug
	   mode.  */
	if (dbg_urv_get_cpu_reset(dbg) != 0) {
		if (!dbg_urv_in_debug_mode(dbg))
			fprintf(stderr, "Huhh, cpu not in debug\n");
		fprintf(stderr, "CPU under reset\n");
		dbg_urv_set_cpu_reset(dbg, 0);
		if (!dbg_urv_in_debug_mode(dbg))
			fprintf(stderr, "Huhh, cpu not anymore in debug\n");
	}

	dbg_urv_writel(dbg, WRC_CPU_CSR_DBG_FORCE, 0);

	if (retry < 0) {
		errno = ETIME;
		return -1;
	}
	return 0;
}

/**
 * Read from a CPU register
 * @dbg: debug port
 * @reg: register number [0, 31]
 *
 * Return: the register content
 */
static uint32_t dbg_urv_read_reg(struct dbg_port *dbg, int reg)
{
	if (verbose > 2)
		printf("dbg_urv_read_reg %d\n", reg);
	dbg_urv_exec_reg_to_mbx(dbg, reg);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);

	return dbg_urv_read_mbx(dbg);
}

/**
 * Write in a CPU register
 * @dbg: debug port
 * @reg: register number [0, 31]
 * @val: value
 */
static void dbg_urv_write_reg(struct dbg_port *dbg,
			       int reg, uint32_t val)
{
	dbg_urv_write_mbx(dbg, val);
	dbg_urv_exec_mbx_to_reg(dbg, reg);
}

/**
 * Copy PC to RA register
 * @dbg: debug port
 *
 * Return PC value
 */
static uint32_t dbg_urv_pc_read_via_ra(struct dbg_port *dbg)
{
	if (verbose > 2)
		printf("dbg_urv_pc_read_via_ra\n");
	dbg_urv_exec_insn(dbg, 0x000000ef); /* ra = pc + 4 */
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_reg_to_mbx(dbg, 1);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	return (dbg_urv_read_mbx(dbg) - 4) & 0xFFFFFFFF;
}

/**
 * Write PC using RA content register
 * @dbg: debug port
 */
static void dbg_urv_pc_write_via_ra(struct dbg_port *dbg)
{
	dbg_urv_exec_insn(dbg, 0x00008067); /* ret */
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
}

/**
 * Increase PC by 4
 * @dbg: debug port
 */
static void dbg_urv_pc_advance_4(struct dbg_port *dbg)
{
	dbg_urv_exec_insn(dbg, 0x00000263); /* beqz zero, +4 */
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
}

static void gdb_maybe_read_term(struct dbg_port *dbg)
{
	if (dbg->flag_term) {
		while (1) {
			int rx = wr_vuart_rx(board);
			if (rx < 0)
				break;
			putchar(rx);
		}
		fflush(stdout);
	}
}

static int gdb_maybe_stop(struct dbg_port *dbg)
{
	int ret;
	struct pollfd p[2];

	p[0].fd = dbg->fd;
	p[0].events = POLLIN;
	p[0].revents = 0;

	if (dbg->flag_term) {
		p[1].fd = 0;
		p[1].events = POLLIN;
		p[1].revents = 0;
		ret = poll(p, 2, 100);
	}
	else
		ret = poll(p, 1, 1000);

	if (ret == 0) {
		/* Timeout, continue */
		return 0;
	}

	if (ret < 0) {
		/* Error: connection closed ? */
		return -1;
	}

	if (dbg->flag_term && (p[1].revents & POLLIN)) {
		/* From stdin to cpu */
		char c;
		if (read(0, &c, 1) == 1)
			wr_vuart_tx(board, c);
	}

	if (p[0].revents & POLLIN) {
		return 1;
	}

	return 0;
}

/**
 * Continue command
 */
static int gdb_urv_handle_c(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	if (in->size > 1) {
		out->size = 0;
		return 0;
	}

	dbg_urv_exec_insn(dbg, 0x00100073); /* ebreak */
	while (1) {
		int ret;

		/* Dump vuart. */
		gdb_maybe_read_term(dbg);

		if (dbg_urv_in_debug_mode(dbg)) {
			/*
			 * TODO not clear but check twice due to possible
			 * race if the ebreak is not yet executed
			 */
			if (dbg_urv_in_debug_mode(dbg)) {
				gdb_packet_put_str(out, "S05");
				break;
			}
		}

		ret = gdb_maybe_stop(dbg);
		if (ret < 0)
			return ret;
		if (ret == 0)
			continue;

		/* GDB wants something from us */
		ret = dbg_urv_debug_mode_force_set(dbg);
		if (ret < 0)
			fprintf(stderr, "Failed to set debug mode\n");
		gdb_packet_put_str(out, "S02");
		break;
	}

	return 0;
}

/**
 * Detach
 */
static int gdb_urv_handle_D(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	dbg_urv_exec_insn(dbg, 0x00100073); /* ebreak */
	gdb_packet_put_str(out, "OK");

	return 0;
}

static void gdb_packet_append_x32(struct gdb_packet *out, uint32_t v)
{
	out->size += snprintf(out->data + out->size,
			      GDB_PACKET_SIZE_MAX,
			      "%08"PRIx32, v);
}

static void gdb_packet_append_x8(struct gdb_packet *out, unsigned char v)
{
	out->size += snprintf(out->data + out->size,
			      GDB_PACKET_SIZE_MAX, "%02x", v);
}

/**
 * Read all registers
 */
static int gdb_urv_handle_g(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	uint32_t regs[32], pc;
	int i;

	out->size = 0;
	for (i = 0; i < 32; ++i) {
		regs[i] = dbg_urv_read_reg(dbg, i);
		gdb_packet_append_x32(out, htonl(regs[i]));
	}
	pc = dbg_urv_pc_read_via_ra(dbg);
	gdb_packet_append_x32(out, htonl(pc));
	dbg_urv_write_reg(dbg, 1, regs[1]);

	return 0;
}

/**
 * Write all register
 */
static int gdb_urv_handle_G(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	uint32_t regs[33]; /* 32 register, 1 PC */
	int i;

	if (in->size != (1 + 33 * 8)) {
		gdb_packet_put_str(out, "E01");
		return 0;
	}

	for (i = 0; i < 33; ++i) {
		int ret = sscanf(in->data + 1 + i * 8, "%08"SCNx32, &regs[i]);

		if (ret != 1) {
			gdb_packet_put_str(out, "E02");
			return 0;
		}
	}

	/* Register 32 is pc.  */
	dbg_urv_write_reg(dbg, 1, ntohl(regs[32]));
	dbg_urv_pc_write_via_ra(dbg);

	for (i = 1; i < 32; ++i)
		dbg_urv_write_reg(dbg, i, ntohl(regs[i]));
	gdb_packet_put_str(out, "OK");

	return 0;
}

/**
 * Set thread for subsequent operations
 *
 * Partially supported
 */
static int gdb_handle_H(struct dbg_port *dbg,
			struct gdb_packet *out,
			struct gdb_packet *in)
{
	/* we just want to keep GDB quiet */
	if (strncmp(in->data, "Hg0", 3) == 0)
		gdb_packet_put_str(out, "OK");
	else
		out->size = 0;

	return 0;
}

/**
 * Kill
 *
 * Not supported yet
 *
 * kill leave the CPU in its current state. In the next connection it
 * will restart exactly from that point and the core remains stopped.
 */
static int gdb_handle_k(struct dbg_port *dbg,
			     struct gdb_packet *out,
			     struct gdb_packet *in)
{
	return 0;
}

/**
 * Write data to memory
 */
static int gdb_urv_handle_M(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	uint32_t addr, n;
	uint32_t a0, a1;
	char *indata;
	int ret;

	indata = strchr(in->data, ':');
	if (!indata) {
		gdb_packet_put_str(out, "E01");
		return 0;
	}
	indata++; /* skip ':' */
	ret = sscanf(in->data + 1, "%"SCNx32",%"SCNx32":", &addr, &n);
	if (ret != 2) {
		gdb_packet_put_str(out, "E02");
		return 0;
	}

	if (n * 2 != in->size - (indata - in->data)) {
		gdb_packet_put_str(out, "E03");
		return 0;
	}

	a0 = dbg_urv_read_reg(dbg, 10);
	a1 = dbg_urv_read_reg(dbg, 11);
	dbg_urv_write_reg(dbg, 10, addr);
	if (addr % 4 == 0) {
		for (; n >= 4; n -= 4, indata += 8) {
			uint32_t w;

			ret = sscanf(indata, "%08"SCNx32, &w);
			if (ret != 1)
				break;

			dbg_urv_write_reg(dbg, 11, ntohl(w));
			/* sw a1, 0(a0) */
			dbg_urv_exec_insn(dbg, 0x00B52023);
			/* addi a0, a0, 4 */
			dbg_urv_exec_insn(dbg, 0x00450513);
		}
	}
	for (; n > 0; --n, indata += 2) {
		uint32_t b;

		ret = sscanf(indata, "%02"SCNx32, &b);
		if (ret != 1)
			break;
		dbg_urv_write_reg(dbg, 11, b);
		/* sb a1, 0(a0) */
		dbg_urv_exec_insn(dbg, 0x00B50023);
		/* addi a0, a0, 4 */
		dbg_urv_exec_insn(dbg, 0x00150513);
	}

	dbg_urv_write_reg(dbg, 10, a0);
	dbg_urv_write_reg(dbg, 11, a1);

	if (n > 0)
		gdb_packet_put_str(out, "E04");
	else
		gdb_packet_put_str(out, "OK");

	return 0;
}

/**
 * Read data from memory
 */
static int gdb_urv_handle_m(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	uint32_t addr, n;
	uint32_t a0, a1;
	int ret;

	ret = sscanf(in->data + 1, "%x,%x", &addr, &n);
	if (ret != 2) {
		gdb_packet_put_str(out, "E01");
		return 0;
	}
	a0 = dbg_urv_read_reg(dbg, 10);
	a1 = dbg_urv_read_reg(dbg, 11);
	dbg_urv_write_reg(dbg, 10, addr);
	out->size = 0;
	for (; n > 0; --n) {
		uint8_t b;

		dbg_urv_exec_insn(dbg, 0x00054583); /* lbu a1, 0(a0) */
		dbg_urv_exec_insn(dbg, 0x00150513); /* addi a0, a0, 1 */
		b = dbg_urv_read_reg(dbg, 11);
		out->size += snprintf(out->data + out->size,
				      GDB_PACKET_SIZE_MAX,
				      "%02"PRIx8, b);
	}

	dbg_urv_write_reg(dbg, 10, a0);
	dbg_urv_write_reg(dbg, 11, a1);

	return 0;
}

/**
 * Read a specific register
 */
static int gdb_urv_handle_p(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	unsigned int val;
	int ret;

	ret = sscanf(in->data + 1, "%x", &val);
	if (ret != 1) {
		out->size = 0;
		return 0;
	}
	printf("0x%x\n", val);
	if (val == (0x301 + 65)) /* MISA CSR */
		out->size = snprintf(out->data, GDB_PACKET_SIZE_MAX,
				     "%08x",
				     (1 << 30) | (1 << ('I' - 65)));
	else
		gdb_packet_put_str(out, "E01");

	return 0;
}

/**
 * Write a specific register
 *
 *
 * Not supported yet
 */
static int gdb_handle_P(struct dbg_port *dbg,
			     struct gdb_packet *out,
			     struct gdb_packet *in)
{
	out->size = 0;

	return 0;
}

/**
 * Answer to qSupported request
 */
static int gdb_handle_q_supported(struct dbg_port *dbg,
				       struct gdb_packet *out,
				       struct gdb_packet *in)
{
	out->size = snprintf(out->data, GDB_PACKET_SIZE_MAX,
			     "PacketSize=%x", GDB_PACKET_SIZE_MAX);

	return 0;
}

static int gdb_handle_qm(struct dbg_port *dbg,
			      struct gdb_packet *out,
			      struct gdb_packet *in)
{
	/* Status: stopped */
	out->size = snprintf(out->data, GDB_PACKET_SIZE_MAX, "S05");

	return 0;
}

/* BUF length should be GDB_PACKET_SIZE_MAX / 2 */
static int gdb_qRcmd_decode(char *buf, struct gdb_packet *in)
{
	unsigned len;

	/* Decode hex input (convert to bytes). 6 is the prefix 'qRcmd,' */
	for (len = 0; len < (in->size - 6) / 2; len++) {
		unsigned val;
		int ret;

		ret = sscanf(in->data + 6 + len * 2, "%02x", &val);
		if (ret != 1)
			return -1;
		buf[len] = val;
	}
	buf[len] = 0;
	return 0;
}

static void gdb_qRcmd_encode(char *buf, struct gdb_packet *out)
{
	unsigned len;

	/* Encode to hex.  */
	for (len = 0; buf[len]; len++)
		sprintf(out->data + len * 2, "%02x", buf[len]);
	out->size = len * 2;
}

static int gdb_urv_handle_qRcmd(struct dbg_port *dbg,
				struct gdb_packet *out,
				struct gdb_packet *in)
{
	char buf[GDB_PACKET_SIZE_MAX / 2];

	if (gdb_qRcmd_decode(buf, in) < 0) {
		out->size = 0;
		return 0;
	}

	if (strcmp(buf, "help") == 0) {
		strcpy(buf, "usage: csr | reset | port | help\n");
	}
	else if (strcmp(buf, "csr") == 0) {
		uint32_t ra;
		uint32_t mepc, mstatus, mcause;
		ra = dbg_urv_read_reg(dbg, 1);
		dbg_urv_exec_insn(dbg, 0x341020f3); /* csrr ra,mepc */
		mepc = dbg_urv_read_reg(dbg, 1);
		dbg_urv_exec_insn(dbg, 0x342020f3); /* csrr ra,mcause */
		mcause = dbg_urv_read_reg(dbg, 1);
		dbg_urv_exec_insn(dbg, 0x300020f3); /* csrr ra,mstatus */
		mstatus = dbg_urv_read_reg(dbg, 1);
		dbg_urv_write_reg(dbg, 1, ra);
		snprintf(buf, sizeof(buf),
			 "mepc:    %08x\nmcause:  %08x\nmstatus: %08x\n",
			 mepc, mcause, mstatus);
	}
	else if (strcmp(buf, "reset") == 0) {
		/* Reset the cpu.  */
		dbg_urv_set_cpu_reset(dbg, 1);
		/* Force debug mode, otherwire it is cleared by reset.  */
		dbg_urv_writel(dbg, WRC_CPU_CSR_DBG_FORCE, (1 << dbg->cpu));
		/* Release reset.  */
		dbg_urv_set_cpu_reset(dbg, 0);
		/* Release force debug.  */
		dbg_urv_writel(dbg, WRC_CPU_CSR_DBG_FORCE, 0);
		if (!dbg_urv_in_debug_mode(dbg))
		  fprintf(stderr, "Huhh, cpu not in debug\n");
		strcpy(buf, "board reset\n");
	}
	else if (strcmp(buf, "port") == 0) {
		snprintf(buf, sizeof(buf),
			 "rst: %04x\ndbg st: %04x\n",
			 dbg_urv_readl (dbg, WRC_CPU_CSR_RESET),
			 dbg_urv_readl (dbg, WRC_CPU_CSR_DBG_STATUS));
	}
	else {
		strcpy(buf,"unhandled mon command, try 'mon help'\n");
	}

	/* Encode to hex.  */
	gdb_qRcmd_encode(buf, out);
	return 0;
}

/* Generic handling of 'q*' commands */
static int gdb_handle_q(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	if (strncmp(in->data, "qSupported:", 11) == 0)
		return gdb_handle_q_supported(dbg, out, in);
	else if (strncmp(in->data, "qm", 2) == 0)
		return gdb_handle_qm(dbg, out, in);
	out->size = 0;

	return 0;
}

static int gdb_urv_handle_q(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	if (strncmp(in->data, "qRcmd,", 6) == 0)
		return gdb_urv_handle_qRcmd(dbg, out, in);
	return gdb_handle_q(dbg, out, in);
}

/**
 * Single step
 */
static int gdb_urv_handle_s(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	uint32_t pc, npc, ra, insn;

	if (in->size > 1) {
		out->size = 0;
		return 0;
	}

	/* Get ra(x1) and pc  */
	ra = dbg_urv_read_reg(dbg, 1);
	pc = dbg_urv_pc_read_via_ra(dbg);

	/* Read the instruction to be executed (at pc) */
	dbg_urv_write_reg(dbg, 1, pc);
	dbg_urv_exec_insn(dbg, 0x0000A083) ;/* lw ra,0(ra) */
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	insn = dbg_urv_read_reg(dbg, 1);
	if (verbose)
		fprintf(stdout, "execute: %08"PRIx32" at pc=%08"PRIx32,
			insn, pc);

	/* Restore ra */
	dbg_urv_write_reg(dbg, 1, ra);

	/* Execute the instruction */
	dbg_urv_exec_insn(dbg, insn);
	dbg_urv_exec_nop(dbg);
	dbg_urv_exec_nop(dbg);
	switch (insn & 0x77) {
	case 0x67: /* jump */
		/* Nothing to do, PC is always updated */
		break;
	case 0x63: /* branch */
		/* Read new PC */
		ra = dbg_urv_read_reg(dbg, 1);
		npc = dbg_urv_pc_read_via_ra(dbg);
		dbg_urv_write_reg(dbg, 1, ra);
		/* In case of no change, the branch has not been taken,
		   so the PC needs to be updated to the next instruction.
		   If the branch has been taken, the PC has been updated.
		   NOTE: it doesn't work in case of conditional jump to the
		   current instruction.  Maybe decode the instruction
		   further. */
		if (npc == pc)
			dbg_urv_pc_advance_4(dbg);
		break;
	default:
		/* The instruction has been executed, the pc needs to
		   be updated */
		dbg_urv_pc_advance_4(dbg);
		break;
	}

	out->size = snprintf(out->data, GDB_PACKET_SIZE_MAX, "S05");

	return 0;
}

/**
 * vAttach command
 *
 * Not supported yet
 */
static int gdb_handle_v_attach(struct dbg_port *dbg,
				    struct gdb_packet *out,
				    struct gdb_packet *in)
{
	out->size = 0;
	return 0;
}

/**
 * vCont command
 *
 * Not supported yet
 */
static int gdb_handle_v_cont(struct dbg_port *dbg,
				  struct gdb_packet *out,
				  struct gdb_packet *in)
{
	out->size = 0;

	return 0;
}

/**
 * vCtrlC command
 *
 * Not supported yet
 */
static int gdb_handle_v_ctrlc(struct dbg_port *dbg,
				   struct gdb_packet *out,
				   struct gdb_packet *in)
{
	out->size = 0;

	return 0;
}

/**
 * v<name> commands
 */
static int gdb_handle_v(struct dbg_port *dbg,
			     struct gdb_packet *out,
			     struct gdb_packet *in)
{
	int ret = 0;

	if (strncmp(in->data, "vAttach", 7) == 0)
		ret = gdb_handle_v_attach(dbg, out, in);
	else if (strncmp(in->data, "vCont", 5) == 0)
		ret = gdb_handle_v_cont(dbg, out, in);
	else if (strncmp(in->data, "vCtrlC", 6) == 0)
		ret = gdb_handle_v_ctrlc(dbg, out, in);
	else if (strncmp(in->data, "vMustReplyEmpty:", 16) == 0)
		out->size = 0;
	else
		out->size = 0;

	return ret;
}

/**
 * Write binary data to memory
 *
 * Not supported yet
 */
static int gdb_handle_X(struct dbg_port *dbg,
			struct gdb_packet *out,
			struct gdb_packet *in)
{
	out->size = 0;

	return 0;
}

static gdb_command_t * const gdb_urv_commands[] = {
	['c'] = gdb_urv_handle_c,
	['D'] = gdb_urv_handle_D,
	['g'] = gdb_urv_handle_g,
	['G'] = gdb_urv_handle_G,
	['H'] = gdb_handle_H,
	['k'] = gdb_handle_k,
	['M'] = gdb_urv_handle_M,
	['m'] = gdb_urv_handle_m,
	['p'] = gdb_urv_handle_p,
	['P'] = gdb_handle_P,
	['q'] = gdb_urv_handle_q,
	['s'] = gdb_urv_handle_s,
	['v'] = gdb_handle_v,
	['v'] = gdb_handle_v,
	['X'] = gdb_handle_X,
	['?'] = gdb_handle_qm,
};

/**
 * Process incoming packet and generate the outcoming
 * @out: outgoing packet
 * @in: incoming packet
 *
 * Return: 0 on success, otherwise -1 and errno is appropriately set
 *
 * The function does not receive or send packets, it only processes them;
 * the caller will handle recv(2) and send(2)
 */
static int gdb_command(struct dbg_port *dbg,
		       struct gdb_packet *out,
		       struct gdb_packet *in)
{
	int cmd = in->data[0];
	gdb_command_t *exec;

	if (in->size == 0)
		return -1;

	if (cmd < dbg->n_cmds) {
		exec = dbg->cmds[cmd];
		if (exec)
			return exec(dbg, out, in);
	}
	out->size = 0;
	return 0;
}

static void debugger_print_packet(struct gdb_packet *pkt, const char *dir)
{
	int i, start, end;

	switch (verbose) {
	case 0:
		return;
	case 1:
		start = 1;
		end = pkt->size - 3;
		break;
	default:
		start = 0;
		end = pkt->size;
		break;
	}

	fputs(dir, stdout);
	fputc(' ', stdout);
	for (i = start; i < end; ++i)
		fputc(pkt->data[i], stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

/**
 * Calculate mod 256 checksum
 * @data: input data
 * @n: number of bytes
 *
 * Return: checksum value
 */
static uint8_t debugger_checksum(uint8_t *data, size_t n)
{
	uint8_t checksum = 0;
	int i;

	for (i = 0; i < n; ++i)
		checksum += data[i];

	return checksum & 0xFF;
}

/**
 * Receive a GDB packet
 * @fd: socket file descriptor
 * @pkt: packet
 *
 * Return: 0 on success, otherwise -1 and errno is appropriately set
 */
static int __debugger_recv(int fd, struct gdb_packet *pkt)
{
	pkt->size = 0;
	do {
		char c;
		int ret;
		struct pollfd p = {
				   .fd = fd,
				   .events = POLLIN,
				   .revents = 0,
		};

		ret = poll(&p, 1, 1000);
		if (ret < 0)
			return ret;
		if (ret == 0)
			continue;
		ret = recv(fd, &c, 1, 0);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			errno = ENOTCONN;
			return -1;
		}
		/* FIXME should we control more ? */
		if (pkt->size == 0 && c != '$')
			continue; /* wait for the beginning */
		pkt->data[pkt->size] = c;
		pkt->size++;

		if (verbose > 3) {
			fprintf(stdout, "Building message: [%zu]: %s\n",
				pkt->size, pkt->data);
		}

		if (pkt->size > GDB_PACKET_SIZE_MAX - 1) {
			/* -1 to leave space for the string terminator */
			errno = EINVAL;
			return -1;
		}

	} while (!(pkt->size > 3 && pkt->data[pkt->size - 3] == '#'));

	return 0;
}

/**
 * Receive a GDB packet
 * @fd: socket file descriptor
 * @pkt: packet
 *
 * Return: 0 on success, otherwise -1 and errno is appropriately set
 */
static int debugger_recv(int fd, struct gdb_packet *pkt)
{
	uint8_t checksum_l, checksum_r;
	char ack[1];
	int ret;

	ret = __debugger_recv(fd, pkt);
	if (ret < 0)
		return ret;
	debugger_print_packet(pkt, "->");

	checksum_l = debugger_checksum((uint8_t *)(pkt->data + 1),
					    pkt->size - 4);

	ret = sscanf(pkt->data + pkt->size - 2, "%02"SCNx8, &checksum_r);
	if (ret != 1) {
		fprintf(stderr, "Received invalid checksum\n");
		return -1;
	}

	/* Remove checksum and special characters $payload#checksum */
	pkt->size -= 4;
	memmove(pkt->data, pkt->data + 1, pkt->size);
	pkt->data[pkt->size] = 0;

	if (checksum_l == checksum_r) {
		ack[0] = '+';
	} else {
		ack[0] = '-';
		if (verbose)
			fprintf(stderr,
				"Invalid checksum ' (L) %x != (R) %x'\n",
				checksum_l, checksum_r);
	}

	ret = send(fd, ack, 1, 0);
	if (ret != 1) {
		fputs("Failed to send acknowledge\n", stderr);
		errno = EIO;
		return -1;
	}

	return 0;
}

/**
 * Send a GDB packet
 * @fd: socket file descriptor
 * @pkt: packet
 *
 * Return: 0 on success, otherwise -1 and errno is appropriately set
 */
static int debugger_send(int fd, struct gdb_packet *pkt)
{
	uint8_t checksum_l;
	int ret;

	checksum_l = debugger_checksum((uint8_t *)pkt->data, pkt->size);

	/* Add checksum and special characters $payload#checksum */
	memmove(pkt->data + 1, pkt->data, pkt->size);
	pkt->data[0] = '$';
	snprintf(pkt->data + 1 + pkt->size, GDB_PACKET_SIZE_MAX,
		 "#%02x", checksum_l);
	pkt->size += 4; /* 1 $, 1 #, 2 checksum */

	debugger_print_packet(pkt, "<-");

	ret = send(fd, pkt->data, pkt->size, 0);
	if (ret < 0)
		return -1;
	if (ret != pkt->size) {
		errno = EIO;
		return -1;
	}

	return 0;
}

/**
 * Run GDB server
 * @addr: MMAP address
 *
 * Return: 0 on success, otherwise -1 and errno is appropriately set
 */
static int debugger_run(struct dbg_port *dbg)
{
	bool run = true;
	int ret;
	struct gdb_packet *pkt, *in, *out;

	pkt = calloc(2, sizeof(struct gdb_packet));
	if (!pkt) {
		fprintf(stderr, "Memory allocation failed: %s\n",
			strerror(errno));
		return -1;
	}
	in = &pkt[0];
	out = &pkt[1];

	if (dbg->post_connect_hook) {
		ret = dbg->post_connect_hook(dbg);
		if (ret < 0) {
			fprintf(stderr, "Failed to set debug mode\n");
			return -1;
		}
	}

	fputs("Start receiving messages from GDB\n", stdout);
	while (run) {
		memset(in, 0, sizeof(*in));
		memset(out, 0, sizeof(*out));

		ret = debugger_recv(dbg->fd, in);
		if (ret) {
			if (errno == ENOTCONN)
				run = false;
			else
				fprintf(stderr,
					"Failed to receive message: %s\n",
					strerror(errno));
			continue;
		}

		ret = gdb_command(dbg, out, in);
		if (ret < 0)
			continue;

		ret = debugger_send(dbg->fd, out);
		if (ret) {
			fprintf(stderr, "Failed to send message: %s\n",
				strerror(errno));
		}
	}

	free(pkt);

	return 0;
}

#define MEMPATH_LEN 128

static int gdb_server(struct dbg_port *dbg, int argc, char *argv[])
{
	int flag_keep = 0;
	int gdb_port = 7471;
	int c, ret, sfd, ret_exit = EXIT_SUCCESS, optval;
	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);

	dbg->flag_term = 0;

	while ((c = getopt(argc, argv, "p:vstk")) != -1) {
		switch (c) {
		case 'p':
			gdb_port = atoi(optarg);
			if (gdb_port == 0) {
				fprintf(stderr, "bad port value\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'v':
			verbose++;
			break;
		case 'k':
                        flag_keep = 1;
			break;
		case 't':
			dbg->flag_term = 1;
			break;
		case '?':
                        printf("%s: unknown option, try -h\n", argv[0]);
                        exit(1);
		}
	}

	sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0) {
		fprintf(stderr, "Failed to open a socket: %s\n",
			strerror(errno));
		ret_exit = EXIT_FAILURE;
		goto out_sock;
	}

	optval = 1;
	ret = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR,
			 &optval, sizeof(optval));
	if (ret < 0) {
		fprintf(stderr, "Failed to set REUSEADDR option: %s\n",
			strerror(errno));
		ret_exit = EXIT_FAILURE;
		goto out_sock;
	}
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(gdb_port);
	ret = bind(sfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
	if (ret < 0) {
		fprintf(stderr, "Failed to bind to an *:%d: %s\n",
			gdb_port, strerror(errno));
		ret_exit = EXIT_FAILURE;
		goto out_sock;
	}

	ret = listen(sfd, 1);
	if (ret < 0) {
		fprintf(stderr, "Failed to listen: %s\n", strerror(errno));
		ret_exit = EXIT_FAILURE;
		goto out_bind;
	}

	do {
		printf ("Waiting for connection on port %d\n", gdb_port);

		dbg->fd = accept(sfd, (struct sockaddr *)&client_addr,
				&client_len);
		if (dbg->fd < 0) {
			fprintf(stderr, "Failed to accept: %s\n",
				strerror(errno));
			ret_exit = EXIT_FAILURE;
			break;
		}
		fprintf(stdout, "Accepted connection from %s\n",
			inet_ntoa(client_addr.sin_addr));

		ret = debugger_run(dbg);
		if (ret < 0) {
			ret_exit = EXIT_FAILURE;
                        break;
		}
	} while (flag_keep);

out_bind:
out_sock:
	close(sfd);
        return ret_exit;
}

static void help_gdbserver(void)
{
	fprintf(stderr, "usage: %s gdbserver BOARD-OPTIONS [options]\n",
		progname);
	fprintf(stderr, " -p PORT       listen on tcp port PORT\n");
	fprintf(stderr, " -v            verbose\n");
	fprintf(stderr, " -t            enable terminal\n");
	fprintf(stderr, " -k            keep connection\n");
}

static int do_gdbserver(int argc, char *argv[])
{
	int ret;
	struct dbg_port dbg;

        /* Decode board options and open the board. */
        if (board_open(&argc, argv) < 0)
          return 1;

	dbg.cmds = gdb_urv_commands;
	dbg.n_cmds = sizeof(gdb_urv_commands) / sizeof(gdb_urv_commands[0]);
	dbg.post_connect_hook = dbg_urv_debug_mode_force_set;

	ret = gdb_server(&dbg, argc, argv);

        board->fini(board);
        return ret;
}

#ifndef SUPPORT_WRS

static void help_wdiags(void)
{
	printf("usage: %s wdiags\n", progname);
	printf("display diagnostic registers\n");
}

#define WDIAG_REG(R) (OFFSET_WDIAGS + offsetof(struct wrc_diags, R))

static void unlock_diag(void)
{
        unsigned v;

	//reset snapshot bit & keep valid bit as it is
        v = board->readl(board, WDIAG_REG(CTRL));
        board->writel(board, WDIAG_REG(CTRL), v & WRC_DIAGS_CTRL_DATA_VALID);
}

static int lock_diag(void)
{
        unsigned v;

	//snapshot diag ( just raise snapshot bit)
        v = board->readl(board, WDIAG_REG(CTRL));
        if (0)
                printf("ctrl @%08x = %08x\n", (unsigned)WDIAG_REG(CTRL), v);
        board->writel(board, WDIAG_REG(CTRL), v | WRC_DIAGS_CTRL_DATA_SNAPSHOT);
        for (unsigned i = 10; i > 0; i--) {
                v = board->readl(board, WDIAG_REG(CTRL));
                if (v & WRC_DIAGS_CTRL_DATA_VALID)
                        return 0;
		usleep(1000);
        }
	fprintf(stderr, "timeout(10ms) expired while waiting for the valid bit "
		"for snapshot\n");
	return 1;
}

static void print_servo_status(uint32_t val)
{
	static const char * const sstat_str[] = {
		"Not initialized",
		"Sync ns",
		"Sync TAI",
		"Sync phase",
		"Track phase",
		"Wait offset stable",
	};

	printf("servo status:\t\t%s\n",
		sstat_str[val >> WRC_DIAGS_WDIAG_SSTAT_SERVOSTATE_SHIFT]);
}

static void print_port_status(uint32_t val)
{
	static int nbits = 2;
	static char *pstat_str[][2] = {
		//bit = 0     	 	bit = 1
		{"Link down", 		"Link up",},
		{"PLL not locked",	"PLL locked",},
	};
	int i, idx;

	printf("Port status:\t\t");
	for (i = 0; i < nbits; ++i) {
		idx = (val & (1 << i)) ? 1 : 0;
		printf("%s, ", pstat_str[i][idx]);
	}
	printf("\n");
}

static void print_ptp_state(uint32_t val)
{
	static const char * const ptpstat_str[] = {
		"None",
		"PPS initializing",
		"PPS faulty",
		"disabled",
		"PPS listening",
		"PPS pre-master",
		"PPS master",
		"PPS passive",
		"PPS uncalibrated",
		"PPS slave",
	};

	printf("PTP state:\t\t");
	if (val <= 9)
		printf("%s", ptpstat_str[val]);
	else if (val >= 100 && val <= 116)
		printf("WR STATES(see ppsi/ieee1588_types.h): %d", val);
	else
		printf("Unknown");
	printf("\n");
}

static void print_aux_state(uint32_t val)
{
	int nch = 8; //should be retrieved from a register
	int i;

	printf("Aux state:\t\t");
	for (i = 0; i < nch; i++) {
		if (val & (1 << i))
			printf("ch%d:enabled ", i);
	}
	printf("\n");
}

static void print_tx_frame_count(uint32_t val)
{
	printf("TX frame count:\t\t%d\n", val);
}

static void print_rx_frame_count(uint32_t val)
{
	printf("RX frame count:\t\t%d\n", val);
}

static void print_rx_error_count(uint32_t val)
{
	printf("RX error count:\t\t%d\n", val);
}

static void print_local_time(uint32_t sec_msw, uint32_t sec_lsw, uint32_t ns)
{
	uint64_t sec = (uint64_t)(sec_msw) << 32 | sec_lsw;
//	fprintf(stderr, "TAI time:\t\t %" PRIu64 "sec %d nsec\n",
//		sec, ns);
	printf("TAI time:\t\t%s", ctime((time_t *)&sec));
}

static void print_roundtrip_time(uint32_t msw, uint32_t lsw)
{
	uint64_t val = (uint64_t)(msw) << 32 | lsw;
	printf("Round trip time:\t%" PRIu64 " ps\n", val);
}

static void print_master_slave_delay(uint32_t msw, uint32_t lsw)
{
	uint64_t val = (uint64_t)(msw) << 32 | lsw;
	printf("Master slave delay:\t%" PRIu64 " ps\n", val);
}

static void print_link_asym(uint32_t val)
{
	printf("Total Link asymmetry:\t%d ps\n", val);
}

static void print_clock_offset(uint32_t val)
{
	printf("Clock offset:\t\t%d ps\n", val);
}

static void print_phase_setpoint(uint32_t val)
{
	printf("Phase setpoint:\t\t%d ps\n", val);
}

static void print_update_counter(uint32_t val)
{
	printf("Update counter:\t\t%d\n", val);
}

static void print_board_temp(uint32_t val)
{
        printf("temp:\t\t\t%d.%04d C\n", val >> 16,
               (int)((val & 0xffff) * 10 * 1000 >> 16));
}

static void print_aux_clock_status_single( int index, uint32_t r )
{
	int mode = (r & WRC_DIAGS_WDIAG_AUX0_DETAIL_STAT_MODE_MASK) >> WRC_DIAGS_WDIAG_AUX0_DETAIL_STAT_MODE_SHIFT;

	char *mode_str = (mode == 0 ? "slave" : "phase monitor");

	int enabled = ( r & WRC_DIAGS_WDIAG_AUX0_DETAIL_STAT_ENABLED) ? 1 : 0;
	int ready = ( r & WRC_DIAGS_WDIAG_AUX0_DETAIL_STAT_LOCKED) ? 1 : 0;

	int phase = (r & WRC_DIAGS_WDIAG_AUX0_DETAIL_STAT_PHASE_MASK) >> WRC_DIAGS_WDIAG_AUX0_DETAIL_STAT_PHASE_SHIFT;

	printf("AUX%d: mode %s enabled %d locked %d phase %d ps\n", index, mode_str, enabled, ready, phase );
}

static void print_aux_clock_status(void)
{
        unsigned v;

        v = board->readl(board, WDIAG_REG(WDIAG_AUX0_DETAIL_STAT));
	print_aux_clock_status_single(0, v);

        v = board->readl(board, WDIAG_REG(WDIAG_AUX1_DETAIL_STAT));
	print_aux_clock_status_single(1, v);

        v = board->readl(board, WDIAG_REG(WDIAG_AUX2_DETAIL_STAT));
	print_aux_clock_status_single(2, v);

        v = board->readl(board, WDIAG_REG(WDIAG_AUX3_DETAIL_STAT));
	print_aux_clock_status_single(3, v);
}

#define WDIAG_READ(REG) board->readl(board, WDIAG_REG(REG))

static int read_diags(unsigned reg_version)
{
	int res;

	res = lock_diag();
	if (res)
                return -1;

        printf("Diag registers layout version: %d\n", reg_version );
        print_servo_status(WDIAG_READ(WDIAG_SSTAT));
        print_port_status(WDIAG_READ(WDIAG_PSTAT));
        print_ptp_state(WDIAG_READ(WDIAG_PTPSTAT));
        print_aux_state(WDIAG_READ(WDIAG_ASTAT));
        print_tx_frame_count(WDIAG_READ(WDIAG_TXFCNT));
        print_rx_frame_count(WDIAG_READ(WDIAG_RXFCNT));
        if( reg_version >= 2 )
                print_rx_error_count(WDIAG_READ(WDIAG_RX_ERR_CNT));
        print_local_time(WDIAG_READ(WDIAG_SEC_MSB),
                         WDIAG_READ(WDIAG_SEC_LSB),
                         WDIAG_READ(WDIAG_NS));
        print_roundtrip_time(WDIAG_READ(WDIAG_MU_MSB),
                             WDIAG_READ(WDIAG_MU_LSB));
        print_master_slave_delay(WDIAG_READ(WDIAG_DMS_MSB),
                                 WDIAG_READ(WDIAG_DMS_LSB));
        print_link_asym(WDIAG_READ(WDIAG_ASYM));
        print_clock_offset(WDIAG_READ(WDIAG_CKO));
        print_phase_setpoint(WDIAG_READ(WDIAG_SETP));
        print_update_counter(WDIAG_READ(WDIAG_UCNT));
        print_board_temp(WDIAG_READ(WDIAG_TEMP));

        if (reg_version >= 2)
                print_aux_clock_status();

	unlock_diag();

	return 0;
}

static int do_wdiags(int argc, char *argv[])
{
	unsigned ver;

	if (board_open(&argc, argv) < 0)
		return 1;

	ver = board->readl(board, WDIAG_REG(VER));
	if (ver != 1 && ver != 2) {
		fprintf (stderr, "incorrect wdiag verion (read %08x)\n", ver);
		board->fini(board);
		return 1;
	}

        read_diags(ver);

	board->fini(board);

	return 0;
}

static void help_aux_logger(void)
{
	printf("usage: %s aux-logger\n", progname);
}

static int do_aux_logger(int argc, char *argv[])
{
	unsigned ver;
        unsigned timeout = 120;
        unsigned good_samples = 0;
	if (board_open(&argc, argv) < 0)
		return 1;

	ver = board->readl(board, WDIAG_REG(VER));
	if (ver != 1 && ver != 2) {
		fprintf (stderr, "incorrect wdiag verion (read %08x)\n", ver);
		board->fini(board);
		return 1;
	}

	while (1)
	{
                int res = lock_diag();
                if (res)
                        return -1;

		uint32_t aux0_stat = board->readl(board, WDIAG_REG(WDIAG_AUX0_DETAIL_STAT));

                if (aux0_stat & WRC_DIAGS_WDIAG_AUX0_DETAIL_STAT_LOCKED)
		{
			unsigned phase = aux0_stat & 0xffffff;
                        unsigned long long mu, dms;

                        mu = ((uint64_t)WDIAG_READ(WDIAG_MU_MSB) << 32)
                                | WDIAG_READ(WDIAG_MU_LSB);

                        dms = ((uint64_t)WDIAG_READ(WDIAG_DMS_MSB) << 32)
                                | WDIAG_READ(WDIAG_DMS_LSB);

			if(good_samples == 3 )
			{
				printf("[%d,%lld,%lld,%d,%d,%d]\n", phase,
                                       mu, dms,
                                       WDIAG_READ(WDIAG_ASYM),
                                       WDIAG_READ(WDIAG_CKO),
                                       WDIAG_READ(WDIAG_SETP));
				break;
			}

			good_samples++;
                }
                timeout--;
                if(!timeout)
                {
                        printf("Timeout!\n");
                        break;
                }

                unlock_diag();
		sleep(1);
	}

        unlock_diag();

	board->fini(board);

	return 0;
}

/**
 * RPU Base Address
 */
#define RPU_BASEADDR      0xff9a0000u
#define R5_DBG_0_BASEADDR 0xfebf0000u
#define R5_DBG_1_BASEADDR 0xfebf2000u

/**
 * Register: RPU_RPU_GLBL_CNTL
 */
#define RPU_RPU_GLBL_CNTL    0x00000000u
#define RPU_RPU_GLBL_CNTL_SLSPLIT_MASK    0x00000008u
#define RPU_RPU_GLBL_CNTL_TCM_COMB_MASK   0x00000040u
#define RPU_RPU_GLBL_CNTL_SLCLAMP_MASK    0x00000010u

/**
 * Register: RPU_RPU_0_CFG
 */
#define RPU_RPU_0_CFG    0X00000100U
#define RPU_RPU_0_CFG_VINITHI_MASK     0x00000004U
#define RPU_RPU_0_CFG_NCPUHALT_MASK    0X00000001U
#define RPU_RPU_0_STATUS	0X00000104U

/**
 * Register: RPU_RPU_1_CFG
 */
#define RPU_RPU_1_CFG    0X00000200U
#define RPU_RPU_1_CFG_VINITHI_MASK     0x00000004U
#define RPU_RPU_1_CFG_NCPUHALT_MASK    0X00000001U
#define RPU_RPU_1_STATUS	0X00000204U

/**
 * CRL_APB Base Address
 */
#define CRL_APB_BASEADDR      0xff5e0000u

/**
 * Register: CRL_APB_CPU_R5_CTRL
 */
#define CRL_APB_CPU_R5_CTRL    0X00000090U
#define CRL_APB_CPU_R5_CTRL_CLKACT_MASK    0X01000000U

/**
 * Register: CRL_APB_RST_LPD_TOP
 */
#define CRL_APB_RST_LPD_TOP    0x0000023cU
#define CRL_APB_RST_LPD_TOP_RPU_R50_RESET_MASK    0x00000001U
#define CRL_APB_RST_LPD_TOP_RPU_AMBA_RESET_MASK   0x00000004U
#define CRL_APB_RST_LPD_TOP_RPU_R51_RESET_MASK    0x00000002U

/**
 * PMU_GLOBAL Base Address
 */
#define PMU_GLOBAL_BASEADDR      0XFFD80000U

/* Register: PMU_GLOBAL_REQ_PWRUP_INT_EN */
#define PMU_GLOBAL_REQ_PWRUP_INT_EN    0X00000118U
#define PMU_GLOBAL_REQ_PWRUP_INT_EN_PL_MASK    0X00800000U

/* Register: PMU_GLOBAL_REQ_PWRUP_TRIG */
#define PMU_GLOBAL_REQ_PWRUP_TRIG    0X00000120U
#define PMU_GLOBAL_REQ_PWRUP_TRIG_PL_MASK    0X00800000U

/* Register: PMU_GLOBAL_REQ_PWRUP_STATUS */
#define PMU_GLOBAL_REQ_PWRUP_STATUS    0X00000110U
#define PMU_GLOBAL_REQ_PWRUP_STATUS_PL_SHIFT   23U
#define PMU_GLOBAL_REQ_PWRUP_STATUS_PL_MASK    0X00800000U

/* Register: PMU_GLOBAL_PWR_STATE */
#define PMU_GLOBAL_PWR_STATE    0X00000100U
#define PMU_GLOBAL_PWR_STATE_PL_MASK  		0X00800000U
#define PMU_GLOBAL_PWR_STATE_FP_MASK    	0X00400000U
#define PMU_GLOBAL_PWR_STATE_USB1_MASK    	0X00200000U
#define PMU_GLOBAL_PWR_STATE_USB0_MASK    	0X00100000U
#define PMU_GLOBAL_PWR_STATE_OCM_BANK3_MASK    	0X00080000U
#define PMU_GLOBAL_PWR_STATE_OCM_BANK2_MASK    	0X00040000U
#define PMU_GLOBAL_PWR_STATE_OCM_BANK1_MASK    	0X00020000U
#define PMU_GLOBAL_PWR_STATE_OCM_BANK0_MASK    	0X00010000U
#define PMU_GLOBAL_PWR_STATE_TCM1B_MASK    	0X00008000U
#define PMU_GLOBAL_PWR_STATE_TCM1A_MASK    	0X00004000U
#define PMU_GLOBAL_PWR_STATE_TCM0B_MASK    	0X00002000U
#define PMU_GLOBAL_PWR_STATE_TCM0A_MASK    	0X00001000U
#define PMU_GLOBAL_PWR_STATE_R5_1_MASK    	0X00000800U
#define PMU_GLOBAL_PWR_STATE_R5_0_MASK    	0X00000400U
#define PMU_GLOBAL_PWR_STATE_L2_BANK0_MASK    	0X00000080U
#define PMU_GLOBAL_PWR_STATE_PP1_MASK    	0X00000020U
#define PMU_GLOBAL_PWR_STATE_PP0_MASK    	0X00000010U
#define PMU_GLOBAL_PWR_STATE_ACPU3_MASK    	0X00000008U
#define PMU_GLOBAL_PWR_STATE_ACPU2_MASK    	0X00000004U
#define PMU_GLOBAL_PWR_STATE_ACPU1_MASK    	0X00000002U
#define PMU_GLOBAL_PWR_STATE_ACPU0_MASK    	0X00000001U

#define ATCM0_ADDRESS 0xFFE00000
#define BTCM0_ADDRESS 0xFFE20000
#define ATCM1_ADDRESS 0xFFE90000
#define BTCM1_ADDRESS 0xFFEB0000
#define OCM_ADDRESS 0xFFFC0000

struct rpu_sram_map_t {
	unsigned vaddr;
	unsigned paddr;
	unsigned len;
};
static const struct rpu_sram_map_t rpu_sram_map[] = {
	{0x00000000, ATCM0_ADDRESS, 0x10000 },
	{0x00020000, BTCM0_ADDRESS, 0x10000 },
	{0xfffc0000, OCM_ADDRESS, 0x10000 },
	{0x00000000, 0,0 },
};

struct rpu_load_data {
	int devmem_fd;
	unsigned char *map;
	unsigned vaddr;
	unsigned len;
};

static int elf_rpu_load_cb (void *data, unsigned char *buf,
			    unsigned len, unsigned vaddr)
{
	struct rpu_load_data *d = (struct rpu_load_data *)data;

	while (len > 0) {
		if (vaddr < d->vaddr || vaddr >= d->vaddr + d->len) {
			/* Not within the mapped area */
			if (d->map != NULL)
				munmap(d->map, d->len);
			const struct rpu_sram_map_t *map;
			for (map = rpu_sram_map; map->len; map++)
				if (vaddr >= map->vaddr
				    && vaddr < map->vaddr + map->len)
					break;
			if (map->len == 0) {
				printf ("rpu load: no vaddr 0x%08x\n",
					vaddr);
				return -1;
			}
			d->map = mmap(NULL, map->len, PROT_READ | PROT_WRITE,
				      MAP_SHARED, d->devmem_fd, map->paddr);
			if (d->map == MAP_FAILED) {
				printf("rpu load: cannot map 0x%08x: %m\n",
				       map->paddr);
				return -1;
			}
			d->vaddr = map->vaddr;
			d->len = map->len;
		}

		printf("load at 0x%08x (up to 0x%08x)\n", vaddr, len);

		while (len > 0 && vaddr < d->vaddr + d->len) {
			d->map[vaddr - d->vaddr] = *buf++;
			vaddr++;
			len--;
		}
	}
	return 0;
}

static int dump_tcm(int fd, unsigned addr)
{
	void *tcm;
	tcm = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, addr);
	if (tcm == MAP_FAILED) {
		printf("cannot mmap tcm: %m\n");
		return 0;
	}
	for (unsigned i = 0; i < 256; i += 4) {
		if (i % 16 == 0)
			printf("%08x:", i);
		printf(" %08x", *(unsigned *)(tcm + i));
		if (i % 16 == 12)
			printf("\n");
	}
	munmap(tcm, 0x1000);
	return 0;
}

static int clear_tcm(int fd, const struct rpu_sram_map_t *map)
{
	void *tcm;
	volatile double *d;

	tcm = mmap(NULL, map->len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map->paddr);
	if (tcm == MAP_FAILED) {
		printf("cannot mmap tcm: %m\n");
		return 0;
	}

	d = (volatile double *)tcm;
	for (unsigned i = 0; i < map->len; i += sizeof(double))
		d[i / sizeof(double)] = 0.0;
	munmap(tcm, map->len);
	return 0;
}

static int zynqmp_pm(const char *str)
{
	int fd;
	static const char pm_file[] = "/sys/kernel/debug/zynqmp-firmware/pm";
	size_t len = strlen(str);

	fd = open(pm_file, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s: %m\n", pm_file);
		return -1;
	}
	if (write(fd, str, len) != len) {
		fprintf(stderr, "cannot write string to %s: %m\n", pm_file);
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int zynqmp_init_rpu(int fd)
{
	/* configure RPU in split mode */
	if (zynqmp_pm ("pm_ioctl 0 1 1 0\n") < 0)
		return -1;
	/* configure TCM in split mode */
	if (zynqmp_pm ("pm_ioctl 0 3 0 0\n") < 0)
		return -1;
	/* set boot address */
	if (zynqmp_pm ("pm_ioctl 7 2 0 0\n") < 0)
		return -1;
	/* power-up and un-reset RPU/TCM (but still halded) */
	if (zynqmp_pm ("pm_request_node 15\n") < 0)
		return -1;
	if (zynqmp_pm ("pm_request_node 16\n") < 0)
		return -1;
	/* Clear ATCM and BTCM.  This is needed for ECC. */
	clear_tcm(fd, &rpu_sram_map[0]);
	clear_tcm(fd, &rpu_sram_map[1]);
	return 0;
}

#define R5_DBG_DIDR	0x0000 // Debug ID register
#define R5_DBG_WFAR	0x0018 // The Watchpoint Fault Address Register
#define R5_DBG_VCR	0x001C // Vector Catch Register
#define R5_DBG_DSCCR	0x0028 // Debug State Cache Control Register
#define R5_DBG_DTRRXext	0x0080 // Read Data Transfer Register
#define R5_DBG_ITR	0x0084 // Instruction Transfer Register
#define R5_DBG_DSCRext	0x0088 // Debug Status and Control Register
#define R5_DBG_DTRTXext	0x008C // Write Data Transfer Register
#define R5_DBG_DRCR	0x0090 // Debug Run Control Register
#define R5_DBG_BVR0	0x0100 // Breakpoint Value Register 0
#define R5_DBG_BVR1	0x0104 // Breakpoint Value Register 1
#define R5_DBG_BVR2	0x0108 // Breakpoint Value Register 2
#define R5_DBG_BVR3	0x010C // Breakpoint Value Register 3
#define R5_DBG_BVR4	0x0110 // Breakpoint Value Register 4
#define R5_DBG_BVR5	0x0114 // Breakpoint Value Register 5
#define R5_DBG_BVR6	0x0118 // Breakpoint Value Register 6
#define R5_DBG_BVR7	0x011C // Breakpoint Value Register 7
#define R5_DBG_BCR0	0x0140 // Breakpoint Control Register 0
#define R5_DBG_BCR1	0x0144 // Breakpoint Control Register 1
#define R5_DBG_BCR2	0x0148 // Breakpoint Control Register 2
#define R5_DBG_BCR3	0x014C // Breakpoint Control Register 3
#define R5_DBG_BCR4	0x0150 // Breakpoint Control Register 4
#define R5_DBG_BCR5	0x0154 // Breakpoint Control Register 5
#define R5_DBG_BCR6	0x0158 // Breakpoint Control Register 6
#define R5_DBG_BCR7	0x015C // Breakpoint Control Register 7
#define R5_DBG_WVR0	0x0180 // Watchpoint Value Register 0
#define R5_DBG_WVR1	0x0184 // Watchpoint Value Register 1
#define R5_DBG_WVR2	0x0188 // Watchpoint Value Register 2
#define R5_DBG_WVR3	0x018C // Watchpoint Value Register 3
#define R5_DBG_WVR4	0x0190 // Watchpoint Value Register 4
#define R5_DBG_WVR5	0x0194 // Watchpoint Value Register 5
#define R5_DBG_WVR6	0x0198 // Watchpoint Value Register 6
#define R5_DBG_WVR7	0x019C // Watchpoint Value Register 7
#define R5_DBG_WCR0	0x01C0 // Watchpoint Control Register 0
#define R5_DBG_WCR1	0x01C4 // Watchpoint Control Register 1
#define R5_DBG_WCR2	0x01C8 // Watchpoint Control Register 2
#define R5_DBG_WCR3	0x01CC // Watchpoint Control Register 3
#define R5_DBG_WCR4	0x01D0 // Watchpoint Control Register 4
#define R5_DBG_WCR5	0x01D4 // Watchpoint Control Register 5
#define R5_DBG_WCR6	0x01D8 // Watchpoint Control Register 6
#define R5_DBG_WCR7	0x01DC // Watchpoint Control Register 7
#define R5_DBG_OSLSR	0x0304 // Operating System Lock Status Register
#define R5_DBG_PRCR	0x0310 // Device Powerdown and Reset Control Reg
#define R5_DBG_PRSR	0x0314 // Device Powerdown and Reset Status Reg
#define R5_DBG_MIDR	0x0D00 // Main ID Register
#define R5_DBG_CTR	0x0D04 // Cache Type Register
#define R5_DBG_TCMTR	0x0D08 // TCM Type Register
#define R5_DBG_MPUIR	0x0D10 // MPU Type Register
#define R5_DBG_MPIDR	0x0D14 // Multiprocessor Affinity Register
#define R5_DBG_ID_PFR0	0x0D20 // Processor Feature Register 0
#define R5_DBG_ID_PFR1	0x0D24 // Processor Feature Register 1
#define R5_DBG_ID_DFR0	0x0D28 // Debug Feature Register 0
#define R5_DBG_ID_AFR0	0x0D2C // Auxiliary Feature Register 0
#define R5_DBG_ID_MMFR0	0x0D30 // Memory Model Feature Register 0
#define R5_DBG_ID_MMFR1	0x0D34 // Memory Model Feature Register 1
#define R5_DBG_ID_MMFR2	0x0D38 // Memory Model Feature Register 2
#define R5_DBG_ID_MMFR3	0x0D3C // Memory Model Feature Register 3
#define R5_DBG_ID_ISAR0	0x0D40 // ISA Feature Register 0
#define R5_DBG_ID_ISAR1	0x0D44 // ISA Feature Register 1
#define R5_DBG_ID_ISAR2	0x0D48 // ISA Feature Register 2
#define R5_DBG_ID_ISAR3	0x0D4C // ISA Feature Register 3
#define R5_DBG_ID_ISAR4	0x0D50 // ISA Feature Register 4
#define R5_DBG_ID_ISAR5	0x0D54 // ISA Feature Register 5
#define R5_DBG_ETMIF	0x0ED8 // ETM Interface Integration Register
#define R5_DBG_MISCOUT	0x0EF8 // Miscellaneous Outputs Integration Register
#define R5_DBG_MISCIN	0x0EFC // Miscellaneous Inputs Integration Register
#define R5_DBG_ITCTRL	0x0F00 // Integration Mode Control Register
#define R5_DBG_CLAIMSET	0x0FA0 // Claim Tag Set Register
#define R5_DBG_CLAIMCLR	0x0FA4 // Claim Tag Clear Register
#define R5_DBG_LAR	0x0FB0 // Lock Access Register
#define R5_DBG_LSR	0x0FB4 // Lock Status Register
#define R5_DBG_AUTHSTATUS 0x0FB8 // Authentication Status Register
#define R5_DBG_DEVID	0x0FC8 // Device Indentifier
#define R5_DBG_DEVTYPE	0x0FCC // Device Type Register
#define R5_DBG_PIDR4	0x0FD0 // Peripheral ID Register 4
#define R5_DBG_PIDR5	0x0FD4 // Peripheral ID Register 5
#define R5_DBG_PIDR6	0x0FD8 // Peripheral ID Register 6
#define R5_DBG_PIDR7	0x0FDC // Peripheral ID Register 7
#define R5_DBG_PIDR0	0x0FE0 // Peripheral ID Register 0
#define R5_DBG_PIDR1	0x0FE4 // Peripheral ID Register 1
#define R5_DBG_PIDR2	0x0FE8 // Peripheral ID Register 2
#define R5_DBG_PIDR3	0x0FEC // Peripheral ID Register 3
#define R5_DBG_CIDR0	0x0FF0 // Component ID Register 0
#define R5_DBG_CIDR1	0x0FF4 // Component ID Register 1
#define R5_DBG_CIDR2	0x0FF8 // Component ID Register 2
#define R5_DBG_CIDR3	0x0FFC

struct bit_xlat_t {
	const char *name;
	unsigned bit;
};

#define DSCR_RXfull (1 << 30)
#define DSCR_TXfull (1 << 29)
#define DSCR_PipeAdv (1 << 25)
#define DSCR_InstrCompl (1 << 24)
#define DSCR_ExtDCCmode1 (1 << 21)
#define DSCR_ExtDCCmode0 (1 << 20)
#define DSCR_ADAdiscard (1 << 19)
#define DSCR_MDBgen (1 << 15)
#define DSCR_HDBGen (1 << 14)
#define DSCR_ITRen (1 << 13)
#define DSCR_UDCCdis (1 << 12)
#define DSCR_INTdis (1 << 11)
#define DSCR_DBGack (1 << 10)
#define DSCR_UND_I (1 << 8)
#define DSCR_ADABORT_I (1 << 7)
#define DSCR_SDABORT_I (1 << 6)
#define DSCR_MOE3 (1 << 5)
#define DSCR_MOE2 (1 << 4)
#define DSCR_MOE1 (1 << 3)
#define DSCR_MOE0 (1 << 2)
#define DSCR_RESTARTED (1 << 1)
#define DSCR_HALTED (1 << 0)

static const struct bit_xlat_t dscr_xlat[] = {
	{ "RXfull", DSCR_RXfull },
	{ "TXfull", DSCR_TXfull },
	{ "PipeAdv", DSCR_PipeAdv },
	{ "InstrCompl", DSCR_InstrCompl },
	{ "ExtDCCmode1", DSCR_ExtDCCmode1 },
	{ "ExtDCCmode0", DSCR_ExtDCCmode0 },
	{ "ADAdiscard", DSCR_ADAdiscard },
	{ "MDBgen", DSCR_MDBgen },
	{ "HDBGen", DSCR_HDBGen },
	{ "ITRen", DSCR_ITRen },
	{ "UDCCdis", DSCR_UDCCdis },
	{ "INTdis", DSCR_INTdis },
	{ "DBGack", DSCR_DBGack },
	{ "UND_I", DSCR_UND_I },
	{ "ADABORT_I", DSCR_ADABORT_I },
	{ "SDABORT_I", DSCR_SDABORT_I },
	{ "MOE3", DSCR_MOE3 },
	{ "MOE2", DSCR_MOE2 },
	{ "MOE1", DSCR_MOE1 },
	{ "MOE0", DSCR_MOE0 },
	{ "RESTARTED", DSCR_RESTARTED },
	{ "HALTED", DSCR_HALTED },
	{ NULL, 0 }
};

#define SCTLR_M (1 << 0)
#define SCTLR_A (1 << 1)
#define SCTLR_C (1 << 2)
#define SCTLR_SW (1 << 10)
#define SCTLR_Z (1 << 11)
#define SCTLR_I (1 << 12)
#define SCTLR_V (1 << 13)
#define SCTLR_RR (1 << 14)
#define SCTLR_BR (1 << 17)
#define SCTLR_DZ (1 << 19)
#define SCTLR_FI (1 << 21)
#define SCTLR_VE (1 << 24)
#define SCTLR_EE (1 << 25)
#define SCTLR_NMFI (1 << 27)
#define SCTLR_TRE (1 << 28)
#define SCTLR_AFE (1 << 29)
#define SCTLR_TE (1 << 30)
#define SCTLR_IE (1 << 31)

static const struct bit_xlat_t sctlr_xlat[] = {
	{ "M", SCTLR_M },
	{ "A", SCTLR_A },
	{ "C", SCTLR_C },
	{ "SW", SCTLR_SW },
	{ "Z", SCTLR_Z },
	{ "I", SCTLR_I },
	{ "V", SCTLR_V },
	{ "RR", SCTLR_RR },
	{ "BR", SCTLR_BR },
	{ "DZ", SCTLR_DZ },
	{ "FI", SCTLR_FI },
	{ "VE", SCTLR_VE },
	{ "EE", SCTLR_EE },
	{ "NMFI", SCTLR_NMFI },
	{ "TRE", SCTLR_TRE },
	{ "AFE", SCTLR_AFE },
	{ "TE", SCTLR_TE },
	{ "IE", SCTLR_IE },
	{ NULL, 0 }
};

#define CPSR_T  (1 << 5)
#define CPSR_F  (1 << 6)
#define CPSR_I  (1 << 7)
#define CPSR_A  (1 << 8)
#define CPSR_E  (1 << 9)
#define CPSR_J  (1 << 24)
#define CPSR_Q  (1 << 27)
#define CPSR_V  (1 << 28)
#define CPSR_C  (1 << 29)
#define CPSR_Z  (1 << 30)
#define CPSR_N  (1 << 31)

static const struct bit_xlat_t cpsr_xlat[] = {
	{ "T", CPSR_T },
	{ "F", CPSR_F },
	{ "I", CPSR_I },
	{ "A", CPSR_A },
	{ "E", CPSR_E },
	{ "J", CPSR_J },
	{ "Q", CPSR_Q },
	{ "V", CPSR_V },
	{ "C", CPSR_C },
	{ "Z", CPSR_Z },
	{ "N", CPSR_N },
	{ NULL, 0 }
};

static void disp_bits(const struct bit_xlat_t *xlat, uint32_t v)
{
	for (; xlat->name; xlat++)
		if (v & xlat->bit)
			printf(" %s", xlat->name);
}

static void dbg_r5_write_dbgreg(void *regs, unsigned off, unsigned val)
{
	*(volatile unsigned *)(regs + off) = val;
}

static unsigned dbg_r5_read_dbgreg(void *regs, unsigned off)
{
	return *(volatile unsigned *)(regs + off);
}

static unsigned dbg_r5_read_dscr(void *regs)
{
	return *(unsigned *)(regs + R5_DBG_DSCRext);
}

static void dbg_r5_write_dscr(void *regs, unsigned val)
{
	*(volatile unsigned *)(regs + R5_DBG_DSCRext) = val;
}

static void dbg_r5_write_drcr(void *regs, unsigned val)
{
	*(volatile unsigned *)(regs + R5_DBG_DRCR) = val;
}

static void dbg_r5_write_vcr(void *regs, unsigned val)
{
	*(volatile unsigned *)(regs + R5_DBG_VCR) = val;
}

static void dbg_r5_write_dsccr(void *regs, unsigned val)
{
	*(volatile unsigned *)(regs + R5_DBG_DSCCR) = val;
}

static void dbg_r5_unlock_access(void *regs)
{
	*(volatile unsigned *)(regs + R5_DBG_LAR) = 0xc5acce55;
}

static void dbg_r5_enable_itr(void *regs)
{
	unsigned dscr = dbg_r5_read_dscr(regs);

	/* ITRen */
	dbg_r5_write_dscr(regs, dscr | DSCR_ITRen);
}

static void dbg_r5_halt_restart(void *regs, unsigned val)
{
	/* Request */
	dbg_r5_write_drcr(regs, val);

	while (1) {
		unsigned dscr = dbg_r5_read_dscr(regs);
		if (dscr & val)
			return;
		usleep(1);
	}
}

static void dbg_r5_restart(void *regs)
{
	dbg_r5_halt_restart(regs, 2);
}

static void dbg_r5_reset(void *regs)
{
	*(volatile unsigned *)(regs + R5_DBG_PRCR) |= 2;
}

static int dbg_r5_wait_dcc_tx(void *regs)
{
	/* Wait until TXfull is set */
	for (unsigned timeout = 10; timeout > 0; timeout--) {
		if (dbg_r5_read_dscr(regs) & DSCR_TXfull)
			return 0;
		usleep(1);
	}
	return -1;
}

static unsigned dbg_r5_read_dcc(void *regs)
{
	if (dbg_r5_wait_dcc_tx(regs) < 0) {
		printf("cannot read dcc\n");
		return 0;
	}

	return *(volatile unsigned *)(regs + R5_DBG_DTRTXext);
}

static void dbg_r5_write_dcc(void *regs, uint32_t val)
{
	/* Wait until RXfull is empty */
	while (dbg_r5_read_dscr(regs) & DSCR_RXfull)
		usleep(1);

	*(volatile unsigned *)(regs + R5_DBG_DTRRXext) = val;
}

static void dbg_r5_exec_insn(void *regs, unsigned insn)
{
	unsigned dscr;

	/* Wait until InstrCompl is set */
	while (1) {
		dscr = dbg_r5_read_dscr(regs);
		if (dscr & (1 << 24))
			break;
		usleep(1);
	}

	if (0) {
		printf ("dscr: %08x\n", dscr);
		/* Clear sticky pipeline advance */
		dbg_r5_write_drcr(regs, (1 << 3));
		printf ("dscr: %08x\n", dbg_r5_read_dscr(regs));
	}

	*(volatile unsigned *)(regs + R5_DBG_ITR) = insn;

	/* Wait until InstrCompl is set */
	while (!(dbg_r5_read_dscr(regs) & (1 << 24)))
		usleep(1);
}

static void dbg_r5_exec_mrc(void *regs, unsigned coproc, unsigned opc1,
			    unsigned crn, unsigned crm, unsigned opc2)
{
	unsigned insn;

	insn = (0xe << 28) | (0xe << 24) | (opc1 << 21) | (1 << 20)
		| (crn << 16) | (0 << 12) | (coproc << 8) | (opc2 << 5)
		| (1 << 4) | (crm << 0);

	dbg_r5_exec_insn(regs, insn);
}

static void dbg_r5_exec_mcr(void *regs, unsigned coproc, unsigned opc1,
			    unsigned crn, unsigned crm, unsigned opc2)
{
	unsigned insn;

	insn = (0xe << 28) | (0xe << 24) | (opc1 << 21) | (0 << 20)
		| (crn << 16) | (0 << 12) | (coproc << 8) | (opc2 << 5)
		| (1 << 4) | (crm << 0);

	dbg_r5_exec_insn(regs, insn);
}

static void dbg_r5_exec_dcc_to_reg(void *regs, unsigned rd)
{
	/* MRC p14, 0, rd, c0, c5, 0 */
	dbg_r5_exec_insn(regs, 0xee100e15 + (rd << 12));
}

static void dbg_r5_exec_reg_to_dcc(void *regs, unsigned rd)
{
	/* MCR p14, 0, rd, c0, c5, 0 */
	dbg_r5_exec_insn(regs, 0xee000e15 + (rd << 12));
}

static void dbg_r5_exec_iciallu(void *regs)
{
	/* MCR p15, 0, r0, cr7, cr5, 0 */
	dbg_r5_exec_insn(regs, 0xee070f15);
}

/* Read a reg, from 0 to 14 */
static unsigned dbg_r5_read_reg(void *regs, unsigned rd)
{
	dbg_r5_exec_reg_to_dcc(regs, rd);
	return dbg_r5_read_dcc(regs);
}

static unsigned dbg_r5_read_pc_via_r0(void *regs)
{
	/* mov r0, pc */
	dbg_r5_exec_insn(regs, 0xe1a0000f);

	dbg_r5_exec_reg_to_dcc(regs, 0);

	return dbg_r5_read_dcc(regs);
}

static unsigned dbg_r5_read_cpsr_via_r0(void *regs)
{
	/* MRS r0, CPSR */
	dbg_r5_exec_insn(regs, 0xe10f0000);

	dbg_r5_exec_reg_to_dcc(regs, 0);

	return dbg_r5_read_dcc(regs);
}

static void dbg_r5_write_pc_via_r0(void *regs, uint32_t val)
{
	dbg_r5_write_dcc(regs, val);

	dbg_r5_exec_dcc_to_reg(regs, 0);

	/* mov pc, r0 */
	dbg_r5_exec_insn(regs, 0xe1a0f000);
}

static void dbg_r5_write_cpsr_via_r0(void *regs, uint32_t val)
{
	dbg_r5_write_dcc(regs, val);

	dbg_r5_exec_dcc_to_reg(regs, 0);

	/* msr CPSR, r0 */
	dbg_r5_exec_insn(regs, 0xe129f000);
}

/* Write a reg, from 0 to 14 */
static void dbg_r5_write_reg(void *regs, unsigned rd, uint32_t val)
{
	dbg_r5_write_dcc(regs, val);

	dbg_r5_exec_dcc_to_reg(regs, rd);
}

static unsigned dbg_r5_read_cp_via_r0(void *regs,
				      unsigned coproc, unsigned opc1,
				      unsigned crn, unsigned crm, unsigned opc2)
{
	dbg_r5_exec_mrc(regs, coproc, opc1, crn, crm, opc2);
	return dbg_r5_read_reg(regs, 0);
}

static void dbg_r5_write_cp_via_r0(void *regs,
				   unsigned coproc, unsigned opc1,
				   unsigned crn, unsigned crm, unsigned opc2,
				   unsigned val)
{
	dbg_r5_write_reg(regs, 0, val);
	dbg_r5_exec_mcr(regs, coproc, opc1, crn, crm, opc2);
}

static const char * const xlat_moe[] = {
	"halt", "bp", "0010", "bkpt",
	"RQm", "0101", "0110", "0111",
	"1000", "1001", "wp", "1011",
	"1100", "1101", "1110", "1111" };

static void dbg_disp_dscr(unsigned dscr)
{
	unsigned moe = (dscr >> 2) & 0x0f;
	printf("dscr: %08x, moe:%s", dscr, xlat_moe[moe]);
	disp_bits(dscr_xlat, dscr);
	printf("\n");
}

static int dbg_r5_halt(void *regs)
{
	unsigned timeout;

	unsigned dscr = dbg_r5_read_dscr(regs);
	if (!(dscr & 1)) {
		/* Not in debug state */

		if (!(dscr & DSCR_HDBGen)) {
			/* Enable halting debug-mode */
			dbg_r5_write_dscr(regs, dscr | DSCR_HDBGen);
		}

		/* Request to halt */
		dbg_r5_write_drcr(regs, 1);

		for (timeout = 10; timeout > 0; timeout--) {
			dscr = dbg_r5_read_dscr(regs);
			if (dscr & 1)
				break;
			usleep(1);
		}
		if (timeout == 0) {
			printf("cannot halt target!\n");
			return -1;
		}
	}

	if (!(dscr & DSCR_ITRen))
		dbg_r5_write_dscr(regs, dscr | DSCR_ITRen);

	if (dscr & DSCR_TXfull) {
		printf ("TXfull was set!\n");

		dbg_r5_read_dbgreg (regs, R5_DBG_DTRTXext);
	}

	if (dscr & DSCR_RXfull) {
		unsigned r0;
		printf ("RXfull was set!\n");

		r0 = dbg_r5_read_reg (regs, 0);
		dbg_r5_exec_dcc_to_reg(regs, 0);
		dbg_r5_write_reg (regs, 0, r0);
	}

	return 0;
}

static void dbg_r5_dump(void *regs)
{
	unsigned dscr = dbg_r5_read_dscr(regs);
	printf ("DIDR: %08x\n", *(unsigned *)(regs + R5_DBG_DIDR));
	printf ("WAFR: %08x\n", *(unsigned *)(regs + R5_DBG_WFAR));
	printf ("DSCR: %08x", dscr);
	disp_bits(dscr_xlat, dscr);
	printf ("\n");
	printf ("LSR:  %08x\n", *(unsigned *)(regs + R5_DBG_LSR));
	printf ("AUTHSTATUS: %08x\n", *(unsigned *)(regs + R5_DBG_AUTHSTATUS));
	printf ("PRSR: %08x\n", *(unsigned *)(regs + R5_DBG_PRSR));
	printf ("PRCR: %08x\n", *(unsigned *)(regs + R5_DBG_PRCR));

	dbg_r5_enable_itr(regs);

	if (verbose > 2 && (dscr & 1)) {
		for (unsigned i = 0; i < 15; i++)
			printf("R%02u: %08x\n", i, dbg_r5_read_reg(regs, i));
	}
}

static void *zynqmp_map_dbg_r5(int fd, unsigned dbg_base)
{
	void *regs;

	regs = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, dbg_base);
	if (regs == MAP_FAILED) {
		printf("cannot mmap dbg regs: %m\n");
		return NULL;
	}
	return regs;
}

static void zynqmp_dbg_dump(int fd, unsigned dbg_base)
{
	void *regs = zynqmp_map_dbg_r5(fd, dbg_base);
	if (regs == NULL)
		return;
	dbg_r5_dump(regs);
}

static void zynqmp_dbg_halt(int fd, unsigned dbg_base)
{
	void *regs = zynqmp_map_dbg_r5(fd, dbg_base);
	if (regs == NULL)
		return;

	dbg_r5_unlock_access(regs);

	dbg_r5_halt(regs);
}

static void zynqmp_dbg_restart(int fd, unsigned dbg_base)
{
	void *regs = zynqmp_map_dbg_r5(fd, dbg_base);
	if (regs == NULL)
		return;
	dbg_r5_restart(regs);
}

static void zynqmp_dbg_reset(int fd, unsigned dbg_base)
{
	void *regs = zynqmp_map_dbg_r5(fd, dbg_base);
	if (regs == NULL)
		return;
	dbg_r5_reset(regs);
}

static int gdb_check_mem_fault(struct dbg_port *dbg, unsigned addr)
{
	unsigned dscr;
	void *regs = dbg->dap;

	dscr = dbg_r5_read_dscr(regs);
	if (dscr & (DSCR_SDABORT_I | DSCR_ADABORT_I)) {
		/* Clear the bit */
		dbg_r5_write_drcr(regs, 4);

		if (verbose) {
			unsigned r0, val;
			printf ("memory access at 0x%08x failed, dscr: %08x",
				addr, dscr);
			disp_bits(dscr_xlat, dscr);

			r0 = dbg_r5_read_reg(regs, 0);

			/* Read DFSR Data Fault Status Register
			   mrc	15, 0, r0, cr5, cr0, {0} */
			dbg_r5_exec_insn(regs, 0xee150f10);
			dbg_r5_exec_reg_to_dcc(regs, 0);
			val = dbg_r5_read_dcc(regs);
			printf (", DFSR: %08x", val);

			/* Read DFAR Data Fault Address Register
			   mrc	15, 0, r0, cr6, cr0, {0} */
			dbg_r5_exec_insn(regs, 0xee160f10);
			dbg_r5_exec_reg_to_dcc(regs, 0);
			val = dbg_r5_read_dcc(regs);
			printf (", DFAR: %08x\n", val);

			dbg_r5_write_reg(regs, 0, r0);
		}

		return -1;
	}
	return 0;
}

/**
 * Write data to memory
 */
static int gdb_r5_handle_M(struct dbg_port *dbg,
			   struct gdb_packet *out,
			   struct gdb_packet *in)
{
	uint32_t addr, n;
	uint32_t r0, r1;
	char *indata;
	int ret;

	indata = strchr(in->data, ':');
	if (!indata) {
		gdb_packet_put_str(out, "E01");
		return 0;
	}
	indata++; /* skip ':' */
	ret = sscanf(in->data + 1, "%"SCNx32",%"SCNx32":", &addr, &n);
	if (ret != 2) {
		gdb_packet_put_str(out, "E02");
		return 0;
	}

	if (n * 2 != in->size - (indata - in->data)) {
		gdb_packet_put_str(out, "E03");
		return 0;
	}

	r0 = dbg_r5_read_reg(dbg->dap, 0);
	r1 = dbg_r5_read_reg(dbg->dap, 1);
	dbg_r5_write_reg(dbg->dap, 0, addr);

	if (addr % 4 == 0) {
		for (; n >= 4; n -= 4, indata += 8, addr += 4) {
			uint32_t w;

			ret = sscanf(indata, "%08"SCNx32, &w);
			if (ret != 1)
				break;

			dbg_r5_write_reg(dbg->dap, 1, ntohl(w));

			/* str r1,[r0],#4 */
			dbg_r5_exec_insn(dbg->dap, 0xe4801004);

			if (gdb_check_mem_fault(dbg, addr) < 0)
				break;
		}
	}
	for (; n > 0; --n, indata += 2, addr++) {
		uint32_t b;

		ret = sscanf(indata, "%02"SCNx32, &b);
		if (ret != 1)
			break;
		dbg_r5_write_reg(dbg->dap, 1, b);
		/* strb r1,[r0],#1 */
		dbg_r5_exec_insn(dbg->dap, 0xe4c01001);

		if (gdb_check_mem_fault(dbg, addr) < 0)
			break;
	}

	dbg_r5_write_reg(dbg->dap, 0, r0);
	dbg_r5_write_reg(dbg->dap, 1, r1);

	if (n > 0)
		gdb_packet_put_str(out, "E04");
	else
		gdb_packet_put_str(out, "OK");

	return 0;
}

/**
 * Read data from memory
 */
static int gdb_r5_handle_m(struct dbg_port *dbg,
			   struct gdb_packet *out,
			   struct gdb_packet *in)
{
	uint32_t addr, n;
	uint32_t r0, r1;
	int ret;

	ret = sscanf(in->data + 1, "%x,%x", &addr, &n);
	if (ret != 2) {
		gdb_packet_put_str(out, "E01");
		return 0;
	}
	r0 = dbg_r5_read_reg(dbg->dap, 0);
	r1 = dbg_r5_read_reg(dbg->dap, 1);
	dbg_r5_write_reg(dbg->dap, 0, addr);
	out->size = 0;
	for (; n > 0; --n, addr++) {
		uint8_t b;

		/* ldrb r1,[r0],1 */
		dbg_r5_exec_insn(dbg->dap, 0xe4d01001);

		if (gdb_check_mem_fault(dbg, addr) < 0) {
			gdb_packet_put_str(out, "E01");
			break;
		}
		b = dbg_r5_read_reg(dbg->dap, 1);
		gdb_packet_append_x8(out, b);
	}

	dbg_r5_write_reg(dbg->dap, 0, r0);
	dbg_r5_write_reg(dbg->dap, 1, r1);

	return 0;
}

/**
 * Write all register
 */
static int gdb_r5_handle_G(struct dbg_port *dbg,
			   struct gdb_packet *out,
			   struct gdb_packet *in)
{
#define NBR_R5_REGS (16 + 3*8 + 1 + 1)
	uint32_t regs[NBR_R5_REGS]; /* 16 regs, 8 fpa, fps, cpsr */
	int i;

	if (in->size != 1 + NBR_R5_REGS * 8) {
		gdb_packet_put_str(out, "E01");
		return 0;
	}

	for (i = 0; i < NBR_R5_REGS; ++i) {
		uint32_t v;
		int ret = sscanf(in->data + 1 + i * 8, "%08"SCNx32, &v);

		regs[i] = ntohl(v);

		if (ret != 1) {
			gdb_packet_put_str(out, "E02");
			return 0;
		}
	}

	dbg_r5_write_pc_via_r0(dbg->dap, regs[15]);
	dbg_r5_write_cpsr_via_r0(dbg->dap, regs[41]);

	for (i = 0; i < 15; ++i)
		dbg_r5_write_reg(dbg->dap, i, regs[i]);

	if (verbose > 1) {
		for (i = 0; i < 16; ++i) {
			printf ("R%02u: %08x", i, regs[i]);
			if (i % 4 == 3)
				printf ("\n");
			else
				printf ("  ");
		}
	}

	gdb_packet_put_str(out, "OK");

	return 0;
}

static int gdb_r5_handle_g(struct dbg_port *dbg,
			   struct gdb_packet *out,
			   struct gdb_packet *in)
{
	uint32_t regs[15], pc, cpsr;
	unsigned i;

	out->size = 0;

	/* 0-15: regs */
	for (i = 0; i < 15; ++i) {
		regs[i] = dbg_r5_read_reg(dbg->dap, i);
		gdb_packet_append_x32(out, htonl(regs[i]));
	}

	cpsr = dbg_r5_read_cpsr_via_r0(dbg->dap);
	pc = dbg_r5_read_pc_via_r0(dbg->dap);

	/* Adjust PC */
	if (cpsr & (1 << 5))
		pc -= 4; /* Thumb mode */
	else
		pc -= 8; /* ARM mode */
	/* 15: PC */
	gdb_packet_append_x32(out, htonl(pc));

	/* 16-24: fp0-fp7 (96b) + fps (32b) */
	for (i = 0; i < 8 * 3 + 1; i++)
		gdb_packet_append_x32(out, 0);

	/* 25: cpsr */
	gdb_packet_append_x32(out, htonl(cpsr));

	/* Restore r0 */
	dbg_r5_write_reg(dbg->dap, 0, regs[0]);

	return 0;
}

static void dbg_r5_disp_sctlr(void *regs)
{
	unsigned val, r0;

	r0 = dbg_r5_read_reg(regs, 0);

	val = dbg_r5_read_cp_via_r0(regs, 15, 0, 1, 0, 0);
	printf("sctlr: %08x", val);
	disp_bits(sctlr_xlat, val);
	printf("\n");

	/* Read ACTLR base register
	   MRC p15, 0, <Rd>, c1, c0, 1 */
	val = dbg_r5_read_cp_via_r0(regs, 15, 0, 1, 0, 1);
	printf ("ACTLR: %08x\n", val);

	dbg_r5_write_reg(regs, 0, r0);
}

static void dbg_r5_disp_mpu(void *regs)
{
	unsigned val, r0;
	unsigned nreg;
	unsigned i;

	if (dbg_r5_halt(regs) < 0)
		return;

	r0 = dbg_r5_read_reg(regs, 0);

	val = dbg_r5_read_cp_via_r0(regs, 15, 0, 0, 0, 0);
	printf("midr: %08x\n", val);

	val = dbg_r5_read_cp_via_r0(regs, 15, 0, 0, 0, 4);
	nreg = (val >> 8) & 0xff;
	printf("mpuir: %08x, nregions=%u\n", val, nreg);

	val = dbg_r5_read_cp_via_r0(regs, 15, 1, 0, 0, 0);
	printf("ccsidr: %08x\n", val);

	val = dbg_r5_read_cp_via_r0(regs, 15, 1, 0, 0, 1);
	printf("clidr: %08x\n", val);

	val = dbg_r5_read_cp_via_r0(regs, 15, 0, 1, 0, 0);
	printf("sctlr: %08x", val);
	disp_bits(sctlr_xlat, val);
	printf("\n");


	/* Note: region 0 has lowest priority, region 15 has highest priority */
	for (i = 0; i < nreg; i++) {
	  unsigned sz;
	  unsigned acc;
	  unsigned addr;
	  const char *ca;

	  /* Set index.  */
	  dbg_r5_write_cp_via_r0(regs, 15, 0, 6, 2, 0, i);

	  /* Read size and enable bit. */
	  sz = dbg_r5_read_cp_via_r0(regs, 15, 0, 6, 1, 2);
	  if ((sz & 1) == 0)
		  continue;

	  addr = dbg_r5_read_cp_via_r0(regs, 15, 0, 6, 1, 0);
	  acc = dbg_r5_read_cp_via_r0(regs, 15, 0, 6, 1, 4);

	  printf("reg %02u: base: %08x-%08x [sz=%04x, acc=%08x ",
		 i, addr, addr + (2 << ((sz >> 1) & 0x1f)) - 1, sz, acc);
	  switch(acc & 0x3f) {
	  case 0x00:
		  ca = "SO";
		  break;
	  case 0x10:
		  ca = "NC";
		  break;
	  case 0x0b:
		  ca = "WB";
		  break;
	  default:
		  ca = "??";
		  break;
	  }
	  printf ("%s", ca);
	  static const char * const rights[] = {
		  "p:-- u:-- ",
		  "p:rw u:-- ",
		  "p:rw u:ro ",
		  "p:rw u:rw ",

		  "p:?? u:?? ",
		  "p:ro u:-- ",
		  "p:ro u:ro ",
		  "p:?? u:?? "
	  };
	  printf(" %s %s]\n",
		 acc & (1 << 12) ? "nx" : "  ",
		 rights[(acc >>  8) & 0x7]);
	}
	dbg_r5_write_reg(regs, 0, r0);
}

static void dbg_r5_disp_tcm(void *regs)
{
	unsigned val, r0;

	r0 = dbg_r5_read_reg(regs, 0);

	/* Read BTCM base register
	   MRC p15, 0, <Rd>, c9, c1, 0 */
	val = dbg_r5_read_cp_via_r0(regs, 15, 0, 9, 1, 0);
	printf ("BTCM:  %08x\n", val);

	/* Read ATCM base register
	   MRC p15, 0, <Rd>, c9, c1, 1 */
	val = dbg_r5_read_cp_via_r0(regs, 15, 0, 9, 1, 1);
	printf ("ATCM:  %08x\n", val);

	/* Read ACTLR base register
	   MRC p15, 0, <Rd>, c1, c0, 1 */
	val = dbg_r5_read_cp_via_r0(regs, 15, 0, 1, 0, 1);
	printf ("ACTLR: %08x\n", val);

	dbg_r5_write_reg(regs, 0, r0);
}

static void dbg_r5_disp_dfault(void *regs)
{
	unsigned r0, val;

	r0 = dbg_r5_read_reg(regs, 0);

	/* Read DFSR Data Fault Status Register
	   mrc	15, 0, r0, cr5, cr0, {0} */
	dbg_r5_exec_mrc(regs, 15, 0, 5, 0, 0);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("DFSR:  %08x\n", val);

	/* Read DFAR Data Fault Address Register
	   mrc	15, 0, r0, cr6, cr0, {0} */
	dbg_r5_exec_mrc(regs, 15, 0, 6, 0, 0);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("DFAR:  %08x\n", val);

	/* Read ADFSR Data Fault Address Register
	   mrc	15, 0, r0, cr5, cr1, {0} */
	dbg_r5_exec_mrc(regs, 15, 0, 5, 1, 0);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("ADFSR: %08x", val);
	printf (" (side: %x, sideext: %x)\n",
		(val >> 22) & 0x3, (val >> 20) & 1);

	dbg_r5_write_reg(regs, 0, r0);
}

static void dbg_r5_disp_ifault(void *regs)
{
	unsigned r0, val;

	r0 = dbg_r5_read_reg(regs, 0);

	/* Read IFSR Instruction Fault Status Register
	   mrc	15, 0, r0, cr5, cr0, {1} */
	dbg_r5_exec_mrc(regs, 15, 0, 5, 0, 1);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("IFSR:  %08x\n", val);

	/* Read IFAR Instruction Fault Address Register
	   mrc	15, 0, r0, cr6, cr0, {2} */
	dbg_r5_exec_mrc(regs, 15, 0, 6, 0, 2);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("IFAR:  %08x\n", val);

	/* Read AIFSR Aux Instruction Fault Address Register
	   mrc	15, 0, r0, cr5, cr1, {1} */
	dbg_r5_exec_mrc(regs, 15, 0, 5, 1, 0);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("AIFSR: %08x", val);
	printf (" (side: %x, sideext: %x)\n",
		(val >> 22) & 0x3, (val >> 20) & 1);

	dbg_r5_write_reg(regs, 0, r0);
}

static const char * const xlat_cpsr_mode[] = {
	"user", "fiq", "irq", "scv",
	"0100", "0101", "mon", "abt",
	"1000", "1001", "hyp", "und",
	"1100", "1101", "1110", "sys"
};

static void dbg_disp_cpsr(unsigned cpsr)
{
	printf ("cpsr: %08x, mode: %s", cpsr, xlat_cpsr_mode[cpsr & 0x0f]);
	disp_bits (cpsr_xlat, cpsr);
	printf("\n");
}

static void gdb_disp_halt_state(struct dbg_port *dbg, unsigned dscr)
{
	unsigned r0;
	unsigned cpsr;
	unsigned pc;

	dbg_disp_dscr(dscr);
	r0 = dbg_r5_read_reg(dbg->dap, 0);

	cpsr = dbg_r5_read_cpsr_via_r0(dbg->dap);
	dbg_disp_cpsr(cpsr);
	pc = dbg_r5_read_pc_via_r0(dbg->dap);
	printf("raw pc: %08x\n", pc);

	dbg_r5_write_reg(dbg->dap, 0, r0);

	for (unsigned i = 0; i < 15; i++)
		printf("R%02u: %08x\n", i, dbg_r5_read_reg(dbg->dap, i));

}

/**
 * Continue command
 */
static int gdb_r5_handle_c(struct dbg_port *dbg,
			    struct gdb_packet *out,
			    struct gdb_packet *in)
{
	if (in->size > 1) {
		out->size = 0;
		return 0;
	}

	/* Invalidate I cache */
	dbg_r5_exec_iciallu(dbg->dap);

	dbg_r5_restart(dbg->dap);

	while (1) {
		int ret;

		/* Dump vuart. */
		gdb_maybe_read_term(dbg);

		unsigned dscr = dbg_r5_read_dscr(dbg->dap);
		if (dscr & 1) {
			if (verbose) {
				printf("target halted\n");
				gdb_disp_halt_state(dbg, dscr);
			}
			gdb_packet_put_str(out, "S05");
			break;
		}

		ret = gdb_maybe_stop(dbg);
		if (ret < 0)
			return ret;
		if (ret == 0)
			continue;

		/* GDB wants something from us */
		dbg_r5_halt(dbg->dap);
		gdb_packet_put_str(out, "S02");
		break;
	}

	return 0;
}

static int gdb_r5_handle_qRcmd(struct dbg_port *dbg,
			       struct gdb_packet *out,
			       struct gdb_packet *in)
{
	char buf[GDB_PACKET_SIZE_MAX / 2];

	if (gdb_qRcmd_decode(buf, in) < 0) {
		out->size = 0;
		return 0;
	}

	if (strcmp(buf, "help") == 0) {
		strcpy(buf, "usage: reset | cpsr | sctlr | mpu | tcm "
		       "| dfault | ifault | help\n");
	}
	else if (strcmp(buf, "reset") == 0) {
		/* svc mode, mask IRQ, FIQ, ASABORT */
		dbg_r5_write_cpsr_via_r0(dbg->dap, 0x1d3);
		dbg_r5_write_pc_via_r0(dbg->dap, 0);
		/* TODO: sctlr ? */
		strcpy(buf, "cpsr initialized\n");
	}
	else if (strcmp(buf, "cpsr") == 0) {
		unsigned dscr = dbg_r5_read_dscr(dbg->dap);
		gdb_disp_halt_state(dbg, dscr);
		strcpy(buf, "ok\n");
	}
	else if (strcmp(buf, "mpu") == 0) {
		dbg_r5_disp_mpu(dbg->dap);
		strcpy(buf, "ok\n");
	}
	else if (strcmp(buf, "tcm") == 0) {
		dbg_r5_disp_tcm(dbg->dap);
		strcpy(buf, "ok\n");
	}
	else if (strcmp(buf, "dfault") == 0) {
		dbg_r5_disp_dfault(dbg->dap);
		strcpy(buf, "ok\n");
	}
	else if (strcmp(buf, "ifault") == 0) {
		dbg_r5_disp_ifault(dbg->dap);
		strcpy(buf, "ok\n");
	}
	else if (strcmp(buf, "sctlr") == 0) {
		dbg_r5_disp_sctlr(dbg->dap);
		strcpy(buf, "ok\n");
	}
	else {
		strcpy(buf,"unhandled mon command, try 'mon help'\n");
	}

	/* Encode to hex.  */
	gdb_qRcmd_encode(buf, out);
	return 0;
}

static int gdb_r5_handle_q(struct dbg_port *dbg,
			   struct gdb_packet *out,
			   struct gdb_packet *in)
{
	if (strncmp(in->data, "qRcmd,", 6) == 0)
		return gdb_r5_handle_qRcmd(dbg, out, in);
	return gdb_handle_q(dbg, out, in);
}

static int gdb_r5_handle_D(struct dbg_port *dbg,
			   struct gdb_packet *out,
			   struct gdb_packet *in)
{
	/* Invalidate I cache */
	dbg_r5_exec_iciallu(dbg->dap);

	dbg_r5_write_vcr(dbg->dap, 0);
	dbg_r5_restart(dbg->dap);
	gdb_packet_put_str(out, "OK");

	return 0;
}

static gdb_command_t * const gdb_r5_commands[] = {
	['c'] = gdb_r5_handle_c,
	['D'] = gdb_r5_handle_D,
	['g'] = gdb_r5_handle_g,
	['G'] = gdb_r5_handle_G,
	['H'] = gdb_handle_H,
	['k'] = gdb_handle_k,
	['M'] = gdb_r5_handle_M,
	['m'] = gdb_r5_handle_m,
//	['p'] = gdb_urv_handle_p,
	['P'] = gdb_handle_P,
	['q'] = gdb_r5_handle_q,
//	['s'] = gdb_urv_handle_s,
	['v'] = gdb_handle_v,
	['v'] = gdb_handle_v,
	['X'] = gdb_handle_X,
	['?'] = gdb_handle_qm,
};

static int gdb_r5_halt(struct dbg_port *dbg)
{
	if (dbg_r5_halt(dbg->dap) < 0)
		return -1;

	/* Catch undefined, svc, prefetch, data */
	dbg_r5_write_vcr(dbg->dap, 0x1e);

	return 0;
}

static void gdb_r5_server(struct dbg_port *dbg, int argc, char **argv)
{
	dbg_r5_unlock_access(dbg->dap);

	/* For write-through */
	dbg_r5_write_dsccr(dbg->dap, 0);

	dbg_r5_dump(dbg->dap);

	/* Check writes */
	dbg_r5_write_dbgreg (dbg->dap, R5_DBG_WFAR, 0x005c3a7e);
	if (dbg_r5_read_dbgreg (dbg->dap, R5_DBG_WFAR) != 0x005c3a7e) {
		printf("cannot write debug registers, unauthorized ?\n");
		return;
	}

	dbg->cmds = gdb_r5_commands;
	dbg->n_cmds = sizeof(gdb_r5_commands) / sizeof(gdb_r5_commands[0]);
	dbg->post_connect_hook = gdb_r5_halt;

	gdb_server(dbg, argc, argv);
}

static void help_zynqmp_rpu(const char *name)
{
	printf("usage: %s zynqmp-rpu SUBCMD\n", progname);
}

static int do_zynqmp_rpu(int argc, char *argv[])
{
	int fd;
	void *rpu_map;
	void *crl_map;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "cannot open /dev/mem: %s\n",
			strerror(errno));
		return -1;
	}

	rpu_map = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, RPU_BASEADDR);

	crl_map = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, CRL_APB_BASEADDR);

	/* Do not try to map PMU, it is probably in the secure part */

	if (rpu_map == MAP_FAILED || crl_map == MAP_FAILED) {
		fprintf(stderr, "cannot map /dev/mem: %s\n",
			strerror(errno));
		close(fd);
		return -1;
	}

	for (unsigned i = 1; i < argc; i++) {
		if (strcmp(argv[i], "dump") == 0) {
			unsigned val;

			val = *(volatile unsigned *)(rpu_map + RPU_RPU_GLBL_CNTL);
			printf ("RPU glbl_cntl:    0x%08x\n", val);
			val = *(volatile unsigned *)(rpu_map + RPU_RPU_0_CFG);
			printf ("RPU rpu0_cfg:     0x%08x\n", val);
			val = *(volatile unsigned *)(rpu_map + RPU_RPU_0_STATUS);
			printf ("RPU rpu0_status:  0x%08x\n", val);
			val = *(volatile unsigned *)(rpu_map + RPU_RPU_1_CFG);
			printf ("RPU rpu1_cfg:     0x%08x\n", val);
			val = *(volatile unsigned *)(rpu_map + RPU_RPU_1_STATUS);
			printf ("RPU rpu1_status:  0x%08x\n", val);
			val = *(volatile unsigned *)(crl_map + CRL_APB_CPU_R5_CTRL);
			printf ("CRL_APB cpu_r5_ctrl:  0x%08x\n", val);
			val = *(volatile unsigned *)(crl_map + CRL_APB_RST_LPD_TOP);
			printf ("CRL_APB rst_lpd_top:  0x%08x\n", val);
		}
		else if (strcmp(argv[i], "clear-tcm") == 0) {
			clear_tcm(fd, &rpu_sram_map[0]);
			clear_tcm(fd, &rpu_sram_map[1]);
		}
		else if (strcmp(argv[i], "dump-atcm0") == 0) {
			if (dump_tcm(fd, ATCM0_ADDRESS) < 0)
				return -1;
		}
		else if (strcmp(argv[i], "dump-btcm0") == 0) {
			if (dump_tcm(fd, BTCM0_ADDRESS) < 0)
				return -1;
		}
		else if (strcmp(argv[i], "dump-ocm0") == 0) {
			if (dump_tcm(fd, OCM_ADDRESS) < 0)
				return -1;
		}
		else if (strcmp(argv[i], "dump-ocm3") == 0) {
			if (dump_tcm(fd, OCM_ADDRESS + 0x30000) < 0)
				return -1;
		}
		else if (strcmp(argv[i], "dump-elf") == 0) {
			if (i + 1 >= argc) {
				printf("missing elf filename\n");
				return -1;
			}
			const char *filename = argv[++i];
			if (elf_foreach_segment(filename, EM_ARM,
						elf_dump_cb, NULL) < 0)
				return -1;
		}
		else if (strcmp(argv[i], "rpu-off") == 0) {
			/* Power down RPU 0 */
			if (zynqmp_pm ("pm_force_powerdown 7\n") < 0)
				return -1;
		}
		else if (strcmp(argv[i], "load") == 0) {
			if (i + 1 >= argc) {
				printf("missing elf filename\n");
				return -1;
			}
			struct rpu_load_data data_cb;

			unsigned rst_lpd = *(volatile unsigned *)(crl_map + CRL_APB_RST_LPD_TOP);

			if ((rst_lpd & CRL_APB_RST_LPD_TOP_RPU_R50_RESET_MASK) != 0) {
				printf("R5-0 under reset, run init sequence\n");
				if (zynqmp_init_rpu(fd) < 0)
					return -1;
			}

			/* Power down RPU 0, in case it was running */
			if (zynqmp_pm ("pm_force_powerdown 7\n") < 0)
				return -1;

			data_cb.devmem_fd = fd;
			data_cb.map = NULL;
			data_cb.len = 0;
			data_cb.vaddr = 0;

			const char *filename = argv[++i];
			if (elf_foreach_segment(filename, EM_ARM,
						elf_rpu_load_cb, &data_cb) < 0)
				return -1;

			/* Wakeup RPU 0 */
			if (zynqmp_pm ("pm_request_wakeup 7 1 0 1\n") < 0)
				return -1;
		}
		else if (strcmp(argv[i], "dbg0-dump") == 0) {
			zynqmp_dbg_dump(fd, R5_DBG_0_BASEADDR);
		}
		else if (strcmp(argv[i], "dbg0-halt") == 0) {
			zynqmp_dbg_halt(fd, R5_DBG_0_BASEADDR);
		}
		else if (strcmp(argv[i], "dbg0-restart") == 0) {
			zynqmp_dbg_restart(fd, R5_DBG_0_BASEADDR);
		}
		else if (strcmp(argv[i], "dbg0-reset") == 0) {
			zynqmp_dbg_reset(fd, R5_DBG_0_BASEADDR);
		}
		else if (strcmp(argv[i], "gdbserver") == 0) {
			struct dbg_port dbg;

			dbg.dap = zynqmp_map_dbg_r5(fd, R5_DBG_0_BASEADDR);
			if (dbg.dap == NULL)
				return -1;

			gdb_r5_server(&dbg, argc - i, argv + i);
			i = argc;
		}
		else
			printf ("unknown subcommand %s\n", argv[i]);
	}
	close(fd);
	return 0;
}

static int dbg_init_rpu(struct dbg_port *dbg, unsigned r5_addr, int *argc, char *argv[])
{
        /* Decode board options and open the board. */
        if (board_open(argc, argv) < 0)
		return -1;

	if (board->map == NULL) {
		fprintf(stderr, "gdbserver-rpu not support on board %s\n",
			board->name);
		return -1;
	}
	dbg->dap = board->map(board, r5_addr);
	if (dbg->dap == NULL)
		return -1;

	return 0;
}

static int do_gdbserver_rpu(unsigned r5_addr, int argc, char *argv[])
{
	struct dbg_port dbg;

	if (dbg_init_rpu(&dbg, r5_addr, &argc, argv) < 0)
		return -1;

	gdb_r5_server(&dbg, argc, argv);

        board->fini(board);
        return 0;
}

static int do_gdbserver_rpu0(int argc, char *argv[])
{
	return do_gdbserver_rpu(R5_DBG_0_BASEADDR, argc, argv);
}

static int do_gdbserver_rpu1(int argc, char *argv[])
{
	return do_gdbserver_rpu(R5_DBG_1_BASEADDR, argc, argv);
}

static int do_check_dbg_rpu(unsigned cpu_idx, int argc, char *argv[])
{
	struct dbg_port dbg;
	void *regs;
	unsigned dscr;
	unsigned timeout;
	unsigned lsr;
	unsigned prsr;
	unsigned r5_addr = R5_DBG_0_BASEADDR | (cpu_idx << 13);
	unsigned rst_lpd;
	unsigned val;

	/* Map CRL */
	if (dbg_init_rpu(&dbg, RPU_BASEADDR, &argc, argv) < 0)
		return -1;
	regs = dbg.dap;

	val = *(volatile unsigned *)(regs + RPU_RPU_GLBL_CNTL);
	printf ("RPU glbl_cntl:    0x%08x\n", val);
	val = *(volatile unsigned *)(regs + RPU_RPU_0_CFG);
	printf ("RPU rpu0_cfg:     0x%08x\n", val);
	val = *(volatile unsigned *)(regs + RPU_RPU_0_STATUS);
	printf ("RPU rpu0_status:  0x%08x\n", val);
	val = *(volatile unsigned *)(regs + RPU_RPU_1_CFG);
	printf ("RPU rpu1_cfg:     0x%08x\n", val);
	val = *(volatile unsigned *)(regs + RPU_RPU_1_STATUS);
	printf ("RPU rpu1_status:  0x%08x\n", val);

	{
		unsigned off = cpu_idx == 1 ? RPU_RPU_1_CFG : RPU_RPU_0_CFG;
		val = *(volatile unsigned *)(regs + off);
		if (!(val & 1)) {
			printf ("set /CPUHALT\n");
			*(volatile unsigned *)(regs + off) = 1;
		}
	}

	regs = board->map(board, CRL_APB_BASEADDR);
	if (regs == NULL)
		return -1;

	rst_lpd = *(volatile unsigned *)(regs + CRL_APB_RST_LPD_TOP);
	printf ("CRL_APB RST_LPD_TOP: %08x\n", rst_lpd);
	if (rst_lpd & (1 << cpu_idx)) {
	    printf ("CPU under reset\n");
	    rst_lpd &= ~(1 << cpu_idx);
	    *(volatile unsigned *)(regs + CRL_APB_RST_LPD_TOP) = rst_lpd;
	}

	val = *(volatile unsigned *)(regs + CRL_APB_CPU_R5_CTRL);
	printf ("CRL_APB cpu_r5_ctrl:  0x%08x\n", val);

	regs = board->map(board, r5_addr);
	if (regs == NULL)
		return -1;

	dbg.dap = regs;

	prsr = dbg_r5_read_dbgreg(regs, R5_DBG_PRSR);
	if (!(prsr & 1)) {
		printf ("CPU is powered-down (prsr: %02x)\n", prsr);
		return -1;
	}
	if (prsr & 4) {
		printf ("CPU is held in reset (prsr: %02x)\n", prsr);
		return -1;
	}

	dbg_r5_dump(regs);

	lsr = dbg_r5_read_dbgreg(regs, R5_DBG_LSR);
	if (lsr & 2) {
		printf ("unlock write access\n");
		dbg_r5_unlock_access(regs);
	}

	dscr = dbg_r5_read_dscr(regs);

	if (!(dscr & DSCR_HDBGen)) {
		printf ("Enable halting debug-mode\n");
		dbg_r5_write_dscr(regs, dscr | DSCR_HDBGen);
		dscr = dbg_r5_read_dscr(regs);
		if (!(dscr & DSCR_HDBGen)) {
			printf ("failed to set HDBGen\n");
			return -1;
		}
	}
	else
		printf ("Halting debug-mode already enabled\n");

	if (dscr & DSCR_HALTED)
		printf ("Target is already halted!\n");
	else {
		printf ("Request to halt...");
		fflush(stdout);

		/* Request to halt */
		dbg_r5_write_drcr(regs, 1);

		for (timeout = 10; timeout > 0; timeout--) {
			dscr = dbg_r5_read_dscr(regs);
			if (dscr & 1)
				break;
			usleep(1);
		}
		if (timeout == 0) {
			printf("cannot halt target!\n");

			if (dscr & DSCR_PipeAdv) {
				printf ("Clear PipeAdv\n");
				dbg_r5_write_drcr(regs, 8);
				dscr = dbg_r5_read_dscr(regs);
				if (dscr & DSCR_PipeAdv) {
					printf ("failed to clear PipeAdv\n");
					return -1;
				}
			}
			printf ("Try to cancel memory request\n");
			dbg_r5_write_drcr(regs, 0x11);

			dscr = dbg_r5_read_dscr(regs);
			if (!(dscr & 1)) {
				printf ("Failed to halt\n");
				return -1;
			}
		}
		printf ("target halted\n");
	}

	if (!(dscr & DSCR_InstrCompl)) {
		printf ("DSCR.InstrCompl not set: "
			"cpu is executing an ITR instruction\n");
		return -1;
	}

	if (dscr & DSCR_PipeAdv) {
		printf ("Clear PipeAdv\n");
		dbg_r5_write_drcr(regs, 8);
		dscr = dbg_r5_read_dscr(regs);
		if (dscr & DSCR_PipeAdv) {
			printf ("failed to clear PipeAdv\n");
			return -1;
		}
	}

	if (dscr & (DSCR_SDABORT_I | DSCR_ADABORT_I | DSCR_UND_I)) {
		printf("clear sticky exception bit\n");
		dbg_r5_write_drcr(regs, 4);
		dscr = dbg_r5_read_dscr(regs);
		if (dscr & (DSCR_SDABORT_I | DSCR_ADABORT_I | DSCR_UND_I)) {
			printf ("failed to clear sticky exception bit\n");
			return -1;
		}
	}

	if (dscr & DSCR_ITRen)
		printf ("ITRen is already set!\n");
	else {
		dbg_r5_write_dscr(regs, dscr | DSCR_ITRen);
		dscr = dbg_r5_read_dscr(regs);
		if (!(dscr & DSCR_ITRen)) {
			printf ("failed to set ITRen\n");
			return -1;
		}
	}

	printf ("Try to execute an instruction... ");
	fflush(stdout);
	dbg_r5_write_dbgreg(regs, R5_DBG_ITR, 0xe320f000); // nop
	for (timeout = 10; timeout > 0; timeout--) {
		dscr = dbg_r5_read_dscr(regs);
		if (dscr & DSCR_InstrCompl)
			break;
		usleep(1);
	}
	if (timeout == 0) {
		printf ("failed (timeout)\n");
		return -1;
	}
	if (!(dscr & DSCR_PipeAdv)) {
		printf ("PipeAdv is not set after instruction execution\n");
		return -1;
	}
	printf ("OK!\n");

	if (dscr & DSCR_TXfull) {
		printf ("TXfull is set\n");
		dbg_r5_read_dbgreg(regs, R5_DBG_DTRTXext);
		dscr = dbg_r5_read_dscr(regs);
		if (dscr & DSCR_TXfull) {
			printf ("Failed to clear TXfull\n");
			return -1;
		}
	}
	if (dscr & DSCR_RXfull) {
		unsigned r0;

		printf ("RXfull is set\n");
		r0 = dbg_r5_read_reg(regs, 0);
		dbg_r5_exec_dcc_to_reg(regs, 0);
		dscr = dbg_r5_read_dscr(regs);
		if (dscr & DSCR_RXfull) {
			printf("cannot cleat RXfull\n");
			return -1;
		}
		dbg_r5_write_reg(regs, 0, r0);
	}

	/* read BTCM region register
	   mrc p15, 0, r0, cr9, cr1, 0 */
	dbg_r5_exec_insn(regs, 0xee190f11);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("BTCM: %08x\n", val);

	/* read ATCM region register
	   mrc p15, 0, r0, cr9, cr1, 1 */
	dbg_r5_exec_insn(regs, 0xee190f31);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("ATCM: %08x\n", val);

	/* read SACR region register
	   mrc	15, 0, r0, cr15, cr0, {0} */
	dbg_r5_exec_insn(regs, 0xee1f0f10);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("SACR: %08x\n", val);

	/* Read DFSR Data Fault Status Register
	   mrc	15, 0, r0, cr5, cr0, {0} */
	dbg_r5_exec_insn(regs, 0xee150f10);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("DFSR: %08x\n", val);

	/* Read DFAR Data Fault Address Register
	   mrc	15, 0, r0, cr6, cr0, {0} */
	dbg_r5_exec_insn(regs, 0xee160f10);
	dbg_r5_exec_reg_to_dcc(regs, 0);
	val = dbg_r5_read_dcc(regs);
	printf ("DFAR: %08x\n", val);

        board->fini(board);
        return 0;
}

static void help_check_dbg(const char *cmd)
{
	fprintf(stderr, "usage: %s %s BOARD-OPTIONS [options]\n",
		progname, cmd);
}

static int do_check_dbg_rpu0(int argc, char *argv[])
{
	return do_check_dbg_rpu(0, argc, argv);
}

static int do_check_dbg_rpu1(int argc, char *argv[])
{
	return do_check_dbg_rpu(1, argc, argv);
}

static int do_clear_tcm_rpu(unsigned cpu_idx, int argc, char *argv[])
{
	/* Enable FPU and clear d0 */
	static const uint32_t r5_fpu_init [] = {
		0xee110f50, 	// mrc	15, 0, r0, cr1, cr0, {2}
		0xe380060f, 	// orr	r0, r0, #(0xf << 20)	@ 0xf00000
		0xee010f50,	// mcr	15, 0, r0, cr1, cr0, {2}
		0xf57ff06f,	// isb	sy
		0xeef83a10,	// vmrs	r3, fpexc
		0xe3831101,	// orr	r1, r3, #(1<<30)	@ 0x40000000
		0xeee81a10,	// vmsr	fpexc, r1
		0xe3a01000,	// mov	r1, #0
		0xec411b10,	// vmov	d0, r1, r1
		0
	};

	struct dbg_port dbg;
	void *regs;
	unsigned r5_addr = R5_DBG_0_BASEADDR | (cpu_idx << 13);
	unsigned i;

	/* Map CRL */
	if (dbg_init_rpu(&dbg, r5_addr, &argc, argv) < 0)
		return -1;
	regs = dbg.dap;

	if (dbg_r5_halt(regs) < 0)
		return -1;

	for (i = 0; r5_fpu_init[i]; i++)
		dbg_r5_exec_insn(regs, r5_fpu_init[i]);

	/* Clear ATCM */
	dbg_r5_write_reg(regs, 0, 0);
	for (i = 0; i < 0x10000 / 8; i++) {
		/* vstr    d0, [r0] */
		dbg_r5_exec_insn(regs, 0xed800b00);

		/* add     r0, r0, #8 */
		dbg_r5_exec_insn(regs, 0xe2800008);
	}

	/* Clear BTCM */
	dbg_r5_write_reg(regs, 0, 0x20000);
	for (i = 0; i < 0x10000 / 8; i++) {
		/* vstr    d0, [r0] */
		dbg_r5_exec_insn(regs, 0xed800b00);

		/* add     r0, r0, #8 */
		dbg_r5_exec_insn(regs, 0xe2800008);

		/* strd    r0, [r2], #8 */
		/* dbg_r5_exec_insn(regs, 0xe0c200f8); */
	}

	return 0;
}

static int do_clear_tcm_rpu0(int argc, char *argv[])
{
	return do_clear_tcm_rpu(0, argc, argv);
}

static int do_clear_tcm_rpu1(int argc, char *argv[])
{
	return do_clear_tcm_rpu(1, argc, argv);
}

static void help_clear_tcm(const char *cmd)
{
	fprintf(stderr, "usage: %s %s BOARD-OPTIONS [options]\n",
		progname, cmd);
}

static void do_rpu_dfault(struct dbg_port *dbg, int argc, char **argv)
{
	void *regs = dbg->dap;

	if (dbg_r5_halt(regs) < 0)
		return;

	dbg_r5_disp_dfault(regs);
}

static void do_rpu_tcm(struct dbg_port *dbg, int argc, char **argv)
{
	void *regs = dbg->dap;

	if (dbg_r5_halt(regs) < 0)
		return;

	dbg_r5_disp_tcm(regs);
}

static void do_rpu_mpu(struct dbg_port *dbg, int argc, char **argv)
{
	void *regs = dbg->dap;

	if (dbg_r5_halt(regs) < 0)
		return;

	dbg_r5_disp_mpu(regs);
}

static int parse_uns(const char *str, char *name, unsigned *res)
{
	char *e;

	*res = strtoul(str, &e, 0);
	if (*e != 0) {
		printf ("cannot parse %s '%s'\n", name, str);
		return -1;
	}
	return 0;
}

static void do_rpu_rd(struct dbg_port *dbg, int argc, char **argv)
{
	void *regs = dbg->dap;
	unsigned val, r0, r1;
	unsigned addr, len;

	if (argc < 2) {
		printf ("missing address\n");
		return;
	}

	if (parse_uns(argv[1], "address", &addr) < 0)
		return;

	if (argc > 2) {
		if (parse_uns(argv[2], "length", &len) < 0)
			return;
	}
	else
		len = 1;

	if (dbg_r5_halt(regs) < 0)
		return;

	r0 = dbg_r5_read_reg(dbg->dap, 0);
	r1 = dbg_r5_read_reg(dbg->dap, 1);

	dbg_r5_write_reg(dbg->dap, 0, addr);

	for (; len != 0; len--) {
		printf("[%08x]=", addr);

		/* ldrb r1,[r0],1 */
		dbg_r5_exec_insn(dbg->dap, 0xe4d01001);
		if (gdb_check_mem_fault(dbg, addr) < 0)
			printf("Fault\n");
		else {
			val = dbg_r5_read_reg(dbg->dap, 1);
			printf("%02x\n", val);
		}

		addr++;
	}
	dbg_r5_write_reg(dbg->dap, 0, r0);
	dbg_r5_write_reg(dbg->dap, 1, r1);
}

static void do_rpu_wr(struct dbg_port *dbg, int argc, char **argv)
{
	void *regs = dbg->dap;
	unsigned val, r0, r1;
	unsigned addr;

	if (argc < 3) {
		printf ("missing address value\n");
		return;
	}

	if (parse_uns(argv[1], "address", &addr) < 0)
		return;

	if (parse_uns(argv[2], "value", &val) < 0)
		return;

	if (dbg_r5_halt(regs) < 0)
		return;

	r0 = dbg_r5_read_reg(dbg->dap, 0);
	r1 = dbg_r5_read_reg(dbg->dap, 1);

	dbg_r5_write_reg(dbg->dap, 0, addr);
	dbg_r5_write_reg(dbg->dap, 1, val);

	/* str r1,[r0],#4 */
	dbg_r5_exec_insn(dbg->dap, 0xe4801004);

	if (gdb_check_mem_fault(dbg, addr) < 0)
		printf("Fault\n");

	dbg_r5_write_reg(dbg->dap, 0, r0);
	dbg_r5_write_reg(dbg->dap, 1, r1);
}

struct tool_rpu {
	const char *name;
	const char *short_help;
	void (*run)(struct dbg_port *dbg, int argc, char **argv);
};

static struct tool_rpu tools_rpu[] = {
  {
	  "dfault",
	  "disp data fault registers",
	  do_rpu_dfault
  },
  {
	  "tcm",
	  "disp TCM registers",
	  do_rpu_tcm
  },
  {
	  "mpu",
	  "disp MPU registers",
	  do_rpu_mpu
  },
  {
	  "rd",
	  "ADDR [LEN]: read memory",
	  do_rpu_rd
  },
  {
	  "wr",
	  "ADDR VAL: write memory",
	  do_rpu_wr
  },
  {
	  NULL, NULL, NULL
  }
};

static int do_rpu(unsigned cpu_idx, int argc, char *argv[])
{
	struct dbg_port dbg;
	const struct tool_rpu *cmd;
	unsigned r5_addr = R5_DBG_0_BASEADDR | (cpu_idx << 13);
	unsigned i;

	if (dbg_init_rpu(&dbg, r5_addr, &argc, argv) < 0)
		return -1;

	if (argc == 1) {
		printf("%s %s: missing sub-command\n", progname, argv[0]);
		for (i = 0; tools_rpu[i].name; i++)
			printf ("%-8s  %s\n", tools_rpu[i].name, tools_rpu[i].short_help);
		return -1;
	}

	cmd = NULL;
	for (i = 0; tools_rpu[i].name; i++)
		if (!strcmp(tools_rpu[i].name, argv[1])) {
			cmd = &tools_rpu[i];
			break;
		}
	if (cmd == NULL) {
		printf ("%s %s: unknown command '%s'\n", progname, argv[0], argv[1]);
		return -1;
	}


	remove_arg1(&argc, argv);

	(*cmd->run)(&dbg, argc, argv);

	board->fini(board);

	return 0;
}

static int do_rpu0(int argc, char *argv[])
{
	return do_rpu(0, argc, argv);
}

static int do_rpu1(int argc, char *argv[])
{
	return do_rpu(1, argc, argv);
}

static void help_rpu(const char *cmd)
{
	fprintf(stderr, "usage: %s %s SUB-CMD BOARD-OPTIONS [options]\n",
		progname, cmd);
}

static int do_rd(int argc, char *argv[])
{
	unsigned pg = getpagesize();
	unsigned *ptr;
	unsigned addr = 0xff040000;

        /* Decode board options and open the board. */
        if (board_open(&argc, argv) < 0)
		return -1;

	if (argc > 2 && !strcmp(argv[1], "-a")) {
		addr = strtoul(argv[2], NULL, 0);
	}

	if (board->map == NULL) {
		fprintf(stderr, "gdbserver-rpu not support on board %s\n",
			board->name);
		return -1;
	}
	ptr = board->map(board, addr & ~(pg - 1));
	if (ptr == NULL)
		return -1;

	ptr += (addr & (pg - 1)) >> 2;
	printf("[%08x] = %08x\n", addr, *ptr);

        board->fini(board);
        return 0;
}

static void help_rd(const char *cmd)
{
        printf("usage: %s rd BOARD-OPTIONS\n", progname);
}

static const struct tool_base tool_help = {
        "help",
        "display list of commands (this help), or help for a command",
        do_help,
        NULL
};

static const struct tool_base tool_board = {
        "board",
        "display list of supported boards, or help for a board",
        do_board,
        NULL
};

static const struct tool_base tool_version = {
        "version",
        "display tool version",
        do_version,
        NULL
};

static const struct tool_base tool_load = {
        "load",
        "load wrpc firmware and restart",
        do_load,
        help_load
};

static const struct tool_base tool_vuart = {
        "vuart",
        "virtual uart, connect to wrpc cli",
        do_vuart,
        help_vuart
};

#ifndef SUPPORT_WRS
static const struct tool_base tool_info = {
        "info",
        "display wrpc info and check board",
        do_info,
        help_info
};

static const struct tool_base tool_mac = {
        "mac",
        "display wrpc mac address",
        do_mac,
        help_mac
};
#endif /* !defined(SUPPORT_WRS) */

static const struct tool_base tool_spll_recorder = {
        "spll-recorder",
        "SoftPLL log recorder",
        do_spll_recorder,
        help_spll_recorder
};

static const struct tool_base tool_spll_display = {
        "spll-display",
        "Display spll-recorder binary output",
        do_spll_display,
        help_spll_display
};

static const struct tool_base tool_gdbserver = {
        "gdbserver",
        "risc-v gdb-sever",
        do_gdbserver,
        help_gdbserver
};

static const struct tool_base tool_gdbserver_rpu0 = {
        "gdbserver-rpu0",
        "cortex-r5 gdb-server for ZynqUS+ RPU0",
        do_gdbserver_rpu0,
        help_gdbserver
};

static const struct tool_base tool_gdbserver_rpu1 = {
        "gdbserver-rpu1",
        "cortex-r5 gdb-server for ZynqUS+ RPU1",
        do_gdbserver_rpu1,
        help_gdbserver
};

static const struct tool_base tool_check_dbg_rpu0 = {
        "checkdbg-rpu0",
        "Check debug port of cortex-r5 ZynqUS+ RPU0",
        do_check_dbg_rpu0,
        help_check_dbg
};

static const struct tool_base tool_check_dbg_rpu1 = {
        "checkdbg-rpu1",
        "Check debug port of cortex-r5 ZynqUS+ RPU1",
        do_check_dbg_rpu1,
        help_check_dbg
};

static const struct tool_base tool_clear_tcm_rpu0 = {
        "clear-tcm-rpu0",
        "Check ATCM and BTCM of cortex-r5 ZynqUS+ RPU0",
        do_clear_tcm_rpu0,
        help_clear_tcm
};

static const struct tool_base tool_clear_tcm_rpu1 = {
        "clear-tcm-rpu1",
        "Check ATCM and BTCM of cortex-r5 ZynqUS+ RPU1",
        do_clear_tcm_rpu1,
        help_clear_tcm
};

static const struct tool_base tool_rpu0 = {
        "rpu0",
        "Sub commands for cortex-r5 ZynqUS+ RPU0",
        do_rpu0,
        help_rpu
};

static const struct tool_base tool_rpu1 = {
        "rpu1",
        "Sub commands for cortex-r5 ZynqUS+ RPU1",
        do_rpu1,
        help_rpu
};

static const struct tool_base tool_rd = {
        "rd",
        "Read a word on the local bus",
        do_rd,
        help_rd
};

static const struct tool_base tool_zynqmp_rpu = {
        "zynqmp-rpu",
        "display RPU status on zynqmp",
        do_zynqmp_rpu,
        help_zynqmp_rpu,
};

#ifndef SUPPORT_WRS
static const struct tool_base tool_wdiags = {
        "wdiags",
        "WR diags dumper",
        do_wdiags,
        help_wdiags
};

static const struct tool_base tool_aux_logger = {
        "aux-logger",
        "display wdiag AUX0 value for logging",
        do_aux_logger,
        help_aux_logger
};

#endif /* !defined(SUPPORT_WRS) */

static const struct tool_base *tools[] = {
	&tool_help,
	&tool_version,
        &tool_board,
	&tool_load,
	&tool_vuart,
#ifndef SUPPORT_WRS
	&tool_info,
	&tool_mac,
#endif /* !defined(SUPPORT_WRS) */
	&tool_spll_recorder,
	&tool_spll_display,
	&tool_gdbserver,
	&tool_gdbserver_rpu0,
	&tool_gdbserver_rpu1,
	&tool_rd,
	&tool_check_dbg_rpu0,
	&tool_check_dbg_rpu1,
	&tool_clear_tcm_rpu0,
	&tool_clear_tcm_rpu1,
	&tool_rpu0,
	&tool_rpu1,
	&tool_zynqmp_rpu,
#ifndef SUPPORT_WRS
	&tool_wdiags,
        &tool_aux_logger,
#endif /* !defined(SUPPORT_WRS) */
	NULL
};

int main(int argc, char *argv[])
{
	const char *progbase;
	const char *toolname;
	const struct tool_base *tool;

	/* Extract tool from argv[0].  */
	progname = argv[0];
	progbase = get_basename(progname);
	if (argc > 1 && argv[1][0] != '-') {
		/* Extract command. */
		toolname = argv[1];

		remove_arg1(&argc, argv);
	}
	else if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		do_version(0, NULL);
		return 0;
	}
	else if (memcmp(progbase, "wrpc-", 5) == 0) {
		toolname = progbase + 5;
	}
	else {
		fprintf (stderr, "no tool name, try %s help\n", progname);
		return 1;
	}

	/* Find tool.  */
	for (unsigned i = 0; (tool = tools[i]); i++) {
		if (strcmp (tool->name, toolname) == 0)
			break;
	}
	if (tool == NULL) {
		fprintf(stderr, "tool '%s' is unknown, try %s help\n",
			toolname, progname);
		return 1;
	}

	if (argc > 1)
		if (strcmp (argv[1], "-h") == 0
		    || strcmp (argv[1], "--help") == 0) {
			tool->help();
			return 0;
		}

	return tool->run(argc, argv);
}
