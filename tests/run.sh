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
# options; the serial log goes to $OUT/NAME.log.
qemu() {
    name=$1
    shift
    vars=$OUT/$name.vars.fd
    cp "$QEMU_SHARE/edk2-i386-vars.fd" "$vars"
    timeout 90 "$QEMU" -machine q35 -m "$QEMU_MEM" -net none -no-reboot \
        -drive "if=pflash,format=raw,readonly=on,file=$QEMU_SHARE/edk2-x86_64-code.fd" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=virtio,readonly=on,format=raw,file=fat:$OUT/$name" \
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
    if [ -n "${KEYS:-}" ]; then
        # shellcheck disable=SC2086 # word splitting of KEYS is intended
        boot_keys "$name" $KEYS
    else
        boot "$name"
    fi
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
    if in_log "$1" "kernel: MYBOOT test kernel running"; then
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
    "Loading kernel \\EFI\\MYBOOT\\KERNEL.ELF ... " \
    "Loading module \\EFI\\MYBOOT\\SELFTEST.MOD ... " \
    "Command line: selftest modules=1" "Starting kernel ..."

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
    "Loading kernel \\EFI\\MYBOOT\\MISSING.ELF ... failed" \
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
    "BOOT.CFG line 3: entry has no kernel line" "BOOT.CFG: 1 more error(s) not shown" \
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
if in_log menu-return "kernel: MYBOOT test kernel running"; then
    result menu-return 0 "kernel ran"
elif ! in_log menu-return "Interactive Shell"; then
    result menu-return 0 "did not return to the firmware (qemu exit $code)"
else
    result menu-return 1
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
