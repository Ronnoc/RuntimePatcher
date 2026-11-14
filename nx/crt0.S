// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — subsdk9 module header (MOD0) for Horizon rtld.
//
// rtld discovers every loaded NSO by reading the u32 at module_base+4 (offset of MOD0),
// relocates it via the MOD0 -> _DYNAMIC pointer and resolves its imports/exports.

    .section .text.crt0, "ax", %progbits
    .global __module_start
    .type   __module_start, %function
__module_start:
    b       1f
    .word   __nx_mod0 - __module_start
#if FEFW_NX_NRO
    .fill   0x78, 1, 0          // 0x10..0x80: NRO0 header, written by `nso_tool.py elf2nro`
#endif
1:
    ret

    .section .rodata.mod0, "a", %progbits
    .balign 4
    .global __nx_mod0
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
    .global __nx_module_runtime
__nx_module_runtime:
    .space  0x200
