#!/usr/bin/env python3
"""
Writes small ext2/ext4 images for the loader's tests, without mkfs (the
build machine is Windows). The layouts are picked to exercise the reader
rather than to look like mke2fs output:

  --fragment N   files are split into extents of at most N blocks with a
                 gap after each, so long files need extent index nodes
                 (ext4) or long runs of indirect pointers (ext2)
  --ext2         block maps (direct, single, double, triple indirect)
                 instead of extents, 32-bit group descriptors
  --meta-bg      descriptors of every group past the first metagroup live
                 in that metagroup's first group
  --extras       a directory spanning several blocks, fast, slow, absolute,
                 relative, chained and looping symlinks, a symlink to a
                 directory, and a file with holes (and, on ext4, an
                 unwritten extent)

Inodes are spread over all block groups, so lookups visit many inode tables.

  mkext4.py OUT [options] [/path=HOSTFILE ...] [--link /path=TARGET ...]

--wrap gpt|mbr puts the filesystem in a partition at 1 MiB, with
--partuuid as the GPT partition GUID or the MBR disk signature (8 hex
digits); --wrap none writes the bare filesystem.
"""
import argparse
import struct
import sys
import uuid
import zlib

S_IFDIR, S_IFREG, S_IFLNK = 0x4000, 0x8000, 0xA000
FT = {S_IFREG: 1, S_IFDIR: 2, S_IFLNK: 7}
FL_EXTENTS = 0x80000
TIME = 1700000000


class Node:
    def __init__(self, kind, data=b"", target=b""):
        self.kind = kind
        self.data = data
        self.target = target
        self.children = {}
        self.holes = set()      # logical blocks never allocated
        self.unwritten = set()  # allocated but flagged unwritten (read as zeros)
        self.ino = 0


class Ext:
    def __init__(self, a):
        self.a = a
        self.bs = a.block_size
        self.ext4 = not a.ext2
        self.is64 = self.ext4
        self.inode_size = 256 if self.ext4 else 128
        self.desc_size = 64 if self.is64 else 32
        self.fdb = 1 if self.bs == 1024 else 0
        self.bpg = a.blocks_per_group
        self.ipg = a.inodes_per_group
        self.blocks = a.size * 1024 * 1024 // self.bs
        self.groups = (self.blocks - self.fdb + self.bpg - 1) // self.bpg
        self.img = bytearray(self.blocks * self.bs)
        self.per = self.bs // self.desc_size
        self.first_meta_bg = 1 if a.meta_bg else 0
        self.used_blocks = set()
        self.used_inodes = set()
        self.dir_inodes = set()
        self.layout()

    # ---- layout ---------------------------------------------------------

    def has_super(self, g):
        if g <= 1:
            return True
        for p in (3, 5, 7):
            x = p
            while x < g:
                x *= p
            if x == g:
                return True
        return False

    def gdt_blocks(self):
        if self.a.meta_bg:
            return self.first_meta_bg
        return (self.groups * self.desc_size + self.bs - 1) // self.bs

    def group_start(self, g):
        return self.fdb + g * self.bpg

    def group_end(self, g):
        return min(self.group_start(g) + self.bpg, self.blocks)

    def desc_off(self, g):
        if self.a.meta_bg and g // self.per >= self.first_meta_bg:
            first = g - g % self.per
            b = self.group_start(first) + (1 if self.has_super(first) else 0)
            return b * self.bs + (g % self.per) * self.desc_size
        return (self.fdb + 1) * self.bs + g * self.desc_size

    def layout(self):
        itb = self.ipg * self.inode_size // self.bs
        assert self.ipg * self.inode_size % self.bs == 0
        self.bb, self.ib, self.it, self.next = [], [], [], []
        for g in range(self.groups):
            cur = self.group_start(g)
            if self.has_super(g):
                cur += 1 + self.gdt_blocks()
            if self.a.meta_bg and g // self.per >= self.first_meta_bg and g % self.per == 0:
                cur += 1  # this metagroup's descriptor block (after any superblock)
            self.bb.append(cur)
            self.ib.append(cur + 1)
            self.it.append(cur + 2)
            cur += 2 + itb
            assert cur < self.group_end(g), "group too small for its metadata"
            for b in range(self.group_start(g), cur):
                self.used_blocks.add(b)
            self.next.append(cur)
        self.alloc_group = 0
        for i in range(1, 11):
            self.used_inodes.add(i)

    # ---- allocation -----------------------------------------------------

    def alloc(self, want, gap=False):
        """Up to `want` contiguous blocks: (first, count)."""
        while self.next[self.alloc_group] >= self.group_end(self.alloc_group):
            self.alloc_group += 1
            if self.alloc_group == self.groups:
                sys.exit("mkext4: image too small")
        g = self.alloc_group
        first = self.next[g]
        n = min(want, self.group_end(g) - first)
        self.next[g] = first + n + (1 if gap else 0)
        for b in range(first, first + n):
            self.used_blocks.add(b)
        return first, n

    def new_inode(self, k):
        first = (k * 7 + 3) % self.groups
        for g in [(first + j) % self.groups for j in range(self.groups)]:
            for i in range(self.ipg):
                ino = g * self.ipg + i + 1
                if ino not in self.used_inodes:
                    self.used_inodes.add(ino)
                    return ino
        sys.exit("mkext4: no free inode")

    def put(self, blk, data, off=0):
        p = blk * self.bs + off
        self.img[p:p + len(data)] = data

    # ---- file data ------------------------------------------------------

    def write_data(self, node):
        """Allocates and writes node.data; returns (i_block, blocks512)."""
        bs = self.bs
        n = (len(node.data) + bs - 1) // bs
        runs = []  # (lblk, pblk, count, unwritten)
        frag = self.a.fragment or 1 << 30
        lblk = 0
        while lblk < n:
            if lblk in node.holes:
                lblk += 1
                continue
            uw = lblk in node.unwritten and self.ext4
            want = 1
            limit = min(frag, 32767 if uw else 32768)
            while (lblk + want < n and want < limit and lblk + want not in node.holes and
                   (lblk + want in node.unwritten and self.ext4) == uw):
                want += 1
            first, got = self.alloc(want, gap=self.a.fragment > 0)
            for i in range(got):
                if not uw:
                    self.put(first + i, node.data[(lblk + i) * bs:(lblk + i + 1) * bs])
            runs.append((lblk, first, got, uw))
            lblk += got
        data_blocks = sum(r[2] for r in runs)
        if self.ext4:
            iblock, meta = self.extent_tree(runs)
        else:
            iblock, meta = self.block_map(runs, n)
        return iblock, (data_blocks + meta) * (bs // 512)

    def extent_tree(self, runs):
        cap = (self.bs - 12) // 12
        entries = [struct.pack("<IHHI", l, c + (32768 if uw else 0), p >> 32, p & 0xFFFFFFFF)
                   for l, p, c, uw in runs]
        firsts = [r[0] for r in runs]
        depth, meta = 0, 0
        while len(entries) > 4:
            new, new_firsts = [], []
            for i in range(0, len(entries), cap):
                chunk = entries[i:i + cap]
                blk, _ = self.alloc(1)
                meta += 1
                self.put(blk, struct.pack("<HHHHI", 0xF30A, len(chunk), cap, depth, 0) + b"".join(chunk))
                new.append(struct.pack("<IIHH", firsts[i], blk & 0xFFFFFFFF, blk >> 32, 0))
                new_firsts.append(firsts[i])
            entries, firsts = new, new_firsts
            depth += 1
        root = struct.pack("<HHHHI", 0xF30A, len(entries), 4, depth, 0) + b"".join(entries)
        return root.ljust(60, b"\0"), meta

    def block_map(self, runs, n):
        apb = self.bs // 4
        ptr = {}
        for l, p, c, _ in runs:
            for i in range(c):
                ptr[l + i] = p + i
        meta = [0]

        def table(level, base):
            span = apb ** (level - 1)
            t = [0] * apb
            for i in range(apb):
                start = base + i * span
                if start >= n:
                    break
                t[i] = ptr.get(start, 0) if level == 1 else table(level - 1, start)
            if not any(t):
                return 0
            blk, _ = self.alloc(1)
            meta[0] += 1
            self.put(blk, struct.pack("<%dI" % apb, *t))
            return blk

        direct = [ptr.get(i, 0) for i in range(12)]
        tops, base = [], 12
        for level in (1, 2, 3):
            tops.append(table(level, base) if base < n else 0)
            base += apb ** level
        assert n <= base, "file too large for a block map"
        return struct.pack("<15I", *(direct + tops)), meta[0]

    # ---- tree -----------------------------------------------------------

    def number(self, node, k=[0]):
        for name in sorted(node.children):
            child = node.children[name]
            k[0] += 1
            child.ino = self.new_inode(k[0])
            if child.kind == S_IFDIR:
                self.number(child, k)

    def dir_data(self, node, parent):
        ents = [(b".", node.ino, 2), (b"..", parent.ino, 2)]
        for name in sorted(node.children):
            c = node.children[name]
            ents.append((name.encode(), c.ino, FT[c.kind]))
        blocks, cur = [], []
        used = 0
        for name, ino, ft in ents:
            rec = (8 + len(name) + 3) & ~3
            if used + rec > self.bs:
                blocks.append(cur)
                cur, used = [], 0
            cur.append([name, ino, ft, rec])
            used += rec
        blocks.append(cur)
        out = b""
        for blk in blocks:
            blk[-1][3] += self.bs - sum(e[3] for e in blk)
            for name, ino, ft, rec in blk:
                out += struct.pack("<IHBB", ino, rec, len(name), ft) + name.ljust(rec - 8, b"\0")
        return out

    def write_tree(self, node, parent):
        if node.kind == S_IFDIR:
            self.dir_inodes.add(node.ino)
            for c in node.children.values():
                self.write_tree(c, node)
            node.data = self.dir_data(node, parent)
        flags = 0
        if node.kind == S_IFLNK and len(node.target) < 60:
            iblock, blocks512 = node.target.ljust(60, b"\0"), 0
            size = len(node.target)
        else:
            if node.kind == S_IFLNK:
                node.data = node.target
            iblock, blocks512 = self.write_data(node)
            size = len(node.data)
            flags = FL_EXTENTS if self.ext4 else 0
        links = 2 + sum(c.kind == S_IFDIR for c in node.children.values()) if node.kind == S_IFDIR else 1
        mode = node.kind | (0o755 if node.kind != S_IFLNK else 0o777)
        raw = struct.pack("<HHIIIIIHHII", mode, 0, size & 0xFFFFFFFF, TIME, TIME, TIME, 0, 0,
                          links, blocks512, flags)
        raw += b"\0" * 4 + iblock + struct.pack("<III", 0, 0, size >> 32)
        if self.inode_size > 128:
            raw = raw.ljust(128, b"\0") + struct.pack("<H", 32)
        g, idx = (node.ino - 1) // self.ipg, (node.ino - 1) % self.ipg
        self.put(self.it[g], raw, idx * self.inode_size)

    # ---- metadata -------------------------------------------------------

    def finish(self):
        a = self.a
        for g in range(self.groups):
            bb = bytearray(self.bs)
            start = self.group_start(g)
            for i in range(self.bs * 8):
                if start + i >= self.group_end(g) or start + i in self.used_blocks:
                    bb[i // 8] |= 1 << (i % 8)
            self.put(self.bb[g], bb)
            ib = bytearray(self.bs)
            for i in range(self.bs * 8):
                if i >= self.ipg or g * self.ipg + i + 1 in self.used_inodes:
                    ib[i // 8] |= 1 << (i % 8)
            self.put(self.ib[g], ib)
            start, end = self.group_start(g), self.group_end(g)
            free_b = sum(1 for b in range(start, end) if b not in self.used_blocks)
            free_i = sum(1 for i in range(self.ipg) if g * self.ipg + i + 1 not in self.used_inodes)
            dirs = sum(1 for i in self.dir_inodes if (i - 1) // self.ipg == g)
            d = struct.pack("<IIIHHH", self.bb[g], self.ib[g], self.it[g], free_b, free_i, dirs)
            p = self.desc_off(g)
            self.img[p:p + self.desc_size] = d.ljust(self.desc_size, b"\0")

        incompat = 0x2 | (0x40 | 0x200 if self.ext4 else 0) | (0x80 if self.is64 else 0) | \
            (0x10 if a.meta_bg else 0)
        ro = 0x1 | 0x2 | (0x8 | 0x20 | 0x40 if self.ext4 else 0)
        free_blocks = self.blocks - len(self.used_blocks)
        sb = bytearray(1024)
        struct.pack_into("<IIIIIIIIIIIIIHHHHHHIIIIHH", sb, 0,
                         self.groups * self.ipg, self.blocks & 0xFFFFFFFF, 0, free_blocks,
                         self.groups * self.ipg - len(self.used_inodes), self.fdb,
                         self.bs.bit_length() - 11, self.bs.bit_length() - 11, self.bpg, self.bpg,
                         self.ipg, TIME, TIME, 0, 0xFFFF, 0xEF53, 1, 1, 0, TIME, 0, 0, 1, 0, 0)
        struct.pack_into("<IHH", sb, 0x54, 11, self.inode_size, 0)
        struct.pack_into("<III", sb, 0x5C, 0, incompat, ro)
        sb[0x68:0x78] = uuid.UUID(a.uuid).bytes
        sb[0x78:0x88] = a.label.encode().ljust(16, b"\0")
        struct.pack_into("<H", sb, 0xFE, self.desc_size if self.is64 else 0)
        struct.pack_into("<I", sb, 0x104, self.first_meta_bg)
        struct.pack_into("<I", sb, 0x150, self.blocks >> 32)
        struct.pack_into("<HH", sb, 0x15C, 32 if self.ext4 else 0, 32 if self.ext4 else 0)
        self.img[1024:2048] = sb
        return bytes(self.img)


def extras(root, bs, ext4):
    many = Node(S_IFDIR)
    for i in range(300):
        name = "file-with-a-longer-name-%03d" % i
        many.children[name] = Node(S_IFREG, (name + "\n").encode())
    root.children["many"] = many

    holes = Node(S_IFREG)
    blocks = 40
    data = bytearray()
    for b in range(blocks):
        data += bytes([(b * 37 + i) & 0xFF for i in range(bs)])
    for b in list(range(5, 12)) + [20]:
        holes.holes.add(b)
        data[b * bs:(b + 1) * bs] = bytes(bs)
    if ext4:
        for b in range(25, 30):
            holes.unwritten.add(b)
            data[b * bs:(b + 1) * bs] = bytes(bs)
    holes.data = bytes(data[:-100])  # end mid-block
    root.children["holes.bin"] = holes

    links = Node(S_IFDIR)
    links.children["short"] = Node(S_IFLNK, target=b"../holes.bin")
    links.children["absolute"] = Node(S_IFLNK, target=b"/holes.bin")
    links.children["long"] = Node(S_IFLNK, target=b"./" * 40 + b"../many/./file-with-a-longer-name-299")
    links.children["chain"] = Node(S_IFLNK, target=b"short")
    links.children["loop1"] = Node(S_IFLNK, target=b"loop2")
    links.children["loop2"] = Node(S_IFLNK, target=b"loop1")
    links.children["dir"] = Node(S_IFLNK, target=b"../many")
    root.children["links"] = links


def wrap(fs, kind, partuuid):
    if kind == "none":
        return fs
    sector = 512
    start = 2048
    count = len(fs) // sector
    total = start + count + 2048
    img = bytearray(total * sector)
    img[start * sector:start * sector + len(fs)] = fs
    if kind == "mbr":
        struct.pack_into("<I", img, 440, int(partuuid, 16))
        struct.pack_into("<B3sB3sII", img, 446, 0, b"\0\2\0", 0x83, b"\xfe\xff\xff", start, count)
        img[510:512] = b"\x55\xaa"
        return bytes(img)
    # GPT: protective MBR, primary header and entries, backup at the end.
    struct.pack_into("<B3sB3sII", img, 446, 0, b"\0\2\0", 0xEE, b"\xfe\xff\xff", 1,
                     min(total - 1, 0xFFFFFFFF))
    img[510:512] = b"\x55\xaa"
    entry = (uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le +
             uuid.UUID(partuuid).bytes_le + struct.pack("<QQQ", start, start + count - 1, 0) +
             "myboot-test".encode("utf-16-le").ljust(72, b"\0"))
    entries = entry.ljust(128 * 128, b"\0")
    ecrc = zlib.crc32(entries)
    last = total - 1

    def header(my, alt, elba):
        h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, my, alt, 34,
                        last - 33, uuid.UUID("5a1d6d4e-0000-4000-8000-000000000001").bytes_le,
                        elba, 128, 128, ecrc)
        return h[:16] + struct.pack("<I", zlib.crc32(h)) + h[20:]

    img[sector:sector + 92] = header(1, last, 2)
    img[2 * sector:2 * sector + len(entries)] = entries
    img[(last - 32) * sector:(last - 32) * sector + len(entries)] = entries
    img[last * sector:last * sector + 92] = header(last, 1, last - 32)
    return bytes(img)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("files", nargs="*", help="/path=HOSTFILE")
    p.add_argument("--link", action="append", default=[], help="/path=TARGET")
    p.add_argument("--block-size", type=int, default=4096)
    p.add_argument("--blocks-per-group", type=int, default=2048)
    p.add_argument("--inodes-per-group", type=int, default=64)
    p.add_argument("--size", type=int, default=64, help="MiB")
    p.add_argument("--ext2", action="store_true")
    p.add_argument("--meta-bg", action="store_true")
    p.add_argument("--fragment", type=int, default=0)
    p.add_argument("--extras", action="store_true")
    p.add_argument("--label", default="")
    p.add_argument("--uuid", default="0a0b0c0d-1111-4222-8333-444455556666")
    p.add_argument("--wrap", choices=("none", "gpt", "mbr"), default="none")
    p.add_argument("--partuuid", default="7e57d15c-0000-4000-8000-00000000babe")
    a = p.parse_args()

    root = Node(S_IFDIR)
    root.children["lost+found"] = Node(S_IFDIR)

    def place(path, node):
        parts = [x for x in path.split("/") if x]
        d = root
        for part in parts[:-1]:
            d = d.children.setdefault(part, Node(S_IFDIR))
        d.children[parts[-1]] = node

    for spec in a.files:
        path, host = spec.split("=", 1)
        place(path, Node(S_IFREG, open(host, "rb").read()))
    for spec in a.link:
        path, target = spec.split("=", 1)
        place(path, Node(S_IFLNK, target=target.encode()))
    fs = Ext(a)
    if a.extras:
        extras(root, fs.bs, fs.ext4)
    root.ino = 2
    root.children["lost+found"].ino = 11
    fs.used_inodes.add(11)
    lf = root.children.pop("lost+found")
    fs.number(root)
    root.children["lost+found"] = lf
    fs.write_tree(root, root)
    open(a.out, "wb").write(wrap(fs.finish(), a.wrap, a.partuuid))


if __name__ == "__main__":
    main()
