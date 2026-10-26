/*
 * Boot menu on the firmware console. Shown only when BOOT.CFG sets a
 * timeout, so a plain boot never waits for or reads the keyboard.
 *
 *   1-9    boot that entry now
 *   Enter  boot the default now
 *   other  stop the countdown and wait for a choice
 *
 * Keys are polled (ReadKeyStroke never blocks) between short Stalls, so no
 * events or timers are created. Output exists in every build configuration:
 * a menu the user cannot see is useless.
 */
#include <stdbool.h>
#include "firmware/uefi_boot.h"
#include "firmware/uefi_console.h"

#define MENU_TICK_US     10000
#define WATCHDOG_DEFAULT 300 /* seconds: what the boot manager arms (UEFI 3.1.2) */

uint32_t menu_choose(const struct boot_config *c)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = g_uefi.st->ConIn;
    uint64_t left_us = (uint64_t)c->timeout * 1000000;
    uint64_t shown = 0;
    bool counting = true;
    uint32_t choice = c->default_entry;

    if (c->timeout == 0 || !in)
        return choice;

    /* The firmware watchdog would reset the machine while the menu waits
     * for a person; re-armed with its default once a choice is made. */
    g_uefi.bs->SetWatchdogTimer(0, 0, 0, NULL);
    in->Reset(in, 0); /* drop keys pressed before the menu appeared */

    con_puts(L"\r\nMYBOOT boot menu\r\n");
    for (uint32_t i = 0; i < c->nentries; i++) {
        con_puts(i == c->default_entry ? L"  * " : L"    ");
        con_put_dec(i + 1);
        con_puts(L") ");
        con_put_ascii(c->entry[i].title[0] ? c->entry[i].title : c->entry[i].kernel, CFG_PATH_MAX);
        con_puts(L"\r\n");
    }
    con_puts(L"Press 1-");
    con_put_dec(c->nentries);
    con_puts(L" to boot an entry, Enter for the default (*).\r\n");

    for (;;) {
        EFI_INPUT_KEY key;
        uint64_t secs = (left_us + 999999) / 1000000;

        if (counting && secs != shown) {
            shown = secs;
            con_puts(L"\rBooting the default in ");
            con_put_dec(secs);
            con_puts(L" s; any other key waits. ");
        }
        while (!EFI_ERROR(in->ReadKeyStroke(in, &key))) {
            if (key.UnicodeChar >= L'1' && key.UnicodeChar < L'1' + c->nentries) {
                choice = key.UnicodeChar - L'1';
                goto done;
            }
            if (key.UnicodeChar == L'\r')
                goto done;
            if (counting) {
                counting = false;
                con_puts(L"\rCountdown stopped. Choose an entry.      ");
            }
        }
        if (counting) {
            if (left_us <= MENU_TICK_US)
                goto done;
            left_us -= MENU_TICK_US;
        }
        g_uefi.bs->Stall(MENU_TICK_US);
    }
done:
    con_puts(L"\r\n");
    g_uefi.bs->SetWatchdogTimer(WATCHDOG_DEFAULT, 0, 0, NULL);
    return choice;
}
