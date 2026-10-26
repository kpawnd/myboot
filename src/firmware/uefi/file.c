/*
 * Kernel file access through the firmware's Simple File System on the
 * volume the loader came from (UEFI 2.10 sections 13.4-13.5). No FAT code
 * of our own runs on the UEFI path.
 *
 * Every byte read goes through kfile_read so the disk-read counters in
 * boot_info are complete.
 */
#include "firmware/uefi_boot.h"

static EFI_GUID guid_sfs = {0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

EFI_STATUS kfile_open(struct kfile *kf, const CHAR16 *path)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS s;

    s = g_uefi.bs->HandleProtocol(g_uefi.li->DeviceHandle, &guid_sfs, (void **)&fs);
    if (EFI_ERROR(s))
        return s;
    if ((s = fs->OpenVolume(fs, &kf->root)) != EFI_SUCCESS)
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    s = kf->root->Open(kf->root, &kf->file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
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
        kf->reads++;
        kf->bytes += got;
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
