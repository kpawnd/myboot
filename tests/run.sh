#!/bin/sh
# Boot scenarios for one build configuration; run through `make test-all`.
#
# Each case builds its own ESP directory under build/$CONFIG/tests/<case>/
# from the built loader, test kernel and self-test module, boots it headless
# in QEMU + OVMF, and checks the exit status and the serial log:
#
#   exit 33   the test kernel ran and every check passed
#   exit 35   the test kernel ran and a check failed
#   other     the kernel never ran. For the cases that expect the loader to
#             give up, the ESP holds a startup.nsh that powers off from the
#             UEFI shell, which OVMF starts after the loader returns.
#
# Broken kernels are made from the real one, so they track its layout.

set -u
CONFIG=${CONFIG:-debug}
QEMU=${QEMU:-qemu-system-x86_64}
QEMU_SHARE=${QEMU_SHARE:-C:/Program Files/qemu/share}
QEMU_MEM=${QEMU_MEM:-256M}
LOADER=build/$CONFIG/esp/EFI/BOOT/BOOTX64.EFI
KERNEL=build/kernel/kernel.elf
MODULE=kernel/selftest.mod
OUT=build/$CONFIG/tests
pass=0
fail=0

for f in "$LOADER" "$KERNEL" "$MODULE"; do
    [ -f "$f" ] || { echo "missing $f: run make CONFIG=$CONFIG first"; exit 1; }
done
rm -rf "$OUT"
mkdir -p "$OUT"

# esp NAME: a fresh ESP with the loader and the good kernel; prints its
# EFI/MYBOOT directory for the case to add files to.
esp() {
    d=$OUT/$1
    mkdir -p "$d/EFI/BOOT" "$d/EFI/MYBOOT"
    cp "$LOADER" "$d/EFI/BOOT/BOOTX64.EFI"
    cp "$KERNEL" "$d/EFI/MYBOOT/KERNEL.ELF"
    echo "$d/EFI/MYBOOT"
}

# poweroff NAME: shut QEMU down once OVMF falls back to its shell.
poweroff() {
    printf 'reset -s\r\n' > "$OUT/$1/startup.nsh"
}

# qemu NAME CONSOLE...: run QEMU on that ESP with the given serial/monitor
# options; the serial log goes to $OUT/NAME.log. With $DISK set, that disk
# image is attached as a second disk (read-only).
DISK=
qemu() {
    name=$1
    shift
    vars=$OUT/$name.vars.fd
    cp "$QEMU_SHARE/edk2-i386-vars.fd" "$vars"
    timeout 90 "$QEMU" -machine q35 -m "$QEMU_MEM" -net none -no-reboot \
        -drive "if=pflash,format=raw,readonly=on,file=$QEMU_SHARE/edk2-x86_64-code.fd" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=virtio,readonly=on,format=raw,file=fat:$OUT/$name" \
        ${DISK:+-drive "if=virtio,readonly=on,format=raw,file=$DISK"} \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -display none "$@"
}

# boot NAME: no keyboard input; sets $code.
boot() {
    qemu "$1" -serial stdio < /dev/null > "$OUT/$1.log" 2>&1
    code=$?
}

# keys NAME KEY...: once the menu is on screen, press each KEY (a QEMU
# 'sendkey' name: up, down, ret, backspace, spc, a, 1, ...) half a second
# apart on the emulated keyboard, as a person would. Keys pressed earlier
# would be dropped by the menu. Serial input is not used: QEMU for Windows
# stalls on escape sequences (arrow keys) read from a stdin pipe.
keys() {
    log=$OUT/$1.log
    shift
    i=0
    until [ -f "$log" ] && grep -aqF "Up/Down select" "$log"; do
        i=$((i + 1))
        [ "$i" -gt 900 ] && return
        sleep 0.1
    done
    for k in "$@"; do
        sleep 0.5
        echo "sendkey $k"
    done
    # Keep the monitor's stdin open until QEMU exits (then echo fails).
    while sleep 1; do
        echo || break
    done
}

# boot_keys NAME KEY...: like boot, pressing KEYs at the menu.
boot_keys() {
    name=$1
    keys "$@" | qemu "$name" -serial "file:$OUT/$name.log" -monitor stdio > /dev/null 2>&1
    code=$?
}

# run_case NAME: boot it, pressing $KEYS if set. KEYS is cleared after
# use: a variable assigned in front of a function call stays set in POSIX sh.
run_case() {
    k=${KEYS:-}
    KEYS=
    if [ -n "$k" ]; then
        # shellcheck disable=SC2086 # word splitting of the key list is intended
        boot_keys "$1" $k
    else
        boot "$1"
    fi
}

# result NAME OK REASON
result() {
    if [ "$2" = 1 ]; then
        pass=$((pass + 1))
        echo "PASS  $1"
    else
        fail=$((fail + 1))
        echo "FAIL  $1: $3 (log: $OUT/$1.log)"
    fi
}

in_log() { tr -d '\r' < "$OUT/$1.log" | grep -aqF -- "$2"; }

# expect_boot NAME [TEXT...]: kernel passed all checks and the log has each
# TEXT. With $KEYS set (space-separated sendkey names), they are pressed at the menu.
expect_boot() {
    name=$1
    shift
    run_case "$name"
    if [ "$code" -ne 33 ]; then
        result "$name" 0 "qemu exit $code, expected 33"
        return
    fi
    for t in "$@"; do
        in_log "$name" "$t" || { result "$name" 0 "log lacks: $t"; return; }
    done
    result "$name" 1
}

# expect_refusal NAME TEXT: the loader printed TEXT and the kernel never ran.
expect_refusal() {
    poweroff "$1"
    boot "$1"
    if in_log "$1" "Started MYBOOT test kernel"; then
        result "$1" 0 "kernel ran"
    elif ! in_log "$1" "$2"; then
        result "$1" 0 "log lacks: $2 (qemu exit $code)"
    else
        result "$1" 1
    fi
}

# Byte offset of the first PT_LOAD program header in an ELF64 file.
first_load() {
    phoff=$(od -An -t u8 -j 32 -N 8 "$1" | tr -d ' ')
    phnum=$(od -An -t u2 -j 56 -N 2 "$1" | tr -d ' ')
    i=0
    while [ "$i" -lt "$phnum" ]; do
        off=$((phoff + i * 56))
        if [ "$(od -An -t u4 -j "$off" -N 4 "$1" | tr -d ' ')" = 1 ]; then
            echo "$off"
            return
        fi
        i=$((i + 1))
    done
}

# poke64 FILE OFFSET VALUE: overwrite a little-endian u64 in place.
poke64() {
    v=$3
    bytes=
    for _ in 1 2 3 4 5 6 7 8; do
        bytes="$bytes\\$(printf '%03o' $((v & 255)))"
        v=$((v >> 8))
    done
    printf "$bytes" | dd of="$1" bs=1 seek="$2" conv=notrunc 2>/dev/null
}

echo "MYBOOT scenarios, $CONFIG build"

# The shipped configuration (BOOT.CFG with a module), with the timeout cut
# to 1 s so the menu and its countdown run without slowing the suite.
d=$(esp shipped)
sed 's/^timeout .*/timeout 1/' kernel/boot.cfg > "$d/BOOT.CFG"
cp "$MODULE" "$d/SELFTEST.MOD"
expect_boot shipped "MYBOOT boot menu" "boot entry     : 1 (Test kernel with self-test module)" \
    "selftest at" "Booting 'Test kernel with self-test module'" \
    "Loaded kernel \\EFI\\MYBOOT\\KERNEL.ELF (" \
    "Loaded module \\EFI\\MYBOOT\\SELFTEST.MOD (170 bytes)" \
    "Command line: selftest modules=1" "Starting kernel ..." \
    "Read configuration \\EFI\\MYBOOT\\BOOT.CFG (2 entries)" \
    "Finished self-test: 31 checks passed, 0 failed"

# No BOOT.CFG: the built-in default entry, no modules.
esp no-config > /dev/null
expect_boot no-config "boot entry     : none (no BOOT.CFG)" "modules        : 0"

# default selects the second entry; relative and absolute paths, '/' separators.
d=$(esp default-entry)
cat > "$d/BOOT.CFG" <<'EOF'
default 2
entry first
kernel KERNEL.ELF
cmdline modules=0
entry second
kernel /EFI/MYBOOT/KERNEL.ELF
cmdline selftest modules=2
module SELFTEST.MOD selftest
module \EFI\MYBOOT\SELFTEST.MOD
EOF
cp "$MODULE" "$d/SELFTEST.MOD"
expect_boot default-entry "boot entry     : 2 (second)" "\\EFI\\MYBOOT\\SELFTEST.MOD at"

# Fallback: the first entry's kernel is missing, the second boots.
d=$(esp fallback-kernel)
cat > "$d/BOOT.CFG" <<'EOF'
entry missing kernel
kernel MISSING.ELF
entry good
kernel KERNEL.ELF
cmdline modules=0
EOF
expect_boot fallback-kernel "cannot open kernel: \\EFI\\MYBOOT\\MISSING.ELF" \
    "Loading kernel \\EFI\\MYBOOT\\MISSING.ELF" \
    "Falling back to entry 2: 'good'" "boot entry     : 2 (good)"

# Fallback: the first entry's module is missing; its kernel must be freed
# and the second entry loads cleanly.
d=$(esp fallback-module)
cat > "$d/BOOT.CFG" <<'EOF'
entry missing module
kernel KERNEL.ELF
module SELFTEST.MOD selftest
module NOPE.MOD
entry good
kernel KERNEL.ELF
cmdline modules=1
module SELFTEST.MOD selftest
EOF
cp "$MODULE" "$d/SELFTEST.MOD"
expect_boot fallback-module "cannot open module: \\EFI\\MYBOOT\\NOPE.MOD" "boot entry     : 2 (good)"

# Config errors are reported with line numbers and do not stop the boot.
# Five errors: the first four are shown, the fifth (line 8) is counted.
d=$(esp config-errors)
cat > "$d/BOOT.CFG" <<'EOF'
timeout soon
kernel KERNEL.ELF
entry no kernel here
bogus directive
entry good
kernel KERNEL.ELF
cmdline modules=0
default 7
EOF
expect_boot config-errors "BOOT.CFG line 1: timeout needs a number of seconds" \
    "BOOT.CFG line 2: not inside an entry" "BOOT.CFG line 4: unknown directive" \
    "BOOT.CFG line 3: entry has no kernel, linux or efi line" "BOOT.CFG: 1 more error(s) not shown" \
    "boot entry     : 1 (good)"

# A BOOT.CFG with no usable entry falls back to the built-in default.
d=$(esp config-empty)
printf '# nothing here\n' > "$d/BOOT.CFG"
expect_boot config-empty "BOOT.CFG has no usable entry" "boot entry     : none (no BOOT.CFG)"

# Interactive menu. The long timeout only matters if a key is lost: the
# default (entry 1) then boots and the case fails on the entry check.
two_entries() {
    cat > "$1/BOOT.CFG" <<'EOF'
timeout 30
entry first
kernel KERNEL.ELF
cmdline modules=0
entry second
kernel KERNEL.ELF
cmdline modules=0
EOF
}

d=$(esp menu-arrows)
two_entries "$d"
KEYS='down ret' expect_boot menu-arrows "boot entry     : 2 (second)"

d=$(esp menu-digit)
two_entries "$d"
KEYS='2' expect_boot menu-digit "boot entry     : 2 (second)"

# Edit the command line "modules=0": Backspace, retype and append, boot.
d=$(esp menu-edit)
two_entries "$d"
KEYS='e backspace 0 spc e d i t e d ret' expect_boot menu-edit \
    "cmdline        : modules=0 edited" "boot entry     : 1 (first)"

# Recovery screen: every entry fails; Up twice from the first entry wraps
# to "Power off" (above "Return to firmware"), which must shut QEMU down.
d=$(esp recovery-poweroff)
cat > "$d/BOOT.CFG" <<'EOF'
entry a
kernel A.ELF
entry b
kernel B.ELF
EOF
boot_keys recovery-poweroff up up ret
if [ "$code" -ne 0 ]; then
    result recovery-poweroff 0 "qemu exit $code, expected 0 (power off)"
elif ! in_log recovery-poweroff "MYBOOT: no entry could be loaded"; then
    result recovery-poweroff 0 "no recovery screen"
elif in_log recovery-poweroff "Interactive Shell"; then
    result recovery-poweroff 0 "returned to the firmware instead of powering off"
else
    result recovery-poweroff 1
fi

# "Return to firmware" from the normal menu: Up from the first entry.
d=$(esp menu-return)
two_entries "$d"
poweroff menu-return
boot_keys menu-return up ret
if in_log menu-return "Started MYBOOT test kernel"; then
    result menu-return 0 "kernel ran"
elif ! in_log menu-return "Interactive Shell"; then
    result menu-return 0 "did not return to the firmware (qemu exit $code)"
else
    result menu-return 1
fi

# Chainloading an EFI program (tests/efi/chaintest.c): it must receive the
# command line as load options and its volume as DeviceHandle, then ends
# QEMU with status 67.
CHAINTEST=${CHAINTEST:-build/$CONFIG/chaintest/CHAINTEST.EFI}
expect_chain() {
    name=$1
    shift
    run_case "$name"
    if [ "$code" -ne 67 ]; then
        result "$name" 0 "qemu exit $code, expected 67 (chaintest ran)"
        return
    fi
    for t in "$@"; do
        in_log "$name" "$t" || { result "$name" 0 "log lacks: $t"; return; }
    done
    result "$name" 1
}

d=$(esp chain-efi)
mkdir -p "$OUT/chain-efi/EFI/TEST"
cp "$CHAINTEST" "$OUT/chain-efi/EFI/TEST/CHAINTEST.EFI"
cat > "$d/BOOT.CFG" <<'EOF'
entry Chainload test
efi /EFI/TEST/CHAINTEST.EFI
cmdline hello from myboot
EOF
expect_chain chain-efi "Loaded EFI program \\EFI\\TEST\\CHAINTEST.EFI (" \
    "Starting EFI program ..." "chaintest: running, load options: hello from myboot" \
    "chaintest: device handle set"

# A program that returns: no fallback to the next entry (that would boot a
# different system behind the user's back); the recovery screen appears.
d=$(esp chain-return)
mkdir -p "$OUT/chain-return/EFI/TEST"
cp "$CHAINTEST" "$OUT/chain-return/EFI/TEST/CHAINTEST.EFI"
cat > "$d/BOOT.CFG" <<'EOF'
entry Returning program
efi /EFI/TEST/CHAINTEST.EFI
cmdline return
entry Kernel that must not boot
kernel KERNEL.ELF
EOF
expect_refusal chain-return "the EFI program returned to the loader"

# Windows Boot Manager is found on its own and listed after the entries.
d=$(esp windows-detect)
mkdir -p "$OUT/windows-detect/EFI/Microsoft/Boot"
cp "$CHAINTEST" "$OUT/windows-detect/EFI/Microsoft/Boot/bootmgfw.efi"
two_entries "$d"
KEYS='3' expect_chain windows-detect "Booting 'Windows Boot Manager'" \
    "Loaded EFI program \\EFI\\Microsoft\\Boot\\bootmgfw.efi ("

# auto_windows no: not listed; '3' then means nothing and Enter boots entry 1.
d=$(esp windows-off)
mkdir -p "$OUT/windows-off/EFI/Microsoft/Boot"
cp "$CHAINTEST" "$OUT/windows-off/EFI/Microsoft/Boot/bootmgfw.efi"
two_entries "$d"
sed -i '1i auto_windows no' "$d/BOOT.CFG"
boot_keys windows-off 3 ret
if [ "$code" -ne 33 ]; then
    result windows-off 0 "qemu exit $code, expected 33"
elif in_log windows-off "Windows Boot Manager"; then
    result windows-off 0 "Windows Boot Manager was listed"
elif ! in_log windows-off "boot entry     : 1 (first)"; then
    result windows-off 0 "entry 1 did not boot"
else
    result windows-off 1
fi

# Linux through its EFI stub, with an initrd over LoadFile2. Needs a real
# kernel (default: the WSL one); its /init may fail outside its own
# environment, so success is the kernel unpacking our initrd and running
# /init. panic=-1 then ends QEMU.
if [ -f "${LINUX_KERNEL:-}" ] && [ -f "${LINUX_INITRD:-}" ]; then
    d=$(esp linux)
    cp "$LINUX_KERNEL" "$d/VMLINUZ"
    cp "$LINUX_INITRD" "$d/INITRD.IMG"
    cat > "$d/BOOT.CFG" <<'EOF'
entry Linux
linux VMLINUZ
initrd INITRD.IMG
cmdline console=ttyS0 panic=-1
EOF
    mem=$QEMU_MEM
    QEMU_MEM=1G
    boot linux
    QEMU_MEM=$mem
    if ! in_log linux "Linux version"; then
        result linux 0 "Linux did not start (qemu exit $code)"
    elif ! in_log linux "Command line: console=ttyS0 panic=-1"; then
        result linux 0 "Linux did not get the command line"
    elif ! in_log linux "Freeing initrd memory"; then
        result linux 0 "Linux did not receive the initrd"
    elif ! in_log linux "Run /init as init process"; then
        result linux 0 "Linux did not run /init from the initrd"
    else
        result linux 1
    fi
else
    echo "SKIP  linux: set LINUX_KERNEL and LINUX_INITRD to an EFI-stub kernel and initrd"
fi

# Files on ext2/3/4 partitions of a second disk, written by
# tests/tools/mkext4.py: GPT + ext4 with fragmented files (extent index
# nodes) and a symlinked kernel, MBR + ext2 with block maps and meta_bg.
PYTHON=${PYTHON:-python}
EXT4_IMG=$OUT/ext4-disk.img
EXT2_IMG=$OUT/ext2-disk.img
EXT4_UUID=0a0b0c0d-1111-4222-8333-444455556666
EXT4_PARTUUID=7e57d15c-0000-4000-8000-00000000babe
linux_files=
set --
if [ -f "${LINUX_KERNEL:-}" ] && [ -f "${LINUX_INITRD:-}" ]; then
    linux_files=yes
    set -- "/boot/vmlinuz=$LINUX_KERNEL" "/boot/initrd.img=$LINUX_INITRD"
fi
# MSYS_NO_PATHCONV: Git Bash would otherwise turn "/boot/..." into Windows paths.
MSYS_NO_PATHCONV=1 "$PYTHON" tests/tools/mkext4.py "$EXT4_IMG" --wrap gpt --fragment 3 \
    --label myboot-ext4 --uuid "$EXT4_UUID" --partuuid "$EXT4_PARTUUID" \
    /boot/kernel.elf="$KERNEL" /boot/selftest.mod="$MODULE" "$@" \
    --link /vmlinuz-myboot=boot/kernel.elf
MSYS_NO_PATHCONV=1 "$PYTHON" tests/tools/mkext4.py "$EXT2_IMG" --wrap mbr --partuuid 0badf00d \
    --ext2 --meta-bg --block-size 1024 --blocks-per-group 256 --inodes-per-group 16 \
    --fragment 5 --size 40 /boot/kernel.elf="$KERNEL" /boot/selftest.mod="$MODULE"

d=$(esp ext4-label)
cat > "$d/BOOT.CFG" <<'EOF'
entry Kernel on ext4
volume label=myboot-ext4
kernel /vmlinuz-myboot
cmdline selftest modules=1
module /boot/selftest.mod selftest
EOF
DISK=$EXT4_IMG expect_boot ext4-label \
    "Found volume label=myboot-ext4 (ext4, PARTUUID $EXT4_PARTUUID)" \
    "Loaded kernel /vmlinuz-myboot (" "Loaded module /boot/selftest.mod (170 bytes)" \
    "Finished self-test: 31 checks passed, 0 failed"
DISK=

d=$(esp ext2-partuuid)
cat > "$d/BOOT.CFG" <<'EOF'
entry Kernel on ext2
volume partuuid=0BADF00D-01
kernel /boot/kernel.elf
cmdline selftest modules=1
module /boot/selftest.mod selftest
EOF
DISK=$EXT2_IMG expect_boot ext2-partuuid \
    "Found volume partuuid=0BADF00D-01 (ext4, PARTUUID 0badf00d-01)" \
    "Loaded kernel /boot/kernel.elf (" "Finished self-test: 31 checks passed, 0 failed"
DISK=

# An entry whose volume is missing falls back, and so does one whose file
# is missing (names are case-sensitive on ext4); the third is found by UUID.
d=$(esp volume-fallback)
cat > "$d/BOOT.CFG" <<EOF
entry Missing disk
volume uuid=deadbeef-0000-0000-0000-000000000000
kernel /boot/kernel.elf
entry Missing file
volume uuid=$EXT4_UUID
kernel /boot/KERNEL.ELF
entry Found by uuid
volume uuid=$EXT4_UUID
kernel /boot/kernel.elf
cmdline modules=0
EOF
DISK=$EXT4_IMG expect_boot volume-fallback \
    "no volume matches: uuid=deadbeef-0000-0000-0000-000000000000 (not found)" \
    "cannot open kernel: /boot/KERNEL.ELF (not found)" "boot entry     : 3 (Found by uuid)"
DISK=

# Linux, its initrd and both read from ext4, split into many extents.
if [ -n "$linux_files" ]; then
    d=$(esp ext4-linux)
    cat > "$d/BOOT.CFG" <<EOF
entry Linux from ext4
volume partuuid=$EXT4_PARTUUID
linux /boot/vmlinuz
initrd /boot/initrd.img
cmdline console=ttyS0 panic=-1
EOF
    mem=$QEMU_MEM
    QEMU_MEM=1G
    DISK=$EXT4_IMG boot ext4-linux
    DISK=
    QEMU_MEM=$mem
    if ! in_log ext4-linux "Loaded Linux /boot/vmlinuz ("; then
        result ext4-linux 0 "kernel not loaded from ext4 (qemu exit $code)"
    elif ! in_log ext4-linux "Freeing initrd memory"; then
        result ext4-linux 0 "Linux did not receive the initrd"
    elif ! in_log ext4-linux "Run /init as init process"; then
        result ext4-linux 0 "Linux did not run /init from the initrd"
    else
        result ext4-linux 1
    fi
else
    echo "SKIP  ext4-linux: set LINUX_KERNEL and LINUX_INITRD"
fi

# Broken kernels: the loader must refuse each one with a precise message.
esp missing-kernel > /dev/null
rm "$OUT/missing-kernel/EFI/MYBOOT/KERNEL.ELF"
expect_refusal missing-kernel "cannot open kernel: \\EFI\\MYBOOT\\KERNEL.ELF"

d=$(esp garbage-kernel)
yes MYBOOT-garbage | head -c 5000 > "$d/KERNEL.ELF"
expect_refusal garbage-kernel "invalid kernel: not an ELF file"

d=$(esp truncated-kernel)
head -c 100 "$KERNEL" > "$d/KERNEL.ELF"
expect_refusal truncated-kernel "invalid kernel: bad program headers"

d=$(esp segment-outside-file)
poke64 "$d/KERNEL.ELF" $(($(first_load "$KERNEL") + 8)) $((0x10000000))
expect_refusal segment-outside-file "invalid kernel: segment outside file"

d=$(esp low-address)
poke64 "$d/KERNEL.ELF" $(($(first_load "$KERNEL") + 16)) $((0x400000))
expect_refusal low-address "invalid kernel: segment below 0xFFFFFFFF80000000"

# Every entry broken: the loader gives up and returns to the firmware.
d=$(esp all-entries-fail)
cat > "$d/BOOT.CFG" <<'EOF'
entry a
kernel A.ELF
entry b
kernel B.ELF
EOF
expect_refusal all-entries-fail "no entry could be loaded"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
