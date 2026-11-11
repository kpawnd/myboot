/* Baseline JPEG decoder; see include/image/jpeg.h. */
#include <stdbool.h>
#include "image/jpeg.h"
#include "lib/mem.h"

#define FAST_BITS 9

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

struct huff {
    uint8_t  sym[256];
    uint16_t fast[1 << FAST_BITS];
    int32_t  maxcode[17];
    int32_t  delta[17];
    uint16_t nsym;
    bool     present;
};

struct comp {
    uint8_t id, h, v, tq, td, ta;
    int32_t pred;
    uint8_t pix[16 * 16];
};

struct jpeg {
    const uint8_t *p, *end;
    uint16_t    q[4][64];
    uint8_t     qpresent;
    struct huff dc[4], ac[4];
    struct comp comp[3];
    uint32_t    ncomp, width, height, hmax, vmax;
    uint32_t    restart;
    bool        frame;
    uint32_t    acc;
    int32_t     nbits;
    bool        marker;
};

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static bool huff_build(struct huff *h, const uint8_t *counts, const uint8_t *syms)
{
    uint32_t code = 0, k = 0;

    memset(h->fast, 0, sizeof(h->fast));
    for (uint32_t l = 1; l <= 16; l++) {
        uint32_t n = counts[l - 1];

        h->delta[l] = (int32_t)k - (int32_t)code;
        for (uint32_t i = 0; i < n; i++, code++, k++) {
            if (code >= 1u << l)
                return false;
            h->sym[k] = syms[k];
            if (l <= FAST_BITS) {
                uint32_t shift = FAST_BITS - l;
                for (uint32_t f = 0; f < 1u << shift; f++)
                    h->fast[(code << shift) + f] = (uint16_t)(l << 8 | syms[k]);
            }
        }
        h->maxcode[l] = n ? (int32_t)code - 1 : -1;
        code <<= 1;
    }
    h->nsym = (uint16_t)k;
    h->present = true;
    return true;
}

static void fill(struct jpeg *j)
{
    while (j->nbits <= 24) {
        uint32_t b = 0;

        if (!j->marker && j->p < j->end) {
            b = *j->p;
            if (b != 0xFF)
                j->p++;
            else if (j->p + 1 < j->end && j->p[1] == 0x00)
                j->p += 2;
            else {
                j->marker = true;
                b = 0;
            }
        }
        j->acc |= b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static uint32_t get_bits(struct jpeg *j, uint32_t n)
{
    uint32_t v;

    fill(j);
    v = j->acc >> (32 - n);
    j->acc <<= n;
    j->nbits -= (int32_t)n;
    return v;
}

static int32_t receive(struct jpeg *j, uint32_t s)
{
    int32_t v;

    if (!s)
        return 0;
    v = (int32_t)get_bits(j, s);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static int decode(struct jpeg *j, const struct huff *h)
{
    uint32_t f;

    fill(j);
    f = h->fast[j->acc >> (32 - FAST_BITS)];
    if (f) {
        j->acc <<= f >> 8;
        j->nbits -= (int32_t)(f >> 8);
        return (int)(f & 0xFF);
    }
    for (uint32_t l = FAST_BITS + 1; l <= 16; l++) {
        int32_t code = (int32_t)(j->acc >> (32 - l)), i;

        if (code <= h->maxcode[l]) {
            i = code + h->delta[l];
            if (i < 0 || i >= h->nsym)
                return -1;
            j->acc <<= l;
            j->nbits -= (int32_t)l;
            return h->sym[i];
        }
    }
    return -1;
}

static void decode_block(struct jpeg *j, struct comp *c, int32_t blk[64])
{
    const uint16_t *q = j->q[c->tq];
    int t = decode(j, &j->dc[c->td]);

    memset(blk, 0, 64 * sizeof(*blk));
    if (t < 0 || t > 16)
        return;
    c->pred += receive(j, (uint32_t)t);
    if (c->pred > 32767)
        c->pred = 32767;
    else if (c->pred < -32768)
        c->pred = -32768;
    blk[0] = c->pred * q[0];

    for (uint32_t k = 1; k < 64;) {
        int rs = decode(j, &j->ac[c->ta]);
        uint32_t r, s;

        if (rs < 0)
            return;
        r = (uint32_t)rs >> 4;
        s = (uint32_t)rs & 15;
        if (!s) {
            if (r != 15)
                return;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return;
        blk[zigzag[k]] = receive(j, s) * q[k];
        k++;
    }
}

static void idct8(const int64_t s[8], int64_t o[8])
{
    int64_t t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;

    p1 = (s[2] + s[6]) * 2217;
    t2 = p1 + s[6] * -7568;
    t3 = p1 + s[2] * 3135;
    t0 = (s[0] + s[4]) * 4096;
    t1 = (s[0] - s[4]) * 4096;
    x0 = t0 + t3;
    x3 = t0 - t3;
    x1 = t1 + t2;
    x2 = t1 - t2;

    t0 = s[7];
    t1 = s[5];
    t2 = s[3];
    t3 = s[1];
    p3 = t0 + t2;
    p4 = t1 + t3;
    p1 = t0 + t3;
    p2 = t1 + t2;
    p5 = (p3 + p4) * 4816;
    t0 *= 1223;
    t1 *= 8410;
    t2 *= 12586;
    t3 *= 6149;
    p1 = p5 + p1 * -3686;
    p2 = p5 + p2 * -10498;
    p3 *= -8035;
    p4 *= -1598;
    t3 += p1 + p4;
    t2 += p2 + p3;
    t1 += p2 + p4;
    t0 += p1 + p3;

    o[0] = x0 + t3;
    o[7] = x0 - t3;
    o[1] = x1 + t2;
    o[6] = x1 - t2;
    o[2] = x2 + t1;
    o[5] = x2 - t1;
    o[3] = x3 + t0;
    o[4] = x3 - t0;
}

static uint8_t clamp8(int64_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void idct_block(const int32_t blk[64], uint8_t *out, uint32_t stride)
{
    int64_t tmp[64], s[8], o[8];

    for (uint32_t x = 0; x < 8; x++) {
        for (uint32_t y = 0; y < 8; y++)
            s[y] = blk[y * 8 + x];
        idct8(s, o);
        for (uint32_t y = 0; y < 8; y++)
            tmp[y * 8 + x] = (o[y] + 512) >> 10;
    }
    for (uint32_t y = 0; y < 8; y++) {
        idct8(&tmp[y * 8], o);
        for (uint32_t x = 0; x < 8; x++)
            out[y * stride + x] = clamp8((o[x] + 65536 + (128 << 17)) >> 17);
    }
}

static uint32_t ycc_rgb(int32_t y, int32_t cb, int32_t cr)
{
    int32_t yy = (y << 16) + 32768, r, g, b;

    cb -= 128;
    cr -= 128;
    r = (yy + 91881 * cr) >> 16;
    g = (yy - 22554 * cb - 46802 * cr) >> 16;
    b = (yy + 116130 * cb) >> 16;
    return (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
}

static void restart(struct jpeg *j)
{
    while (j->p + 1 < j->end) {
        if (j->p[0] == 0xFF && j->p[1] != 0x00 && j->p[1] != 0xFF)
            break;
        j->p++;
    }
    if (j->p + 1 < j->end && j->p[1] >= 0xD0 && j->p[1] <= 0xD7)
        j->p += 2;
    j->acc = 0;
    j->nbits = 0;
    j->marker = false;
    for (uint32_t c = 0; c < j->ncomp; c++)
        j->comp[c].pred = 0;
}

static void decode_scan(struct jpeg *j, uint32_t *out, uint32_t stride)
{
    uint32_t mw = 8 * j->hmax, mh = 8 * j->vmax;
    uint32_t mcux = (j->width + mw - 1) / mw, mcuy = (j->height + mh - 1) / mh, n = 0;
    int32_t blk[64];

    for (uint32_t my = 0; my < mcuy; my++) {
        for (uint32_t mx = 0; mx < mcux; mx++) {
            uint32_t w, h;

            if (j->restart && n && n % j->restart == 0)
                restart(j);
            n++;
            for (uint32_t ci = 0; ci < j->ncomp; ci++) {
                struct comp *c = &j->comp[ci];
                for (uint32_t by = 0; by < c->v; by++)
                    for (uint32_t bx = 0; bx < c->h; bx++) {
                        decode_block(j, c, blk);
                        idct_block(blk, &c->pix[by * 8 * 16 + bx * 8], 16);
                    }
            }

            w = j->width - mx * mw < mw ? j->width - mx * mw : mw;
            h = j->height - my * mh < mh ? j->height - my * mh : mh;
            for (uint32_t y = 0; y < h; y++) {
                uint32_t *row = &out[(uint64_t)(my * mh + y) * stride + mx * mw];
                const struct comp *c = j->comp;

                for (uint32_t x = 0; x < w; x++) {
                    uint32_t yv = c[0].pix[y * 16 + x];
                    if (j->ncomp == 1) {
                        row[x] = yv * 0x010101u;
                        continue;
                    }
                    row[x] = ycc_rgb((int32_t)yv,
                                     c[1].pix[(y * c[1].v / j->vmax) * 16 + x * c[1].h / j->hmax],
                                     c[2].pix[(y * c[2].v / j->vmax) * 16 + x * c[2].h / j->hmax]);
                }
            }
        }
    }
}

static enum jpeg_error read_dqt(struct jpeg *j, const uint8_t *s, uint32_t n)
{
    while (n) {
        uint32_t pq = s[0] >> 4, tq = s[0] & 15, need = 1 + 64 * (pq + 1);

        if (pq > 1 || tq > 3 || n < need)
            return JPEG_CORRUPT;
        for (uint32_t k = 0; k < 64; k++)
            j->q[tq][k] = (uint16_t)(pq ? be16(s + 1 + 2 * k) : s[1 + k]);
        j->qpresent |= (uint8_t)(1 << tq);
        s += need;
        n -= need;
    }
    return JPEG_OK;
}

static enum jpeg_error read_dht(struct jpeg *j, const uint8_t *s, uint32_t n)
{
    while (n) {
        uint32_t tc, th, total = 0;

        if (n < 17)
            return JPEG_CORRUPT;
        tc = s[0] >> 4;
        th = s[0] & 15;
        for (uint32_t i = 1; i <= 16; i++)
            total += s[i];
        if (tc > 1 || th > 3 || total > 256 || n < 17 + total ||
            !huff_build(tc ? &j->ac[th] : &j->dc[th], s + 1, s + 17))
            return JPEG_CORRUPT;
        s += 17 + total;
        n -= 17 + total;
    }
    return JPEG_OK;
}

static enum jpeg_error read_sof(struct jpeg *j, const uint8_t *s, uint32_t n)
{
    if (j->frame)
        return JPEG_CORRUPT;
    if (n < 6)
        return JPEG_CORRUPT;
    if (s[0] != 8)
        return JPEG_UNSUPPORTED;
    j->height = be16(s + 1);
    j->width = be16(s + 3);
    j->ncomp = s[5];
    if (!j->width || !j->height)
        return JPEG_UNSUPPORTED;
    if (j->ncomp != 1 && j->ncomp != 3)
        return JPEG_UNSUPPORTED;
    if (n < 6 + 3 * j->ncomp)
        return JPEG_CORRUPT;
    if (j->width > JPEG_MAX_DIM || j->height > JPEG_MAX_DIM)
        return JPEG_TOO_LARGE;
    j->hmax = j->vmax = 1;
    for (uint32_t i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];

        c->id = s[6 + 3 * i];
        c->h = s[7 + 3 * i] >> 4;
        c->v = s[7 + 3 * i] & 15;
        c->tq = s[8 + 3 * i];
        if (c->tq > 3)
            return JPEG_CORRUPT;
        if (j->ncomp == 1)
            c->h = c->v = 1;
        if (c->h < 1 || c->h > 2 || c->v < 1 || c->v > 2)
            return JPEG_UNSUPPORTED;
        if (c->h > j->hmax)
            j->hmax = c->h;
        if (c->v > j->vmax)
            j->vmax = c->v;
    }
    if (j->comp[0].h != j->hmax || j->comp[0].v != j->vmax)
        return JPEG_UNSUPPORTED;
    j->frame = true;
    return JPEG_OK;
}

static enum jpeg_error read_sos(struct jpeg *j, const uint8_t *s, uint32_t n)
{
    uint32_t ns;

    if (!j->frame || n < 1)
        return JPEG_CORRUPT;
    ns = s[0];
    if (n < 1 + 2 * ns + 3)
        return JPEG_CORRUPT;
    if (ns != j->ncomp)
        return JPEG_UNSUPPORTED;
    for (uint32_t i = 0; i < ns; i++) {
        struct comp *c = &j->comp[i];

        if (s[1 + 2 * i] != c->id)
            return JPEG_CORRUPT;
        c->td = s[2 + 2 * i] >> 4;
        c->ta = s[2 + 2 * i] & 15;
        if (c->td > 3 || c->ta > 3 || !j->dc[c->td].present || !j->ac[c->ta].present ||
            !(j->qpresent & (1 << c->tq)))
            return JPEG_CORRUPT;
        c->pred = 0;
    }
    s += 1 + 2 * ns;
    if (s[0] != 0 || s[1] != 63 || s[2] != 0)
        return JPEG_UNSUPPORTED;
    return JPEG_OK;
}

static enum jpeg_error parse(struct jpeg *j, const uint8_t *data, size_t len, uint32_t *out,
                             uint32_t width, uint32_t height, uint32_t stride)
{
    enum jpeg_error e;

    memset(j, 0, sizeof(*j));
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return JPEG_NOT_JPEG;
    j->p = data + 2;
    j->end = data + len;

    for (;;) {
        const uint8_t *seg;
        uint32_t marker, seglen;

        if (j->p >= j->end || *j->p != 0xFF)
            return JPEG_CORRUPT;
        while (j->p < j->end && *j->p == 0xFF)
            j->p++;
        if (j->p >= j->end)
            return JPEG_CORRUPT;
        marker = *j->p++;
        if (marker == 0xD9)
            return JPEG_CORRUPT;
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
            continue;
        if (j->end - j->p < 2 || (seglen = be16(j->p)) < 2 || (size_t)(j->end - j->p) < seglen)
            return JPEG_CORRUPT;
        seg = j->p + 2;
        seglen -= 2;
        j->p += seglen + 2;

        switch (marker) {
        case 0xC0:
        case 0xC1:
            if ((e = read_sof(j, seg, seglen)) != JPEG_OK || !out)
                return e;
            if (j->width != width || j->height != height)
                return JPEG_CORRUPT;
            break;
        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return JPEG_UNSUPPORTED;
        case 0xC4:
            if ((e = read_dht(j, seg, seglen)) != JPEG_OK)
                return e;
            break;
        case 0xDB:
            if ((e = read_dqt(j, seg, seglen)) != JPEG_OK)
                return e;
            break;
        case 0xDD:
            if (seglen < 2)
                return JPEG_CORRUPT;
            j->restart = be16(seg);
            break;
        case 0xDA:
            if ((e = read_sos(j, seg, seglen)) != JPEG_OK)
                return e;
            decode_scan(j, out, stride);
            return JPEG_OK;
        default:
            break;
        }
    }
}

enum jpeg_error jpeg_size(const uint8_t *data, size_t len, uint32_t *width, uint32_t *height)
{
    struct jpeg j;
    enum jpeg_error e = parse(&j, data, len, NULL, 0, 0, 0);

    if (e == JPEG_OK && !j.frame)
        e = JPEG_CORRUPT;
    *width = e == JPEG_OK ? j.width : 0;
    *height = e == JPEG_OK ? j.height : 0;
    return e;
}

enum jpeg_error jpeg_decode(const uint8_t *data, size_t len, uint32_t *out, uint32_t width,
                            uint32_t height, uint32_t stride)
{
    struct jpeg j;

    if (stride < width)
        return JPEG_CORRUPT;
    return parse(&j, data, len, out, width, height, stride);
}

const char *jpeg_strerror(enum jpeg_error e)
{
    switch (e) {
    case JPEG_OK:          return "no error";
    case JPEG_NOT_JPEG:    return "not a JPEG file";
    case JPEG_CORRUPT:     return "damaged JPEG file";
    case JPEG_UNSUPPORTED: return "progressive or other unsupported JPEG";
    case JPEG_TOO_LARGE:   return "image larger than 8192x8192";
    }
    return "unknown error";
}
