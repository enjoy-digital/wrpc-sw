/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2026 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
ENTRY(_reset_vector)

MEMORY
{
  atcm : ORIGIN = 0x00000000, LENGTH = 0x10000
  btcm : ORIGIN = 0x00020000, LENGTH = 0x10000
/*  ocm : ORIGIN = 0xfffc0000, LENGTH = 0x10000 */
}

ATCM_LOADADDR = 0xffe00000 + 0x90000; /* RPU0: 0xffe00000, RPU1: + 0x90000 */
BTCM_LOADADDR = 0xffe20000 + 0x90000; /* RPU0: 0xffe20000, RPU1: + 0x90000 */
	
_STACK_SIZE = CONFIG_STACKSIZE;
_IRQ_STACK_SIZE = 0x200;

SECTIONS
{
    .text : {
	KEEP(*(.vector_table))
	*(.text)
	*(.text.*)
        . = ALIGN(8);
    } > atcm
    .rodata : {
        *(.rodata)
	*(.rodata.*)
        . = ALIGN(8);
    } > btcm
    .data : {
        *(.data)
	*(.data.*)
        . = ALIGN(8);
    } > btcm
    
    .bss : {
    	__bss_start__ = .;
	*(.bss) *(.bss.*)
        . = ALIGN(8);
        __bss_end__ = .;
    } > btcm

    .stack : {
       . = ALIGN(16);
       _endram = .;
       . += _STACK_SIZE;
       __stack = .;
       . = ALIGN(16);
       . += _IRQ_STACK_SIZE;
       __irq_stack = .;
       . = ALIGN(8);

       __stacks_end__ = .;

     } > btcm
     
}

