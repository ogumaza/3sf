/* SPDX-License-Identifier: MIT */

/* 3SF guest driver: loaded at 0x0E000000 in the emulated application's address space.
 * The entry branch is first and the parameter block is at a fixed address after it. driver_end
 * marks the end of the bss, which has to fit in the driver's page as well. */
ENTRY(driver_entry)
SECTIONS {
    . = 0x0E000000;
    .entry : { KEEP(*(.text.entry)) }
    . = 0x0E000010;
    .params : { KEEP(*(.params)) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    driver_end = .;
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
