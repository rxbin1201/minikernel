/* Selbsttests: Auswahl und Ablauf der Selbsttests */

#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "arch/x86_64/apic.h"
#include "core/cmdline.h"
#include "drivers/keymap.h"
#include "tests/selftest.h"

/* Selbsttests: "selftest" fuehrt alle aus, "selftest=disk,user" nur die genannten Gruppen (Namen wie in der Liste
 * unten, z.B. console unicode mouse clipboard keymap disk usb foreign user). Hinter jeder Gruppe steht ihre Dauer. */
static const char *test_filter;

static int test_selected(const char *name)
{
    if (!test_filter || !test_filter[0])
        return 1;
    size_t n = strlen(name);
    for (const char *p = test_filter; *p;) {
        const char *e = p;
        while (*e && *e != ',')
            e++;
        if ((size_t)(e - p) == n && memcmp(p, name, n) == 0)
            return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

#define RUN(name, call)                                                                     \
    do {                                                                                    \
        if (test_selected(name)) {                                                          \
            uint64_t t0_ = time_ms();                                                       \
            call;                                                                           \
            kprintf("  (%s: %lu ms)\n", name, (unsigned long)(time_ms() - t0_));          \
            if (!heap_broken && !heap_check()) { /* zeigt, in welcher Gruppe der Heap kaputt ging */ \
                heap_broken = 1;                                                            \
                check("Kernel-Heap nach der Gruppe " name " intakt", 0);                    \
            }                                                                               \
        }                                                                                   \
    } while (0)

static int heap_broken;

void run_selftests(BootInfo *info)
{
    /* Die Tests tippen ueber US-Scancodes: waehrenddessen US-Layout, danach das gewaehlte wieder */
    char layout[KEYMAP_NAME_MAX + 1];
    const char *ln = keymap_name();
    memcpy(layout, ln, strlen(ln) + 1);
    keymap_set("us");
    test_filter = cmdline_get("selftest");
    uint64_t t_all = time_ms();
    selftest_ok = selftest_failed = 0;
    RUN("console", test_console());
    RUN("unicode", test_unicode());
    RUN("scrollback", test_scrollback());
    RUN("mouse", test_mouse());
    RUN("clipboard", test_clipboard());
    RUN("keymap", test_keymap());
    RUN("multiclick", test_multiclick());
    RUN("video", test_video(info));
    RUN("sound", test_sound());
    RUN("rtc", test_rtc());
    RUN("kprintf", test_kprintf());
    RUN("paging", test_paging());
    RUN("pmm", test_pmm());
    RUN("heap", test_heap());
    RUN("interrupts", test_interrupts());
    RUN("console_speed", test_console_speed());
    RUN("sched", test_sched());
    RUN("smp", test_smp());
    RUN("vfs", test_vfs(info));
    RUN("disk", test_disk());
    RUN("usb", test_usb());
    RUN("foreign", test_foreign());
    RUN("user", test_user(info));
    RUN("editor", test_editor());
    RUN("script", test_script());
    RUN("tools", test_tools());
    RUN("graphics", test_graphics());
    RUN("crypto", test_crypto());
    RUN("net", test_net());
    if (test_filter && strstr_(test_filter, "netpeer")) /* nur ausdruecklich (braucht die Test-Gegenstelle) */
        RUN("netpeer", test_netpeer());
    /* Diese Zeile wertet "make test" aus */
    kprintf("\nSelbsttests: %u OK, %u FEHLER, %lu ms\n", selftest_ok, selftest_failed, (unsigned long)(time_ms() - t_all));
    keymap_set(layout);
}
