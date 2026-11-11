#!/usr/bin/env python3
import math
import os
import random
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "host"))
from fuzz import asan_env  # noqa: E402

DEC = os.path.abspath("build/host/jpegdec.exe")
DEC_ASAN = os.path.abspath("build/host/jpegdec-asan.exe")
OUT = "build/host/jpeg"
BACKGROUND = "kernel/background.jpg"

CASES = {
    "420-64x48": ["64", "48"],
    "420-odd": ["37", "23"],
    "420-1x1": ["1", "1"],
    "444": ["40", "24", "--sampling", "1x1"],
    "422": ["41", "17", "--sampling", "2x1"],
    "440": ["19", "35", "--sampling", "1x2"],
    "grey": ["33", "21", "--grey"],
    "restart-1": ["48", "40", "--restart", "1"],
    "restart-3": ["70", "30", "--restart", "3", "--sampling", "2x1"],
    "grey-restart": ["30", "30", "--grey", "--restart", "2"],
    "q16": ["32", "32", "--q16"],
    "fine": ["48", "32", "--quality", "1"],
    "coarse": ["48", "32", "--quality", "20"],
}

def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3]

def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 24, "unexpected %d-bit BMP" % bpp
    stride = (w * 3 + 3) & ~3
    out = bytearray()
    rows = range(h - 1, -1, -1) if h > 0 else range(-h)
    for y in rows:
        row = d[off + y * stride: off + y * stride + w * 3]
        for x in range(w):
            b, g, r = row[3 * x: 3 * x + 3]
            out += bytes((r, g, b))
    return w, abs(h), bytes(out)

def psnr_of(mse):
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)

def luma_of(px, i):
    return 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]

def compare(name, jpg):
    r = subprocess.run([DEC, jpg, jpg + ".ppm"], capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        return "decoder: exit %d %s" % (r.returncode, r.stdout.strip())
    w, h, ours = read_ppm(jpg + ".ppm")
    rw, rh, ref = read_bmp(jpg + ".ref.bmp")
    if (w, h) != (rw, rh):
        return "size %dx%d, GDI+ says %dx%d" % (w, h, rw, rh)
    diff = [abs(a - b) for a, b in zip(ours, ref)]
    mad = sum(diff) / len(diff)
    psnr = psnr_of(sum(d * d for d in diff) / len(diff))
    luma = psnr_of(sum((luma_of(ours, i) - luma_of(ref, i)) ** 2
                       for i in range(0, len(ours), 3)) / (w * h))
    detail = "%dx%d, mean diff %.2f, PSNR %.1f dB, luma %.1f dB" % (w, h, mad, psnr, luma)
    exact = name.startswith(("444", "grey", "420-1x1", "background"))
    if luma <= 45 or psnr <= (35 if exact else 25) or (exact and mad >= 2):
        return detail
    print("PASS  %-14s %s" % (name, detail))
    return None

def refusals():
    good = open(os.path.join(OUT, "420-64x48.jpg"), "rb").read()
    sof = good.index(b"\xFF\xC0")
    cases = {
        "progressive": good[:sof] + b"\xFF\xC2" + good[sof + 2:],
        "arithmetic": good[:sof] + b"\xFF\xC9" + good[sof + 2:],
        "12-bit": good[:sof + 4] + b"\x0C" + good[sof + 5:],
        "no-soi": good[2:],
        "truncated-header": good[:sof + 6],
        "empty": b"",
    }
    return cases

def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    failures = 0

    jpgs = {}
    for name, args in CASES.items():
        path = os.path.join(OUT, name + ".jpg")
        subprocess.run([sys.executable, "tests/tools/mkjpeg.py", path] + args, check=True)
        jpgs[name] = path
    if os.path.exists(BACKGROUND):
        jpgs["background"] = os.path.join(OUT, "background.jpg")
        shutil.copy(BACKGROUND, jpgs["background"])
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        "tests/tools/gdiplus-decode.ps1"] + list(jpgs.values()),
                       capture_output=True, text=True, check=True)
    for line in r.stdout.splitlines():
        if line.startswith("ERROR"):
            print("FAIL  GDI+ could not decode a test image: " + line)
            failures += 1
    for name, path in jpgs.items():
        why = compare(name, path)
        if why:
            print("FAIL  %-14s %s" % (name, why))
            failures += 1

    for name, data in refusals().items():
        path = os.path.join(OUT, "refuse-" + name + ".jpg")
        open(path, "wb").write(data)
        r = subprocess.run([DEC, path], capture_output=True, text=True, timeout=20)
        if r.returncode == 1 and r.stdout.startswith("ERROR"):
            print("PASS  refuse %-8s %s" % (name, r.stdout.strip()[6:]))
        else:
            print("FAIL  refuse %s: exit %d %s" % (name, r.returncode, r.stdout.strip()))
            failures += 1

    env = asan_env()
    seeds = [open(p, "rb").read() for p in jpgs.values() if os.path.getsize(p) < 64 * 1024]
    crashes = 0
    for it in range(iterations):
        img = bytearray(rng.choice(seeds))
        for _ in range(rng.randint(1, 10)):
            pos = rng.randrange(min(len(img), 700)) if rng.random() < 0.7 else rng.randrange(len(img))
            img[pos] = rng.choice([0, 0xFF, 0x80, 0x7F, 1, 0x10, rng.randrange(256)])
        if rng.random() < 0.1:
            img = img[:rng.randrange(len(img))]
        path = os.path.join(OUT, "fuzz.jpg")
        open(path, "wb").write(img)
        try:
            r = subprocess.run([DEC_ASAN, path, path + ".ppm"], capture_output=True, text=True,
                               timeout=20, env=env)
            bad = r.returncode not in (0, 1)
            why = "exit %d %s\n%s" % (r.returncode, r.stdout.strip(), r.stderr[-3000:])
        except subprocess.TimeoutExpired:
            bad, why = True, "timeout (hang)"
        if bad:
            crashes += 1
            keep = os.path.join(OUT, "crash-%d.jpg" % crashes)
            shutil.copy(path, keep)
            print("FAIL  fuzz iteration %d: %s (kept %s)" % (it, why, keep))
            if crashes >= 5:
                break
    print("%s  fuzz: %d corrupted images" % ("FAIL" if crashes else "PASS", iterations))
    failures += crashes
    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
