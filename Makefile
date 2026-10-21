# NovaOS build system
#
#   make                      -> $(BUILD)/kernel.elf and $(BUILD)/NovaOS.iso (also copied to ./NovaOS.iso
#                                when BUILD is the default "build")
#   make BUILD=build-foo      -> everything (objects, elf, iso) goes under build-foo/
#   make CMDLINE="autorun=About testmouse=1"   -> bake a kernel command line into grub.cfg
#   make run                  -> boot the ISO in QEMU with a window
#   make run-kernel           -> boot kernel.elf directly via QEMU -kernel (BGA framebuffer path)
#   make clean
#
# Apps are picked up automatically from apps/*.c (and apps/*/*.c); kernel sources from
# kernel/**/*.c and *.asm.  Adding an app never requires editing this file.

BUILD    ?= build
CMDLINE  ?=
GRUB_TIMEOUT ?= 2
ISO_NAME ?= NovaOS.iso

CC   := gcc
AS   := nasm
QEMU ?= qemu-system-i386

GCC_INC := $(shell $(CC) -m32 -print-file-name=include)

CFLAGS := -m32 -march=i686 -std=gnu11 -O2 -g \
          -ffreestanding -nostdinc -isystem $(GCC_INC) -Iinclude -Ikernel \
          -fno-stack-protector -fno-pie -fno-pic -fno-omit-frame-pointer \
          -fno-strict-aliasing -fno-delete-null-pointer-checks -fno-common -fno-asynchronous-unwind-tables \
          -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mfpmath=387 -mno-red-zone \
          -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -MMD -MP
ifeq ($(WERROR),1)
CFLAGS += -Werror
endif
ASFLAGS := -f elf32 -g -F dwarf
LDFLAGS := -m32 -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-T,linker.ld -Wl,-z,noexecstack

KERNEL_C   := $(shell find kernel -name '*.c' | sort)
KERNEL_ASM := $(shell find boot kernel -name '*.asm' | sort)
APP_C      := $(sort $(wildcard apps/*.c) $(wildcard apps/*/*.c))

OBJS := $(patsubst %.asm,$(BUILD)/obj/%.asm.o,$(KERNEL_ASM)) \
        $(patsubst %.c,$(BUILD)/obj/%.o,$(KERNEL_C) $(APP_C))

KERNEL := $(BUILD)/kernel.elf
ISO    := $(BUILD)/$(ISO_NAME)

.PHONY: all kernel iso run run-kernel clean
all: $(ISO)
ifeq ($(BUILD),build)
	@cp -f $(ISO) ./$(ISO_NAME)
	@echo "==> ./$(ISO_NAME) ready"
endif

kernel: $(KERNEL)
iso: $(ISO)

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Disk boot loader (bootloader/*.asm, flat binaries) embedded into the kernel by
# kernel/disk/blobs.asm so the installer can write it to a hard disk.
$(BUILD)/bootloader/%.bin: bootloader/%.asm
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(AS) -f bin $< -o $@

$(BUILD)/obj/kernel/disk/blobs.asm.o: $(BUILD)/bootloader/mbr.bin $(BUILD)/bootloader/stage2.bin
$(BUILD)/obj/kernel/disk/blobs.asm.o: ASFLAGS += -I$(BUILD)/bootloader/

# Two-pass link: kernel0.elf with an empty symbol table, then the final kernel with
# the real one (the .ksyms section sits after all code, so no function moves).
$(BUILD)/ksyms_stub.c: tools/gensyms.py
	@mkdir -p $(BUILD)
	@python3 tools/gensyms.py < /dev/null > $@

$(BUILD)/kernel0.elf: $(OBJS) $(BUILD)/obj/ksyms_stub.o linker.ld
	@echo "  LD    $@"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/obj/ksyms_stub.o -lgcc

$(BUILD)/ksyms.c: $(BUILD)/kernel0.elf tools/gensyms.py
	@nm -n --defined-only $< | python3 tools/gensyms.py > $@

$(BUILD)/obj/%.o: $(BUILD)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) $(BUILD)/obj/ksyms.o linker.ld
	@echo "  LD    $@"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/obj/ksyms.o -lgcc
	@if [ "$$(nm $(BUILD)/kernel0.elf | grep ' T wm_run$$')" != "$$(nm $@ | grep ' T wm_run$$')" ]; then \
	  echo "warning: symbol table moved code; stack traces may be off"; fi
	@echo "==> $@ ($$(stat -c %s $@) bytes)"

# grub.cfg is regenerated whenever CMDLINE / GRUB_TIMEOUT change (stamp file trick).
$(BUILD)/cfgstamp-$(shell echo '$(CMDLINE)|$(GRUB_TIMEOUT)' | md5sum | cut -c1-12):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/cfgstamp-*
	@touch $@

$(ISO): $(KERNEL) $(BUILD)/cfgstamp-$(shell echo '$(CMDLINE)|$(GRUB_TIMEOUT)' | md5sum | cut -c1-12)
	@rm -rf $(BUILD)/isodir
	@mkdir -p $(BUILD)/isodir/boot/grub
	@cp $(KERNEL) $(BUILD)/isodir/boot/kernel.elf
	@printf '%s\n' \
	  'set timeout=$(GRUB_TIMEOUT)' \
	  'set default=0' \
	  'insmod all_video' \
	  'insmod vbe' \
	  'insmod vga' \
	  'set gfxpayload=1024x768x32,1024x768x24,800x600x32,auto' \
	  'set color_normal=light-gray/black' \
	  'set menu_color_normal=light-gray/black' \
	  'set menu_color_highlight=white/blue' \
	  '' \
	  'menuentry "NovaOS 1.5 Developer Preview" {' \
	  '  multiboot /boot/kernel.elf $(CMDLINE)' \
	  '  boot' \
	  '}' \
	  'menuentry "NovaOS 1.5 Developer Preview (safe graphics 800x600)" {' \
	  '  set gfxpayload=800x600x32,800x600x24,auto' \
	  '  multiboot /boot/kernel.elf res=800x600 $(CMDLINE)' \
	  '  boot' \
	  '}' \
	  'menuentry "Boot from the first hard disk (installed NovaOS)" {' \
	  '  set root=(hd0)' \
	  '  chainloader +1' \
	  '  boot' \
	  '}' \
	  'menuentry "Reboot" {' \
	  '  reboot' \
	  '}' > $(BUILD)/isodir/boot/grub/grub.cfg
	@echo "  ISO   $@"
	@grub-mkrescue -o $@ $(BUILD)/isodir >/dev/null 2>$(BUILD)/grub-mkrescue.log || \
	  (cat $(BUILD)/grub-mkrescue.log; exit 1)
	@echo "==> $@"

run: $(ISO)
	$(QEMU) -cdrom $(ISO) -m 256 -vga std -serial stdio -boot d

run-kernel: $(KERNEL)
	$(QEMU) -kernel $(KERNEL) -append "$(CMDLINE)" -m 256 -vga std -serial stdio

clean:
	rm -rf $(BUILD) ./$(ISO_NAME)

-include $(OBJS:.o=.d)
