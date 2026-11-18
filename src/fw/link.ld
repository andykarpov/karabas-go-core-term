/* Ф0 bench: program, data, heap and stack in the SDRAM (16 MB at 0x10000000); the input stream of the
   bench at IN_BASE 0x11000000 (loaded by the ISS); MMIO at 0x80000000. */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY { RAM (rwx) : ORIGIN = 0x10000000, LENGTH = 16M }

STACK_SIZE = 64K;

SECTIONS
{
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > RAM
    .rodata : ALIGN(8) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > RAM
    .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE_HIDDEN(__init_array_end = .); } > RAM
    .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) PROVIDE_HIDDEN(__fini_array_end = .); } > RAM
    .data : ALIGN(8) {
        *(.data .data.*)
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        . = ALIGN(8);
    } > RAM
    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM
    __heap_start = .;
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
    __heap_end = __stack_top - STACK_SIZE;
}
