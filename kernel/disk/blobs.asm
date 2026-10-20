; blobs.asm - the disk boot loader binaries, embedded into the kernel at build time
; so the running system can install itself onto a hard disk (kernel/disk/install.c).
; mbr.bin and stage2.bin are assembled from bootloader/*.asm by the Makefile and
; found through NASM's include path (-I$(BUILD)/bootloader/).

section .rodata
align 16
global bootblob_mbr, bootblob_mbr_end, bootblob_stage2, bootblob_stage2_end
bootblob_mbr:
    incbin "mbr.bin"
bootblob_mbr_end:
align 16
bootblob_stage2:
    incbin "stage2.bin"
bootblob_stage2_end:

section .note.GNU-stack noalloc noexec nowrite progbits
