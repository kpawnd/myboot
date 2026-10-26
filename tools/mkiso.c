/*
 * mkiso: build a UEFI-bootable hybrid ISO without xorriso/mtools.
 *
 *   mkiso OUT.iso DEST=SRC [DEST=SRC ...]
 *   e.g. mkiso myboot.iso EFI/BOOT/BOOTX64.EFI=build/release/.../BOOTX64.EFI
 *
 * Host tool (runs on the build machine, uses the host C library). Layout:
 *
 *   ISO 2048-byte sectors
 *     0-15   system area: an MBR whose single partition (type 0xEF) covers
 *            the embedded FAT image, so the same file boots from USB
 *     16     Primary Volume Descriptor (ECMA-119 8.4)
 *     17     El Torito Boot Record (El Torito 1.0, 2.0)
 *     18     Volume Descriptor Set Terminator
 *     19/20  path tables (little/big endian), root only
 *     21     root directory: ".", "..", EFIBOOT.IMG
 *     22     El Torito boot catalog: platform 0xEF (UEFI), no emulation
 *     23..   EFIBOOT.IMG: FAT12 image holding the DEST files
 *
 * UEFI firmware finds the catalog entry (CD/DVD) or the MBR partition (USB),
 * mounts the FAT image and runs \EFI\BOOT\BOOTX64.EFI.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ISO_SECTOR 2048u
#define FAT_SECTOR 512u
#define MAX_FILES  16
#define MAX_DIRS   16
#define FAT12_MAX_CLUSTERS 4084u
#define ROOT_ENTRIES 224u   /* 14 sectors, the floppy standard */

/* FAT date 2026-10-26 00:00 (fixed: reproducible images) */
#define FAT_DATE (((2026 - 1980) << 9) | (10 << 5) | 26)

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "mkiso: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put16be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32be(uint8_t *p, uint32_t v) { put16be(p, v >> 16); put16be(p + 2, v); }
static void both16(uint8_t *p, uint32_t v) { put16(p, v); put16be(p + 2, v); }
static void both32(uint8_t *p, uint32_t v) { put32(p, v); put32be(p + 4, v); }

/* ---- input files -------------------------------------------------------- */

struct file {
    char     dir[64];   /* "EFI/BOOT" */
    char     name[11];  /* 8.3, space padded */
    uint8_t *data;
    uint32_t size;
    uint32_t cluster;
};

struct dir {
    char     path[64];  /* "" = root */
    char     name[11];
    int      parent;    /* index into dirs, -1 for root */
    uint32_t cluster;   /* 0 for root (fixed region) */
};

static struct file files[MAX_FILES];
static struct dir dirs[MAX_DIRS];
static int nfiles, ndirs;

static void to_83(const char *comp, size_t len, char out[11])
{
    const char *dot = memchr(comp, '.', len);
    size_t base = dot ? (size_t)(dot - comp) : len, ext = dot ? len - base - 1 : 0;

    if (base == 0 || base > 8 || ext > 3)
        die("not an 8.3 name", comp);
    memset(out, ' ', 11);
    for (size_t i = 0; i < base; i++)
        out[i] = (char)(comp[i] >= 'a' && comp[i] <= 'z' ? comp[i] - 32 : comp[i]);
    for (size_t i = 0; i < ext; i++)
        out[8 + i] = (char)(dot[1 + i] >= 'a' && dot[1 + i] <= 'z' ? dot[1 + i] - 32 : dot[1 + i]);
}

static int find_or_add_dir(const char *path)
{
    const char *slash;
    char parent[64];

    for (int i = 0; i < ndirs; i++)
        if (!strcmp(dirs[i].path, path))
            return i;
    if (ndirs == MAX_DIRS)
        die("too many directories", path);
    slash = strrchr(path, '/');
    snprintf(parent, sizeof(parent), "%.*s", slash ? (int)(slash - path) : 0, path);
    {
        int p = path[0] ? find_or_add_dir(parent) : -1;
        struct dir *d = &dirs[ndirs];
        snprintf(d->path, sizeof(d->path), "%s", path);
        d->parent = p;
        if (path[0]) {
            const char *comp = slash ? slash + 1 : path;
            to_83(comp, strlen(comp), d->name);
        }
        return ndirs++;
    }
}

static void add_file(const char *spec)
{
    const char *eq = strchr(spec, '='), *slash;
    struct file *f;
    FILE *fp;
    long sz;

    if (!eq || nfiles == MAX_FILES)
        die("bad argument (want DEST=SRC)", spec);
    f = &files[nfiles++];
    slash = NULL;
    for (const char *p = spec; p < eq; p++)
        if (*p == '/')
            slash = p;
    snprintf(f->dir, sizeof(f->dir), "%.*s", slash ? (int)(slash - spec) : 0, spec);
    to_83(slash ? slash + 1 : spec, (size_t)(eq - (slash ? slash + 1 : spec)), f->name);
    find_or_add_dir(f->dir);

    if (!(fp = fopen(eq + 1, "rb")))
        die("cannot open", eq + 1);
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0 || sz > 64L * 1024 * 1024)
        die("bad file size", eq + 1);
    f->size = (uint32_t)sz;
    f->data = malloc(f->size ? f->size : 1);
    if (!f->data || fread(f->data, 1, f->size, fp) != f->size)
        die("read failed", eq + 1);
    fclose(fp);
}

/* ---- FAT12 image ----------------------------------------------------------- */

struct fat {
    uint8_t *img;
    uint32_t bytes, spc, cluster_bytes, fat_sectors, clusters;
    uint32_t fat_off, root_off, data_off;
    uint32_t next_cluster;
};

static void fat_set(struct fat *f, uint32_t c, uint32_t v)
{
    for (int copy = 0; copy < 2; copy++) {
        uint8_t *t = f->img + f->fat_off + (uint32_t)copy * f->fat_sectors * FAT_SECTOR;
        uint32_t o = c + c / 2;
        if (c & 1) {
            t[o] = (uint8_t)((t[o] & 0x0F) | (v << 4));
            t[o + 1] = (uint8_t)(v >> 4);
        } else {
            t[o] = (uint8_t)v;
            t[o + 1] = (uint8_t)((t[o + 1] & 0xF0) | ((v >> 8) & 0x0F));
        }
    }
}

/* Contiguous chain of n clusters; returns the first (0 if n == 0). */
static uint32_t fat_alloc(struct fat *f, uint32_t n)
{
    uint32_t first = f->next_cluster;

    if (n == 0)
        return 0;
    for (uint32_t i = 0; i < n; i++)
        fat_set(f, first + i, i + 1 < n ? first + i + 1 : 0xFFF);
    f->next_cluster += n;
    return first;
}

static uint8_t *cluster_ptr(struct fat *f, uint32_t c)
{
    return f->img + f->data_off + (c - 2) * f->cluster_bytes;
}

static void dirent(uint8_t *e, const char name[11], uint8_t attr, uint32_t cluster, uint32_t size)
{
    memcpy(e, name, 11);
    e[11] = attr;
    put16(e + 16, FAT_DATE); /* creation date */
    put16(e + 18, FAT_DATE); /* access date */
    put16(e + 24, FAT_DATE); /* write date */
    put16(e + 26, cluster);
    put32(e + 28, size);
}

static uint8_t *dir_slot(struct fat *f, int d, unsigned *used)
{
    unsigned cap = dirs[d].cluster ? f->cluster_bytes / 32 : ROOT_ENTRIES;
    uint8_t *base = dirs[d].cluster ? cluster_ptr(f, dirs[d].cluster) : f->img + f->root_off;

    if (*used >= cap)
        die("directory full", dirs[d].path);
    return base + 32 * (*used)++;
}

static void build_fat(struct fat *f)
{
    uint32_t need = 0, total_sectors;
    unsigned used[MAX_DIRS] = {0};
    uint8_t *b;

    /* Smallest cluster size that keeps FAT12; at least a 1.44 MB floppy
     * geometry, which every FAT driver handles. */
    for (f->spc = 1; ; f->spc *= 2) {
        f->cluster_bytes = f->spc * FAT_SECTOR;
        need = (uint32_t)ndirs - 1; /* one cluster per subdirectory */
        for (int i = 0; i < nfiles; i++)
            need += (files[i].size + f->cluster_bytes - 1) / f->cluster_bytes;
        if (need + 16 <= FAT12_MAX_CLUSTERS)
            break;
        if (f->spc == 64)
            die("files too large for FAT12", NULL);
    }
    f->clusters = need + 16 > 2847 ? need + 16 : 2847;
    f->fat_sectors = ((f->clusters + 2) * 3 / 2 + FAT_SECTOR - 1) / FAT_SECTOR;
    total_sectors = 1 + 2 * f->fat_sectors + ROOT_ENTRIES * 32 / FAT_SECTOR + f->clusters * f->spc;
    f->bytes = total_sectors * FAT_SECTOR;
    f->fat_off = FAT_SECTOR;
    f->root_off = f->fat_off + 2 * f->fat_sectors * FAT_SECTOR;
    f->data_off = f->root_off + ROOT_ENTRIES * 32;
    f->next_cluster = 2;
    if (!(f->img = calloc(1, f->bytes)))
        die("out of memory", NULL);

    /* Boot sector / BPB (Microsoft FAT spec, BPB for FAT12/16). */
    b = f->img;
    b[0] = 0xEB, b[1] = 0x3C, b[2] = 0x90;
    memcpy(b + 3, "MYBOOT  ", 8);
    put16(b + 11, FAT_SECTOR);
    b[13] = (uint8_t)f->spc;
    put16(b + 14, 1);                 /* reserved sectors */
    b[16] = 2;                        /* FATs */
    put16(b + 17, ROOT_ENTRIES);
    if (total_sectors < 65536)
        put16(b + 19, total_sectors);
    else
        put32(b + 32, total_sectors);
    b[21] = 0xF0;                     /* media: removable */
    put16(b + 22, f->fat_sectors);
    put16(b + 24, 18);                /* sectors per track (unused by UEFI) */
    put16(b + 26, 2);                 /* heads */
    b[38] = 0x29;                     /* extended boot signature */
    put32(b + 39, 0x4D594254);        /* volume serial */
    memcpy(b + 43, "MYBOOT     ", 11);
    memcpy(b + 54, "FAT12   ", 8);
    b[510] = 0x55, b[511] = 0xAA;

    fat_set(f, 0, 0xFF0);             /* media byte */
    fat_set(f, 1, 0xFFF);

    /* Directories first (parents before children by construction). */
    for (int i = 1; i < ndirs; i++) {
        struct dir *d = &dirs[i];
        uint8_t *e;
        d->cluster = fat_alloc(f, 1);
        e = dir_slot(f, d->parent, &used[d->parent]);
        dirent(e, d->name, 0x10, d->cluster, 0);
        e = dir_slot(f, i, &used[i]);
        dirent(e, ".          ", 0x10, d->cluster, 0);
        e = dir_slot(f, i, &used[i]);
        dirent(e, "..         ", 0x10, dirs[d->parent].cluster, 0);
    }
    for (int i = 0; i < nfiles; i++) {
        struct file *fl = &files[i];
        int d = find_or_add_dir(fl->dir);
        uint32_t n = (fl->size + f->cluster_bytes - 1) / f->cluster_bytes;
        fl->cluster = fat_alloc(f, n);
        if (n)
            memcpy(cluster_ptr(f, fl->cluster), fl->data, fl->size);
        dirent(dir_slot(f, d, &used[d]), fl->name, 0x20, fl->cluster, fl->size);
    }
}

/* ---- ISO9660 + El Torito ----------------------------------------------------- */

static void iso_date7(uint8_t *p)
{
    p[0] = 2026 - 1900, p[1] = 10, p[2] = 26, p[3] = p[4] = p[5] = 0, p[6] = 0;
}

static void iso_date17(uint8_t *p, int zero)
{
    memcpy(p, zero ? "0000000000000000" : "2026102600000000", 16);
    p[16] = 0;
}

/* Directory record (ECMA-119 9.1); returns its length. */
static uint32_t iso_dirrec(uint8_t *p, uint32_t lba, uint32_t size, uint8_t flags,
                           const char *name, uint8_t name_len)
{
    uint32_t len = 33 + name_len + ((name_len & 1) ? 0 : 1);

    p[0] = (uint8_t)len;
    both32(p + 2, lba);
    both32(p + 10, size);
    iso_date7(p + 18);
    p[25] = flags;
    both16(p + 28, 1);
    p[32] = name_len;
    memcpy(p + 33, name, name_len);
    return len;
}

static void pad_str(uint8_t *p, size_t n, const char *s)
{
    memset(p, ' ', n);
    memcpy(p, s, strlen(s) < n ? strlen(s) : n);
}

int main(int argc, char **argv)
{
    enum { LBA_PVD = 16, LBA_BOOTREC, LBA_TERM, LBA_PT_L, LBA_PT_M, LBA_ROOT, LBA_CATALOG, LBA_IMAGE };
    struct fat fat = {0};
    uint32_t img_iso_sectors, total, img512;
    uint8_t *iso, *s;
    uint16_t sum = 0;
    FILE *out;

    if (argc < 3)
        die("usage: mkiso OUT.iso DEST=SRC [DEST=SRC ...]", NULL);
    find_or_add_dir(""); /* root is dirs[0] */
    for (int i = 2; i < argc; i++)
        add_file(argv[i]);
    build_fat(&fat);

    img_iso_sectors = (fat.bytes + ISO_SECTOR - 1) / ISO_SECTOR;
    total = LBA_IMAGE + img_iso_sectors;
    img512 = fat.bytes / FAT_SECTOR;
    if (!(iso = calloc(total, ISO_SECTOR)))
        die("out of memory", NULL);

    /* Hybrid MBR in the system area: one EFI System partition = the image. */
    s = iso;
    put32(s + 440, 0x4D594254);       /* disk signature */
    s[446 + 0] = 0x00;                /* not active (UEFI ignores it) */
    s[446 + 1] = 0xFE, s[446 + 2] = 0xFF, s[446 + 3] = 0xFF; /* CHS: use LBA */
    s[446 + 4] = 0xEF;                /* EFI System Partition */
    s[446 + 5] = 0xFE, s[446 + 6] = 0xFF, s[446 + 7] = 0xFF;
    put32(s + 446 + 8, LBA_IMAGE * (ISO_SECTOR / FAT_SECTOR));
    put32(s + 446 + 12, img512);
    s[510] = 0x55, s[511] = 0xAA;

    /* Primary Volume Descriptor */
    s = iso + LBA_PVD * ISO_SECTOR;
    s[0] = 1;
    memcpy(s + 1, "CD001", 5);
    s[6] = 1;
    pad_str(s + 8, 32, "");
    pad_str(s + 40, 32, "MYBOOT");
    both32(s + 80, total);
    both16(s + 120, 1);
    both16(s + 124, 1);
    both16(s + 128, ISO_SECTOR);
    both32(s + 132, 10);              /* path table: one 10-byte root entry */
    put32(s + 140, LBA_PT_L);
    put32be(s + 148, LBA_PT_M);
    iso_dirrec(s + 156, LBA_ROOT, ISO_SECTOR, 0x02, "\0", 1);
    pad_str(s + 190, 128, "");
    pad_str(s + 318, 128, "");
    pad_str(s + 446, 128, "");
    pad_str(s + 574, 128, "MYBOOT MKISO");
    pad_str(s + 702, 37, "");
    pad_str(s + 739, 37, "");
    pad_str(s + 776, 37, "");
    iso_date17(s + 813, 0);
    iso_date17(s + 830, 0);
    iso_date17(s + 847, 1);
    iso_date17(s + 864, 1);
    s[881] = 1;

    /* El Torito Boot Record Volume Descriptor */
    s = iso + LBA_BOOTREC * ISO_SECTOR;
    s[0] = 0;
    memcpy(s + 1, "CD001", 5);
    s[6] = 1;
    memcpy(s + 7, "EL TORITO SPECIFICATION", 23);
    put32(s + 71, LBA_CATALOG);

    /* Terminator */
    s = iso + LBA_TERM * ISO_SECTOR;
    s[0] = 255;
    memcpy(s + 1, "CD001", 5);
    s[6] = 1;

    /* Path tables: root only (identifier 0x00, parent 1) */
    s = iso + LBA_PT_L * ISO_SECTOR;
    s[0] = 1, put32(s + 2, LBA_ROOT), put16(s + 6, 1);
    s = iso + LBA_PT_M * ISO_SECTOR;
    s[0] = 1, put32be(s + 2, LBA_ROOT), put16be(s + 6, 1);

    /* Root directory */
    s = iso + LBA_ROOT * ISO_SECTOR;
    s += iso_dirrec(s, LBA_ROOT, ISO_SECTOR, 0x02, "\0", 1);
    s += iso_dirrec(s, LBA_ROOT, ISO_SECTOR, 0x02, "\1", 1);
    iso_dirrec(s, LBA_IMAGE, fat.bytes, 0x00, "EFIBOOT.IMG;1", 13);

    /* Boot catalog: validation entry (platform EFI) + default entry */
    s = iso + LBA_CATALOG * ISO_SECTOR;
    s[0] = 1;
    s[1] = 0xEF;
    memcpy(s + 4, "MYBOOT", 6);
    s[30] = 0x55, s[31] = 0xAA;
    for (int i = 0; i < 32; i += 2)
        sum = (uint16_t)(sum + (s[i] | (s[i + 1] << 8)));
    put16(s + 28, (uint16_t)(0x10000 - sum)); /* words sum to zero */
    s[32] = 0x88;                     /* bootable */
    s[33] = 0;                        /* no emulation */
    put16(s + 38, img512 <= 0xFFFF ? img512 : 0);
    put32(s + 40, LBA_IMAGE);

    memcpy(iso + LBA_IMAGE * ISO_SECTOR, fat.img, fat.bytes);

    if (!(out = fopen(argv[1], "wb")) || fwrite(iso, ISO_SECTOR, total, out) != total || fclose(out))
        die("write failed", argv[1]);
    printf("mkiso: %s, %u bytes (FAT12 image %u bytes, %u files)\n",
           argv[1], total * ISO_SECTOR, fat.bytes, (unsigned)nfiles);
    return 0;
}
