/* Boot configuration: \EFI\MYBOOT\BOOT.CFG on the boot volume. */
#ifndef CONFIG_CONFIG_H
#define CONFIG_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include "bootinfo/bootinfo.h"

#define CFG_MAX_ENTRIES  9
#define CFG_MAX_MODULES  8
#define CFG_MAX_ERRORS   4
#define CFG_TITLE_MAX    63
#define CFG_PATH_MAX     127
#define CFG_VOLUME_MAX   63
#define CFG_CMDLINE_MAX  1023
#define CFG_MODSTR_MAX   BOOT_MODULE_STRING_MAX
#define CFG_DIR          "\\EFI\\MYBOOT\\"
#define CFG_RESOLVED_MAX (sizeof(CFG_DIR) + CFG_PATH_MAX)

struct cfg_module {
    const char *path;
    const char *string;
};

enum cfg_type {
    CFG_KERNEL,
    CFG_LINUX,
    CFG_EFI
};

struct cfg_entry {
    const char *title;
    uint32_t    type;
    const char *kernel;
    const char *volume_spec;
    void       *volume;
    const char *cmdline;
    uint32_t    cmdline_len;
    uint32_t    nmod;
    struct cfg_module mod[CFG_MAX_MODULES];
};

struct cfg_error {
    uint32_t    line;
    const char *msg;
};

struct boot_config {
    uint32_t timeout;
    uint32_t default_entry;
    uint32_t auto_windows;
    const char *background;
    uint32_t nentries;
    struct cfg_entry entry[CFG_MAX_ENTRIES];
    uint32_t nerrors;
    struct cfg_error error[CFG_MAX_ERRORS];
};

void cfg_parse(char *text, size_t len, struct boot_config *out);

int cfg_resolve_path(const char *path, char sep, char *out);

#endif
