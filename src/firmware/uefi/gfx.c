/*
 * Graphical menu screen: the BOOT.CFG background picture, scaled to cover
 * the screen, with text drawn over it. Everything reaches the screen
 * through GOP Blt (UEFI 12.9), so the framebuffer's pixel format does not
 * matter and Blt-only adapters work too.
 *
 * The picture is decoded once into a screen-sized buffer (`g_bg`); a line
 * of text is composed in `g_line` from the picture under it, an optional
 * bar and the glyphs, then sent with one Blt. Text is font8x8 with each
 * dot drawn as scale x scale pixels.
 */
#include "core/util.h"
#include "firmware/uefi_boot.h"
#include "image/jpeg.h"
#include "lib/font8x8.h"
#include "lib/mem.h"

#define BG_FILE_MAX MiB(32)
#define PAGES(bytes) (((bytes) + PAGE_SIZE - 1) / PAGE_SIZE)

static EFI_GUID guid_gop = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};

static EFI_GRAPHICS_OUTPUT_PROTOCOL *g_gop;
static uint32_t *g_bg, *g_line;
static uint64_t  g_bg_pages, g_line_pages;
static uint32_t  g_w, g_h, g_scale;

static void *alloc(uint64_t bytes, uint64_t *pages)
{
    EFI_PHYSICAL_ADDRESS a;

    *pages = PAGES(bytes);
    if (EFI_ERROR(g_uefi.bs->AllocatePages(AllocateAnyPages, EfiLoaderData, *pages, &a))) {
        *pages = 0;
        return NULL;
    }
    return (void *)(uintptr_t)a;
}

static void release(void *p, uint64_t pages)
{
    if (p && pages)
        g_uefi.bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)p, pages);
}

/* Scale the iw x ih picture to cover the screen, keeping its aspect ratio
 * and cutting the overhang evenly. Each screen pixel is the average of the
 * picture pixels it covers, so shrinking does not alias. */
static void cover(const uint32_t *img, uint32_t iw, uint32_t ih)
{
    uint64_t sw = iw, sh = ih, sx = 0, sy = 0; /* the part of the picture shown */

    if ((uint64_t)iw * g_h > (uint64_t)ih * g_w) {
        sw = (uint64_t)ih * g_w / g_h;
        sx = (iw - sw) / 2;
    } else {
        sh = (uint64_t)iw * g_h / g_w;
        sy = (ih - sh) / 2;
    }
    if (!sw)
        sw = 1;
    if (!sh)
        sh = 1;
    for (uint32_t y = 0; y < g_h; y++) {
        uint64_t y0 = sy + y * sh / g_h, y1 = sy + (y + 1) * sh / g_h;

        if (y1 <= y0)
            y1 = y0 + 1;
        for (uint32_t x = 0; x < g_w; x++) {
            uint64_t x0 = sx + x * sw / g_w, x1 = sx + (x + 1) * sw / g_w, r = 0, g = 0, b = 0, n;

            if (x1 <= x0)
                x1 = x0 + 1;
            for (uint64_t yy = y0; yy < y1; yy++)
                for (uint64_t xx = x0; xx < x1; xx++) {
                    uint32_t p = img[yy * iw + xx];
                    r += p >> 16 & 0xFF;
                    g += p >> 8 & 0xFF;
                    b += p & 0xFF;
                }
            n = (y1 - y0) * (x1 - x0);
            g_bg[(uint64_t)y * g_w + x] = (uint32_t)(r / n << 16 | g / n << 8 | b / n);
        }
    }
}

/* Read and decode the file into g_bg. False after a boot_error. */
static bool load(const char *cfg_path)
{
    char path[CFG_RESOLVED_MAX];
    struct kfile f = {0};
    uint8_t *data = NULL;
    uint32_t *img = NULL;
    uint64_t data_pages = 0, img_pages = 0;
    uint32_t iw, ih;
    enum jpeg_error je;
    EFI_STATUS s;
    bool ok = false;

    if (!cfg_resolve_path(cfg_path, kfile_sep(vol_boot()), path)) {
        boot_error(L"menu background path too long", cfg_path, 0);
        return false;
    }
    boot_progress(L"menu background", path);
    if (EFI_ERROR(s = kfile_open(&f, path)))
        boot_error(L"cannot open the menu background", path, s);
    else if (f.size > BG_FILE_MAX)
        boot_error(L"menu background larger than 32 MiB", path, 0);
    else if (!(data = alloc(f.size ? f.size : 1, &data_pages)))
        boot_error(L"no memory for the menu background", path, EFI_OUT_OF_RESOURCES);
    else if (EFI_ERROR(s = kfile_read(&f, 0, data, f.size)))
        boot_error(L"menu background read failed", path, s);
    else if ((je = jpeg_size(data, f.size, &iw, &ih)) != JPEG_OK)
        boot_error(L"cannot use the menu background", jpeg_strerror(je), 0);
    else if (!(img = alloc((uint64_t)iw * ih * 4, &img_pages)))
        boot_error(L"no memory for the menu background", path, EFI_OUT_OF_RESOURCES);
    else if ((je = jpeg_decode(data, f.size, img, iw, ih, iw)) != JPEG_OK)
        boot_error(L"cannot use the menu background", jpeg_strerror(je), 0);
    else {
        boot_progress_done(f.size);
        cover(img, iw, ih);
        ok = true;
    }
    kfile_close(&f);
    release(img, img_pages);
    release(data, data_pages);
    return ok;
}

bool gfx_open(const char *cfg_path)
{
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

    if (EFI_ERROR(g_uefi.bs->LocateProtocol(&guid_gop, NULL, (void **)&g_gop)) || !g_gop ||
        !g_gop->Mode || !(info = g_gop->Mode->Info))
        return false; /* no graphics: the text menu is all there is */
    g_w = info->HorizontalResolution;
    g_h = info->VerticalResolution;
    if (g_w < 320 || g_h < 200 || g_w > 16384 || g_h > 16384)
        return false;
    /* 8x8 dots: 16 px glyphs at 1080 lines, 8 px at 600 */
    g_scale = g_h / 384 ? g_h / 384 : 1;

    if (!(g_bg = alloc((uint64_t)g_w * g_h * 4, &g_bg_pages)) ||
        !(g_line = alloc((uint64_t)g_w * GFX_LINE(g_scale) * 4, &g_line_pages))) {
        boot_error(L"no memory for the menu background", NULL, EFI_OUT_OF_RESOURCES);
        gfx_close();
        return false;
    }
    if (!load(cfg_path)) {
        gfx_close();
        return false;
    }
    return true;
}

void gfx_close(void)
{
    release(g_bg, g_bg_pages);
    release(g_line, g_line_pages);
    g_bg = g_line = NULL;
    g_bg_pages = g_line_pages = 0;
}

void gfx_geometry(uint32_t *width, uint32_t *height, uint32_t *scale)
{
    *width = g_w;
    *height = g_h;
    *scale = g_scale;
}

static uint32_t darken(uint32_t p)
{
    /* 3/8 of each channel */
    return ((p >> 16 & 0xFF) * 3 / 8) << 16 | ((p >> 8 & 0xFF) * 3 / 8) << 8 | (p & 0xFF) * 3 / 8;
}

void gfx_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t border)
{
    uint32_t t = g_scale;

    if (x >= g_w || y >= g_h)
        return;
    if (w > g_w - x)
        w = g_w - x;
    if (h > g_h - y)
        h = g_h - y;
    for (uint32_t yy = y; yy < y + h; yy++)
        for (uint32_t xx = x; xx < x + w; xx++) {
            uint32_t *p = &g_bg[(uint64_t)yy * g_w + xx];
            bool edge = xx < x + t || xx >= x + w - t || yy < y + t || yy >= y + h - t;
            *p = edge ? border : darken(*p);
        }
}

void gfx_show(void)
{
    g_gop->Blt(g_gop, g_bg, EfiBltBufferToVideo, 0, 0, 0, 0, g_w, g_h, 0);
}

void gfx_text(uint32_t x, uint32_t y, uint32_t w, uint32_t indent, const char *s, uint32_t fg,
              uint32_t bar)
{
    uint32_t lh = GFX_LINE(g_scale), cw = 8 * g_scale, top = 2 * g_scale;

    if (x >= g_w || y >= g_h || !w)
        return;
    if (w > g_w - x)
        w = g_w - x;
    if (lh > g_h - y)
        lh = g_h - y;
    for (uint32_t r = 0; r < lh; r++)
        for (uint32_t c = 0; c < w; c++)
            g_line[r * w + c] = bar == GFX_NONE ? g_bg[(uint64_t)(y + r) * g_w + x + c] : bar;

    for (uint32_t cx = indent; *s && cx + cw <= w; s++, cx += cw) {
        const uint8_t *glyph = font8x8_basic[(uint8_t)*s & 0x7F];

        for (uint32_t gy = 0; gy < 8 * g_scale && top + gy < lh; gy++) {
            uint8_t bits = glyph[gy / g_scale];
            for (uint32_t gx = 0; gx < cw; gx++)
                if (bits >> (gx / g_scale) & 1)
                    g_line[(top + gy) * w + cx + gx] = fg;
        }
    }
    g_gop->Blt(g_gop, g_line, EfiBltBufferToVideo, 0, 0, x, y, w, lh, (UINTN)w * 4);
}
