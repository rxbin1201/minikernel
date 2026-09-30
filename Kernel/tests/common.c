/* Selbsttests: gemeinsame Helfer */

#include "console/console.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "drivers/keyboard.h"
#include "core/sched.h"
#include "core/process.h"
#include "fs/fs.h"
#include "drivers/mouse.h"
#include "lib/utf8.h"
#include "tests/selftest.h"

unsigned selftest_ok, selftest_failed;

void check(const char *name, int ok)
{
    if (ok)
        selftest_ok++;
    else
        selftest_failed++;
    kprintf("  %s: ", name);
    console_set_color(ok ? COLOR_OK : COLOR_FAIL, 0);
    kprintf("%s\n", ok ? "OK" : "FEHLER");
    console_set_color(COLOR_DEFAULT, 0);
}

void title(const char *name)
{
    console_set_color(COLOR_TITLE, 0);
    kprintf("[%s]\n", name);
    console_set_color(COLOR_DEFAULT, 0);
}

int ieq(const char *a, const char *b)
{
    for (;;) { /* wie das Dateisystem: Schreibweise egal, auch bei Umlauten */
        uint32_t x = utf8_next(&a), y = utf8_next(&b);
        if (uni_upper(x) != uni_upper(y))
            return 0;
        if (!x)
            return 1;
    }
}

int dir_has(const char *dir, const char *name, uint64_t *size)
{
    FsDirEnt e;
    for (unsigned i = 0; fs_readdir(dir, i, &e) == 0; i++) {
        if (ieq(e.name, name)) {
            if (size)
                *size = e.size;
            return 1;
        }
    }
    return 0;
}

/* Liest eine Datei ganz (hoechstens max-1 Bytes) und haengt ein 0 an. Liefert die Laenge oder -1. */
int slurp(const char *path, char *buf, int max)
{
    FsFile f;
    if (fs_open(path, FAT_O_RDONLY, &f) != 0)
        return -1;
    int n = 0;
    int64_t r;
    while (n < max - 1 && (r = fs_read(&f, buf + n, (uint64_t)(max - 1 - n))) > 0)
        n += (int)r;
    fs_close(&f);
    buf[n] = 0;
    return n;
}

int strstr_(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (memcmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

/* Liest, was im Tastaturpuffer steht (bis max-1 Bytes) */
int drain_keys(char *out, int max)
{
    int n = 0, c;
    thread_sleep_ms(30); /* PS/2: der Interrupt muss die Bytes erst abholen */
    while ((c = keyboard_getchar()) >= 0)
        if (n < max - 1)
            out[n++] = (char)c;
    out[n] = 0;
    return n;
}

/* Setzt den Mauszeiger auf Pixel (x, y): erst in die Ecke, dann in kleinen Schritten (ohne Beschleunigung) */
void mouse_goto(int x, int y)
{
    for (int i = 0; i < 400; i++)
        mouse_report(0, -127, -127, 0);
    int s = (int)console_scale();
    for (int i = 0; i < x / s; i++)
        mouse_report(0, 1, 0, 0);
    for (int i = 0; i < y / s; i++)
        mouse_report(0, 0, 1, 0);
}

int write_text(const char *path, const char *text)
{
    FsFile f;
    if (fs_open(path, FAT_O_WRONLY | FAT_O_CREAT | FAT_O_TRUNC, &f) != 0)
        return -1;
    int64_t n = (int64_t)strlen(text);
    int ok = fs_write(&f, text, (uint64_t)n) == n;
    fs_close(&f);
    return ok ? 0 : -1;
}

int file_has(const char *path, const char *part)
{
    static char buf[2048];
    return slurp(path, buf, sizeof(buf)) > 0 && strstr_(buf, part);
}

int run_sh(const char *cmdline)
{
    int pid = process_spawn("/bin/sh", cmdline, 0);
    int code = 0, faulted = 0;
    return pid > 0 && process_wait(pid, 0, &code, &faulted, 20000) == 0 && !faulted ? code : -1;
}
