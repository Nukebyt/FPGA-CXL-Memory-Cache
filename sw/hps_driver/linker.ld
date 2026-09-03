/* Load address 0x00100000 (1MB into HPS DDR3) -- a conventional, safe
 * bare-metal load address for SoCFPGA boards, clear of low memory U-Boot
 * itself may still be using. Adjust at load time (`loadaddr` in U-Boot)
 * if this ever needs to change; nothing here depends on the specific
 * value beyond ORIGIN below. */
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00100000, LENGTH = 0x00100000
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text : {
        KEEP(*(.text.startup))
        *(.text*)
    } > RAM

    .rodata : { *(.rodata*) } > RAM

    .data : { *(.data*) } > RAM

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(16);
    . = . + 0x4000; /* 16KB stack */
    __stack_top = .;
}
