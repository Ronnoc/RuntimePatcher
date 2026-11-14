// SPDX-License-Identifier: MIT
// NRO module start: word 0 = branch, word 1 = MOD0 offset, 0x10..0x80 reserved for the NRO0 header
// (filled in by `nso_tool.py elf2nro`). Shares nx/link.ld with the subsdk9 module.
    .section .text.crt0, "ax", %progbits
    .global __module_start
__module_start:
    b       1f
    .word   __nx_mod0 - __module_start
    .fill   0x78, 1, 0
1:
    ret

    .section .rodata.mod0, "a", %progbits
    .balign 4
__nx_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC              - __nx_mod0
    .word   __bss_start__         - __nx_mod0
    .word   __bss_end__           - __nx_mod0
    .word   __eh_frame_hdr_start  - __nx_mod0
    .word   __eh_frame_hdr_end    - __nx_mod0
    .word   __nx_module_runtime   - __nx_mod0

    .section .bss.module_runtime, "aw", %nobits
    .balign 8
__nx_module_runtime:
    .space  0x200
