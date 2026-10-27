/*
 * BOOT.CFG parser (format in include/config/config.h). Firmware-independent:
 * it sees only a byte buffer, so the BIOS backend can reuse it unchanged.
 */
#include <stdbool.h>
#include "config/config.h"
#include "lib/mem.h"

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

static size_t str_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static bool str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void add_error(struct boot_config *c, uint32_t line, const char *msg)
{
    if (c->nerrors < CFG_MAX_ERRORS) {
        c->error[c->nerrors].line = line;
        c->error[c->nerrors].msg = msg;
    }
    c->nerrors++;
}

/* Decimal, 1 to 6 digits, nothing else. */
static bool parse_uint(const char *s, uint32_t *out)
{
    uint32_t v = 0, n = 0;

    for (; *s >= '0' && *s <= '9'; s++) {
        if (++n > 6)
            return false;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    if (n == 0 || *s)
        return false;
    *out = v;
    return true;
}

/* Terminates the first word of `s` and returns the rest with leading
 * blanks skipped ("" if there is no rest). */
static char *split_word(char *s)
{
    while (*s && !is_blank(*s))
        s++;
    if (!*s)
        return s;
    *s++ = 0;
    while (is_blank(*s))
        s++;
    return s;
}

static const char *check_path(const char *p)
{
    size_t n = str_len(p);

    if (n == 0)
        return "missing path";
    if (n > CFG_PATH_MAX)
        return "path longer than 127 characters";
    for (size_t i = 0; i < n; i++)
        if (is_blank(p[i]))
            return "path contains a space";
    return NULL;
}

static const char *after(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return NULL;
    return s;
}

static const char *check_volume(const char *v)
{
    const char *id;

    if (str_len(v) > CFG_VOLUME_MAX)
        return "volume longer than 63 characters";
    if ((id = after(v, "uuid=")) || (id = after(v, "partuuid="))) {
        uint32_t digits = 0;
        for (; *id; id++) {
            char c = *id;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                digits++;
            else if (c != '-')
                return "uuid and partuuid take hex digits and dashes";
        }
        return digits ? NULL : "volume has an empty identifier";
    }
    if ((id = after(v, "label=")))
        return *id ? NULL : "volume has an empty label";
    return "volume needs uuid=, partuuid= or label=";
}

static void entry_directive(struct boot_config *c, struct cfg_entry *e, uint32_t line,
                            const char *key, char *val)
{
    const char *err = NULL;

    if (str_eq(key, "volume")) {
        if (e->volume_spec)
            err = "second volume line in one entry";
        else if (!(err = check_volume(val)))
            e->volume_spec = val;
    } else if (str_eq(key, "kernel") || str_eq(key, "linux") || str_eq(key, "efi")) {
        if (e->kernel)
            err = "an entry takes one kernel, linux or efi line";
        else if (!(err = check_path(val))) {
            e->kernel = val;
            e->type = key[0] == 'k' ? CFG_KERNEL : key[0] == 'l' ? CFG_LINUX : CFG_EFI;
        }
    } else if (str_eq(key, "cmdline")) {
        size_t n = str_len(val);
        if (e->cmdline)
            err = "second cmdline line in one entry";
        else if (n > CFG_CMDLINE_MAX)
            err = "cmdline longer than 1023 characters";
        else {
            e->cmdline = val;
            e->cmdline_len = (uint32_t)n;
        }
    } else { /* module or initrd: which one must match the entry's type,
              * so it is checked once the entry is complete (cfg_parse) */
        char *str = key[0] == 'm' ? split_word(val) : (char *)"";
        if (e->nmod == CFG_MAX_MODULES)
            err = "more than 8 modules or initrds in one entry";
        else if (!(err = check_path(val))) {
            if (str_len(str) > CFG_MODSTR_MAX)
                err = "module string longer than 111 characters";
            else {
                e->mod[e->nmod].path = val;
                e->mod[e->nmod].string = *str ? str : NULL;
                e->nmod++;
            }
        }
    }
    if (err)
        add_error(c, line, err);
}

void cfg_parse(char *text, size_t len, struct boot_config *c)
{
    char *p = text, *end = text + len;
    struct cfg_entry *cur = NULL;   /* entry being filled */
    bool overflow = false;          /* past CFG_MAX_ENTRIES: ignore entry lines */
    uint32_t entry_line[CFG_MAX_ENTRIES];
    uint8_t file_kind[CFG_MAX_ENTRIES] = {0}; /* bit 0: module lines, bit 1: initrd lines */
    uint32_t line = 0, nraw = 0, want_default = 1, default_line = 0, n = 0;
    bool default_found = false;

    memset(c, 0, sizeof(*c));
    c->auto_windows = 1;
    *end = 0;
    while (p < end) {
        char *eol = p, *next, *t, *key, *val;

        while (eol < end && *eol != '\n')
            eol++;
        *eol = 0;
        next = eol + 1;
        line++;
        for (t = eol; t > p && (is_blank(t[-1]) || t[-1] == '\r'); )
            *--t = 0;
        while (is_blank(*p))
            p++;
        if (*p == 0 || *p == '#') {
            p = next;
            continue;
        }
        for (t = p; *t; t++)
            if (((uint8_t)*t < 0x20 && *t != '\t') || (uint8_t)*t > 0x7E)
                break;
        if (*t) {
            add_error(c, line, "control or non-ASCII character");
            p = next;
            continue;
        }

        key = p;
        val = split_word(p);
        p = next;
        if (str_eq(key, "timeout")) {
            if (!parse_uint(val, &c->timeout))
                add_error(c, line, "timeout needs a number of seconds");
        } else if (str_eq(key, "default")) {
            if (!parse_uint(val, &want_default) || want_default == 0) {
                add_error(c, line, "default needs an entry number");
                want_default = 1;
            } else {
                default_line = line;
            }
        } else if (str_eq(key, "entry")) {
            if (nraw == CFG_MAX_ENTRIES) {
                if (!overflow)
                    add_error(c, line, "more than 9 entries; the rest are ignored");
                overflow = true;
                cur = NULL;
                continue;
            }
            if (str_len(val) > CFG_TITLE_MAX)
                val[CFG_TITLE_MAX] = 0;
            cur = &c->entry[nraw];
            cur->title = val;
            entry_line[nraw++] = line;
        } else if (str_eq(key, "auto_windows")) {
            if (str_eq(val, "yes") || str_eq(val, "no"))
                c->auto_windows = val[0] == 'y';
            else
                add_error(c, line, "auto_windows needs yes or no");
        } else if (str_eq(key, "kernel") || str_eq(key, "linux") || str_eq(key, "efi") ||
                   str_eq(key, "cmdline") || str_eq(key, "module") || str_eq(key, "initrd") ||
                   str_eq(key, "volume")) {
            if (cur) {
                if (str_eq(key, "module") || str_eq(key, "initrd"))
                    file_kind[nraw - 1] |= key[0] == 'm' ? 1 : 2;
                entry_directive(c, cur, line, key, val);
            } else if (!overflow) {
                add_error(c, line, "not inside an entry");
            }
        } else {
            add_error(c, line, "unknown directive");
        }
    }

    /* Drop entries without a kernel; "default" counts entries as written. */
    for (uint32_t i = 0; i < nraw; i++) {
        static const uint8_t allowed[] = {[CFG_KERNEL] = 1, [CFG_LINUX] = 2, [CFG_EFI] = 0};

        if (!c->entry[i].kernel) {
            add_error(c, entry_line[i], "entry has no kernel, linux or efi line");
            continue;
        }
        if (file_kind[i] & ~allowed[c->entry[i].type]) {
            add_error(c, entry_line[i], c->entry[i].type == CFG_LINUX
                                            ? "linux entries take initrd, not module"
                                            : c->entry[i].type == CFG_KERNEL
                                                ? "kernel entries take module, not initrd"
                                                : "efi entries take no module or initrd");
            continue;
        }
        if (i + 1 == want_default) {
            c->default_entry = n;
            default_found = true;
        }
        c->entry[n++] = c->entry[i];
    }
    c->nentries = n;
    if (!default_found) {
        if (default_line)
            add_error(c, default_line, "default names no usable entry");
        c->default_entry = 0;
    }
}

int cfg_resolve_path(const char *path, char sep, char *out)
{
    size_t n = 0;

    if (str_len(path) > CFG_PATH_MAX)
        return 0;
    if (*path != '/' && *path != '\\') {
        for (const char *d = CFG_DIR; *d; d++)
            out[n++] = *d == '\\' ? sep : *d;
    }
    for (; *path; path++)
        out[n++] = *path == '/' || *path == '\\' ? sep : *path;
    out[n] = 0;
    return 1;
}
