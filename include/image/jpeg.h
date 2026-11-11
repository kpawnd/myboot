/*
 * Baseline JPEG decoder (ITU T.81), for the menu background.
 *
 * Supported: sequential Huffman-coded frames (SOF0, SOF1) with 8-bit
 * samples, one component (greyscale) or three (YCbCr, as JFIF and Exif
 * files store them), sampling factors 1 or 2, restart intervals. Not
 * supported: progressive, lossless, arithmetic-coded and 12-bit files, and
 * a frame split over several scans; jpeg_decode refuses them with
 * JPEG_UNSUPPORTED.
 *
 * Everything is integer arithmetic (the loader may not touch SSE). Chroma
 * is upsampled by repetition. The decoder never reads outside `data` and
 * never writes outside the pixel buffer it is given, whatever the file
 * holds; a damaged entropy-coded segment gives a damaged picture, not an
 * error.
 */
#ifndef IMAGE_JPEG_H
#define IMAGE_JPEG_H

#include <stddef.h>
#include <stdint.h>

#define JPEG_MAX_DIM 8192

enum jpeg_error {
    JPEG_OK,
    JPEG_NOT_JPEG,     /* no SOI marker */
    JPEG_CORRUPT,      /* malformed marker segment or table */
    JPEG_UNSUPPORTED,  /* see above */
    JPEG_TOO_LARGE     /* wider or taller than JPEG_MAX_DIM */
};

/* Dimensions from the frame header, without decoding. */
enum jpeg_error jpeg_size(const uint8_t *data, size_t len, uint32_t *width, uint32_t *height);

/* Decode into `out`: width * height pixels as 0x00RRGGBB, `stride` pixels
 * from one row to the next (stride >= width). `width` and `height` must be
 * what jpeg_size returned. Uses about 13 KiB of stack. */
enum jpeg_error jpeg_decode(const uint8_t *data, size_t len, uint32_t *out, uint32_t width,
                            uint32_t height, uint32_t stride);

const char *jpeg_strerror(enum jpeg_error e);

#endif
