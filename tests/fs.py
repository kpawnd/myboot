#!/usr/bin/env python3
"""
Host tests for the filesystem driver (src/fs/ext4.c), run by `make test-fs`.

Each image is read twice: by 7-Zip, an independent ext2/3/4 reader, and by
build/host/ext4cat.exe, which is the loader's own driver compiled for the
host. Every regular file, and every symlink whose target is a regular file,
must read back identically. Paths that differ only in case are skipped,
because the extraction goes to a case-insensitive Windows filesystem.

Images: ext4 and ext2 layouts written by tests/tools/mkext4.py, and the
ext4 image WSL ships (C:/Program Files/WSL/tools/artifacts.vhd) when it is
installed: one made by the real mkfs.
"""
import os
import posixpath
import shutil
import subprocess
import sys

SEVENZIP = os.environ.get("SEVENZIP", "C:/Program Files/7-Zip/7z.exe")
HARNESS = os.path.abspath("build/host/ext4cat.exe")
OUT = "build/host/fs"
WSL_VHD = "C:/Program Files/WSL/tools/artifacts.vhd"


def listing(image):
    text = subprocess.run([SEVENZIP, "l", "-slt", image], capture_output=True, text=True,
                          encoding="utf-8", check=True).stdout
    body = text.split("\n----------\n", 1)[1]
    entries = []
    for block in body.split("\n\n"):
        e = {}
        for line in block.splitlines():
            if " = " in line:
                k, v = line.split(" = ", 1)
                e[k] = v
            elif line.endswith(" ="):
                e[line[:-2]] = ""
        if "Path" in e:
            e["Path"] = e["Path"].replace("\\", "/")
            entries.append(e)
    return entries


def resolve(path, files, links, depth=0):
    """Follows symlinks the way the driver should; None if it leads nowhere."""
    if depth > 8:
        return None
    parts = [p for p in path.split("/") if p]
    for i in range(len(parts)):
        head = "/".join(parts[:i + 1])
        if head in links:
            target = links[head]
            base = "" if target.startswith("/") else posixpath.dirname(head)
            rest = "/".join(parts[i + 1:])
            new = posixpath.normpath(posixpath.join("/", base, target, rest)).lstrip("/")
            return resolve(new, files, links, depth + 1)
    return path if path in files else None


def check(name, image, offset, size, truth=None):
    """`truth`: an image with the same files that 7-Zip reads, for layouts
    7-Zip does not understand (meta_bg)."""
    work = os.path.join(OUT, name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "x"))
    os.makedirs(os.path.join(work, "ours"))
    entries = listing(truth or image)
    files = {e["Path"] for e in entries if e.get("Folder") == "-" and e.get("Mode", "-").startswith("-")}
    links = {e["Path"]: e["Symbolic Link"] for e in entries if e.get("Mode", "").startswith("l")}
    lower = {}
    for p in files:
        lower.setdefault(p.lower(), []).append(p)
    clash = {p for group in lower.values() if len(group) > 1 for p in group}

    # Symlinks cannot be created without privileges on Windows; 7-Zip then
    # exits with 2 but extracts every regular file.
    r = subprocess.run([SEVENZIP, "x", "-y", "-o" + os.path.join(work, "x"), truth or image],
                       capture_output=True, text=True)
    if r.returncode not in (0, 2):
        print("FAIL  %-14s 7-Zip cannot extract it:\n%s" % (name, r.stdout[-2000:]))
        return False
    # When 7-Zip opens a container (VHD), files land under the inner image's name.
    root = os.path.join(work, "x")
    inner = [d for d in os.listdir(root) if d.endswith(".ext")]
    if inner and os.path.isdir(os.path.join(root, inner[0])):
        root = os.path.join(root, inner[0])

    cases = []  # (path asked for, file whose content it must have)
    for p in sorted(files - clash):
        cases.append((p, p))
    for p in sorted(links):
        t = resolve(p, files, links)
        if t and t not in clash:
            cases.append((p, t))
    with open(os.path.join(work, "list"), "w", newline="\n") as f:
        for asked, _ in cases:
            f.write("/" + asked + "\n")

    r = subprocess.run([HARNESS, image, str(offset), str(size), os.path.join(work, "list"),
                        os.path.join(work, "ours")], capture_output=True, text=True)
    errors = {}
    for line in r.stdout.splitlines():
        idx, _, msg = line.partition(" ERROR ")
        errors[idx] = msg
    bad = []
    for i, (asked, real) in enumerate(cases):
        if str(i) in errors:
            bad.append("%s: %s" % (asked, errors[str(i)]))
            continue
        with open(os.path.join(root, *real.split("/")), "rb") as f:
            want = f.read()
        with open(os.path.join(work, "ours", str(i)), "rb") as f:
            got = f.read()
        if got != want:
            bad.append("%s: content differs (%d bytes, expected %d)" % (asked, len(got), len(want)))
    if "mount" in errors:
        bad.insert(0, "mount: " + errors["mount"])
    print("%s  %-14s %d files, %d via symlinks, %d skipped (case clash)%s" % (
        "FAIL" if bad else "PASS", name, len(files - clash), len(cases) - len(files - clash),
        len(clash), "" if not bad else ""))
    for b in bad[:20]:
        print("      " + b)
    sys.stdout.flush()
    return not bad


def expect_errors(name, image, want):
    """Paths that must fail with a given error."""
    work = os.path.join(OUT, name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    with open(os.path.join(work, "list"), "w", newline="\n") as f:
        for path, _ in want:
            f.write(path + "\n")
    r = subprocess.run([HARNESS, image, "0", str(os.path.getsize(image)), os.path.join(work, "list"),
                        work], capture_output=True, text=True)
    got = {}
    for line in r.stdout.splitlines():
        idx, _, msg = line.partition(" ERROR ")
        got[idx] = msg
    bad = ["%s: got %r, expected %r" % (p, got.get(str(i), "success"), msg)
           for i, (p, msg) in enumerate(want) if got.get(str(i)) != msg]
    print("%s  %-14s %d error paths" % ("FAIL" if bad else "PASS", name, len(want)))
    for b in bad:
        print("      " + b)
    return not bad


def mkext4(out, *args):
    subprocess.run([sys.executable, "tests/tools/mkext4.py", out] + list(args), check=True)
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    kernel = "build/kernel/kernel.elf"
    big = os.environ.get("LINUX_KERNEL", "C:/Program Files/WSL/tools/kernel")
    payload = ["/boot/kernel.elf=" + kernel, "/boot/selftest.mod=kernel/selftest.mod"]
    if os.path.isfile(big):
        payload.append("/boot/vmlinuz=" + big)
    ok = True

    img = mkext4(os.path.join(OUT, "ext4.img"), "--extras", "--fragment", "3", *payload)
    ok &= check("ext4", img, 0, os.path.getsize(img))
    img = mkext4(os.path.join(OUT, "ext4-deep.img"), "--fragment", "1", "--block-size", "1024",
                 "--blocks-per-group", "8192", "--inodes-per-group", "64", *payload)
    ok &= check("ext4-1k-deep", img, 0, os.path.getsize(img))
    ext2 = ["--ext2", "--block-size", "1024", "--blocks-per-group", "256",
            "--inodes-per-group", "16", "--extras", "--fragment", "5"] + payload
    ref = mkext4(os.path.join(OUT, "ext2.img"), *ext2)
    ok &= check("ext2", ref, 0, os.path.getsize(ref))
    # 7-Zip ignores meta_bg: the same files without it are the reference.
    img = mkext4(os.path.join(OUT, "ext2-meta.img"), "--meta-bg", *ext2)
    ok &= check("ext2-meta-bg", img, 0, os.path.getsize(img), truth=ref)
    ok &= expect_errors("errors", os.path.join(OUT, "ext4.img"), [
        ("/links/loop1", "too many symlinks or path too long"),
        ("/nope", "not found"),
        ("/many", "not a regular file"),
        ("/holes.bin/x", "a path component is not a directory"),
        ("/links/dir/missing", "not found"),
    ])
    if os.path.isfile(WSL_VHD):
        ok &= check("wsl-vhd", WSL_VHD, 0, os.path.getsize(WSL_VHD) - 512)
    else:
        print("SKIP  wsl-vhd: %s not installed" % WSL_VHD)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
