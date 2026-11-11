#!/usr/bin/env python3
import argparse
import math
import random
import struct

ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
          12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
          35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
          58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]
COS = [[math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)] for u in range(8)]
C = [1 / math.sqrt(2)] + [1.0] * 7

def picture(w, h, seed):
    rng = random.Random(seed)
    px = []
    for y in range(h):
        row = []
        for x in range(w):
            r = 255 * x / max(1, w - 1)
            g = 255 * y / max(1, h - 1)
            b = 128 + 100 * math.sin(x / 5.0) * math.cos(y / 7.0)
            if (x // 13 + y // 11) % 3 == 0:
                r, g, b = 255 - g, b, r
            n = rng.uniform(-20, 20)
            row.append(tuple(min(255, max(0, int(v + n))) for v in (r, g, b)))
        px.append(row)
    return px

def fdct(block):
    tmp = [[sum(block[y][x] * COS[u][x] for x in range(8)) for u in range(8)] for y in range(8)]
    return [0.25 * C[u] * C[v] * sum(tmp[y][u] * COS[v][y] for y in range(8))
            for v in range(8) for u in range(8)]

def plane(px, w, h, f, sub_x, sub_y, pw, ph):
    out = []
    for y in range(ph):
        row = []
        for x in range(pw):
            acc = 0
            for dy in range(sub_y):
                for dx in range(sub_x):
                    sx = min(w - 1, x * sub_x + dx)
                    sy = min(h - 1, y * sub_y + dy)
                    acc += f(*px[sy][sx])
            row.append(acc / (sub_x * sub_y))
        out.append(row)
    return out

def category(v):
    v = abs(v)
    n = 0
    while v:
        n += 1
        v >>= 1
    return n

def bits_of(v, n):
    return v if v >= 0 else v + (1 << n) - 1

def build_table(freq):
    syms = sorted(freq, key=lambda s: (-freq[s], s))
    lengths, used, prev = {}, 0, 1
    for i, s in enumerate(syms):
        d = max(prev, min(16, 2 + i // 2))
        left = len(syms) - i - 1
        while used + (1 << (16 - d)) + left > 65535:
            d += 1
        assert d <= 16
        lengths[s] = d
        used += 1 << (16 - d)
        prev = d
    counts = [0] * 16
    order = sorted(syms, key=lambda s: (lengths[s], syms.index(s)))
    for s in order:
        counts[lengths[s] - 1] += 1
    codes, code, k = {}, 0, 0
    for l in range(1, 17):
        for _ in range(counts[l - 1]):
            codes[order[k]] = (code, l)
            code += 1
            k += 1
        code <<= 1
    return counts, order, codes

class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, v, n):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0)
                self.acc = self.n = 0

    def flush(self):
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("--sampling", default="2x2")
    ap.add_argument("--grey", action="store_true")
    ap.add_argument("--restart", type=int, default=0)
    ap.add_argument("--quality", type=int, default=6, help="quantiser step scale, 1 = finest")
    ap.add_argument("--q16", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    w, h = a.width, a.height
    hs, vs = (1, 1) if a.grey else tuple(int(v) for v in a.sampling.split("x"))
    px = picture(w, h, a.seed)

    comps = [(1, hs, vs, 0, lambda r, g, b: 0.299 * r + 0.587 * g + 0.114 * b)]
    if not a.grey:
        comps += [(2, 1, 1, 1, lambda r, g, b: -0.168736 * r - 0.331264 * g + 0.5 * b + 128),
                  (3, 1, 1, 1, lambda r, g, b: 0.5 * r - 0.418688 * g - 0.081312 * b + 128)]
    mcux, mcuy = -(-w // (8 * hs)), -(-h // (8 * vs))
    qt = [[min(65535 if a.q16 else 255,
               (1 + a.quality * (1 + k // 8 + k % 8) * (1 + t)) * (3 if a.q16 else 1))
           for k in range(64)] for t in range(2)]

    planes = []
    for cid, ch, cv, tq, f in comps:
        planes.append(plane(px, w, h, f, hs // ch, vs // cv, mcux * ch * 8, mcuy * cv * 8))

    def block(ci, bx, by):
        p = planes[ci]
        b = [[p[by * 8 + y][bx * 8 + x] - 128 for x in range(8)] for y in range(8)]
        coef = fdct(b)
        q = qt[comps[ci][3]]
        return [int(round(coef[ZIGZAG[k]] / q[k])) for k in range(64)]

    mcus = []
    for my in range(mcuy):
        for mx in range(mcux):
            mcu = []
            for ci, (cid, ch, cv, tq, f) in enumerate(comps):
                for y in range(cv):
                    for x in range(ch):
                        mcu.append((ci, block(ci, mx * ch + x, my * cv + y)))
            mcus.append(mcu)

    def symbols(emit):
        pred = [0] * len(comps)
        for i, mcu in enumerate(mcus):
            if a.restart and i and i % a.restart == 0:
                emit(None, None, None, None)
                pred = [0] * len(comps)
            for ci, z in mcu:
                cls = 0 if ci == 0 else 1
                d = z[0] - pred[ci]
                pred[ci] = z[0]
                emit(("dc", cls), category(d), d, category(d))
                run = 0
                for k in range(1, 64):
                    if z[k] == 0:
                        run += 1
                        continue
                    while run > 15:
                        emit(("ac", cls), 0xF0, 0, 0)
                        run -= 16
                    s = category(z[k])
                    emit(("ac", cls), run << 4 | s, z[k], s)
                    run = 0
                if run:
                    emit(("ac", cls), 0x00, 0, 0)

    freq = {}

    def count(tab, sym, v, n):
        if tab:
            freq.setdefault(tab, {}).setdefault(sym, 0)
            freq[tab][sym] += 1

    symbols(count)
    tables = {tab: build_table(fr) for tab, fr in freq.items()}

    bits = Bits()
    rst = [0]

    def write(tab, sym, v, n):
        if tab is None:
            bits.flush()
            bits.out += bytes([0xFF, 0xD0 + rst[0] % 8])
            rst[0] += 1
            return
        code, l = tables[tab][2][sym]
        bits.put(code, l)
        if n:
            bits.put(bits_of(v, n), n)

    symbols(write)
    bits.flush()

    def seg(marker, body):
        return bytes([0xFF, marker]) + struct.pack(">H", len(body) + 2) + body

    out = bytearray(b"\xFF\xD8")
    out += seg(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    for t in range(1 if a.grey else 2):
        if a.q16:
            out += seg(0xDB, bytes([0x10 | t]) + b"".join(struct.pack(">H", v) for v in qt[t]))
        else:
            out += seg(0xDB, bytes([t]) + bytes(qt[t]))
    sof = struct.pack(">BHHB", 8, h, w, len(comps))
    for cid, ch, cv, tq, f in comps:
        sof += bytes([cid, ch << 4 | cv, tq])
    out += seg(0xC1 if a.q16 else 0xC0, sof)
    for (kind, cls), (counts, order, codes) in sorted(tables.items()):
        out += seg(0xC4, bytes([(0 if kind == "dc" else 0x10) | cls]) + bytes(counts) + bytes(order))
    if a.restart:
        out += seg(0xDD, struct.pack(">H", a.restart))
    sos = bytes([len(comps)])
    for ci, (cid, ch, cv, tq, f) in enumerate(comps):
        cls = 0 if ci == 0 else 1
        sos += bytes([cid, cls << 4 | cls])
    out += seg(0xDA, sos + bytes([0, 63, 0]))
    out += bits.out + b"\xFF\xD9"
    open(a.out, "wb").write(out)

if __name__ == "__main__":
    main()
