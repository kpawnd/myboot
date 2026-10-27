/*
 * Internal interface of the UEFI backend (src/firmware/uefi/). Nothing here
 * is valid after ExitBootServices except the uefi_mmap buffer contents.
 */
#ifndef FIRMWARE_UEFI_BOOT_H
#define FIRMWARE_UEFI_BOOT_H

#include <stdbool.h>
#include <stdint.h>
#include "firmware/uefi.h"
#include "bootinfo/bootinfo.h"
#include "acpi/acpi.h"
#include "smbios/smbios.h"
#include "config/config.h"
#include "fs/ext4.h"

struct uefi_ctx {
    EFI_HANDLE                 image;
    EFI_SYSTEM_TABLE          *st;
    EFI_BOOT_SERVICES         *bs;
    EFI_LOADED_IMAGE_PROTOCOL *li;
};
extern struct uefi_ctx g_uefi;

extern EFI_GUID g_guid_loaded_image;

/* Raw firmware memory map in one page allocation we own. The buffer is
 * sized once with slack and reused for the final pre-exit read, so no
 * allocation happens between the final GetMemoryMap and ExitBootServices. */
struct uefi_mmap {
    uint8_t *buf;
    UINTN    capacity;  /* bytes allocated */
    UINTN    size;      /* bytes valid after the last read */
    UINTN    key;
    UINTN    desc_size; /* stride; may exceed sizeof(EFI_MEMORY_DESCRIPTOR) */
    uint32_t desc_version;
    uint32_t pages;
};

EFI_STATUS uefi_mmap_alloc(struct uefi_mmap *m, uint32_t slack_descs);
EFI_STATUS uefi_mmap_read(struct uefi_mmap *m);
void       uefi_mmap_free(struct uefi_mmap *m);
uint32_t   uefi_mmap_count(const struct uefi_mmap *m);
uint32_t   uefi_mmap_capacity(const struct uefi_mmap *m);
const EFI_MEMORY_DESCRIPTOR *uefi_mmap_desc(const struct uefi_mmap *m, uint32_t i);
int        uefi_mmap_type_at(const struct uefi_mmap *m, uint64_t addr); /* -1: not mapped */
/* A range of our own EfiLoaderData allocations that the kernel must see
 * with a more specific type (the kernel image, modules). */
struct mmap_carve {
    uint64_t base, end;
    uint32_t type; /* enum boot_mem_type */
};
/* Convert to boot protocol entries, giving each carve its type. Carves are
 * sorted by base and do not overlap; each adds at most 2 entries. Output is
 * unsorted; cannot fail. */
uint32_t   uefi_mmap_normalize(const struct uefi_mmap *m, struct boot_mem_entry *out, uint32_t cap,
                               const struct mmap_carve *carve, uint32_t ncarve);

/* Discovery: each runs once, caches its result and returns it. */
struct uefi_memory {
    struct uefi_mmap map;
    uint64_t usable; /* free after handoff: conventional + boot services + loader */
    uint64_t total;  /* all RAM-backed types */
    uint64_t top;    /* highest end address of any RAM-backed entry */
};
struct uefi_memory            *discover_memory(void); /* NULL on failure */
const struct acpi_info        *discover_acpi(void);   /* rsdp == 0 if absent */
const struct smbios_info      *discover_smbios(void); /* entry == 0 if absent */
const struct boot_framebuffer *discover_graphics(void); /* base == 0 if absent */
const struct boot_device      *discover_storage(void);
bool uefi_phys_range_ok(uint64_t addr, uint64_t len);

/* Volumes (volume.c): partitions and whole disks holding a filesystem the
 * loader can read, either through the firmware (FAT, and whatever its
 * drivers add) or with its own driver (ext2/3/4). */
enum vol_fs { VOL_FS_NONE, VOL_FS_FIRMWARE, VOL_FS_EXT4 };
#define VOL_MAX    32
#define VOL_ID_MAX 37 /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" and the NUL */
struct uefi_vol {
    struct blockdev dev;               /* first: dev pointers convert back */
    EFI_HANDLE      handle;
    uint32_t        fs;                /* enum vol_fs */
    char            partuuid[VOL_ID_MAX]; /* GPT GUID or MBR "ssssssss-pp"; "" if none */
    char            uuid[VOL_ID_MAX];  /* ext4 UUID or FAT serial "XXXX-XXXX"; "" if none */
    char            label[17];
    uint8_t         is_fat;
    uint32_t        fs_err;            /* enum fs_error: why ext4 could not be mounted */
    EFI_DISK_IO_PROTOCOL *dio;
    uint32_t        media_id;
    struct ext4_fs  ext4;
};
/* The volume the loader was started from; never scans. */
struct uefi_vol *vol_boot(void);
/* The first volume matching a BOOT.CFG volume spec (uuid=, partuuid=,
 * label=), NULL if none. Scans every disk the first time. */
struct uefi_vol *vol_find(const char *spec);
/* Every volume found; scans the first time. */
uint32_t         vol_list(struct uefi_vol **out);
/* The volume an entry's files are on (its volume line, else the boot
 * volume), found and logged the first time; NULL after logging why not. */
struct uefi_vol *vol_for_entry(struct cfg_entry *e);
const char      *vol_fs_name(const struct uefi_vol *v);

/* Files (config, kernel, modules, images). Every read goes through
 * kfile_read, which counts it in g_file_stats. */
struct kfile {
    struct uefi_vol   *vol;
    EFI_FILE_PROTOCOL *root, *file; /* firmware volumes */
    struct ext4_file   ext4;        /* ext4 volumes */
    uint64_t size;
};
struct file_stats {
    uint32_t reads;
    uint64_t bytes;
};
extern struct file_stats g_file_stats;
extern EFI_GUID g_guid_sfs; /* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
/* `path` is ASCII and absolute (see cfg_resolve_path). kfile_open reads
 * from the boot volume, kfile_open_on from any volume. Filesystem errors
 * of the loader's own drivers come back as FS_STATUS codes, which
 * boot_status_name describes. */
#define FS_STATUS(e) (EFI_ERROR_BIT | EFI_OEM_BIT | (EFI_STATUS)(e))
EFI_STATUS kfile_open(struct kfile *kf, const char *path);
EFI_STATUS kfile_open_on(struct kfile *kf, struct uefi_vol *vol, const char *path);
EFI_STATUS kfile_read(struct kfile *kf, uint64_t offset, void *dst, uint64_t len);
void       kfile_close(struct kfile *kf);
/* The path separator files on `vol` are named with. */
char       kfile_sep(const struct uefi_vol *vol);

/* The boot log (error.c): one line per step, tagged like systemd's boot
 * messages: "[  OK  ] Loaded kernel X (46 KiB)", "[FAILED] ...",
 * "[ WARN ] ...", nine spaces for plain information, or nothing for a
 * heading. */
enum boot_tag { TAG_INFO, TAG_OK, TAG_WARN, TAG_FAILED, TAG_BUSY, TAG_NONE };

/* "[  ..  ] Loading <what> <path>", rewritten in place as "[  OK  ] Loaded
 * <what> <path> (<size>)", or as [FAILED] when a boot_error comes first. */
void boot_progress(const CHAR16 *what, const char *path); /* what: L"kernel" */
void boot_progress_done(uint64_t bytes);

/* Any other line: the blog_* calls build it, blog_end prints it. */
void blog_str(const char *s);
void blog_wstr(const CHAR16 *s);
void blog_ascii(const char *s, size_t max);
void blog_dec(uint64_t v);
void blog_hex(uint64_t v);
void blog_size(uint64_t bytes); /* "170 bytes", "46 KiB", "15.0 MiB" */
void blog_end(enum boot_tag tag);

/* Errors ([FAILED]) and config warnings ([ WARN ]): printed at once and kept
 * for the menu screens. */
#define BOOT_ERRORS_MAX 12
#define BOOT_ERROR_LINE 200
void        boot_error(const CHAR16 *what, const char *detail, EFI_STATUS status); /* status 0: omitted */
void        boot_error_line(const char *file, uint32_t line, const char *msg);
void        boot_error_more(const char *file, uint32_t count);
const char *boot_status_name(EFI_STATUS s); /* NULL if not a common one */
uint32_t    boot_errors_count(void);
/* Draws kept errors from `row`, one per row cut to `width`; returns rows used. */
uint32_t boot_errors_draw(uint32_t row, uint32_t max_rows, uint32_t width);
void     boot_errors_clear(void);

/* Chainloading (chain.c): Linux EFI-stub kernels and other EFI programs.
 * Returns only if the entry could not be started (*started false: try the
 * next entry) or the started program returned (*started true). */
EFI_STATUS chain_boot(const struct cfg_entry *e, struct uefi_vol *vol, const char *cmdline,
                      uint32_t cmdline_len, bool *started);
/* Adds a "Windows Boot Manager" entry if one is found on any volume.
 * Opens every volume, so it runs only when the menu is shown; once. */
void chain_add_windows(struct boot_config *c);

/* Boot menu and recovery screen (menu.c). Reboot, power off and firmware
 * setup are carried out inside the menu and do not return. */
enum menu_action {
    MENU_BOOT,   /* load entry `entry` (then the others as fallbacks) */
    MENU_RETURN  /* return to the firmware boot manager */
};
struct menu_choice {
    uint32_t    action;      /* enum menu_action */
    uint32_t    entry;       /* 0-based */
    const char *cmdline;     /* non-NULL: edited at the menu, replaces the entry's */
    uint32_t    cmdline_len;
};
/* The normal menu: shown when BOOT.CFG sets a timeout, or when a key is
 * already waiting at this point; otherwise boots the default at once. */
void menu_choose(const struct boot_config *c, struct menu_choice *out, void (*prepare)(void));
/* After every entry failed: errors plus the menu, with a countdown that
 * returns to the firmware. Without a keyboard it returns at once. */
void menu_recover(const struct boot_config *c, struct menu_choice *out, void (*prepare)(void));

#endif
