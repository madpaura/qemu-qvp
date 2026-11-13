/* qvp stub firmware: every core links to its own local TCM view */
ENTRY(_start)

MEMORY
{
    ITCM (rx)  : ORIGIN = 0x00000000, LENGTH = 128K   /* fcore0 has 128K */
    DTCM (rw)  : ORIGIN = 0x00040000, LENGTH = 256K
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
    } > ITCM

    .data : {
        *(.data*)
    } > DTCM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DTCM

    .stack (NOLOAD) : {
        . = ALIGN(8);
        . += 0x1000;
        __svc_stack_top = .;
        . += 0x800;
        __irq_stack_top = .;
    } > DTCM
}
