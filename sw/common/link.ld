/* Pulse SoC linker script: code in IMEM, everything else in DMEM. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    IMEM (rx)  : ORIGIN = 0x00000000, LENGTH = 64K
    DMEM (rw)  : ORIGIN = 0x10000000, LENGTH = 256K
}

STACK_SIZE = DEFINED(STACK_SIZE) ? STACK_SIZE : 8K;

SECTIONS
{
    .text : {
        KEEP(*(.text.init))
        *(.text .text.*)
    } > IMEM

    .rodata : ALIGN(4) {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > DMEM

    .data : ALIGN(4) {
        *(.data .data.* .tohost)
        . = ALIGN(4);
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
    } > DMEM

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DMEM

    _end = .;
    _stack_top = ORIGIN(DMEM) + LENGTH(DMEM);
    ASSERT(_end + STACK_SIZE <= _stack_top, "DMEM overflow: data + stack do not fit")

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
