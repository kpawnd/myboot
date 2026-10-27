/*
 * File access (config, kernel, modules). FAT and other firmware-readable
 * volumes go through the firmware's Simple File System (UEFI 2.10 sections
 * 13.4-13.5): no FAT code of our own runs on the UEFI path. ext2/3/4
 * volumes go through our own driver (src/fs/ext4.c).
 *
 * Every byte read goes through kfile_read so the disk-read counters in
 * boot_info are complete.
 */
#include "firmware/uefi_boot.h"

EFI_GUID g_guid_sfs = {0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

struct file_stats g_file_stats;

EFI_STATUS kfile_open(struct kfile *kf, const char *path)
{
    return kfile_open_on(kf, vol_boot(), path);
}

char kfile_sep(const struct uefi_vol *vol)
{
    return vol->fs == VOL_FS_EXT4 ? '/' : '\\';
}

static EFI_STATUS fs_status(enum fs_error e)
{
    switch (e) {
    case FS_OK:        return EFI_SUCCESS;
    case FS_NOT_FOUND: return EFI_NOT_FOUND;
    case FS_IO:        return EFI_DEVICE_ERROR;
    case FS_CORRUPT:   return EFI_VOLUME_CORRUPTED;
    default:           return FS_STATUS(e);
    }
}

EFI_STATUS kfile_open_on(struct kfile *kf, struct uefi_vol *vol, const char *path)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    CHAR16 wpath[CFG_RESOLVED_MAX];
    EFI_STATUS s;
    size_t i;

    kf->vol = vol;
    if (vol->fs == VOL_FS_EXT4) {
        if (EFI_ERROR(s = fs_status(ext4_open(&vol->ext4, path, &kf->ext4))))
            return s;
        kf->size = kf->ext4.size;
        return EFI_SUCCESS;
    }
    if (vol->fs != VOL_FS_FIRMWARE)
        return vol->fs_err ? fs_status(vol->fs_err) : EFI_UNSUPPORTED;

    /* ASCII to UTF-16: config paths are validated printable ASCII. */
    for (i = 0; path[i]; i++) {
        if (i + 1 == CFG_RESOLVED_MAX)
            return EFI_INVALID_PARAMETER;
        wpath[i] = path[i] == '/' ? '\\' : (uint8_t)path[i];
    }
    wpath[i] = 0;

    s = g_uefi.bs->HandleProtocol(vol->handle, &g_guid_sfs, (void **)&fs);
    if (EFI_ERROR(s))
        return s;
    if ((s = fs->OpenVolume(fs, &kf->root)) != EFI_SUCCESS)
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    s = kf->root->Open(kf->root, &kf->file, wpath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) {
        kf->file = 0;
        return s;
    }
    /* Size without EFI_FILE_INFO (no buffer, no GetInfo): seeking to
     * all-ones moves to end of file (13.5.13), then read the position. */
    if (EFI_ERROR(s = kf->file->SetPosition(kf->file, UINT64_MAX)) ||
        EFI_ERROR(s = kf->file->GetPosition(kf->file, &kf->size)))
        return s;
    return EFI_SUCCESS;
}

EFI_STATUS kfile_read(struct kfile *kf, uint64_t offset, void *dst, uint64_t len)
{
    uint8_t *p = dst;
    EFI_STATUS s;

    if (offset > kf->size || len > kf->size - offset)
        return EFI_INVALID_PARAMETER;
    if (kf->vol && kf->vol->fs == VOL_FS_EXT4) {
        if (EFI_ERROR(s = fs_status(ext4_read(&kf->ext4, offset, dst, len))))
            return s;
        g_file_stats.reads++;
        g_file_stats.bytes += len;
        return EFI_SUCCESS;
    }
    if (EFI_ERROR(s = kf->file->SetPosition(kf->file, offset)))
        return s;
    /* Read may legitimately return fewer bytes than asked; loop until done.
     * A zero-byte return before the end means the file shrank or is corrupt. */
    while (len) {
        UINTN got = len;
        if (EFI_ERROR(s = kf->file->Read(kf->file, &got, p)))
            return s;
        if (got == 0 || got > len)
            return EFI_VOLUME_CORRUPTED;
        g_file_stats.reads++;
        g_file_stats.bytes += got;
        p += got;
        len -= got;
    }
    return EFI_SUCCESS;
}

void kfile_close(struct kfile *kf)
{
    if (kf->file)
        kf->file->Close(kf->file);
    if (kf->root)
        kf->root->Close(kf->root);
    kf->file = kf->root = 0;
}
