/*
 * Boot configuration: \EFI\MYBOOT\BOOT.CFG on the boot volume.
 *
 * Plain ASCII, one directive per line, '#' starts a comment line:
 *
 *   timeout 3                 seconds the menu waits; 0 = no menu unless a
 *                             key is pressed during startup (default)
 *   default 2                 1-based entry to boot (default 1)
 *   entry Debug kernel        starts an entry; the rest of the line is its title
 *   kernel KERNEL.ELF         the kernel to load (required in every entry)
 *   cmdline root=/dev/sda2    the kernel command line
 *   module INITRD.IMG initrd  a file loaded for the kernel, then an optional
 *                             string the kernel sees with it
 *
 * Paths are on the boot volume. '/' and '\' are both accepted; a path that
 * starts with neither is relative to \EFI\MYBOOT\. Paths cannot contain
 * spaces. If an entry cannot be loaded, the loader tries the next one.
 *
 * The parser works in place: it NUL-terminates fields inside the caller's
 * buffer and stores pointers to them, so nothing is copied. A line it
 * cannot use is recorded as an error and skipped; parsing as a whole never
 * fails, so one typo does not make every entry unbootable.
 */
#ifndef CONFIG_CONFIG_H
#define CONFIG_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include "bootinfo/bootinfo.h"

#define CFG_MAX_ENTRIES  9    /* one digit each in the menu */
#define CFG_MAX_MODULES  8
#define CFG_MAX_ERRORS   4    /* errors kept with their line; more are counted */
#define CFG_TITLE_MAX    63   /* longer titles are truncated */
#define CFG_PATH_MAX     127
#define CFG_CMDLINE_MAX  1023
#define CFG_MODSTR_MAX   BOOT_MODULE_STRING_MAX
#define CFG_DIR          "\\EFI\\MYBOOT\\"
/* Longest resolved path, including CFG_DIR and the NUL. */
#define CFG_RESOLVED_MAX (sizeof(CFG_DIR) + CFG_PATH_MAX)

struct cfg_module {
    const char *path;
    const char *string; /* NULL if not given */
};

struct cfg_entry {
    const char *title;
    const char *kernel;
    const char *cmdline; /* NULL if not given */
    uint32_t    cmdline_len;
    uint32_t    nmod;
    struct cfg_module mod[CFG_MAX_MODULES];
};

struct cfg_error {
    uint32_t    line;
    const char *msg;
};

struct boot_config {
    uint32_t timeout;       /* seconds; 0 = boot the default without a menu */
    uint32_t default_entry; /* 0-based, always < nentries when nentries > 0 */
    uint32_t nentries;      /* entries that have a kernel */
    struct cfg_entry entry[CFG_MAX_ENTRIES];
    uint32_t nerrors;       /* total; only the first CFG_MAX_ERRORS are kept */
    struct cfg_error error[CFG_MAX_ERRORS];
};

/* `text` holds `len` bytes and must have room for one more (a NUL). */
void cfg_parse(char *text, size_t len, struct boot_config *out);

/* Config path -> absolute path with '\' separators. `out` holds
 * CFG_RESOLVED_MAX bytes; returns 0 if the path is longer than CFG_PATH_MAX. */
int cfg_resolve_path(const char *path, char *out);

#endif
