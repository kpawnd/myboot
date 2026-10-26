# MYBOOT

An x86-64 UEFI bootloader written from scratch in C: no GRUB, Limine, gnu-efi
or libc. It loads an ELF64 kernel and its modules from the boot volume, builds
the kernel's page tables, exits boot services and jumps to the kernel with a
documented machine state (boot protocol: [`include/bootinfo/bootinfo.h`](include/bootinfo/bootinfo.h)).

Status: the UEFI path is complete and tested in QEMU + OVMF. The legacy BIOS
backend is not started. It has not yet been run on real hardware.

## Build and test

Toolchain (Windows): LLVM (`clang`, `lld-link`, `ld.lld`), QEMU, GNU make,
run from Git Bash.

```sh
make [CONFIG=debug|balanced|release]   # loader + test kernel into build/$CONFIG/esp
make run                               # boot that ESP in QEMU (window + serial)
make test                              # one headless boot; passes on exit status 33
make test-all                          # every scenario in tests/run.sh
make iso                               # build/$CONFIG/myboot.iso, hybrid CD/USB image
make size                              # binary sizes
```

`debug` and `balanced` print a boot summary; `release` prints only errors and
the menu.

## Files on the boot volume

```
\EFI\BOOT\BOOTX64.EFI      the loader (removable-media path), or anywhere else
\EFI\MYBOOT\BOOT.CFG       configuration (optional)
\EFI\MYBOOT\KERNEL.ELF     kernel used when there is no BOOT.CFG
\EFI\MYBOOT\...            kernels and modules BOOT.CFG names
```

Without `BOOT.CFG` the loader boots `\EFI\MYBOOT\KERNEL.ELF` and passes the
firmware boot entry's load options as the command line.

## BOOT.CFG

```
# comment
timeout 3                    # seconds the menu waits; 0 or absent = no menu
default 1                    # 1-based entry booted when the countdown ends

entry My kernel              # title shown in the menu
kernel KERNEL.ELF            # relative paths are under \EFI\MYBOOT\
cmdline root=/dev/nvme0n1p2 quiet
module INITRD.IMG initrd     # file, then the string the kernel sees with it

entry Known-good kernel
kernel /EFI/MYBOOT/KERNEL.OLD
cmdline root=/dev/nvme0n1p2
```

- Up to 9 entries, 8 modules each. Paths may use `/` or `\`, at most 127
  characters, no spaces. The cmdline can be up to 1023 characters, and a
  module string up to 111.
- **Fallback:** if the chosen entry cannot be loaded (missing file, invalid
  ELF, out of memory), the loader frees what it loaded and tries the next
  entry, then the rest in order. Keep a known-good kernel as the last entry.
- A line the loader cannot use is reported with its line number and skipped.
  The rest of the file still works. The errors also appear at the top of the
  menu.

## The menu

```
MYBOOT boot menu

  1) My kernel                          <- highlighted selection
  2) Known-good kernel

     Firmware setup                     (only if the firmware supports it)
     Reboot
     Power off
     Return to firmware

Up/Down select, Enter boot, e edit command line, 1-9 boot that entry

Booting the selected entry in 10 s. Press any key to stop.
```

- **Up/Down, Home/End** move the selection. **Enter** activates it. **1-9**
  boot that entry at once. Any key stops the countdown.
- **e** edits the selected entry's command line before booting it: Left,
  Right, Home and End move the cursor, and Backspace and Delete erase. Enter
  boots with the edited line and Esc goes back. The change applies to this
  boot only.
- **Firmware setup** reboots into the firmware's own settings, using the UEFI
  OsIndications variable. **Return to firmware** hands control back to the
  firmware's boot manager, which tries the next boot option.
- With `timeout 0` there is no menu. To get it anyway, tap any key while the
  machine starts.
- **Recovery screen:** if every entry fails to load, the loader shows the
  errors and the same menu. You can retry an entry, edit its command line,
  open firmware setup, reboot or power off. After 10 s without a key it
  returns to the firmware, so an unattended machine still moves on to its
  next boot option.

## Running it on a real PC

Secure Boot must be off, because the loader is unsigned. To keep Secure Boot
on, you have to enroll your own key in `db` and sign `BOOTX64.EFI` with it.

### From a USB stick (the safest first test)

1. Format a USB stick as FAT32.
2. Copy the contents of `build/release/esp/` (or `build/debug/esp/`) to the
   root of the stick, so it has `EFI\BOOT\BOOTX64.EFI`. Alternatively, write
   `build/$CONFIG/myboot.iso` to the stick with Rufus (DD mode) or `dd`.
3. Pick the stick in the firmware's one-time boot menu (often F12, F8 or Esc).

This changes nothing on the internal disk. The test kernel prints its checks
on the screen and on COM1. On real hardware it halts afterwards, because it
only exits QEMU when it detects a hypervisor.

### Alongside Windows, on the internal disk

**If the system drive uses BitLocker** (it does by default on many laptops),
suspend it first and have the recovery key at hand. A change to the boot
configuration can otherwise trigger the recovery prompt:
`Suspend-BitLocker -MountPoint C: -RebootCount 1`.

In an administrator PowerShell:

```powershell
mountvol S: /S                                   # mount the EFI system partition as S:
New-Item -ItemType Directory S:\EFI\MYBOOT
Copy-Item build\release\esp\EFI\BOOT\BOOTX64.EFI S:\EFI\MYBOOT\
Copy-Item build\release\esp\EFI\MYBOOT\*         S:\EFI\MYBOOT\
bcdedit /copy "{bootmgr}" /d "MYBOOT"            # prints the new entry's {GUID}
bcdedit /set "{GUID}" path \EFI\MYBOOT\BOOTX64.EFI
bcdedit /set "{fwbootmgr}" displayorder "{GUID}" /addlast
mountvol S: /D
```

This adds MYBOOT as an extra firmware boot entry, last in the order, so
Windows still boots by default. Choose MYBOOT from the firmware boot menu.

- Never overwrite `\EFI\BOOT\BOOTX64.EFI` or `\EFI\Microsoft\` on a disk
  Windows boots from.
- To uninstall: `bcdedit /delete "{GUID}"`, then mount the ESP again and
  delete `S:\EFI\MYBOOT`.

On Linux, the equivalent of the `bcdedit` steps is
`efibootmgr -c -d /dev/nvme0n1 -p 1 -L MYBOOT -l '\EFI\MYBOOT\BOOTX64.EFI'`.

## Writing a kernel for it

[`include/bootinfo/bootinfo.h`](include/bootinfo/bootinfo.h) is the whole
contract: the kernel ELF requirements, machine state at entry, address space,
memory map types, modules, the raw EFI memory map for runtime services, and
the compatibility rule (fields are only appended; check `size`).
[`kernel/kernel.c`](kernel/kernel.c) is a working example that checks every
promise in the contract.

In short:

- ELF64 `ET_EXEC`, linked in the top 2 GiB (`-mcmodel=kernel`), at most 1 GiB.
- Entry in long mode with interrupts off, no IDT, and `RDI` = `struct boot_info *`.
- All physical memory is reachable at `hhdm_base + phys`.
