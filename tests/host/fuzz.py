#!/usr/bin/env python3
"""
Corrupts ext2/ext4 test images at random and reads them with the driver
built with AddressSanitizer and UBSan (build/host/ext4cat-asan.exe). The
driver may refuse a corrupt image, but must never crash, read out of
bounds or hang.

  fuzz.py [ITERATIONS] [SEED]

A failing image is kept as build/host/fuzz/crash-<n>.img.
"""
import glob
import os
import random
import shutil
import subprocess
import sys

HARNESS = os.path.abspath("build/host/ext4cat-asan.exe")
OUT = "build/host/fuzz"
PATHS = ["/boot/kernel.elf", "/boot/selftest.mod", "/holes.bin", "/links/long",
         "/links/chain", "/links/dir/file-with-a-longer-name-150", "/links/loop1",
         "/many/file-with-a-longer-name-299", "/lost+found", "/nope/x"]


def asan_env():
    """The sanitizer runtime is a DLL next to clang's other libraries."""
    env = dict(os.environ)
    llvm = os.environ.get("LLVM_DIR", "C:/Program Files/LLVM")
    for dll in glob.glob(llvm + "/lib/clang/*/lib/windows/clang_rt.asan_dynamic-x86_64.dll"):
        env["PATH"] = os.path.dirname(dll) + os.pathsep + env["PATH"]
    return env


def main():
    env = asan_env()
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    with open(os.path.join(OUT, "list"), "w", newline="\n") as f:
        f.write("\n".join(PATHS) + "\n")
    payload = ["/boot/kernel.elf=build/kernel/kernel.elf", "/boot/selftest.mod=kernel/selftest.mod",
               "--extras", "--size", "8"]
    bases = {
        "ext4": ["--fragment", "2", "--blocks-per-group", "512", "--inodes-per-group", "128"],
        "ext2": ["--ext2", "--meta-bg", "--block-size", "1024", "--blocks-per-group", "128",
                 "--inodes-per-group", "16", "--fragment", "3"],
    }
    failures = 0
    for name, args in bases.items():
        base = os.path.join(OUT, name + ".img")
        subprocess.run([sys.executable, "tests/tools/mkext4.py", base] + payload + args, check=True)
        orig = open(base, "rb").read()
        # Bytes that differ from zero in the first part of the image are
        # mostly metadata: superblock, descriptors, inodes, directories,
        # extent nodes and indirect blocks.
        hot = [i for i in range(min(len(orig), 2 * 1024 * 1024)) if orig[i]]
        for it in range(iterations):
            img = bytearray(orig)
            for _ in range(rng.randint(1, 12)):
                pos = rng.choice(hot) if rng.random() < 0.9 else rng.randrange(len(img))
                img[pos] = rng.choice([0, 0xFF, 0x80, 0x7F, 1, rng.randrange(256)])
            path = os.path.join(OUT, "case.img")
            open(path, "wb").write(img)
            try:
                r = subprocess.run([HARNESS, path, "0", str(len(img)), os.path.join(OUT, "list"),
                                    OUT], capture_output=True, text=True, timeout=20, env=env)
                bad = r.returncode not in (0, 1)
                why = "exit %d\n%s" % (r.returncode, r.stderr[-3000:])
            except subprocess.TimeoutExpired:
                bad, why = True, "timeout (hang)"
            if bad:
                failures += 1
                keep = os.path.join(OUT, "crash-%d.img" % failures)
                shutil.copy(path, keep)
                print("FAIL  %s iteration %d: %s (kept %s)" % (name, it, why, keep))
                if failures >= 5:
                    sys.exit(1)
        print("%s  fuzz %s: %d corrupted images" % ("FAIL" if failures else "PASS", name, iterations))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
