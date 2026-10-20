; NovaOS disk boot loader - stage 1 (MBR)
;
; Loaded by the BIOS at 0000:7C00 with DL = boot drive. Reads the install
; header (LBA 1) to 0000:7E00, checks its "NOVADISK" magic, loads stage 2
; (header.stage2_lba / stage2_sectors) to 0000:8000 with INT 13h extensions
; (AH=42h) and jumps to it with DL = boot drive.
;
; Only the first 440 bytes are code: the installer fills in the disk
; signature, the partition table and the 0x55AA boot signature.
;
; nasm -f bin mbr.asm -o mbr.bin

bits 16
org 0x7C00

HDR_ADDR        equ 0x7E00
STAGE2_ADDR     equ 0x8000
HDR_STAGE2_LBA  equ HDR_ADDR + 16
HDR_STAGE2_CNT  equ HDR_ADDR + 20

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    jmp 0:.go                   ; normalise CS:IP (some BIOSes use 07C0:0000)
.go:
    mov [boot_drive], dl

    mov si, msg_hello
    call print

    ; INT 13h extensions present?
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no_ext
    cmp bx, 0xAA55
    jne .no_ext
    test cl, 1                  ; DAP access supported
    jz .no_ext

    ; install header -> 0000:7E00
    mov word [dap_count], 1
    mov word [dap_off], HDR_ADDR
    mov dword [dap_lba], 1
    call read_disk

    cmp dword [HDR_ADDR], 'NOVA'
    jne .bad_header
    cmp dword [HDR_ADDR + 4], 'DISK'
    jne .bad_header

    ; stage 2 -> 0000:8000 (at most 62 sectors: stays below 64 KiB)
    mov ax, [HDR_STAGE2_CNT]
    test ax, ax
    jz .bad_header
    cmp ax, 62
    ja .bad_header
    mov [dap_count], ax
    mov word [dap_off], STAGE2_ADDR
    mov eax, [HDR_STAGE2_LBA]
    mov [dap_lba], eax
    call read_disk

    mov dl, [boot_drive]
    jmp 0:STAGE2_ADDR

.no_ext:
    mov si, msg_noext
    jmp fatal
.bad_header:
    mov si, msg_header
    jmp fatal

; read_disk: DAP is filled in; retries three times with a disk reset
read_disk:
    mov di, 3
.retry:
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jnc .ok
    xor ax, ax                  ; reset disk system
    mov dl, [boot_drive]
    int 0x13
    dec di
    jnz .retry
    mov si, msg_read
    jmp fatal
.ok:
    ret

; print: DS:SI -> zero terminated string (BIOS teletype)
print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
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

align 4
dap:
    db 16, 0
dap_count:  dw 0
dap_off:    dw 0
dap_seg:    dw 0
dap_lba:    dq 0

boot_drive: db 0x80

msg_hello:  db "NovaOS ", 0
msg_noext:  db "error: BIOS lacks INT 13h extensions", 0
msg_header: db "error: no NovaOS install header", 0
msg_read:   db "error: disk read failed", 0
msg_halt:   db 13, 10, "System halted.", 0

%if ($ - $$) > 440
%error "stage 1 code is larger than 440 bytes"
%endif
times 440 - ($ - $$) db 0
    dd 0                        ; 440: disk signature (installer)
    dw 0                        ; 444
    times 64 db 0               ; 446: partition table (installer)
    dw 0xAA55                   ; 510
