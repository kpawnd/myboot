/*
 * Test EFI program for chainloading (tests/run.sh). Prints what it was
 * given, then either ends QEMU through isa-debug-exit with status 67
 * ((0x21 << 1) | 1), or returns to its caller if its load options contain
 * "return", to exercise the loader's handling of a program that comes back.
 */
#include <stddef.h>
#include "firmware/uefi.h"

static EFI_GUID guid_loaded_image = {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static void out(EFI_SYSTEM_TABLE *st, const CHAR16 *s)
{
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)s);
}

static int has_return(const CHAR16 *s, uint32_t n)
{
    static const CHAR16 key[] = L"return";

    for (uint32_t i = 0; i + 6 <= n; i++) {
        uint32_t k = 0;
        while (k < 6 && s[i + k] == key[k])
            k++;
        if (k == 6)
            return 1;
    }
    return 0;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    const CHAR16 *opts = L"";
    uint32_t n = 0;

    st->BootServices->HandleProtocol(image, &guid_loaded_image, (void **)&li);
    if (li && li->LoadOptions && li->LoadOptionsSize >= 2) {
        opts = li->LoadOptions;
        n = li->LoadOptionsSize / 2;
    }
    out(st, L"chaintest: running, load options: ");
    out(st, opts);
    out(st, li && li->DeviceHandle ? L"\r\nchaintest: device handle set\r\n"
                                   : L"\r\nchaintest: no device handle\r\n");
    if (has_return(opts, n)) {
        out(st, L"chaintest: returning to the loader\r\n");
        return EFI_SUCCESS;
    }
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0x21), "Nd"((uint16_t)0xF4));
    for (;;)
        __asm__ volatile("hlt");
}
