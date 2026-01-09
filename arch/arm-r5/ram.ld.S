ENTRY(_reset_vector)

MEMORY
{
  atcm : ORIGIN = 0x00000000, LENGTH = 0x10000
  btcm : ORIGIN = 0x00020000, LENGTH = 0x10000
/*  ocm : ORIGIN = 0xfffc0000, LENGTH = 0x10000 */
}

_STACK_SIZE = CONFIG_STACKSIZE;
_IRQ_STACK_SIZE = 0x200;

SECTIONS
{
    .text : { KEEP(*(.vector_table)) *(.text) *(.text.*) } > atcm
    . = ALIGN(8);
    .rodata : { *(.rodata) *(.rodata.*) } > btcm
    . = ALIGN(8);
    .data : { *(.data) *(.data.*) } > btcm
    
    .bss : {
        . = ALIGN(8);
    	__bss_start__ = .;
	*(.bss) *(.bss.*)
        __bss_end__ = .;
    } > btcm

    .stack : {
       . = ALIGN(16);
       _endram = .;
       _stack_end = .;
       . += _STACK_SIZE;
       __stack = .;
       . = ALIGN(16);
       _irq_stack_end = .;
       . += _IRQ_STACK_SIZE;
       __irq_stack = .;
     } > btcm
     
}

