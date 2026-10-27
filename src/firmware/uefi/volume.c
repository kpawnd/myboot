/*
 * Volumes: every partition the firmware exposes through Block I/O (and
 * every unpartitioned disk with a filesystem), probed once for what the
 * loader can read on it:
 *
 *   ext2/3/4   our own driver (src/fs/ext4.c) over the firmware's Disk I/O
 *   FAT        the firmware's Simple File System; we only read the volume
 *              serial and label from its boot sector, for matching
 *   other      whatever else a firmware driver offers Simple File System on
 *
 * BOOT.CFG names a volume as uuid=, partuuid= or label=, the identifiers
 * Linux tools (blkid, lsblk, /etc/fstab) show. Scanning opens every disk,
 * so it happens only when an entry names a volume or the menu looks for
 * Windows; booting from the loader's own volume never scans.
 */
#include "firmware/uefi_boot.h"
#include "core/util.h"
#include "lib/mem.h"

static EFI_GUID guid_blockio = {0x964E5B21, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID guid_diskio = {0xCE345171, 0xBA0B, 0x11D2, {0x8E, 0x4F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID guid_devpath = {0x09576E91, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static struct uefi_vol g_vols[VOL_MAX];
static uint32_t g_nvols;
static bool g_scanned;
static struct uefi_vol g_boot;

static const char g_hex[] = "0123456789abcdef";

/* 16 bytes as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, taking byte order[i]
 * as the i-th printed byte. */
static void id_str(char *out, const uint8_t *b, const uint8_t *order)
{
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t v = b[order ? order[i] : i];
        if (i == 4 || i == 6 || i == 8 || i == 10)
            *out++ = '-';
        *out++ = g_hex[v >> 4];
        *out++ = g_hex[v & 15];
    }
    *out = 0;
}

static void hex_str(char *out, uint64_t v, uint32_t digits, bool upper)
{
    for (uint32_t i = 0; i < digits; i++) {
        char c = g_hex[(v >> (4 * (digits - 1 - i))) & 15];
        out[i] = upper && c >= 'a' ? (char)(c - 32) : c;
    }
    out[digits] = 0;
}

static int disk_read(struct blockdev *dev, uint64_t offset, void *dst, uint64_t len)
{
    struct uefi_vol *v = (struct uefi_vol *)dev;

    return EFI_ERROR(v->dio->ReadDisk(v->dio, v->media_id, offset, len, dst)) ? -1 : 0;
}

/* PARTUUID from the partition's HardDrive device path node (10.3.5.1):
 * the GPT unique partition GUID, or for MBR the disk signature and
 * partition number as Linux prints them ("1234abcd-02"). */
static void partition_id(struct uefi_vol *v)
{
    const uint8_t *node;

    if (EFI_ERROR(g_uefi.bs->HandleProtocol(v->handle, &guid_devpath, (void **)&node)) || !node)
        return;
    for (unsigned i = 0; i < 64 && node[0] != DP_TYPE_END; i++) {
        uint16_t len = rd16(node + 2);
        if (len < 4)
            return;
        if (node[0] == DP_TYPE_MEDIA && node[1] == DP_MEDIA_HD && len >= 42) {
            static const uint8_t guid_order[16] = {3, 2, 1, 0, 5, 4, 7, 6,
                                                   8, 9, 10, 11, 12, 13, 14, 15};
            if (node[41] == 2) {
                id_str(v->partuuid, node + 24, guid_order);
            } else if (node[41] == 1) {
                hex_str(v->partuuid, rd32(node + 24), 8, false);
                v->partuuid[8] = '-';
                hex_str(v->partuuid + 9, rd32(node + 4) & 0xFF, 2, false);
            }
        }
        node += len;
    }
}

/* Serial and label from a FAT boot sector, as blkid shows them. */
static void fat_ids(struct uefi_vol *v, const uint8_t *bs)
{
    uint32_t off, serial, n = 11;

    if (rd16(bs + 510) != 0xAA55)
        return;
    if (!memcmp(bs + 82, "FAT32   ", 8))
        off = 67;
    else if (!memcmp(bs + 54, "FAT", 3))
        off = 39;
    else
        return;
    if (bs[off - 1] != 0x29) /* extended boot signature: serial and label present */
        return;
    serial = rd32(bs + off);
    hex_str(v->uuid, serial >> 16, 4, true);
    v->uuid[4] = '-';
    hex_str(v->uuid + 5, serial & 0xFFFF, 4, true);
    while (n && bs[off + 4 + n - 1] == ' ')
        n--;
    if (n && !(n == 7 && !memcmp(bs + off + 4, "NO NAME", 7))) {
        memcpy(v->label, bs + off + 4, n);
        v->label[n] = 0;
    }
    v->is_fat = 1;
}

static void probe(struct uefi_vol *v)
{
    EFI_BLOCK_IO_PROTOCOL *bio;
    void *sfs;
    uint8_t buf[2048];

    v->fs = VOL_FS_NONE;
    partition_id(v);
    if (EFI_ERROR(g_uefi.bs->HandleProtocol(v->handle, &guid_blockio, (void **)&bio)) || !bio ||
        !bio->Media || !bio->Media->MediaPresent)
        return;
    if (!EFI_ERROR(g_uefi.bs->HandleProtocol(v->handle, &guid_diskio, (void **)&v->dio)) && v->dio) {
        v->media_id = bio->Media->MediaId;
        v->dev.read = disk_read;
        v->dev.size = (bio->Media->LastBlock + 1) * bio->Media->BlockSize;
        if (v->dev.size >= sizeof(buf) && !bdev_read(&v->dev, 0, buf, sizeof(buf))) {
            if (ext4_probe(buf + 1024)) {
                v->fs_err = ext4_mount(&v->ext4, &v->dev);
                id_str(v->uuid, v->ext4.uuid, NULL);
                memcpy(v->label, v->ext4.label, sizeof(v->label));
                if (v->fs_err == FS_OK)
                    v->fs = VOL_FS_EXT4;
                return;
            }
            fat_ids(v, buf);
        }
    }
    if (!EFI_ERROR(g_uefi.bs->HandleProtocol(v->handle, &g_guid_sfs, &sfs)) && sfs)
        v->fs = VOL_FS_FIRMWARE;
}

struct uefi_vol *vol_boot(void)
{
    if (!g_boot.handle) {
        g_boot.handle = g_uefi.li->DeviceHandle;
        g_boot.fs = VOL_FS_FIRMWARE;
    }
    return &g_boot;
}

uint32_t vol_list(struct uefi_vol **out)
{
    EFI_HANDLE *h = NULL;
    UINTN n = 0;

    *out = g_vols;
    if (g_scanned)
        return g_nvols;
    g_scanned = true;
    if (EFI_ERROR(g_uefi.bs->LocateHandleBuffer(ByProtocol, &guid_blockio, NULL, &n, &h)) || !h)
        return 0;
    for (UINTN i = 0; i < n && g_nvols < VOL_MAX; i++) {
        struct uefi_vol *v = &g_vols[g_nvols];

        memset(v, 0, sizeof(*v));
        v->handle = h[i];
        probe(v);
        /* A whole disk counts only if it holds a filesystem itself; a
         * partitioned disk is listed through its partitions. */
        if (v->fs == VOL_FS_NONE && !v->partuuid[0] && v->fs_err == FS_OK)
            continue;
        g_nvols++;
    }
    g_uefi.bs->FreePool(h);
    return g_nvols;
}

/* Same identifier, ignoring case and dashes. */
static bool id_eq(const char *have, const char *want)
{
    for (;;) {
        while (*have == '-')
            have++;
        while (*want == '-')
            want++;
        char a = *have, b = *want;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return false;
        if (!a)
            return true;
        have++;
        want++;
    }
}

static const char *after(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return NULL;
    return s;
}

struct uefi_vol *vol_find(const char *spec)
{
    struct uefi_vol *v;
    uint32_t n = vol_list(&v);
    const char *val;

    for (uint32_t i = 0; i < n; i++, v++) {
        if ((val = after(spec, "uuid=")) && v->uuid[0] && id_eq(v->uuid, val))
            return v;
        if ((val = after(spec, "partuuid=")) && v->partuuid[0] && id_eq(v->partuuid, val))
            return v;
        if ((val = after(spec, "label=")) && v->label[0]) {
            const char *a = v->label;
            while (*a && *a == *val)
                a++, val++;
            if (*a == *val)
                return v;
        }
    }
    return NULL;
}

const char *vol_fs_name(const struct uefi_vol *v)
{
    switch (v->fs) {
    case VOL_FS_EXT4:     return "ext4";
    case VOL_FS_FIRMWARE: return v->is_fat ? "FAT" : "firmware filesystem";
    default:              return v->fs_err ? fs_strerror(v->fs_err) : "no filesystem MYBOOT can read";
    }
}

struct uefi_vol *vol_for_entry(struct cfg_entry *e)
{
    struct uefi_vol *v;

    if (e->volume)
        return e->volume;
    if (!e->volume_spec)
        return e->volume = vol_boot();
    if (!(v = vol_find(e->volume_spec))) {
        boot_error(L"no volume matches", e->volume_spec, EFI_NOT_FOUND);
        return NULL;
    }
    if (v->fs == VOL_FS_NONE) {
        boot_error(L"cannot read volume", e->volume_spec,
                   v->fs_err ? FS_STATUS(v->fs_err) : EFI_UNSUPPORTED);
        return NULL;
    }
    blog_str("Found volume ");
    blog_str(e->volume_spec);
    blog_str(" (");
    blog_str(vol_fs_name(v));
    if (v->partuuid[0]) {
        blog_str(", PARTUUID ");
        blog_str(v->partuuid);
    }
    blog_str(")");
    blog_end(TAG_OK);
    return e->volume = v;
}
