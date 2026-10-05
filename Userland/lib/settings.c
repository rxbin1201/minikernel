#include "settings.h"
#include "libc.h"

/* Einstellungen in settings.cfg lesen und schreiben (siehe settings.h) */

static char path[64];

const char *settings_file(void)
{
    if (path[0])
        return path;
    char dirs[1][40];
    Stat st;
    if (find_boot_volumes(dirs, 1) == 1)
        snprintf(path, sizeof(path), "%s/settings.cfg", dirs[0]);
    else if (sys_stat("/disk", &st) == 0 && st.is_dir)
        snprintf(path, sizeof(path), "/disk/settings.cfg");
    return path;
}

void settings_default(Settings *s)
{
    memset(s, 0, sizeof(*s));
    s->dock = 100;
    s->date = 1;
}

static const struct {
    const char *key;
    int         off, lo, hi;
} keys[] = {
    {"ui_scale", (int)__builtin_offsetof(Settings, ui_scale), 0, 150},
    {"cursor", (int)__builtin_offsetof(Settings, cursor), 0, 250},
    {"dock", (int)__builtin_offsetof(Settings, dock), 85, 125},
    {"seconds", (int)__builtin_offsetof(Settings, seconds), 0, 1},
    {"date", (int)__builtin_offsetof(Settings, date), 0, 1},
    {"wallpaper", (int)__builtin_offsetof(Settings, wallpaper), 0, 15},
};
#define NKEYS ((int)(sizeof(keys) / sizeof(keys[0])))

int settings_load(Settings *s)
{
    settings_default(s);
    const char *p = settings_file();
    if (!p[0])
        return -1;
    s64 fd = sys_open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[1024];
    s64 n = sys_read((int)fd, buf, sizeof(buf) - 1);
    sys_close((int)fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    for (char *line = buf; *line;) {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        char save = *end;
        *end = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            for (int k = 0; k < NKEYS; k++)
                if (strcmp(line, keys[k].key) == 0) {
                    int v = atoi(eq + 1);
                    if (v >= keys[k].lo && v <= keys[k].hi)
                        *(int *)((char *)s + keys[k].off) = v;
                }
        }
        line = save ? end + 1 : end;
    }
    return 0;
}

int settings_save(const Settings *s)
{
    const char *p = settings_file();
    if (!p[0])
        return -1;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "# Einstellungen des Desktops (Programm \"Einstellungen\")\n");
    for (int k = 0; k < NKEYS; k++)
        n += snprintf(buf + n, sizeof(buf) - (u64)n, "%s=%d\n", keys[k].key, *(const int *)((const char *)s + keys[k].off));
    s64 fd = sys_open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    s64 w = sys_write((int)fd, buf, (u64)n);
    sys_close((int)fd);
    return w == n ? 0 : -1;
}
