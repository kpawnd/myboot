# MYBOOT build.

CONFIG ?= debug
BUILD  := build/$(CONFIG)
ESP    := $(BUILD)/esp
EFI    := $(ESP)/EFI/BOOT/BOOTX64.EFI
ESP_KERNEL := $(ESP)/EFI/MYBOOT/KERNEL.ELF
ESP_CFG    := $(ESP)/EFI/MYBOOT/BOOT.CFG
ESP_MOD    := $(ESP)/EFI/MYBOOT/SELFTEST.MOD
ESP_BG     := $(ESP)/EFI/MYBOOT/BACKGROUND.JPG

CC    := clang
LD    := lld-link
KLD   := ld.lld

CFLAGS := --target=x86_64-unknown-windows -std=c11 -ffreestanding \
          -fno-stack-protector -mno-stack-arg-probe -mno-red-zone \
          -mgeneral-regs-only -fshort-wchar \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -Wall -Wextra -Werror -Iinclude -MMD -MP

CSME_QUERY ?= 1
CFLAGS += -DBOOT_CSME_QUERY=$(CSME_QUERY)

LDFLAGS := /subsystem:efi_application /entry:efi_main /nodefaultlib \
           /machine:x64 /base:0x140000000

ifeq ($(CONFIG),debug)
  CFLAGS  += -Og -g -gcodeview -DBOOT_CONFIG_DEBUG
  LDFLAGS += /debug /pdb:$(BUILD)/BOOTX64.pdb
else ifeq ($(CONFIG),balanced)
  CFLAGS  += -O2 -DBOOT_CONFIG_BALANCED
else ifeq ($(CONFIG),release)
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
        src/firmware/uefi/menu.c \
        src/firmware/uefi/error.c \
        src/firmware/uefi/chain.c \
        src/firmware/uefi/volume.c \
        src/firmware/uefi/gfx.c \
        src/image/jpeg.c \
        src/fs/fs.c \
        src/fs/ext4.c \
        src/config/config.c \
        src/cpu/cpu.c \
        src/memory/memmap.c \
        src/acpi/acpi.c \
        src/smbios/smbios.c \
        src/csme/csme.c \
        src/elf/elf.c \
        src/arch/x86_64/paging.c \
        src/arch/x86_64/handoff.S \
        src/arch/x86_64/smp.c \
        src/arch/x86_64/smp_tramp.S \
        src/arch/x86_64/exc.S \
        src/lib/mem.c
OBJS := $(patsubst %,$(BUILD)/obj/%.obj,$(basename $(SRCS)))

KBUILD  := build/kernel
KERNEL  := $(KBUILD)/kernel.elf
KCFLAGS := --target=x86_64-unknown-none-elf -std=c11 -ffreestanding -fno-pic \
           -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -fno-stack-protector \
           -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Werror -Iinclude -MMD -MP
KOBJS   := $(KBUILD)/kernel.o $(KBUILD)/mem.o

HOSTCC ?= gcc
MKISO  := build/tools/mkiso.exe
ISO    := $(BUILD)/myboot.iso

.PHONY: all configs size run run-gdb test test-all test-fs test-csme test-jpeg screenshots iso run-iso clean
all: $(EFI) $(ESP_KERNEL) $(ESP_CFG) $(ESP_MOD) $(ESP_BG)

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

$(ESP_CFG): kernel/boot.cfg
	@mkdir -p $(dir $@)
	cp $< $@

$(ESP_MOD): kernel/selftest.mod
	@mkdir -p $(dir $@)
	cp $< $@

$(ESP_BG): kernel/background.jpg
	@mkdir -p $(dir $@)
	cp $< $@

$(MKISO): tools/mkiso.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

$(ISO): $(EFI) $(KERNEL) kernel/boot.cfg kernel/selftest.mod kernel/background.jpg $(MKISO)
	$(MKISO) $@ EFI/BOOT/BOOTX64.EFI=$(EFI) EFI/MYBOOT/KERNEL.ELF=$(KERNEL) \
	  EFI/MYBOOT/BOOT.CFG=kernel/boot.cfg EFI/MYBOOT/SELFTEST.MOD=kernel/selftest.mod \
	  EFI/MYBOOT/BACKGROUND.JPG=kernel/background.jpg

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

QEMU       ?= qemu-system-x86_64
QEMU_SHARE ?= C:/Program Files/qemu/share
QEMU_MEM   ?= 256M
QEMU_SMP   ?= 4
VARS       := build/vars.fd

QEMU_ARGS = -machine q35 -m $(QEMU_MEM) -smp $(QEMU_SMP) -net none -no-reboot \
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

test: all
	@$(FRESH_VARS)
	@timeout 60 "$(QEMU)" $(QEMU_ARGS) -display none < /dev/null; code=$$?; \
	if [ $$code -eq 33 ]; then echo "TEST PASS ($(CONFIG))"; \
	else echo "TEST FAIL ($(CONFIG)): qemu exit $$code"; exit 1; fi

CHAINTEST := $(BUILD)/chaintest/CHAINTEST.EFI
$(CHAINTEST): $(BUILD)/obj/tests/efi/chaintest.obj
	@mkdir -p $(dir $@)
	$(LD) /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 /out:$@ $<

LINUX_KERNEL ?= C:/Program Files/WSL/tools/kernel
LINUX_INITRD ?= C:/Program Files/WSL/tools/initrd.img

test-all: all $(CHAINTEST)
	@CONFIG=$(CONFIG) QEMU="$(QEMU)" QEMU_SHARE="$(QEMU_SHARE)" QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) \
	  CHAINTEST="$(CHAINTEST)" LINUX_KERNEL="$(LINUX_KERNEL)" LINUX_INITRD="$(LINUX_INITRD)" \
	  sh tests/run.sh

PYTHON   ?= python
FS_SRCS  := tests/host/ext4cat.c src/fs/ext4.c src/fs/fs.c
FS_HOST  := build/host/ext4cat.exe
FS_ASAN  := build/host/ext4cat-asan.exe
FS_HOSTFLAGS := -O1 -g -Wall -Wextra -Werror -Iinclude -D_CRT_SECURE_NO_WARNINGS
FUZZ_RUNS ?= 300

$(FS_HOST): $(FS_SRCS) include/fs/*.h
	@mkdir -p $(dir $@)
	$(CC) $(FS_HOSTFLAGS) $(FS_SRCS) -o $@

$(FS_ASAN): $(FS_SRCS) include/fs/*.h
	@mkdir -p $(dir $@)
	$(CC) $(FS_HOSTFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all $(FS_SRCS) -o $@

test-fs: $(FS_HOST) $(FS_ASAN) $(KERNEL)
	LINUX_KERNEL="$(LINUX_KERNEL)" $(PYTHON) tests/fs.py
	$(PYTHON) tests/host/fuzz.py $(FUZZ_RUNS)

CSME_SRCS := tests/host/csmetest.c src/csme/csme.c
CSME_HOST := build/host/csmetest.exe
CSME_ASAN := build/host/csmetest-asan.exe

$(CSME_HOST): $(CSME_SRCS) include/csme/csme.h include/bootinfo/bootinfo.h
	@mkdir -p $(dir $@)
	$(CC) $(FS_HOSTFLAGS) $(CSME_SRCS) -o $@

$(CSME_ASAN): $(CSME_SRCS) include/csme/csme.h include/bootinfo/bootinfo.h
	@mkdir -p $(dir $@)
	$(CC) $(FS_HOSTFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all $(CSME_SRCS) -o $@

LLVM_DIR ?= C:/Program Files/LLVM
test-csme: $(CSME_HOST) $(CSME_ASAN)
	$(CSME_HOST)
	for d in "$(LLVM_DIR)"/lib/clang/*/lib/windows; do PATH="$$(cygpath -u "$$d"):$$PATH"; done; \
	$(CSME_ASAN)

JPEG_SRCS := tests/host/jpegdec.c src/image/jpeg.c
JPEG_HOST := build/host/jpegdec.exe
JPEG_ASAN := build/host/jpegdec-asan.exe

$(JPEG_HOST): $(JPEG_SRCS) include/image/jpeg.h
	@mkdir -p $(dir $@)
	$(CC) $(FS_HOSTFLAGS) $(JPEG_SRCS) -o $@

$(JPEG_ASAN): $(JPEG_SRCS) include/image/jpeg.h
	@mkdir -p $(dir $@)
	$(CC) $(FS_HOSTFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all $(JPEG_SRCS) -o $@

test-jpeg: $(JPEG_HOST) $(JPEG_ASAN)
	$(PYTHON) tests/jpeg.py $(FUZZ_RUNS)

screenshots: all
	@CONFIG=$(CONFIG) QEMU="$(QEMU)" QEMU_SHARE="$(QEMU_SHARE)" QEMU_SMP=$(QEMU_SMP) \
	  sh tests/tools/screenshots.sh

clean:
	rm -rf build

-include $(OBJS:.obj=.d) $(KOBJS:.o=.d)
