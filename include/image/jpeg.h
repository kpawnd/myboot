/* Baseline JPEG decoder (ITU T.81), for the menu background. */
#ifndef IMAGE_JPEG_H
#define IMAGE_JPEG_H

#include <stddef.h>
#include <stdint.h>

#define JPEG_MAX_DIM 8192

enum jpeg_error {
    JPEG_OK,
    JPEG_NOT_JPEG,
    JPEG_CORRUPT,
    JPEG_UNSUPPORTED,
    JPEG_TOO_LARGE
};

enum jpeg_error jpeg_size(const uint8_t *data, size_t len, uint32_t *width, uint32_t *height);

enum jpeg_error jpeg_decode(const uint8_t *data, size_t len, uint32_t *out, uint32_t width,
                            uint32_t height, uint32_t stride);

const char *jpeg_strerror(enum jpeg_error e);

#endif
