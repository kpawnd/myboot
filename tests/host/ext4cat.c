/*
 * Host test for src/fs/ext4.c: reads files from a disk image through the
 * loader's own driver, so tests/fs.py can compare them with what an
 * independent reader (7-Zip) extracts.
 *
 *   ext4cat IMAGE OFFSET SIZE LIST OUTDIR
 *
 * The filesystem starts at byte OFFSET of IMAGE and is SIZE bytes long.
 * LIST holds one absolute path per line; file i of the list is written to
 * OUTDIR/i, or reported on stdout as "i ERROR <reason>". Every file is read
 * twice, whole and in chunks of awkward sizes, and the two must agree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs/ext4.h"

struct filedev {
    struct blockdev dev; /* first: the driver's pointer converts back */
    FILE *f;
    unsigned long long base;
};

static int file_read(struct blockdev *dev, uint64_t offset, void *dst, uint64_t len)
{
    struct filedev *d = (struct filedev *)dev;

    if (_fseeki64(d->f, (long long)(d->base + offset), SEEK_SET))
        return -1;
    return fread(dst, 1, len, d->f) == len ? 0 : -1;
}

int main(int argc, char **argv)
{
    static const uint64_t chunks[] = {1, 7, 511, 4096, 4097, 65537, 1048573};
    struct filedev d = {0};
    struct ext4_fs fs;
    enum fs_error e;
    char line[4096], out[4200];
    unsigned long n = 0, bad = 0;
    FILE *list;

    if (argc != 6) {
        fprintf(stderr, "usage: ext4cat IMAGE OFFSET SIZE LIST OUTDIR\n");
        return 2;
    }
    if (!(d.f = fopen(argv[1], "rb")) || !(list = fopen(argv[4], "r"))) {
        perror("open");
        return 2;
    }
    d.base = strtoull(argv[2], NULL, 0);
    d.dev.size = strtoull(argv[3], NULL, 0);
    d.dev.read = file_read;
    if ((e = ext4_mount(&fs, &d.dev)) != FS_OK) {
        printf("mount ERROR %s\n", fs_strerror(e));
        return 1;
    }

    while (fgets(line, sizeof(line), list)) {
        struct ext4_file f;
        uint8_t *a, *b;
        FILE *o;

        line[strcspn(line, "\r\n")] = 0;
        if ((e = ext4_open(&fs, line, &f)) != FS_OK) {
            printf("%lu ERROR %s\n", n++, fs_strerror(e));
            continue;
        }
        if (f.size > (64u << 20)) { /* corrupt sizes in the fuzz tests */
            printf("%lu ERROR larger than the harness reads\n", n++);
            continue;
        }
        a = malloc(f.size + 1);
        b = malloc(f.size + 1);
        if ((e = ext4_read(&f, 0, a, f.size)) != FS_OK) {
            printf("%lu ERROR %s\n", n++, fs_strerror(e));
            free(a);
            free(b);
            continue;
        }
        for (uint64_t off = 0, k = n; off < f.size; k++) {
            uint64_t len = chunks[k % (sizeof(chunks) / sizeof(chunks[0]))];
            if (len > f.size - off)
                len = f.size - off;
            if ((e = ext4_read(&f, off, b + off, len)) != FS_OK)
                break;
            off += len;
        }
        if (e != FS_OK || memcmp(a, b, f.size)) {
            printf("%lu ERROR chunked read differs (%s)\n", n, fs_strerror(e));
            bad++;
        }
        snprintf(out, sizeof(out), "%s/%lu", argv[5], n);
        if (!(o = fopen(out, "wb")) || fwrite(a, 1, f.size, o) != f.size) {
            perror(out);
            return 2;
        }
        fclose(o);
        free(a);
        free(b);
        n++;
    }
    fprintf(stderr, "ext4cat: %lu paths, %llu device reads, %llu bytes\n", n,
            (unsigned long long)d.dev.reads, (unsigned long long)d.dev.bytes);
    return bad ? 1 : 0;
}
