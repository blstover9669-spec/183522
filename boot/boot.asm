; NovaOS - Multiboot 1 entry point
;
; GRUB (or QEMU -kernel) loads us in 32-bit protected mode with paging off.
; EAX = 0x2BADB002, EBX = physical address of the multiboot info structure.

MB_MAGIC      equ 0x1BADB002
MB_ALIGN      equ 1 << 0          ; page-align modules
MB_MEMINFO    equ 1 << 1          ; give us a memory map
MB_VIDEO      equ 1 << 2          ; give us a linear framebuffer
MB_FLAGS      equ MB_ALIGN | MB_MEMINFO | MB_VIDEO
MB_CHECKSUM   equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    ; a.out kludge fields (unused for ELF but must be present before video fields)
    dd 0, 0, 0, 0, 0
    ; video mode request: linear graphics, 1024x768x32
    dd 0            ; mode_type 0 = linear framebuffer
    dd 1024         ; width
    dd 768          ; height
    dd 32           ; depth

section .bss
align 16
global kernel_stack_bottom, kernel_stack_top
kernel_stack_bottom:
    resb 1024 * 1024            ; 1 MiB kernel stack (apps run on it too)
kernel_stack_top:

section .text.entry
global _start
extern kmain
extern __bss_start, __bss_end

_start:
    cli
    cld
    mov esp, kernel_stack_top

    ; Save multiboot registers across the BSS clear.
    ; (kept in registers: the stack itself lives in .bss and is about to be zeroed)
    mov esi, eax
    mov edx, ebx

    ; Zero .bss.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    shr ecx, 2
    xor eax, eax
    rep stosd

    mov esp, kernel_stack_top
    xor ebp, ebp                ; terminate stack traces
    push edx                    ; multiboot info pointer
    push esi                    ; multiboot magic
    call kmain

.hang:
    cli
    hlt
    jmp .hang
