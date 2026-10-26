# MYBOOT build. Run from Git Bash (recipes use POSIX sh).
#
#   make [CONFIG=debug|balanced|release]   build BOOTX64.EFI + test kernel into the ESP
#   make configs                           build all three configurations
#   make size                              byte size of every built config
#   make run                               boot the ESP in QEMU + OVMF (window + serial)
#   make test                              headless boot; passes iff the test kernel
#                                          reports every check passed
#   make run-gdb                           like run, paused, gdbstub on :1234
#   make iso                               build/$(CONFIG)/myboot.iso (UEFI, hybrid CD/USB)
#   make run-iso                           boot that ISO in QEMU as a CD-ROM
#   make clean

CONFIG ?= debug
BUILD  := build/$(CONFIG)
ESP    := $(BUILD)/esp
EFI    := $(ESP)/EFI/BOOT/BOOTX64.EFI
ESP_KERNEL := $(ESP)/EFI/MYBOOT/KERNEL.ELF

CC    := clang
LD    := lld-link
KLD   := ld.lld

# --target=x86_64-unknown-windows : emit COFF objects with the MS x64 ABI that
#                                   UEFI uses; lld-link then writes PE32+.
# -ffreestanding                  : no hosted libc assumptions; also stops clang
#                                   rewriting loops into memset/memcpy calls.
# -fno-stack-protector            : no __stack_chk_guard runtime exists.
# -mno-stack-arg-probe            : no __chkstk calls for large frames (no CRT).
# -mno-red-zone                   : nothing may assume memory below RSP is safe
#                                   (interrupt handlers share our stack).
# -mgeneral-regs-only             : never touch SSE/x87 state; firmware owns it.
# -fshort-wchar                   : L"" literals are 16-bit, matching CHAR16.
# -fno-*unwind-tables             : no .pdata/.xdata; nothing unwinds the stack.
CFLAGS := --target=x86_64-unknown-windows -std=c11 -ffreestanding \
          -fno-stack-protector -mno-stack-arg-probe -mno-red-zone \
          -mgeneral-regs-only -fshort-wchar \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -Wall -Wextra -Werror -Iinclude -MMD -MP

# /subsystem:efi_application : PE subsystem 10, what firmware requires.
# /entry:efi_main            : no CRT startup; firmware calls us directly.
# /nodefaultlib              : link nothing we did not write.
# /base:0x140000000          : fixed link base (lld-link's x64 default, made
#                              explicit) so the debugger slide is
#                              "load address" - 0x140000000.
# Relocations (.reloc) are kept (/dynamicbase is the default): firmware loads
# the image wherever it has free memory.
LDFLAGS := /subsystem:efi_application /entry:efi_main /nodefaultlib \
           /machine:x64 /base:0x140000000

ifeq ($(CONFIG),debug)
  # -Og keeps variables inspectable; CodeView debug info goes to a PDB that
  # stays out of the ESP.
  CFLAGS  += -Og -g -gcodeview -DBOOT_CONFIG_DEBUG
  LDFLAGS += /debug /pdb:$(BUILD)/BOOTX64.pdb
else ifeq ($(CONFIG),balanced)
  CFLAGS  += -O2 -DBOOT_CONFIG_BALANCED
else ifeq ($(CONFIG),release)
  # -Os: size over speed (boot code is I/O- and firmware-bound, not CPU-bound).
  # -flto + per-function sections + /opt:ref,icf: drop and fold unused code.
  CFLAGS  += -Os -flto -ffunction-sections -fdata-sections -DBOOT_CONFIG_RELEASE
  LDFLAGS += /opt:ref /opt:icf
else
  $(error CONFIG must be debug, balanced or release)
endif

SRCS := src/firmware/uefi/entry.c \
        src/firmware/uefi/console.c \
        src/firmware/uefi/memmap.c \
        src/firmware/uefi/discover.c \
        src/firmware/uefi/file.c \
        src/firmware/uefi/report.c \
        src/cpu/cpu.c \
        src/memory/memmap.c \
        src/acpi/acpi.c \
        src/smbios/smbios.c \
        src/elf/elf.c \
        src/arch/x86_64/paging.c \
        src/arch/x86_64/handoff.S \
        src/lib/mem.c
OBJS := $(patsubst %,$(BUILD)/obj/%.obj,$(basename $(SRCS)))

# --- test kernel (ELF, independent of CONFIG) -----------------------------
# -mcmodel=kernel : code and data live in the top 2 GiB (sign-extended 32-bit
#                   addresses), as the boot protocol requires.
KBUILD  := build/kernel
KERNEL  := $(KBUILD)/kernel.elf
KCFLAGS := --target=x86_64-unknown-none-elf -std=c11 -ffreestanding -fno-pic \
           -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -fno-stack-protector \
           -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Werror -Iinclude -MMD -MP
KOBJS   := $(KBUILD)/kernel.o $(KBUILD)/mem.o

# --- ISO (host tool, built with the host compiler and its C library) -------
HOSTCC ?= gcc
MKISO  := build/tools/mkiso.exe
ISO    := $(BUILD)/myboot.iso

.PHONY: all configs size run run-gdb test iso run-iso clean
all: $(EFI) $(ESP_KERNEL)

$(BUILD)/obj/%.obj: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.obj: %.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(EFI): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) /out:$@ $(OBJS)

$(KBUILD)/kernel.o: kernel/kernel.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(KBUILD)/mem.o: src/lib/mem.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(KERNEL): $(KOBJS) kernel/kernel.ld
	$(KLD) -T kernel/kernel.ld -nostdlib -static -z max-page-size=4096 -o $@ $(KOBJS)

$(ESP_KERNEL): $(KERNEL)
	@mkdir -p $(dir $@)
	cp $< $@

$(MKISO): tools/mkiso.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

$(ISO): $(EFI) $(KERNEL) $(MKISO)
	$(MKISO) $@ EFI/BOOT/BOOTX64.EFI=$(EFI) EFI/MYBOOT/KERNEL.ELF=$(KERNEL)

iso: $(ISO)

configs:
	$(MAKE) CONFIG=debug
	$(MAKE) CONFIG=balanced
	$(MAKE) CONFIG=release

size:
	@for c in debug balanced release; do \
	  f=build/$$c/esp/EFI/BOOT/BOOTX64.EFI; \
	  if [ -f $$f ]; then printf '%-9s %7d bytes\n' $$c $$(wc -c < $$f); \
	  else printf '%-9s (not built)\n' $$c; fi; \
	done
	@[ -f $(KERNEL) ] && printf '%-9s %7d bytes\n' kernel $$(wc -c < $(KERNEL)) || true

# --- QEMU --------------------------------------------------------------------
# The Windows QEMU installer ships OVMF as edk2-x86_64-code.fd. Every run
# starts from a fresh copy of the NVRAM template: saved boot options from an
# earlier run (e.g. a CD) otherwise make OVMF skip the disk under test.
QEMU       ?= qemu-system-x86_64
QEMU_SHARE ?= C:/Program Files/qemu/share
QEMU_MEM   ?= 256M
VARS       := build/vars.fd

# -net none : skip OVMF's PXE attempts (saves seconds per test boot).
# fat:DIR   : QEMU presents the ESP directory as a FAT disk. Read-only vvfat
#             must sit on a device that supports read-only (virtio-blk); the
#             default q35 AHCI disk fails with "Block node is read-only".
# -serial stdio : OVMF mirrors ConOut to COM1, and the kernel writes COM1.
# isa-debug-exit : lets the test kernel end QEMU with a status code.
QEMU_ARGS = -machine q35 -m $(QEMU_MEM) -net none -no-reboot \
            -drive "if=pflash,format=raw,readonly=on,file=$(QEMU_SHARE)/edk2-x86_64-code.fd" \
            -drive if=pflash,format=raw,file=$(VARS) \
            -drive if=virtio,readonly=on,format=raw,file=fat:$(ESP) \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
            -serial stdio $(QEMU_EXTRA)

FRESH_VARS = mkdir -p build && cp "$(QEMU_SHARE)/edk2-i386-vars.fd" $(VARS)

run: all
	@$(FRESH_VARS)
	-"$(QEMU)" $(QEMU_ARGS)

QEMU_ISO_ARGS = -machine q35 -m $(QEMU_MEM) -net none -no-reboot \
            -drive "if=pflash,format=raw,readonly=on,file=$(QEMU_SHARE)/edk2-x86_64-code.fd" \
            -drive if=pflash,format=raw,file=$(VARS) \
            -cdrom $(ISO) \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
            -serial stdio $(QEMU_EXTRA)

run-iso: $(ISO)
	@$(FRESH_VARS)
	-"$(QEMU)" $(QEMU_ISO_ARGS)

run-gdb: all
	@$(FRESH_VARS)
	"$(QEMU)" $(QEMU_ARGS) -s -S

# The kernel exits QEMU with (0x10 << 1) | 1 = 33 when every check passed.
test: all
	@$(FRESH_VARS)
	@timeout 60 "$(QEMU)" $(QEMU_ARGS) -display none; code=$$?; \
	if [ $$code -eq 33 ]; then echo "TEST PASS ($(CONFIG))"; \
	else echo "TEST FAIL ($(CONFIG)): qemu exit $$code"; exit 1; fi

clean:
	rm -rf build

-include $(OBJS:.obj=.d) $(KOBJS:.o=.d)
