/* Ф0 bench firmware start (from core-doom / core-pico8): stack at the top of the region, clear .bss, ctors, main, exit via MMIO. */
    .section .text.start
    .global _start
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top
    la      a0, __bss_start
    la      a1, __bss_end
1:  bgeu    a0, a1, 2f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       1b
2:  call    __libc_init_array
    li      a0, 0
    li      a1, 0
    call    main
    call    exit
3:  j       3b
