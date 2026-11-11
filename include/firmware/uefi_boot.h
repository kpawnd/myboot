/* Internal interface of the UEFI backend (src/firmware/uefi/). */
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
#include "arch/x86_64/smp.h"

struct uefi_ctx {
    EFI_HANDLE                 image;
    EFI_SYSTEM_TABLE          *st;
    EFI_BOOT_SERVICES         *bs;
    EFI_LOADED_IMAGE_PROTOCOL *li;
};
extern struct uefi_ctx g_uefi;

extern EFI_GUID g_guid_loaded_image;

struct uefi_mmap {
    uint8_t *buf;
    UINTN    capacity;
    UINTN    size;
    UINTN    key;
    UINTN    desc_size;
    uint32_t desc_version;
    uint32_t pages;
};

EFI_STATUS uefi_mmap_alloc(struct uefi_mmap *m, uint32_t slack_descs);
EFI_STATUS uefi_mmap_read(struct uefi_mmap *m);
void       uefi_mmap_free(struct uefi_mmap *m);
uint32_t   uefi_mmap_count(const struct uefi_mmap *m);
uint32_t   uefi_mmap_capacity(const struct uefi_mmap *m);
const EFI_MEMORY_DESCRIPTOR *uefi_mmap_desc(const struct uefi_mmap *m, uint32_t i);
int        uefi_mmap_type_at(const struct uefi_mmap *m, uint64_t addr);
struct mmap_carve {
    uint64_t base, end;
    uint32_t type;
};
uint32_t   uefi_mmap_normalize(const struct uefi_mmap *m, struct boot_mem_entry *out, uint32_t cap,
                               const struct mmap_carve *carve, uint32_t ncarve);

struct uefi_memory {
    struct uefi_mmap map;
    uint64_t usable;
    uint64_t total;
    uint64_t top;
};
struct uefi_memory            *discover_memory(void);
const struct acpi_info        *discover_acpi(void);
const struct smbios_info      *discover_smbios(void);
const struct boot_framebuffer *discover_graphics(void);
const struct boot_device      *discover_storage(void);
const struct boot_csme        *discover_csme(void);
const struct csme_clients     *discover_csme_clients(void);

struct smp_cpus {
    uint32_t                   total, count;
    const struct smp_madt_cpu *cpu;
};
const struct smp_cpus         *discover_smp(void);
bool uefi_phys_range_ok(uint64_t addr, uint64_t len);

enum vol_fs { VOL_FS_NONE, VOL_FS_FIRMWARE, VOL_FS_EXT4 };
#define VOL_MAX    32
#define VOL_ID_MAX 37
struct uefi_vol {
    struct blockdev dev;
    EFI_HANDLE      handle;
    uint32_t        fs;
    char            partuuid[VOL_ID_MAX];
    char            uuid[VOL_ID_MAX];
    char            label[17];
    uint8_t         is_fat;
    uint32_t        fs_err;
    EFI_DISK_IO_PROTOCOL *dio;
    uint32_t        media_id;
    struct ext4_fs  ext4;
};
struct uefi_vol *vol_boot(void);
struct uefi_vol *vol_find(const char *spec);
uint32_t         vol_list(struct uefi_vol **out);
struct uefi_vol *vol_for_entry(struct cfg_entry *e);
const char      *vol_fs_name(const struct uefi_vol *v);

struct kfile {
    struct uefi_vol   *vol;
    EFI_FILE_PROTOCOL *root, *file;
    struct ext4_file   ext4;
    uint64_t size;
};
struct file_stats {
    uint32_t reads;
    uint64_t bytes;
};
extern struct file_stats g_file_stats;
extern EFI_GUID g_guid_sfs;
#define FS_STATUS(e) (EFI_ERROR_BIT | EFI_OEM_BIT | (EFI_STATUS)(e))
EFI_STATUS kfile_open(struct kfile *kf, const char *path);
EFI_STATUS kfile_open_on(struct kfile *kf, struct uefi_vol *vol, const char *path);
EFI_STATUS kfile_read(struct kfile *kf, uint64_t offset, void *dst, uint64_t len);
void       kfile_close(struct kfile *kf);
char       kfile_sep(const struct uefi_vol *vol);

enum boot_tag { TAG_INFO, TAG_OK, TAG_WARN, TAG_FAILED, TAG_BUSY, TAG_NONE };

void boot_progress(const CHAR16 *what, const char *path);
void boot_progress_done(uint64_t bytes);

void blog_str(const char *s);
void blog_wstr(const CHAR16 *s);
void blog_ascii(const char *s, size_t max);
void blog_dec(uint64_t v);
void blog_hex(uint64_t v);
void blog_size(uint64_t bytes);
void blog_end(enum boot_tag tag);

#define BOOT_ERRORS_MAX 12
#define BOOT_ERROR_LINE 200
void        boot_error(const CHAR16 *what, const char *detail, EFI_STATUS status);
void        boot_error_line(const char *file, uint32_t line, const char *msg);
void        boot_error_more(const char *file, uint32_t count);
const char *boot_status_name(EFI_STATUS s);
uint32_t    boot_errors_count(void);
uint32_t boot_errors_draw(uint32_t row, uint32_t max_rows, uint32_t width);
void     boot_errors_clear(void);

EFI_STATUS chain_boot(const struct cfg_entry *e, struct uefi_vol *vol, const char *cmdline,
                      uint32_t cmdline_len, bool *started);
void chain_add_windows(struct boot_config *c);

#define GFX_NONE 0xFFFFFFFFu
#define GFX_LINE(scale) (12 * (scale))
bool gfx_open(const char *cfg_path);
void gfx_close(void);
void gfx_geometry(uint32_t *width, uint32_t *height, uint32_t *scale);
void gfx_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t border);
void gfx_show(void);
void gfx_text(uint32_t x, uint32_t y, uint32_t w, uint32_t indent, const char *s, uint32_t fg,
              uint32_t bar);

enum menu_action {
    MENU_BOOT,
    MENU_RETURN
};
struct menu_choice {
    uint32_t    action;
    uint32_t    entry;
    const char *cmdline;
    uint32_t    cmdline_len;
};
void menu_choose(const struct boot_config *c, struct menu_choice *out, void (*prepare)(void));
void menu_recover(const struct boot_config *c, struct menu_choice *out, void (*prepare)(void));

#endif
