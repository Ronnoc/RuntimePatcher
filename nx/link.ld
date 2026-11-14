/* SPDX-License-Identifier: MIT
 * FEFW RuntimePatcher — subsdk9 linker script (3 page-aligned PT_LOADs: RX / R / RW). */
OUTPUT_ARCH(aarch64)
ENTRY(__module_start)

PHDRS
{
    text    PT_LOAD FLAGS(5);
    rodata  PT_LOAD FLAGS(4);
    data    PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC;
}

SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.crt0))
        *(.text .text.*)
        *(.plt .plt.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        KEEP(*(.rodata.mod0))
        *(.rodata .rodata.*)
    } :rodata
    .dynsym   : { *(.dynsym) }   :rodata
    .dynstr   : { *(.dynstr) }   :rodata
    .hash     : { *(.hash) }     :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .rela.dyn : { *(.rela.dyn) *(.rela.*) } :rodata
    .rela.plt : { *(.rela.plt) } :rodata
    .eh_frame_hdr : {
        __eh_frame_hdr_start = .;
        *(.eh_frame_hdr)
        __eh_frame_hdr_end = .;
    } :rodata
    .eh_frame : { KEEP(*(.eh_frame)) } :rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data .data.*)
    } :data
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :data
    .fini_array : { KEEP(*(.fini_array .fini_array.*)) } :data
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .dynamic : { *(.dynamic) } :data :dynamic
    .got     : { *(.got) }     :data
    .got.plt : { *(.got.plt) } :data

    .bss (NOLOAD) : {
        __bss_start__ = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end__ = .;
    } :data

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.interp) }
}
