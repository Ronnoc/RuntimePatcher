/* FEFW NRO bootstrap: one flat RX blob at FEFW_BOOT_VA inside main (main-relative VA). */
OUTPUT_ARCH(aarch64)
ENTRY(fefw_boot_stub_0)
SECTIONS
{
    . = FEFW_BOOT_VA;
    .boot : {
        KEEP(*(.text.boot_stubs))
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    /DISCARD/ : { *(.data*) *(.bss*) *(.comment) *(.note*) *(.eh_frame*) }
}
