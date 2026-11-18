/* the board firmware: program, data, heap, stack in the first 1 MB of the SRAM (term_soc.v) */
OUTPUT_ARCH("riscv")
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 1M }
STACK_SIZE = 32K;
SECTIONS
{
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > RAM
    .rodata : ALIGN(8) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > RAM
    .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE_HIDDEN(__init_array_end = .); } > RAM
    .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) PROVIDE_HIDDEN(__fini_array_end = .); } > RAM
    .data : ALIGN(8) { *(.data .data.*) __global_pointer$ = . + 0x800; *(.sdata .sdata.*) . = ALIGN(8); } > RAM
    .bss (NOLOAD) : ALIGN(8) { __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) . = ALIGN(8); __bss_end = .; } > RAM
    __heap_start = .;
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
    __heap_end = __stack_top - STACK_SIZE;
}
