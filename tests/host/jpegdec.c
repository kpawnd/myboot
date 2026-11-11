/*
 * Host test for src/image/jpeg.c: decodes a JPEG file with the loader's
 * own decoder, so tests/jpeg.py can compare the pixels with what an
 * independent decoder (Windows GDI+) makes of the same file.
 *
 *   jpegdec IN.JPG [OUT.PPM]
 *
 * Prints "WIDTHxHEIGHT" and writes the picture as binary PPM, or prints
 * "ERROR <reason>" and exits 1. Exit codes above 1 are harness failures.
 */
#include <stdio.h>
#include <stdlib.h>
#include "image/jpeg.h"

int main(int argc, char **argv)
{
    FILE *f;
    long len;
    uint8_t *data;
    uint32_t w, h, *px;
    enum jpeg_error e;

    if (argc < 2 || !(f = fopen(argv[1], "rb")))
        return 2;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* exactly len bytes: AddressSanitizer catches any read past the end */
    if (!(data = malloc(len ? (size_t)len : 1)) || fread(data, 1, (size_t)len, f) != (size_t)len)
        return 2;
    fclose(f);

    if ((e = jpeg_size(data, (size_t)len, &w, &h)) != JPEG_OK) {
        printf("ERROR %s\n", jpeg_strerror(e));
        return 1;
    }
    /* stride w + 3: the decoder must not touch the columns past the picture */
    if (!(px = malloc((size_t)(w + 3) * h * sizeof(*px))))
        return 2;
    for (size_t i = 0; i < (size_t)(w + 3) * h; i++)
        px[i] = 0xA5A5A5A5u;
    if ((e = jpeg_decode(data, (size_t)len, px, w, h, w + 3)) != JPEG_OK) {
        printf("ERROR %s\n", jpeg_strerror(e));
        return 1;
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = w; x < w + 3; x++)
            if (px[(size_t)y * (w + 3) + x] != 0xA5A5A5A5u) {
                printf("ERROR wrote past the row at %u,%u\n", x, y);
                return 3;
            }
    printf("%ux%u\n", w, h);

    if (argc > 2) {
        if (!(f = fopen(argv[2], "wb")))
            return 2;
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t p = px[(size_t)y * (w + 3) + x];
                if (p >> 24) {
                    printf("ERROR pixel %u,%u not written\n", x, y);
                    return 3;
                }
                fputc((p >> 16) & 0xFF, f);
                fputc((p >> 8) & 0xFF, f);
                fputc(p & 0xFF, f);
            }
        fclose(f);
    }
    free(px);
    free(data);
    return 0;
}
