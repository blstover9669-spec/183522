; NovaOS disk boot loader - stage 2
;
; Loaded by stage 1 at 0000:8000 with DL = boot drive; the install header is at
; 0000:7E00. Steps:
;   1. enable A20 (BIOS, keyboard controller, port 0x92) and verify it
;   2. load the flat kernel image (header.kernel_lba / kernel_sectors) through a
;      bounce buffer at 1000:0000 and copy it above 1 MiB in unreal mode
;   3. collect the E820 memory map, INT 12h / E801 memory sizes
;   4. pick a VBE linear framebuffer mode: the size given by "res=WxH" on the
;      command line if any, then 1024x768x32, 800x600x32, 1024x768x24
;      (4F00 / 4F01 / 4F02); if none works the kernel falls back to BGA
;   5. build a Multiboot 1 information structure (mem, boot device, cmdline,
;      mmap, loader name, VBE + framebuffer fields)
;   6. switch to 32-bit protected mode and jump to the kernel entry with
;      EAX = 0x2BADB002 and EBX = the info structure, exactly like GRUB
;
; nasm -f bin stage2.asm -o stage2.bin

bits 16
org 0x8000

HDR             equ 0x7E00
HDR_KERNEL_LBA  equ HDR + 24
HDR_KERNEL_CNT  equ HDR + 28
HDR_KERNEL_LOAD equ HDR + 32
HDR_KERNEL_ENTRY equ HDR + 36
HDR_CMDLINE     equ HDR + 64

MMAP_BUF        equ 0x5000          ; up to 96 entries x 24 bytes
VBE_INFO        equ 0x6000          ; 512 bytes
MODE_INFO       equ 0x6200          ; 256 bytes
MBI             equ 0x6400          ; multiboot info (116 bytes)
BOUNCE_SEG      equ 0x1000          ; 1000:0000 = 0x10000, 64 sectors = 32 KiB
BOUNCE_ADDR     equ 0x10000
CHUNK           equ 64
MAX_MMAP        equ 96

stage2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [boot_drive], dl

    ; clear the multiboot info structure
    mov di, MBI
    mov cx, 128 / 2
    xor ax, ax
    rep stosw

    mov si, msg_loading
    call print

    call enable_a20
    call enter_unreal
    call load_kernel
    call read_memory
    call setup_video

    ; ---- multiboot info -------------------------------------------------------
    mov dword [MBI + 0], (1 << 0) | (1 << 1) | (1 << 2) | (1 << 9)
    movzx eax, byte [boot_drive]
    shl eax, 24
    or eax, 0x00FFFFFF
    mov [MBI + 12], eax             ; boot_device
    mov dword [MBI + 16], HDR_CMDLINE
    mov dword [MBI + 64], loader_name
    cmp word [mmap_count], 0
    je .no_mmap
    or dword [MBI + 0], 1 << 6
    movzx eax, word [mmap_count]
    imul eax, eax, 24
    mov [MBI + 44], eax             ; mmap_length
    mov dword [MBI + 48], MMAP_BUF  ; mmap_addr: first entry's size field
.no_mmap:
    cmp byte [have_fb], 0
    je .no_fb
    or dword [MBI + 0], (1 << 11) | (1 << 12)
.no_fb:

    ; ---- protected mode ----------------------------------------------------------
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm_entry

; ======================================================================================
; A20
; ======================================================================================
; check_a20: ZF = 1 if A20 is disabled (memory wraps at 1 MiB)
check_a20:
    push ds
    push es
    push si
    push di
    xor ax, ax
    mov ds, ax
    not ax
    mov es, ax                      ; ES = FFFF
    mov si, 0x0500
    mov di, 0x0510
    mov al, [ds:si]
    push ax
    mov al, [es:di]
    push ax
    mov byte [ds:si], 0x00
    mov byte [es:di], 0xFF
    cmp byte [ds:si], 0xFF
    pop ax
    mov [es:di], al
    pop ax
    mov [ds:si], al
    pop di
    pop si
    pop es
    pop ds
    ret

kbc_wait:
    mov cx, 0xFFFF
.w: in al, 0x64
    test al, 2
    jz .done
    loop .w
.done:
    ret

enable_a20:
    call check_a20
    jnz .ok
    mov ax, 0x2401                  ; BIOS
    int 0x15
    call check_a20
    jnz .ok
    cli                             ; keyboard controller output port
    call kbc_wait
    mov al, 0xD1
    out 0x64, al
    call kbc_wait
    mov al, 0xDF
    out 0x60, al
    call kbc_wait
    sti
    call check_a20
    jnz .ok
    in al, 0x92                     ; "fast A20"
    or al, 2
    and al, 0xFE
    out 0x92, al
    mov cx, 0x4000
.settle:
    call check_a20
    jnz .ok
    loop .settle
    mov si, msg_a20
    jmp fatal
.ok:
    ret

; ======================================================================================
; unreal mode: DS/ES keep a 4 GiB limit after returning to real mode
; ======================================================================================
enter_unreal:
    pushf
    cli
    push ds
    push es
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pm                   ; flush prefetch
.pm:
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    and al, 0xFE
    mov cr0, eax
    jmp short .rm
.rm:
    pop es
    pop ds
    popf
    ret

; ======================================================================================
; kernel loading
; ======================================================================================
load_kernel:
    mov eax, [HDR_KERNEL_LBA]
    mov [cur_lba], eax
    mov eax, [HDR_KERNEL_CNT]
    test eax, eax
    jz .bad
    mov [remaining], eax
    mov eax, [HDR_KERNEL_LOAD]
    mov [dest], eax
.loop:
    mov eax, [remaining]
    test eax, eax
    jz .done
    cmp eax, CHUNK
    jbe .n_ok
    mov eax, CHUNK
.n_ok:
    mov [dap_count], ax
    mov word [dap_off], 0
    mov word [dap_seg], BOUNCE_SEG
    mov ebx, [cur_lba]
    mov [dap_lba], ebx
    mov dword [dap_lba + 4], 0
    call read_disk
    call enter_unreal               ; the BIOS may have reloaded segment limits
    ; copy n * 512 bytes from the bounce buffer to [dest]
    movzx ecx, word [dap_count]
    push ecx
    shl ecx, 7                      ; dwords
    mov esi, BOUNCE_ADDR
    mov edi, [dest]
    cli
    a32 rep movsd
    sti
    pop ecx
    add [cur_lba], ecx
    sub [remaining], ecx
    shl ecx, 9
    add [dest], ecx
    ; a dot every 256 KiB
    inc byte [dots]
    test byte [dots], 7
    jnz .loop
    mov al, '.'
    call putc
    jmp .loop
.done:
    mov si, msg_ok
    call print
    ret
.bad:
    mov si, msg_kernel
    jmp fatal

; read_disk: DAP filled in; three attempts with a disk reset in between
read_disk:
    mov di, 3
.retry:
    push di
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    pop di
    jnc .ok
    xor ax, ax
    mov dl, [boot_drive]
    int 0x13
    dec di
    jnz .retry
    mov si, msg_read
    jmp fatal
.ok:
    ret

; ======================================================================================
; memory: E820 map (multiboot format: u32 size = 20, u64 base, u64 length, u32 type)
; ======================================================================================
read_memory:
    int 0x12                        ; KiB below 1 MiB
    movzx eax, ax
    mov [MBI + 4], eax              ; mem_lower

    ; E801: AX/CX = KiB between 1 and 16 MiB, BX/DX = 64 KiB blocks above 16 MiB
    xor cx, cx
    xor dx, dx
    mov ax, 0xE801
    int 0x15
    jc .no_e801
    jcxz .use_ax
    mov ax, cx
    mov bx, dx
.use_ax:
    movzx eax, ax
    movzx ebx, bx
    shl ebx, 6
    add eax, ebx
    mov [MBI + 8], eax              ; mem_upper
    jmp .e820
.no_e801:
    mov ah, 0x88
    int 0x15
    movzx eax, ax
    mov [MBI + 8], eax
.e820:
    xor ebx, ebx
    mov di, MMAP_BUF + 4
    mov word [mmap_count], 0
.next:
    mov eax, 0xE820
    mov edx, 0x534D4150             ; 'SMAP'
    mov ecx, 24
    mov dword [di + 20], 1          ; ACPI 3 attributes: valid unless the BIOS says otherwise
    push di
    int 0x15
    pop di
    jc .done
    cmp eax, 0x534D4150
    jne .done
    cmp ecx, 20
    jb .skip
    je .keep
    test byte [di + 20], 1          ; "ignore this entry"
    jz .skip
.keep:
    mov eax, [di + 8]
    or eax, [di + 12]
    jz .skip                        ; zero length
    mov dword [di - 4], 20
    add di, 24
    inc word [mmap_count]
    cmp word [mmap_count], MAX_MMAP
    jae .done
.skip:
    test ebx, ebx
    jnz .next
.done:
    ret

; ======================================================================================
; video: VBE linear framebuffer
; ======================================================================================
setup_video:
    mov byte [have_fb], 0
    mov di, VBE_INFO
    mov dword [di], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .fail
    cmp dword [VBE_INFO], 'VESA'
    jne .fail
    ; "res=WxH" on the command line (GRUB's safe graphics entry, carried over by
    ; the installer): try that size first, then the usual list
    call parse_res
    mov si, wanted_res
    jnc .try_next_wanted
    mov si, wanted
.try_next_wanted:
    mov ax, [si]
    test ax, ax
    jz .fail
    mov [want_w], ax
    mov ax, [si + 2]
    mov [want_h], ax
    mov al, [si + 4]
    mov [want_bpp], al
    push si
    call find_mode
    pop si
    jnc .found
    add si, 6
    jmp .try_next_wanted
.found:
    ; set the mode with the linear framebuffer bit
    mov bx, [mode_num]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .fail
    call fill_fb_info
    mov byte [have_fb], 1
    ret
.fail:
    mov si, msg_novbe
    call print
    ret

; parse_res: look for "res=WxH" (640..4096 x 480..4096) in the command line of the
; install header. CF clear: wanted_res holds W x H x 32 and W x H x 24.
parse_res:
    mov si, HDR_CMDLINE
.scan:
    cmp byte [si], 0
    je .none
    cmp dword [si], 'res='
    jne .advance
    cmp si, HDR_CMDLINE             ; start of an option: first or after a space
    je .found
    cmp byte [si - 1], ' '
    je .found
.advance:
    inc si
    cmp si, HDR_CMDLINE + 252
    jb .scan
.none:
    stc
    ret
.found:
    add si, 4
    call parse_dec
    jc .none
    mov bx, ax
    mov al, [si]
    or al, 0x20                     ; 'x' or 'X'
    cmp al, 'x'
    jne .none
    inc si
    call parse_dec
    jc .none
    cmp bx, 640
    jb .none
    cmp bx, 4096
    ja .none
    cmp ax, 480
    jb .none
    cmp ax, 4096
    ja .none
    mov [wanted_res + 0], bx
    mov [wanted_res + 2], ax
    mov [wanted_res + 6], bx
    mov [wanted_res + 8], ax
    clc
    ret

; parse_dec: decimal number at DS:SI (at most 5 digits) -> AX, SI past it; CF if none
parse_dec:
    xor ax, ax
    xor cx, cx
.digit:
    movzx dx, byte [si]
    sub dx, '0'
    cmp dx, 9
    ja .end
    imul ax, ax, 10
    add ax, dx
    inc si
    inc cx
    cmp cx, 5
    jb .digit
.end:
    test cx, cx
    jz .bad
    clc
    ret
.bad:
    stc
    ret

; find_mode: search the VBE mode list for want_w x want_h x want_bpp with a usable
; linear framebuffer. CF clear + mode_num set on success. MODE_INFO holds its info.
find_mode:
    push fs
    mov ax, [VBE_INFO + 16]         ; mode list far pointer (segment)
    mov fs, ax
    mov bx, [VBE_INFO + 14]         ; offset
.loop:
    mov cx, [fs:bx]
    cmp cx, 0xFFFF
    je .none
    add bx, 2
    push bx
    push cx
    mov ax, 0x4F01
    mov di, MODE_INFO
    int 0x10
    pop cx
    pop bx
    cmp ax, 0x004F
    jne .loop
    mov ax, [MODE_INFO + 0]         ; attributes: supported, colour, graphics, LFB
    and ax, 0x0099
    cmp ax, 0x0099
    jne .loop
    mov al, [MODE_INFO + 27]        ; memory model: 6 = direct colour
    cmp al, 6
    jne .loop
    mov ax, [MODE_INFO + 18]
    cmp ax, [want_w]
    jne .loop
    mov ax, [MODE_INFO + 20]
    cmp ax, [want_h]
    jne .loop
    mov al, [MODE_INFO + 25]
    cmp al, [want_bpp]
    jne .loop
    cmp dword [MODE_INFO + 40], 0   ; PhysBasePtr
    je .loop
    mov [mode_num], cx
    pop fs
    clc
    ret
.none:
    pop fs
    stc
    ret

fill_fb_info:
    mov dword [MBI + 72], VBE_INFO
    mov dword [MBI + 76], MODE_INFO
    mov ax, [mode_num]
    mov [MBI + 80], ax
    mov eax, [MODE_INFO + 40]
    mov [MBI + 88], eax             ; framebuffer_addr (low)
    mov dword [MBI + 92], 0
    movzx eax, word [MODE_INFO + 16]    ; BytesPerScanLine
    cmp word [VBE_INFO + 4], 0x0300
    jb .pitch
    movzx ecx, word [MODE_INFO + 50]    ; LinBytesPerScanLine (VBE 3)
    test ecx, ecx
    jz .pitch
    mov eax, ecx
.pitch:
    mov [MBI + 96], eax
    movzx eax, word [MODE_INFO + 18]
    mov [MBI + 100], eax
    movzx eax, word [MODE_INFO + 20]
    mov [MBI + 104], eax
    mov al, [MODE_INFO + 25]
    mov [MBI + 108], al             ; bpp
    mov byte [MBI + 109], 1         ; type: direct RGB
    ; colour layout: VBE 3 linear fields if present, else the banked ones
    mov si, MODE_INFO + 31
    cmp word [VBE_INFO + 4], 0x0300
    jb .masks
    cmp byte [MODE_INFO + 54], 0     ; LinRedMaskSize (VBE 3)
    je .masks
    mov si, MODE_INFO + 54
.masks:
    ; VBE: RedMaskSize, RedFieldPosition, GreenMaskSize, ... (size first)
    ; multiboot: red_pos, red_size, green_pos, ...          (position first)
    mov al, [si + 1]
    mov [MBI + 110], al
    mov al, [si + 0]
    mov [MBI + 111], al
    mov al, [si + 3]
    mov [MBI + 112], al
    mov al, [si + 2]
    mov [MBI + 113], al
    mov al, [si + 5]
    mov [MBI + 114], al
    mov al, [si + 4]
    mov [MBI + 115], al
    ret

; ======================================================================================
; text output
; ======================================================================================
putc:
    push bx
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop bx
    ret

print:
    lodsb
    test al, al
    jz .done
    call putc
    jmp print
.done:
    ret

fatal:
    call print
    mov si, msg_halt
    call print
.hang:
    cli
    hlt
    jmp .hang

; ======================================================================================
; 32-bit entry
; ======================================================================================
bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00
    mov ecx, [HDR_KERNEL_ENTRY]
    mov eax, 0x2BADB002
    mov ebx, MBI
    jmp ecx

; ======================================================================================
; data
; ======================================================================================
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF           ; 0x08: 32-bit code, base 0, 4 GiB
    dq 0x00CF92000000FFFF           ; 0x10: 32-bit data, base 0, 4 GiB
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

align 4
dap:
    db 16, 0
dap_count:  dw 0
dap_off:    dw 0
dap_seg:    dw 0
dap_lba:    dq 0

cur_lba:    dd 0
remaining:  dd 0
dest:       dd 0
mmap_count: dw 0
mode_num:   dw 0
want_w:     dw 0
want_h:     dw 0
want_bpp:   db 0
have_fb:    db 0
dots:       db 0
boot_drive: db 0x80

; preferred modes, in order (width, height, bpp); wanted_res is filled in from
; "res=WxH" and runs on into the default list
wanted_res:
    dw 0, 0
    db 32, 0
    dw 0, 0
    db 24, 0
wanted:
    dw 1024, 768
    db 32, 0
    dw 800, 600
    db 32, 0
    dw 1024, 768
    db 24, 0
    dw 0

loader_name: db "NovaOS disk loader", 0
msg_loading: db "loader", 13, 10, "Loading NovaOS", 0
msg_ok:      db " ok", 13, 10, 0
msg_a20:     db 13, 10, "error: cannot enable the A20 line", 0
msg_kernel:  db 13, 10, "error: bad kernel location in the install header", 0
msg_read:    db 13, 10, "error: disk read failed", 0
msg_novbe:   db "No VESA graphics mode found, continuing", 13, 10, 0
msg_halt:    db 13, 10, "System halted.", 0

; pad to whole sectors
times (512 - (($ - $$) % 512)) % 512 db 0
