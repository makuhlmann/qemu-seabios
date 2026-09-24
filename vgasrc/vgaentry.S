// Rom layout and bios assembler to C interface.
//
// Copyright (C) 2009-2013  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.


#include "asm-offsets.h" // BREGS_*
#include "config.h" // CONFIG_*
#include "entryfuncs.S" // ENTRY_*


/****************************************************************
 * Rom Header
 ****************************************************************/

        .section .rom.header
        .code16
        .global _rom_header, _rom_header_size, _rom_header_checksum
_rom_header:
        .word 0xaa55
_rom_header_size:
        .byte 0
_rom_header_entry:
        jmp _optionrom_entry
_rom_header_checksum:
        .byte 0
_rom_header_other:
        .space 17
_rom_header_pcidata:
#if CONFIG_VGA_PCI == 1
        .word rom_pci_data
#else
        .word 0
#endif
_rom_header_pnpdata:
        .word 0
_rom_header_other2:
        .word 0
_rom_header_signature:
        .asciz "IBM"

#if CONFIG_VGA_ATI
        .org 0x48
        .global _rom_header_ati_table_anchor
_rom_header_ati_table_anchor:
        .word 0
        .balign 16
#if !CONFIG_VGA_ATI_TABLES
        // Room for the ATI tables that the machine adds to the image
        .space 0x100
#endif
#endif

#if CONFIG_VGA_MACH64
// ATI BIOS tables of the mach64 native drivers: layouts from the RAGE PRO
// Programmer's Guide (PRG-215R3) App. A.14 and A.35, Tables A-11, A-13 and
// A-14.  The Windows miniports read them from the 0xC0000 shadow, ati2mpad
// also from the BAR6 image.
        .org 0x30
        .ascii " 761295520"
        .org 0x48
        .word mach_rom_table
        .word 0x1002, 0x4752    // RAGE XL straps: subsystem vendor and ID
        .byte 0                 // chg_id
        .org 0x78
        .word mach_dit

// Reference 29.50 MHz and divider 0x36 match the PLL seeds of the model
// (hw/display/mach64.c mach64_vga_reset); MCLK is what those seeds give,
// MCLK_FB_DIV 0x97 * 2 / 0x36 divided by 3 (PLL_GEN_CNTL MCLK_SRC_SEL).
#define MACH_REF_FREQ   2950
#define MACH_REF_DIV    0x36
#define MACH_MCLK       5499
#define MACH_PCLK_MIN   2000
#define MACH_PCLK_MAX   23000

        .org 0x80
        .word mach_rom_table_end - mach_rom_table
        .global mach_rom_table
mach_rom_table:
        .byte 0, 0xa0           // type, extended function code
        .byte 1, 0              // revision
        .word 0                 // no sparse I/O
        .word 0, 0, 0, 0, 0
        .word mach_clock_table
        .word mach_signon
        .word mach_signon
        .global mach_rom_pciloc
mach_rom_pciloc:
        .word 0                 // bus << 8 | devfn, set at POST
        .word 0
        .global mach_rom_iobase
mach_rom_iobase:
        .word 0                 // block I/O base, set at POST
        .word 0, 0, 0
        .word 0, 0              // no coprocessor-only INT 10h
        .word 0
        .global mach_rom_mmbase
mach_rom_mmbase:
        .long 0                 // register block, set at POST
        .long 0
        .word 0xffff            // no TV-out
        .word 0xffff, 0, 0xffff
        .word 0xc000            // runtime segment
        .word 0
        .word 0x1002, 0x1002, 0x4752, 0x4752
        .word mach_signon
        .word 0                 // no multimedia table
        .word mach_hwinfo
mach_rom_table_end:

        .word 0, 0, 0           // -6: no mode-limit table
        .word mach_pclk_table
        .global mach_clock_table
mach_clock_table:
        .byte 4                 // internal PLL
        .byte 0
        .word MACH_PCLK_MIN, MACH_PCLK_MAX
        .byte 3, 0xff           // extended modes program clock entry 3
        .word MACH_REF_FREQ, MACH_REF_DIV
        .word 0
        .word MACH_MCLK, MACH_MCLK, MACH_MCLK, MACH_MCLK
        .byte 0, 0
        .word 0, 0
        .word 0xff

// VGA clocks, then VESA DMT pixel clocks, in 10 kHz
        .global mach_pclk_table
mach_pclk_table:
        .word 2518, 2832, 3150, 3600, 4000, 4490, 5000, 5664
        .word 6500, 7500, 7875, 9450, 10800, 13500, 15750, 0

mach_hwinfo:
        .ascii "$ATI"
        .byte 2, 10
        .byte 15                // I2C type: no AMC, so no multimedia probe
        .byte 0, 0, 0

// Revision 3 keeps ati2mpad's SetupScriptInitEntry off its script pointers.
mach_dit:
        .ascii "$XLT"
        .byte 3, mach_dit_end - mach_dit
        .word 0, 0              // no internal CRT table
        .word 0                 // no panel
        .word 0                 // no TV standard table
        .word MACH_REF_DIV, MACH_PCLK_MIN, MACH_PCLK_MAX, MACH_REF_FREQ
        .word mach_hwinfo
        .word 0, 0
        .space 12
mach_dit_end:

mach_signon:
        .asciz "SeaVGABIOS MACH64 RAGE XL"
#endif

/****************************************************************
 * Entry points
 ****************************************************************/

        // This macro implements a call while avoiding instructions
        // that old versions of x86emu have problems with.
        .macro VGA_CALLL cfunc
#if CONFIG_VGA_FIXUP_ASM
        pushw %ax
        callw \cfunc
#else
        calll \cfunc
#endif
        .endm

        // This macro is the same as ENTRY_ARG except VGA_CALLL is used.
        .macro ENTRY_ARG_VGA cfunc
        cli
        cld
        PUSHBREGS
        movw %ss, %ax           // Move %ss to %ds
        movw %ax, %ds
        // %esi, not %ebx: VGA_X86COMPAT takes %bx for its rewrites
        movl %esp, %esi         // Backup %esp, then zero high bits
        movzwl %sp, %esp
        movl %esp, %eax         // First arg is pointer to struct bregs
        VGA_CALLL \cfunc
        movl %esi, %esp         // Restore %esp (including high bits)
        POPBREGS
        .endm

        DECLFUNC entry_104f05
entry_104f05:
        ENTRY_ARG_VGA vbe_104f05
        lretw

        DECLFUNC _optionrom_entry
_optionrom_entry:
        ENTRY_ARG_VGA vga_post
        lretw

        DECLFUNC entry_10
entry_10:
        ENTRY_ARG_VGA handle_10
        iretw

#if CONFIG_VGA_ALLOCATE_EXTRA_STACK
#define VGA_CUSTOM_BDA_FLAGS 0xb9
#define BF_EXTRA_STACK 0x40

        // Entry point using extra stack
        DECLFUNC entry_10_extrastack
entry_10_extrastack:
        cli
        cld
        pushw %ds
        pushl %eax

        movw $SEG_BDA, %ax      // Check if extra stack is enabled
        movw %ax, %ds
        testb $BF_EXTRA_STACK, VGA_CUSTOM_BDA_FLAGS
        jz 1f

        movw %cs:ExtraStackSeg, %ds // Set %ds:%eax to space on ExtraStack
        movl $(CONFIG_VGA_EXTRA_STACK_SIZE-PUSHBREGS_size-16), %eax
        SAVEBREGS_POP_DSEAX     // Save registers on extra stack
        movl %esp, PUSHBREGS_size+8(%eax)
        movw %ss, PUSHBREGS_size+12(%eax)
        popl BREGS_code(%eax)
        popw BREGS_flags(%eax)

        movw %ds, %dx           // Setup %ss/%esp and call function
        movw %dx, %ss
        movl %eax, %esp
        VGA_CALLL handle_10

        movl %esp, %eax         // Restore registers and return
        movw PUSHBREGS_size+12(%eax), %ss
        movl PUSHBREGS_size+8(%eax), %esp
        popl %edx
        popw %dx
        pushw BREGS_flags(%eax)
        pushl BREGS_code(%eax)
        RESTOREBREGS_DSEAX
        iretw

1:      // Use regular entry point if the extra stack is disabled
        popl %eax
        popw %ds
        jmp entry_10
#endif

        // Timer irq handling
        DECLFUNC entry_timer_hook
entry_timer_hook:
        ENTRY handle_timer_hook
        ljmpw *%cs:Timer_Hook_Resume

#if CONFIG_VGA_ALLOCATE_EXTRA_STACK
        // Timer irq handling on extra stack
        DECLFUNC entry_timer_hook_extrastack
entry_timer_hook_extrastack:
        cli
        cld
        pushw %ds               // Set %ds:%eax to space on ExtraStack
        pushl %eax
        movw %cs:ExtraStackSeg, %ds
        movl $(CONFIG_VGA_EXTRA_STACK_SIZE-PUSHBREGS_size-8), %eax
        SAVEBREGS_POP_DSEAX
        movl %esp, PUSHBREGS_size(%eax)
        movw %ss, PUSHBREGS_size+4(%eax)

        movw %ds, %dx           // Setup %ss/%esp and call function
        movw %dx, %ss
        movl %eax, %esp
        calll handle_timer_hook

        movl %esp, %eax         // Restore registers and return
        movw PUSHBREGS_size+4(%eax), %ss
        movl PUSHBREGS_size(%eax), %esp
        RESTOREBREGS_DSEAX
        ljmpw *%cs:Timer_Hook_Resume
#endif
