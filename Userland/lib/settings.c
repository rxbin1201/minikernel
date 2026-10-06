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

/* ---------- zuletzt verbundenes WLAN ---------- */

static const char *wlan_file(char *buf, int max)
{
    const char *p = settings_file();
    if (!p[0])
        return 0;
    snprintf(buf, max, "%s", p);
    char *slash = strrchr(buf, '/');
    if (!slash)
        return 0;
    snprintf(slash + 1, (u64)(max - (slash + 1 - buf)), "wlan.cfg");
    return buf;
}

int wlan_cfg_load(char ssid[33], char pass[65])
{
    char fn[72], buf[256];
    ssid[0] = pass[0] = 0;
    if (!wlan_file(fn, sizeof(fn)))
        return -1;
    s64 fd = sys_open(fn, O_RDONLY);
    if (fd < 0)
        return -1;
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
        if (strncmp(line, "ssid=", 5) == 0 && strlen(line + 5) <= 32)
            strcpy(ssid, line + 5);
        else if (strncmp(line, "pass=", 5) == 0 && strlen(line + 5) <= 64)
            strcpy(pass, line + 5);
        line = save ? end + 1 : end;
    }
    return ssid[0] ? 0 : -1;
}

int wlan_cfg_save(const char *ssid, const char *pass)
{
    char fn[72], buf[160];
    if (!wlan_file(fn, sizeof(fn)) || strchr(ssid, '\n') || strchr(pass, '\n'))
        return -1;
    int n = snprintf(buf, sizeof(buf), "# zuletzt verbundenes WLAN (Desktop, wlan connect)\nssid=%s\npass=%s\n", ssid, pass);
    s64 fd = sys_open(fn, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    s64 w = sys_write((int)fd, buf, (u64)n);
    sys_close((int)fd);
    return w == n ? 0 : -1;
}
/* ---------- Bluetooth: Schluessel und zuletzt verbundenes Geraet ---------- */

static const char *cfg_path(const char *name, char *buf, int max)
{
    const char *p = settings_file();
    if (!p[0])
        return 0;
    snprintf(buf, max, "%s", p);
    char *slash = strrchr(buf, '/');
    if (!slash)
        return 0;
    snprintf(slash + 1, (u64)(max - (slash + 1 - buf)), "%s", name);
    return buf;
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int bt_parse_addr(const char *s, unsigned char a[6])
{
    for (int i = 0; i < 6; i++) {
        int h = hexv(s[0]), l = hexv(s[1]);
        if (h < 0 || l < 0 || (i < 5 && s[2] != ':'))
            return -1;
        a[i] = (unsigned char)(h << 4 | l);
        s += 3;
    }
    return 0;
}

int bt_cfg_load(unsigned char last[6], char last_name[48])
{
    char fn[80], buf[2048];
    int have_last = 0;
    last_name[0] = 0;
    if (!cfg_path("bt_keys.cfg", fn, sizeof(fn)))
        return -1;
    s64 fd = sys_open(fn, O_RDONLY);
    if (fd < 0)
        return -1;
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
        BtKey k;
        memset(&k, 0, sizeof(k));
        if (strncmp(line, "last ", 5) == 0 && strlen(line) >= 22 && bt_parse_addr(line + 5, last) == 0) {
            have_last = 1;
            snprintf(last_name, 48, "%s", line[22] == ' ' ? line + 23 : "");
        } else if (strlen(line) >= 17 + 3 + 32 && bt_parse_addr(line, k.addr) == 0) {
            char *q = line + 18;
            k.type = (unsigned char)atoi(q);
            while (*q && *q != ' ')
                q++;
            int ok = *q == ' ';
            for (int i = 0; ok && i < 16; i++) {
                int h = hexv(q[1 + 2 * i]), l = hexv(q[2 + 2 * i]);
                ok = h >= 0 && l >= 0;
                k.key[i] = (unsigned char)(h << 4 | l);
            }
            if (ok)
                sys_bt_key_add(&k);
        }
        line = save ? end + 1 : end;
    }
    return have_last ? 0 : -1;
}

int bt_cfg_save(const unsigned char *last, const char *last_name)
{
    char fn[80], buf[2048];
    unsigned char old[6];
    char old_name[48];
    if (!last && bt_cfg_load(old, old_name) == 0) { /* das bisherige Geraet behalten */
        last = old;
        last_name = old_name;
    }
    if (!cfg_path("bt_keys.cfg", fn, sizeof(fn)))
        return -1;
    int n = snprintf(buf, sizeof(buf), "# Bluetooth: Verbindungsschluessel und zuletzt verbundenes Geraet\n");
    if (last)
        n += snprintf(buf + n, sizeof(buf) - (u64)n, "last %02x:%02x:%02x:%02x:%02x:%02x %s\n", last[0], last[1], last[2],
                      last[3], last[4], last[5], last_name ? last_name : "");
    BtKey k;
    for (u64 i = 0; sys_bt_key_get(i, &k) == 0 && n < (int)sizeof(buf) - 80; i++) {
        n += snprintf(buf + n, sizeof(buf) - (u64)n, "%02x:%02x:%02x:%02x:%02x:%02x %u ", k.addr[0], k.addr[1],
                      k.addr[2], k.addr[3], k.addr[4], k.addr[5], k.type);
        for (int b = 0; b < 16; b++)
            n += snprintf(buf + n, sizeof(buf) - (u64)n, "%02x", k.key[b]);
        n += snprintf(buf + n, sizeof(buf) - (u64)n, "\n");
    }
    s64 fd = sys_open(fn, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    s64 w = sys_write((int)fd, buf, (u64)n);
    sys_close((int)fd);
    return w == n ? 0 : -1;
}