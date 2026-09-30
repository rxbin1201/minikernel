#ifndef TESTS_SELFTEST_H
#define TESTS_SELFTEST_H

/* Selbsttests des Kernels (Kommandozeile "selftest" bzw. "selftest=gruppe,...", siehe tests/selftest.c).
 * Jede Gruppe liegt in einer eigenen Datei; hier stehen die gemeinsamen Helfer (tests/common.c). */

#include <stdint.h>
#include "boot_info.h"

#define COLOR_DEFAULT 0x00C0C0C0
#define COLOR_OK      0x0000FF00
#define COLOR_FAIL    0x00FF3030
#define COLOR_TITLE   0x0060C0FF

/* Gemeinsame Helfer (tests/common.c): check() gibt OK/FEHLER aus, title() eine Ueberschrift */
extern unsigned selftest_ok, selftest_failed; /* von check() gezaehlt */
void check(const char *name, int ok);
void title(const char *name);
int ieq(const char *a, const char *b);
int dir_has(const char *dir, const char *name, uint64_t *size);
int slurp(const char *path, char *buf, int max);
int strstr_(const char *hay, const char *needle);
int drain_keys(char *out, int max);
void mouse_goto(int x, int y);
int write_text(const char *path, const char *text);
int file_has(const char *path, const char *part);
int run_sh(const char *cmdline);

/* Testgruppen */
void test_kprintf(void);
void test_console(void);
void test_console_speed(void);
void test_unicode(void);
void test_scrollback(void);
void test_video(BootInfo *info);
void test_mouse(void);
void test_clipboard(void);
void test_keymap(void);
void test_multiclick(void);
void test_paging(void);
void test_pmm(void);
void test_heap(void);
void test_interrupts(void);
void test_sched(void);
void test_rtc(void);
void test_smp(void);
void test_usb(void);
void test_vfs(BootInfo *info);
void test_disk(void);
void test_foreign(void);
void test_user(BootInfo *info);
void test_editor(void);
void test_script(void);
void test_tools(void);
void test_graphics(void);
void test_net(void);
void test_netpeer(void);

/* Fuehrt die per Kommandozeile gewaehlten Gruppen aus */
void run_selftests(BootInfo *info);

#endif
