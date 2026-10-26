/*
 * Boot menu and recovery screen on the firmware console.
 *
 *   Up/Down, Home/End  move the selection (wraps around)
 *   Enter              activate the selected item
 *   1-9                boot that entry at once
 *   e                  edit the selected entry's command line, then boot it
 *   any key            stops the countdown
 *
 * Besides the BOOT.CFG entries the menu offers Firmware setup (when the
 * firmware supports OsIndications), Reboot, Power off and Return to
 * firmware (its boot manager then tries the next boot option).
 *
 * The normal menu appears only when BOOT.CFG sets a timeout or a key is
 * already waiting, so a plain boot never waits for the keyboard. The
 * recovery screen appears when every entry failed to load; its countdown
 * returns to the firmware, so an unattended machine still moves on to its
 * next boot option.
 *
 * Keys are polled (ReadKeyStroke never blocks) between short Stalls: no
 * events or timers. Output exists in every build configuration.
 */
#include <stdbool.h>
#include "firmware/uefi_boot.h"
#include "firmware/uefi_console.h"
#include "lib/mem.h"

#define MENU_TICK_US     10000
#define RECOVERY_TIMEOUT 10  /* seconds before the recovery screen gives up */
#define WATCHDOG_DEFAULT 300 /* seconds: what the boot manager arms (UEFI 3.1.2) */
#define ATTR_NORMAL      EFI_LIGHTGRAY
#define ATTR_TITLE       EFI_WHITE
#define ATTR_SELECTED    EFI_BACKGROUND_LIGHTGRAY /* black on light grey */

static EFI_GUID guid_global = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};

enum item { ITEM_ENTRY, ITEM_FW_SETUP, ITEM_REBOOT, ITEM_POWEROFF, ITEM_RETURN };

static const char *const item_label[] = {
    [ITEM_FW_SETUP] = "Firmware setup",
    [ITEM_REBOOT] = "Reboot",
    [ITEM_POWEROFF] = "Power off",
    [ITEM_RETURN] = "Return to firmware",
};

struct menu {
    const struct boot_config *cfg;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in;
    bool     recovery;
    uint32_t special[4];   /* enum item, after the entries */
    uint32_t nspecial;
    uint32_t nitems;
    uint32_t sel;
    uint32_t cols, rows;
    uint32_t top;          /* row of the first item */
    uint32_t help_row;
    bool     counting;
    uint64_t left_us;
    uint64_t shown_secs;
    const char *message;   /* replaces the status line until the next key */
};

static char g_edit[CFG_CMDLINE_MAX + 1];

/* ---- small text helpers --------------------------------------------- */

struct text {
    char  buf[160];
    size_t n;
};

static void t_str(struct text *t, const char *s)
{
    for (; *s && t->n + 1 < sizeof(t->buf); s++)
        t->buf[t->n++] = *s;
    t->buf[t->n] = 0;
}

static void t_num(struct text *t, uint64_t v)
{
    char d[20];
    unsigned i = 0;

    do {
        d[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i && t->n + 1 < sizeof(t->buf))
        t->buf[t->n++] = d[--i];
    t->buf[t->n] = 0;
}

static void row_text(uint32_t row, uint32_t attr, const char *s, uint32_t width)
{
    con_goto(0, row);
    con_attr(attr);
    con_put_field(s, width, width);
    con_attr(ATTR_NORMAL);
}

/* ---- firmware actions ------------------------------------------------- */

static bool fw_setup_supported(void)
{
    EFI_RUNTIME_SERVICES *rt = g_uefi.st->RuntimeServices;
    uint64_t v = 0;
    UINTN size = sizeof(v);

    return rt && !EFI_ERROR(rt->GetVariable((CHAR16 *)L"OsIndicationsSupported", &guid_global,
                                            NULL, &size, &v)) &&
           (v & EFI_OS_INDICATIONS_BOOT_TO_FW_UI);
}

/* UEFI 8.5.4: set the BOOT_TO_FW_UI bit of OsIndications, then reset. */
static const char *enter_fw_setup(void)
{
    EFI_RUNTIME_SERVICES *rt = g_uefi.st->RuntimeServices;
    uint64_t v = 0;
    UINTN size = sizeof(v);

    if (EFI_ERROR(rt->GetVariable((CHAR16 *)L"OsIndications", &guid_global, NULL, &size, &v)))
        v = 0;
    v |= EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
    if (EFI_ERROR(rt->SetVariable((CHAR16 *)L"OsIndications", &guid_global,
                                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                      EFI_VARIABLE_RUNTIME_ACCESS,
                                  sizeof(v), &v)))
        return "The firmware refused the request to open its setup.";
    rt->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    return "Reset failed.";
}

static const char *reset(uint32_t type)
{
    if (g_uefi.st->RuntimeServices)
        g_uefi.st->RuntimeServices->ResetSystem(type, EFI_SUCCESS, 0, NULL);
    return "Reset failed.";
}

/* ---- drawing ---------------------------------------------------------- */

static uint32_t item_kind(const struct menu *m, uint32_t i)
{
    return i < m->cfg->nentries ? ITEM_ENTRY : m->special[i - m->cfg->nentries];
}

static uint32_t item_row(const struct menu *m, uint32_t i)
{
    /* one blank row between the entries and the built-in items */
    return m->top + i + (i >= m->cfg->nentries ? 1 : 0);
}

static void draw_item(const struct menu *m, uint32_t i)
{
    const struct cfg_entry *e;
    struct text t = {.n = 0};

    if (item_kind(m, i) == ITEM_ENTRY) {
        e = &m->cfg->entry[i];
        t_str(&t, "  ");
        t_num(&t, i + 1);
        t_str(&t, ") ");
        t_str(&t, e->title[0] ? e->title : e->kernel);
    } else {
        t_str(&t, "     ");
        t_str(&t, item_label[item_kind(m, i)]);
    }
    row_text(item_row(m, i), i == m->sel ? ATTR_SELECTED : ATTR_NORMAL, t.buf, m->cols - 1);
}

static void draw_status(struct menu *m)
{
    struct text t = {.n = 0};

    if (m->message)
        t_str(&t, m->message);
    else if (m->counting) {
        t_str(&t, m->recovery ? "Returning to the firmware in " : "Booting the selected entry in ");
        t_num(&t, m->shown_secs);
        t_str(&t, " s. Press any key to stop.");
    }
    row_text(m->help_row + 2, ATTR_NORMAL, t.buf, m->cols - 1);
}

static void draw(struct menu *m)
{
    uint32_t avail, nerr;

    con_attr(ATTR_NORMAL);
    con_clear();
    con_cursor(0);
    row_text(0, ATTR_TITLE, m->recovery ? "MYBOOT: no entry could be loaded" : "MYBOOT boot menu",
             m->cols - 1);

    /* Rows: title, blank, errors (+ blank), items, blank, help, blank,
     * status. Errors get whatever the items leave free. */
    avail = m->rows > m->nitems + 8 ? m->rows - (m->nitems + 8) : 0;
    nerr = boot_errors_draw(2, avail, m->cols - 1);
    m->top = 2 + nerr + (nerr ? 1 : 0);
    for (uint32_t i = 0; i < m->nitems; i++)
        draw_item(m, i);
    m->help_row = item_row(m, m->nitems - 1) + 2;
    row_text(m->help_row, ATTR_NORMAL,
             "Up/Down select, Enter boot, e edit command line, 1-9 boot that entry", m->cols - 1);
    draw_status(m);
}

/* ---- input ------------------------------------------------------------ */

/* Next key, ticking the countdown while it runs. Returns false when the
 * countdown expires. The first key stops the countdown for good. */
static bool next_key(struct menu *m, EFI_INPUT_KEY *key)
{
    for (;;) {
        if (!EFI_ERROR(m->in->ReadKeyStroke(m->in, key))) {
            if (m->counting || m->message) {
                m->counting = false;
                m->message = NULL;
                draw_status(m);
            }
            return true;
        }
        if (m->counting) {
            uint64_t secs = (m->left_us + 999999) / 1000000;
            if (secs != m->shown_secs) {
                m->shown_secs = secs;
                draw_status(m);
            }
            if (m->left_us <= MENU_TICK_US)
                return false;
            m->left_us -= MENU_TICK_US;
        }
        g_uefi.bs->Stall(MENU_TICK_US);
    }
}

/* Single-line editor with horizontal scrolling. Returns true to boot with
 * the edited line, false to go back to the menu. */
static bool edit_cmdline(struct menu *m, uint32_t idx, struct menu_choice *out)
{
    const struct cfg_entry *e = &m->cfg->entry[idx];
    uint32_t len = e->cmdline ? e->cmdline_len : 0, cur, scroll = 0, w = m->cols - 1;
    struct text t = {.n = 0};
    EFI_INPUT_KEY key;

    memcpy(g_edit, e->cmdline ? e->cmdline : "", len);
    g_edit[len] = 0;
    cur = len;

    con_attr(ATTR_NORMAL);
    con_clear();
    t_str(&t, "Command line of: ");
    t_str(&t, e->title[0] ? e->title : e->kernel);
    row_text(0, ATTR_TITLE, t.buf, w);
    row_text(4, ATTR_NORMAL, "Enter: boot with this command line   Esc: back to the menu", w);
    row_text(5, ATTR_NORMAL, "Left/Right/Home/End: move   Backspace/Delete: erase", w);
    row_text(6, ATTR_NORMAL, "The change applies to this boot only.", w);

    for (;;) {
        if (cur < scroll)
            scroll = cur;
        if (cur >= scroll + w)
            scroll = cur - w + 1;
        con_cursor(0);
        con_goto(0, 2);
        con_put_field(g_edit + scroll, len - scroll, w);
        con_goto(cur - scroll, 2);
        con_cursor(1);

        next_key(m, &key);
        if (key.UnicodeChar == L'\r') {
            con_cursor(0);
            out->cmdline = g_edit;
            out->cmdline_len = len;
            return true;
        }
        if (key.ScanCode == SCAN_ESC) {
            con_cursor(0);
            return false;
        }
        if (key.ScanCode == SCAN_LEFT && cur > 0)
            cur--;
        else if (key.ScanCode == SCAN_RIGHT && cur < len)
            cur++;
        else if (key.ScanCode == SCAN_HOME)
            cur = 0;
        else if (key.ScanCode == SCAN_END)
            cur = len;
        else if (key.UnicodeChar == 0x08 && cur > 0) { /* Backspace */
            memmove(g_edit + cur - 1, g_edit + cur, len - cur);
            g_edit[--len] = 0;
            cur--;
        } else if (key.ScanCode == SCAN_DELETE && cur < len) {
            memmove(g_edit + cur, g_edit + cur + 1, len - cur - 1);
            g_edit[--len] = 0;
        } else if (key.UnicodeChar >= 0x20 && key.UnicodeChar < 0x7F && len < CFG_CMDLINE_MAX) {
            memmove(g_edit + cur + 1, g_edit + cur, len - cur);
            g_edit[cur++] = (char)key.UnicodeChar;
            g_edit[++len] = 0;
        }
    }
}

/* ---- the menu loop ------------------------------------------------------ */

static void run(struct menu *m, struct menu_choice *out)
{
    EFI_INPUT_KEY key;

    /* The firmware watchdog would reset the machine while a person reads
     * the menu; re-armed with its default once a choice is made. */
    g_uefi.bs->SetWatchdogTimer(0, 0, 0, NULL);
    if (fw_setup_supported())
        m->special[m->nspecial++] = ITEM_FW_SETUP;
    m->special[m->nspecial++] = ITEM_REBOOT;
    m->special[m->nspecial++] = ITEM_POWEROFF;
    m->special[m->nspecial++] = ITEM_RETURN;
    m->nitems = m->cfg->nentries + m->nspecial;
    con_size(&m->cols, &m->rows);
    m->shown_secs = (m->left_us + 999999) / 1000000;
    draw(m);

    for (;;) {
        uint32_t kind;

        if (!next_key(m, &key)) {
            /* Countdown expired: boot the default, or give up. */
            out->action = m->recovery ? MENU_RETURN : MENU_BOOT;
            out->entry = m->cfg->default_entry;
            break;
        }
        if (key.UnicodeChar >= L'1' && key.UnicodeChar < L'1' + m->cfg->nentries) {
            out->action = MENU_BOOT;
            out->entry = key.UnicodeChar - L'1';
            break;
        }
        if (key.ScanCode == SCAN_UP || key.ScanCode == SCAN_DOWN ||
            key.ScanCode == SCAN_HOME || key.ScanCode == SCAN_END) {
            uint32_t old = m->sel;
            if (key.ScanCode == SCAN_UP)
                m->sel = (m->sel + m->nitems - 1) % m->nitems;
            else if (key.ScanCode == SCAN_DOWN)
                m->sel = (m->sel + 1) % m->nitems;
            else
                m->sel = key.ScanCode == SCAN_HOME ? 0 : m->nitems - 1;
            draw_item(m, old);
            draw_item(m, m->sel);
            continue;
        }

        kind = item_kind(m, m->sel);
        if ((key.UnicodeChar == L'e' || key.UnicodeChar == L'E') && kind == ITEM_ENTRY) {
            if (edit_cmdline(m, m->sel, out)) {
                out->action = MENU_BOOT;
                out->entry = m->sel;
                break;
            }
            draw(m);
            continue;
        }
        if (key.UnicodeChar != L'\r')
            continue;
        if (kind == ITEM_ENTRY) {
            out->action = MENU_BOOT;
            out->entry = m->sel;
            break;
        }
        if (kind == ITEM_RETURN) {
            out->action = MENU_RETURN;
            break;
        }
        m->message = kind == ITEM_FW_SETUP ? enter_fw_setup()
                   : reset(kind == ITEM_REBOOT ? EfiResetCold : EfiResetShutdown);
        draw_status(m);
    }

    con_attr(ATTR_NORMAL);
    con_clear();
    con_cursor(1);
    g_uefi.bs->SetWatchdogTimer(WATCHDOG_DEFAULT, 0, 0, NULL);
}

void menu_choose(const struct boot_config *c, struct menu_choice *out)
{
    struct menu m = {.cfg = c, .in = g_uefi.st->ConIn, .sel = c->default_entry};
    EFI_INPUT_KEY key;

    memset(out, 0, sizeof(*out));
    out->action = MENU_BOOT;
    out->entry = c->default_entry;
    if (!m.in)
        return;
    if (c->timeout) {
        m.counting = true;
        m.left_us = (uint64_t)c->timeout * 1000000;
        m.in->Reset(m.in, 0); /* drop keys pressed before the menu appeared */
    } else if (EFI_ERROR(m.in->ReadKeyStroke(m.in, &key))) {
        return; /* no menu, no key held: boot the default without waiting */
    }
    run(&m, out);
}

void menu_recover(const struct boot_config *c, struct menu_choice *out)
{
    struct menu m = {.cfg = c, .in = g_uefi.st->ConIn, .recovery = true,
                     .counting = true, .left_us = (uint64_t)RECOVERY_TIMEOUT * 1000000};

    memset(out, 0, sizeof(*out));
    out->action = MENU_RETURN;
    if (!m.in) {
        g_uefi.bs->Stall(3000000); /* no keyboard: leave the errors readable */
        return;
    }
    m.in->Reset(m.in, 0);
    run(&m, out);
}
