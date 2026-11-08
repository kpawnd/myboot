# MYBOOT

An x86-64 UEFI bootloader written from scratch in C: no GRUB, Limine, gnu-efi
or libc. It boots three kinds of entries:

- **Linux** kernels through their EFI stub, with initrds and a command line.
- **Other EFI programs**: Windows Boot Manager (found automatically), another
  loader, a UEFI shell.
- **Kernels for its own boot protocol**: ELF64, with modules, page tables
  built for the kernel, and a documented machine state
  ([`include/bootinfo/bootinfo.h`](include/bootinfo/bootinfo.h)).

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
make test-fs                           # filesystem driver on the host (needs Python, 7-Zip)
make test-csme                         # CSME detection and HBM on the host, simulated device
make iso                               # build/$CONFIG/myboot.iso, hybrid CD/USB image
make size                              # binary sizes
```

The loader prints a boot log in the style of systemd, one tagged line per
step:

```
[  OK  ] Read configuration \EFI\MYBOOT\BOOT.CFG (2 entries)
         Booting 'Test kernel with self-test module'
[  OK  ] Loaded kernel \EFI\MYBOOT\KERNEL.ELF (46 KiB)
[FAILED] cannot open module: \EFI\MYBOOT\NOPE.MOD (not found)
```

Every build logs the files it loads, errors and config warnings. `debug` and
`balanced` also log what the hardware discovery found, and `debug` adds
detail lines (CPU features, ACPI tables, timings). The loader never waits on
purpose: its lines appear as fast as the work is done.

The kernel receives the same log (`loader_log` in `boot_info`). The test
kernel shows it again on its own screen, followed by its checks, one line at
a time when its command line has `log_delay=MS` (the shipped BOOT.CFG uses
40 ms).

`make test-all` also chainloads a small test program and, when a Linux
kernel with an EFI stub is available (`LINUX_KERNEL`, `LINUX_INITRD`; the
WSL kernel by default), boots Linux with an initrd.

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

entry Linux
linux /EFI/linux/vmlinuz     # any distro kernel (EFI stub)
initrd /EFI/linux/initrd.img # several initrd lines are concatenated
cmdline root=UUID=... rw quiet

entry Linux from /boot
volume uuid=0a3f...          # the ext4 filesystem holding /boot (see below)
linux /boot/vmlinuz          # symlinks such as /boot/vmlinuz are followed
initrd /boot/initrd.img
cmdline root=UUID=0a3f... rw quiet

entry UEFI shell
efi /EFI/tools/shellx64.efi  # any EFI program; cmdline = its load options
```

Each entry has exactly one of `kernel`, `linux` or `efi`. Windows Boot
Manager needs no entry: if `\EFI\Microsoft\Boot\bootmgfw.efi` exists on any
partition, the menu lists it after your entries (`auto_windows no` turns
this off). The partitions are scanned only when the menu is shown.

The initrd is passed the way Linux 5.8 and later expect (the `LoadFile2`
protocol).

### Files on other partitions: `volume`

Without a `volume` line, an entry's files are on the partition the loader
was started from (the ESP). `volume` points the entry at another partition,
by the identifiers `blkid` and `/etc/fstab` use:

```
volume uuid=0a3f1c2e-...     # filesystem UUID (ext4), or FAT serial like 1A2B-3C4D
volume partuuid=7e57d15c-... # GPT partition GUID, or MBR "1234abcd-02"
volume label=rootfs          # filesystem label (case-sensitive)
```

Case and dashes in UUIDs do not matter. The loader reads **FAT** through the
firmware and **ext2, ext3 and ext4** with its own read-only driver, so
`/boot` on a typical Linux install works directly, with no copying to the
ESP after kernel updates. On ext4, names are case-sensitive, symlinks are
followed, and relative paths still mean `/EFI/MYBOOT/`, so give absolute
paths. If no partition matches, the entry fails and the next one is tried.

The ext4 driver reads extents, ext2/3 block maps, 64-bit and `meta_bg`
layouts, and htree directories. It does not read encrypted (fscrypt) files
or inline data larger than 60 bytes, and it does not replay the journal: a
`/boot` that was not cleanly unmounted may show the state of its last
checkpoint. Partitions are scanned only when an entry names a `volume` or
the menu looks for Windows.

### Limits

- Up to 9 entries, 8 modules or initrds each. Paths may use `/` or `\`, at most 127
  characters, no spaces. The cmdline can be up to 1023 characters, and a
  module string up to 111.
- **Fallback:** if the chosen entry cannot be loaded (missing file, invalid
  ELF, out of memory), the loader frees what it loaded and tries the next
  entry, then the rest in order. Keep a known-good kernel as the last entry.
  A Linux kernel or EFI program that started and then returned does not
  fall back; the recovery screen appears instead.
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

## Intel CSME detection

On Intel CPUs the loader looks for the Converged Security and Management
Engine through its host interface, HECI1 (PCI `00:16.0`). It reports what it
finds in the boot log and in `boot_info.csme`:

```
[  OK  ] Detected Intel CSME: Tiger Lake-LP (HECI 8086:A0E0), normal, firmware 15.0.42.2235
[  OK  ] Talked to Intel CSME over HBM 2.2: 11 clients (MKHI, AMTHI, watchdog, 8 unnamed)
[  OK  ] Intel CSME MKHI client answered over HBM: firmware 15.0.42.2235
[ INFO ] Detected Intel CSME: Alder Lake-P (HECI 8086:51E0), normal, mode: soft temporary disable
[ INFO ] No Intel CSME visible (HECI hidden or absent)
[ WARN ] Intel CSME did not answer at MKHI's fixed address: no reply within 1 s
```

It works in three steps:

1. **Status, read-only.** The loader reads configuration space through ECAM
   (from the ACPI `MCFG` table), or through ports `0xCF8`/`0xCFC` when there is
   no MCFG. This gives the device ID, which names the chipset, and the
   firmware status registers HFSTS1-6. From HFSTS1 the loader decodes the
   working state, operating mode, error code and whether init is complete.
   `boot_info.csme.hfsts[]` holds all six raw registers.
2. **Firmware version at MKHI's fixed address.** The loader sends one MKHI
   `GET_FW_VERSION` request to address 7, the way firmware does, and reads
   the reply.
3. **An HBM session.** The loader speaks the HECI bus protocol, as Linux's
   `mei` driver does: `HOST_START`, enumerate the ME clients, read each
   one's properties (UUID, protocol, message size), then ask the MKHI client
   for the firmware version through it. A fixed-address client is used
   directly; any other is connected to with flow control and disconnected
   afterwards. The session ends with `HOST_STOP`, whose answer is optional (Arrow
   Lake sends none). Known client UUIDs are
   named (MKHI, AMTHI, watchdog, HDCP, PAVP, NFC, DAL). The debug build logs
   every client on its own line.

Steps 2 and 3 run only if the CSME is in normal state and normal mode, and
they have limits:

- They use only a memory BAR the firmware already assigned.
- They use the link only if the firmware left both sides ready and both
  buffers empty. They never reset the link.
- If memory decoding was off, they turn it on for the exchange and then
  restore the Command register.
- They run at `TPL_HIGH_LEVEL`, so no firmware timer event can use HECI in
  the middle. A CSME that does not answer holds that for up to 1 s per step.

`boot_info.csme` records how each went (`version_status`, `hbm_status`,
`mkhi_status`) and how many clients the session found (`client_count`).
Any failure leaves that part unknown. The log line says why, and booting
continues. To drop steps 2 and 3 and keep only the read-only status, run
`make clean` and then `make CSME_QUERY=0`.

Limits:

- "No Intel CSME visible" can mean the CSME is fused off, the firmware hid
  HECI1 (a common BIOS option), or the platform has none. The loader cannot
  tell these apart.
- Some firmware closes the MKHI fixed-address client after End-of-POST. On
  those machines step 2 fails with "request refused" or a timeout, and the
  version comes from step 3 instead.
- If the firmware itself already started an HBM session and left it open,
  the CSME may reject the second `HOST_START` or reset the link. The loader
  reports that and does not retry. Linux resets the link when it loads
  `mei_me`, so the kernel is unaffected either way.
- HFSTS1 bit 4 means manufacturing mode on older CSMEs and SPI protection
  on newer ones, so the loader does not decode it. The raw value is in
  `hfsts[0]`.
- Chipset names come from a short table of HECI device IDs from Linux's
  `hw-me-regs.h`. Other IDs show only the hex ID.
- QEMU has no HECI device, so `make test-all` only covers the "not visible"
  path. `make test-csme` covers the rest against a simulated device.
  Results on real hardware are unverified. To check them under Linux, compare
  with `/sys/class/mei/mei0/fw_ver` and `fw_status`.

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
