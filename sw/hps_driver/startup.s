@ Minimal Cortex-A9 entry point. Assumes U-Boot's `go <addr>` already left the
@ core in a sane state (SVC mode, caches/MMU as U-Boot set them) -- this just
@ establishes our own stack (defined in linker.ld) and zeroes .bss before
@ handing off to main(), rather than relying on whatever U-Boot's own stack
@ happened to be.
.section .text.startup
.global _start
_start:
    ldr sp, =__stack_top

    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
bss_clear_loop:
    cmp r0, r1
    bge bss_clear_done
    str r2, [r0], #4
    b bss_clear_loop
bss_clear_done:

    bl main
hang:
    b hang
