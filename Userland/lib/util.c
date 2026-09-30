/* Helfer fuer die Werkzeuge, siehe util.h */

#include "util.h"

/* Liest alles aus fd in einen neuen Puffer (mit abschliessender 0). NULL bei Speichermangel. */
char *read_fd_all(int fd, u64 *len)
{
    u64 cap = 4096, n = 0;
    char *b = u_malloc(cap);
    while (b) {
        if (n + 1 >= cap) {
            char *nb = u_malloc(cap * 2);
            if (!nb) {
                u_free(b);
                return 0;
            }
            memcpy(nb, b, n);
            u_free(b);
            b = nb;
            cap *= 2;
        }
        s64 r = sys_read(fd, b + n, cap - n - 1);
        if (r <= 0)
            break;
        n += (u64)r;
    }
    if (b)
        b[n] = 0;
    *len = n;
    return b;
}

/* Datei (oder "-"/NULL = stdin) ganz lesen; Fehler werden gemeldet (prog: Programmname) */
char *read_file_all(const char *prog, const char *path, u64 *len)
{
    int fd = 0;
    if (path && strcmp(path, "-") != 0) {
        s64 f = sys_open(path, O_RDONLY);
        if (f < 0) {
            fprintf(2, "%s: '%s': nicht gefunden (Fehler %d)\n", prog, path, (int)f);
            return 0;
        }
        fd = (int)f;
    }
    char *b = read_fd_all(fd, len);
    if (fd)
        sys_close(fd);
    if (!b)
        fprintf(2, "%s: kein Speicher\n", prog);
    return b;
}

/* Zerlegt den Puffer in Zeilen (ersetzt \n durch 0, entfernt \r). Ein abschliessendes \n ergibt keine leere Zeile. */
char **split_lines(char *buf, u64 len, int *count)
{
    int n = 0;
    for (u64 i = 0; i < len; i++)
        if (buf[i] == '\n')
            n++;
    if (len && buf[len - 1] != '\n')
        n++;
    char **lines = u_malloc(sizeof(char *) * (u64)(n + 1));
    if (!lines) {
        *count = 0;
        return 0;
    }
    int k = 0;
    char *start = buf;
    for (u64 i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            buf[i] = 0;
            if (&buf[i] > start && buf[i - 1] == '\r')
                buf[i - 1] = 0;
            lines[k++] = start;
            start = buf + i + 1;
        }
    }
    if (len && buf[len - 1] != 0)
        lines[k++] = start;
    lines[k] = 0;
    *count = k;
    return lines;
}

/* 1536 -> "1,5K", 5000000 -> "4,8M" */
void fmt_size(u64 b, char *out, int max)
{
    static const char units[] = "BKMGT";
    int u = 0;
    u64 whole = b, tenth = 0;
    while (whole >= 1024 && u < 4) {
        tenth = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(out, (size_t)max, "%lluB", (unsigned long long)b);
    else if (whole < 10)
        snprintf(out, (size_t)max, "%llu,%llu%c", (unsigned long long)whole, (unsigned long long)tenth, units[u]);
    else
        snprintf(out, (size_t)max, "%llu%c", (unsigned long long)whole, units[u]);
}

/* Platzhalter: * ? [abc] [a-z] [!x] */
int glob_match(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p)
                return 1;
            for (; *s; s++)
                if (glob_match(p, s))
                    return 1;
            return glob_match(p, s);
        }
        if (!*s)
            return 0;
        if (*p == '?') {
            s += utf8_len_at(s);
            p++;
            continue;
        }
        if (*p == '[') {
            const char *q = p + 1;
            int neg = *q == '!' || *q == '^', ok = 0;
            if (neg)
                q++;
            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2])
                        ok = 1;
                    q += 3;
                } else {
                    if (*s == *q)
                        ok = 1;
                    q++;
                }
            }
            if (*q && ok != neg) {
                p = q + 1;
                s++;
                continue;
            }
            return 0;
        }
        if (*p != *s)
            return 0;
        p++;
        s++;
    }
    return *s == 0;
}

/* Pfad aus Verzeichnis und Name */
void join_path(char *out, int max, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    snprintf(out, (size_t)max, "%s%s%s", dir, l && dir[l - 1] == '/' ? "" : "/", name);
}

/* Gepufferte Ausgabe auf stdout (am Ende out_flush aufrufen) */
static char out_buf[8192];
static int  out_n;

void out_flush(void)
{
    if (out_n)
        write_all(1, out_buf, (size_t)out_n);
    out_n = 0;
}

void out_write(const char *s, u64 n)
{
    if (n >= sizeof(out_buf)) {
        out_flush();
        write_all(1, s, n);
        return;
    }
    if (out_n + n > sizeof(out_buf))
        out_flush();
    memcpy(out_buf + out_n, s, n);
    out_n += (int)n;
}

void out_str(const char *s) { out_write(s, strlen(s)); }

void out_printf(const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(tmp) - 1)
        n = (int)sizeof(tmp) - 1;
    if (n > 0)
        out_write(tmp, (u64)n);
}
