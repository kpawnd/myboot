#!/bin/sh
# Screenshots of the loader for README.md; run through `make screenshots`.
#
# Boots the built ESP in QEMU with the monitor on stdin, waits for the menu
# on the serial log, and saves PNGs with the monitor's screendump:
#
#   menu.png       the menu over the BOOT.CFG background picture
#   menu-text.png  the same menu without a background (text console)
#   editor.png     the command-line editor ('e')
#   kernel-NN.png  the test kernel's screen, every 0.2 s until it exits;
#                  pick the frame to keep by hand
#
# Output goes to $OUT (build/$CONFIG/screenshots).

set -u
CONFIG=${CONFIG:-balanced}
QEMU=${QEMU:-qemu-system-x86_64}
QEMU_SHARE=${QEMU_SHARE:-C:/Program Files/qemu/share}
QEMU_SMP=${QEMU_SMP:-4}
ESP=build/$CONFIG/esp
OUT=${OUT:-build/$CONFIG/screenshots}

[ -f "$ESP/EFI/BOOT/BOOTX64.EFI" ] || { echo "missing $ESP: run make CONFIG=$CONFIG first"; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT"

# wait_menu LOG: until the menu's key help is on the serial log.
wait_menu() {
    i=0
    until [ -f "$1" ] && grep -aqF "Up/Down select" "$1"; do
        i=$((i + 1))
        [ "$i" -gt 600 ] && return 1
        sleep 0.1
    done
    sleep 1
}

shot() { echo "screendump $OUT/$1.png -f png"; sleep 0.5; }

# drive NAME: the monitor commands for one boot of $OUT/NAME.
drive() {
    log=$OUT/$1.log
    wait_menu "$log" || return
    if [ "$1" = graphical ]; then
        shot menu
        echo "sendkey e"
        sleep 1
        shot editor
        echo "sendkey esc"
        sleep 1
        echo "sendkey ret"
        n=10
        while echo "screendump $OUT/kernel-$n.png -f png"; do
            n=$((n + 1))
            sleep 0.2
        done
    else
        shot menu-text
        echo "quit"
    fi
}

# boot NAME: copy the ESP to $OUT/NAME, optionally without the background.
boot() {
    d=$OUT/$1
    cp -r "$ESP" "$d"
    [ "$1" = text ] && sed -i '/^background /d' "$d/EFI/MYBOOT/BOOT.CFG"
    vars=$OUT/$1.vars.fd
    cp "$QEMU_SHARE/edk2-i386-vars.fd" "$vars"
    drive "$1" | timeout 90 "$QEMU" -machine q35 -m 256M -smp "$QEMU_SMP" -net none -no-reboot \
        -drive "if=pflash,format=raw,readonly=on,file=$QEMU_SHARE/edk2-x86_64-code.fd" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=virtio,readonly=on,format=raw,file=fat:$d" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -display none -serial "file:$OUT/$1.log" -monitor stdio > /dev/null 2>&1
}

boot graphical
boot text
ls "$OUT"/*.png
