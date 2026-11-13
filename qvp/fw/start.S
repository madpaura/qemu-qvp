/*
 * qvp stub firmware: vectors and reset for one Cortex-R core.
 * Each core runs from its own ITCM at 0x0, stacks in its own DTCM.
 */
    .syntax unified
    .arm

    .section .vectors, "ax"
    .global _start
_start:
    b       reset
    b       hang            @ undefined
    b       hang            @ svc
    b       hang            @ prefetch abort
    b       hang            @ data abort
    b       hang            @ reserved
    b       irq_entry
    b       hang            @ fiq

    .text
reset:
    cps     #0x12           @ IRQ mode
    ldr     sp, =__irq_stack_top
    cps     #0x13           @ SVC mode
    ldr     sp, =__svc_stack_top

    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    bl      main
    .global hang
hang:
    wfi
    b       hang

irq_entry:
    sub     lr, lr, #4
    push    {r0-r3, r12, lr}
    bl      irq_handler
    ldm     sp!, {r0-r3, r12, pc}^
