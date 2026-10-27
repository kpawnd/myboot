/*
 * Starting other EFI programs: Linux kernels through their EFI stub, and
 * anything else (Windows Boot Manager, another loader, a UEFI shell).
 *
 * The image file is read by us (one read, counted, progress shown) and
 * handed to the firmware's LoadImage together with its device path, so the
 * started program sees a normal LoadedImage: DeviceHandle is its volume
 * (Windows Boot Manager finds its BCD store through it) and FilePath its
 * path. The firmware relocates the image and, with Secure Boot on,
 * verifies its signature.
 *
 * Linux: the command line goes into LoadOptions as UTF-16. Initrds are
 * served by a LoadFile2 protocol on a vendor media device path with
 * LINUX_EFI_INITRD_MEDIA_GUID, the interface the EFI stub queries since
 * Linux 5.8 (also used by systemd-boot). The stub copies the data into its
 * own memory, so several initrds are simply concatenated, each padded to
 * 4 bytes as cpio requires.
 *
 * StartImage does not return on success: Linux exits boot services itself,
 * and so does Windows. If it does return, everything is undone.
 */
#include "firmware/uefi_boot.h"
#include "firmware/uefi_console.h"
#include "core/util.h"
#include "lib/mem.h"

#define WINDOWS_PATH "\\EFI\\Microsoft\\Boot\\bootmgfw.efi"
#define DEVPATH_MAX  1024

static EFI_GUID guid_devpath = {0x09576E91, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID guid_load_file2 = {0x4006C0C1, 0xFCB3, 0x403E, {0x99, 0x6D, 0x4A, 0x6C, 0x87, 0x24, 0xE0, 0x6D}};

/* ---- initrd service ------------------------------------------------------ */

struct initrd_part {
    uint64_t phys;
    uint64_t pages;
    uint64_t size;
};

static struct initrd_part g_initrd[CFG_MAX_MODULES];
static uint32_t g_ninitrd;
static uint64_t g_initrd_total; /* bytes served, parts padded to 4 */
static EFI_HANDLE g_initrd_handle;

/* VenMedia(LINUX_EFI_INITRD_MEDIA_GUID)/End: what the stub looks up. */
static const struct __attribute__((packed)) {
    uint8_t  type, subtype;
    uint16_t length;
    EFI_GUID guid;
    uint8_t  end_type, end_subtype;
    uint16_t end_length;
} g_initrd_path = {DP_TYPE_MEDIA, DP_MEDIA_VENDOR, 20,
                   {0x5568E427, 0x68FC, 0x4F3D, {0xAC, 0x74, 0xCA, 0x55, 0x52, 0x31, 0xCC, 0x68}},
                   DP_TYPE_END, DP_END_ENTIRE, 4};

static EFI_STATUS EFIAPI initrd_load(EFI_LOAD_FILE2_PROTOCOL *self, void *path, uint8_t boot_policy,
                                     UINTN *size, void *buf)
{
    uint8_t *dst = buf;

    (void)self;
    (void)path;
    if (boot_policy)
        return EFI_UNSUPPORTED;
    if (!size)
        return EFI_INVALID_PARAMETER;
    if (!buf || *size < g_initrd_total) {
        *size = g_initrd_total;
        return EFI_BUFFER_TOO_SMALL;
    }
    for (uint32_t i = 0; i < g_ninitrd; i++) {
        uint64_t padded = align_up(g_initrd[i].size, 4);
        memcpy(dst, (void *)(uintptr_t)g_initrd[i].phys, g_initrd[i].size);
        memset(dst + g_initrd[i].size, 0, padded - g_initrd[i].size);
        dst += padded;
    }
    *size = g_initrd_total;
    return EFI_SUCCESS;
}

static EFI_LOAD_FILE2_PROTOCOL g_initrd_proto = {initrd_load};

static void initrd_release(void)
{
    if (g_initrd_handle) {
        g_uefi.bs->UninstallProtocolInterface(g_initrd_handle, &guid_load_file2, &g_initrd_proto);
        g_uefi.bs->UninstallProtocolInterface(g_initrd_handle, &guid_devpath, (void *)&g_initrd_path);
        g_initrd_handle = NULL;
    }
    for (uint32_t i = 0; i < g_ninitrd; i++)
        if (g_initrd[i].pages)
            g_uefi.bs->FreePages(g_initrd[i].phys, g_initrd[i].pages);
    g_ninitrd = 0;
    g_initrd_total = 0;
}

/* ---- file helpers -------------------------------------------------------- */

/* Whole file into fresh pages. Progress line and errors included. */
static EFI_STATUS read_file(struct uefi_vol *vol, const CHAR16 *what, const char *path,
                            uint64_t *phys, uint64_t *pages, uint64_t *size)
{
    struct kfile f = {0};
    EFI_PHYSICAL_ADDRESS addr = 0;
    EFI_STATUS s;

    *pages = 0;
    boot_progress(what, path);
    if (EFI_ERROR(s = kfile_open_on(&f, vol, path))) {
        kfile_close(&f);
        boot_error(L"cannot open", path, s);
        return s;
    }
    *size = f.size;
    if (f.size > GiB(16)) {
        kfile_close(&f);
        boot_error(L"file larger than 16 GiB", path, EFI_UNSUPPORTED);
        return EFI_UNSUPPORTED;
    }
    if (f.size) {
        UINTN n = (f.size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (EFI_ERROR(s = g_uefi.bs->AllocatePages(AllocateAnyPages, EfiLoaderData, n, &addr))) {
            kfile_close(&f);
            boot_error(L"no memory for", path, s);
            return s;
        }
        *pages = n;
        s = kfile_read(&f, 0, (void *)(uintptr_t)addr, f.size);
    }
    kfile_close(&f);
    *phys = addr;
    if (EFI_ERROR(s)) {
        boot_error(L"read failed", path, s);
        return s;
    }
    boot_progress_done(f.size);
    return EFI_SUCCESS;
}

/* Device path of `vol` followed by a file path node for `path`, in a
 * static buffer. NULL if the volume has no device path or it is too long. */
static void *file_device_path(EFI_HANDLE vol, const char *path)
{
    static uint8_t buf[DEVPATH_MAX];
    const uint8_t *node;
    size_t n = 0, plen = 0;
    uint16_t len;

    if (EFI_ERROR(g_uefi.bs->HandleProtocol(vol, &guid_devpath, (void **)&node)) || !node)
        return NULL;
    for (unsigned i = 0; i < 64 && node[0] != DP_TYPE_END; i++) {
        len = rd16(node + 2);
        if (len < 4 || n + len > sizeof(buf))
            return NULL;
        memcpy(buf + n, node, len);
        n += len;
        node += len;
    }
    while (path[plen])
        plen++;
    len = (uint16_t)(4 + (plen + 1) * 2);
    if (n + len + 4 > sizeof(buf))
        return NULL;
    buf[n] = DP_TYPE_MEDIA;
    buf[n + 1] = DP_MEDIA_FILEPATH;
    memcpy(buf + n + 2, &len, 2);
    for (size_t i = 0; i <= plen; i++) {
        uint16_t c = path[i] == '/' ? 0x5C : (uint8_t)path[i]; /* firmware paths use backslashes */
        memcpy(buf + n + 4 + 2 * i, &c, 2);
    }
    n += len;
    buf[n] = DP_TYPE_END;
    buf[n + 1] = DP_END_ENTIRE;
    buf[n + 2] = 4;
    buf[n + 3] = 0;
    return buf;
}

/* ---- boot ---------------------------------------------------------------- */

EFI_STATUS chain_boot(const struct cfg_entry *e, struct uefi_vol *vol, const char *cmdline,
                      uint32_t cmdline_len, bool *started)
{
    static CHAR16 options[CFG_CMDLINE_MAX + 1];
    bool is_linux = e->type == CFG_LINUX;
    char path[CFG_RESOLVED_MAX];
    uint64_t img = 0, img_pages = 0, img_size = 0;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_HANDLE child = NULL;
    void *dp;
    EFI_STATUS s;

    *started = false;
    if (!cfg_resolve_path(e->kernel, kfile_sep(vol), path)) {
        boot_error(L"path too long", e->kernel, EFI_INVALID_PARAMETER);
        return EFI_INVALID_PARAMETER;
    }
    if (EFI_ERROR(s = read_file(vol, is_linux ? L"Linux" : L"EFI program", path,
                                &img, &img_pages, &img_size)))
        goto out;

    for (uint32_t i = 0; is_linux && i < e->nmod; i++) {
        char ipath[CFG_RESOLVED_MAX];
        struct initrd_part *p = &g_initrd[g_ninitrd];

        if (!cfg_resolve_path(e->mod[i].path, kfile_sep(vol), ipath)) {
            boot_error(L"path too long", e->mod[i].path, s = EFI_INVALID_PARAMETER);
            goto out;
        }
        s = read_file(vol, L"initrd", ipath, &p->phys, &p->pages, &p->size);
        if (p->pages)
            g_ninitrd++;
        if (EFI_ERROR(s))
            goto out;
        g_initrd_total += align_up(p->size, 4);
    }
    if (g_ninitrd) {
        if (EFI_ERROR(s = g_uefi.bs->InstallProtocolInterface(&g_initrd_handle, &guid_devpath,
                                                              EFI_NATIVE_INTERFACE,
                                                              (void *)&g_initrd_path)) ||
            EFI_ERROR(s = g_uefi.bs->InstallProtocolInterface(&g_initrd_handle, &guid_load_file2,
                                                              EFI_NATIVE_INTERFACE,
                                                              &g_initrd_proto))) {
            boot_error(L"cannot offer the initrd to Linux", NULL, s);
            goto out;
        }
    }

    dp = file_device_path(vol->handle, path);
    s = g_uefi.bs->LoadImage(0, g_uefi.image, dp, (void *)(uintptr_t)img, img_size, &child);
    g_uefi.bs->FreePages(img, img_pages); /* LoadImage made its own copy */
    img_pages = 0;
    if (EFI_ERROR(s)) {
        boot_error(is_linux ? L"firmware cannot load this kernel (no EFI stub?)"
                         : L"firmware cannot load this EFI program",
                   path, s);
        child = NULL;
        goto out;
    }

    if (cmdline_len) {
        if (EFI_ERROR(s = g_uefi.bs->HandleProtocol(child, &g_guid_loaded_image, (void **)&li)) ||
            !li) {
            boot_error(L"cannot pass the command line", NULL, s);
            goto out;
        }
        for (uint32_t i = 0; i < cmdline_len; i++)
            options[i] = (uint8_t)cmdline[i];
        options[cmdline_len] = 0;
        li->LoadOptions = options;
        li->LoadOptionsSize = (cmdline_len + 1) * 2;
    }

    blog_str(is_linux ? "Starting Linux ..." : "Starting EFI program ...");
    blog_end(TAG_INFO);
    *started = true;
    s = g_uefi.bs->StartImage(child, NULL, NULL);
    child = NULL; /* an application that returned has been unloaded */
    boot_error(is_linux ? L"Linux returned to the loader" : L"the EFI program returned to the loader",
               path, EFI_ERROR(s) ? s : 0);
    if (!EFI_ERROR(s))
        s = EFI_ABORTED;

out:
    if (child)
        g_uefi.bs->UnloadImage(child);
    if (img_pages)
        g_uefi.bs->FreePages(img, img_pages);
    initrd_release();
    return s;
}

/* ---- Windows detection -------------------------------------------------- */

static bool ends_with_ci(const char *s, const char *tail)
{
    size_t n = 0, t = 0;

    while (s[n])
        n++;
    while (tail[t])
        t++;
    if (t > n)
        return false;
    for (size_t i = 0; i < t; i++) {
        char a = s[n - t + i], b = tail[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return false;
    }
    return true;
}

void chain_add_windows(struct boot_config *c)
{
    static bool done;
    struct uefi_vol *vols;
    uint32_t n;

    if (done || !c->auto_windows || c->nentries == CFG_MAX_ENTRIES)
        return;
    done = true;
    for (uint32_t i = 0; i < c->nentries; i++)
        if (c->entry[i].type == CFG_EFI && ends_with_ci(c->entry[i].kernel, "bootmgfw.efi"))
            return; /* the config already has one */
    n = vol_list(&vols);
    for (uint32_t i = 0; i < n; i++) {
        struct kfile f = {0};
        EFI_STATUS s;

        if (vols[i].fs != VOL_FS_FIRMWARE)
            continue;
        s = kfile_open_on(&f, &vols[i], WINDOWS_PATH);

        kfile_close(&f);
        if (!EFI_ERROR(s)) {
            struct cfg_entry *e = &c->entry[c->nentries++];
            memset(e, 0, sizeof(*e));
            e->title = "Windows Boot Manager";
            e->type = CFG_EFI;
            e->kernel = WINDOWS_PATH;
            e->volume = &vols[i];
            break;
        }
    }
}
