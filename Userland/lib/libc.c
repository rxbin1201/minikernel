#include "libc.h"

/* ---------- Speicher und Strings ---------- */

/* Kopieren und Fuellen mit den String-Befehlen der CPU: 8 Byte je Schritt, den Rest byteweise */
void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;
    const void *s = src;
    size_t q = n >> 3, r = n & 7;
    __asm__ __volatile__("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(r) : : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        memcpy(d, s, n); /* vorwaerts kopieren ist bei d < s immer sicher */
    } else if (d > s) {
        d += n;
        s += n;
        while (n >= 8) {
            d -= 8;
            s -= 8;
            unsigned long long v;
            __builtin_memcpy(&v, s, 8);
            __builtin_memcpy(d, &v, 8);
            n -= 8;
        }
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned long long v = (unsigned char)c * 0x0101010101010101ULL;
    void *d = dst;
    size_t q = n >> 3, r = n & 7;
    __asm__ __volatile__("rep stosq" : "+D"(d), "+c"(q) : "a"(v) : "memory");
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(r) : "a"(v) : "memory");
    return dst;
}

/* 32-Bit-Werte fuellen (Pixel) */
void memset32(void *dst, unsigned int v, size_t count)
{
    void *d = dst;
    __asm__ __volatile__("rep stosl" : "+D"(d), "+c"(count) : "a"(v) : "memory");
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return c == 0 ? (char *)s : NULL;
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && strncmp(hay, needle, n) == 0)
            return (char *)hay;
    return NULL;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v * sign;
}

/* ---------- Ausgabe ---------- */

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        s64 w = sys_write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

typedef struct {
    char  *buf;     /* Ziel im Speicher (fd < 0) */
    size_t size, pos;
    int    fd;      /* oder gepuffert auf einen Deskriptor */
    char   tmp[128];
    int    n;
} Sink;

static void put(Sink *s, char c)
{
    if (s->fd < 0) {
        if (s->size && s->pos + 1 < s->size)
            s->buf[s->pos] = c;
    } else {
        s->tmp[s->n++] = c;
        if (s->n == (int)sizeof(s->tmp)) {
            write_all(s->fd, s->tmp, (size_t)s->n);
            s->n = 0;
        }
    }
    s->pos++;
}

static void pad(Sink *s, char c, int n)
{
    while (n-- > 0)
        put(s, c);
}

static void number(Sink *s, u64 v, int negative, int base, int upper, int width, int left, int zero)
{
    char digits[24];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[n++] = set[v % (u64)base];
        v /= (u64)base;
    } while (v);

    int len = n + (negative ? 1 : 0);
    if (!left && !zero)
        pad(s, ' ', width - len);
    if (negative)
        put(s, '-');
    if (!left && zero)
        pad(s, '0', width - len);
    while (n)
        put(s, digits[--n]);
    if (left)
        pad(s, ' ', width - len);
}

static void format(Sink *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(s, *fmt);
            continue;
        }
        fmt++;

        int left = 0, zero = 0, width = 0, precision = -1, longs = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    precision = precision * 10 + (*fmt++ - '0');
            }
        }
        while (*fmt == 'l' || *fmt == 'z') {
            longs++;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            s64 v = longs ? va_arg(ap, long long) : va_arg(ap, int);
            number(s, v < 0 ? (u64)-v : (u64)v, v < 0, 10, 0, width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            u64 v = longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            number(s, v, 0, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, left, zero);
            break;
        }
        case 'p':
            put(s, '0');
            put(s, 'x');
            number(s, (u64)va_arg(ap, void *), 0, 16, 0, 0, 0, 0);
            break;
        case 'c':
            pad(s, ' ', left ? 0 : width - 1);
            put(s, (char)va_arg(ap, int));
            pad(s, ' ', left ? width - 1 : 0);
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            int len = 0;
            while (str[len] && (precision < 0 || len < precision))
                len++;
            pad(s, ' ', left ? 0 : width - len);
            for (int i = 0; i < len; i++)
                put(s, str[i]);
            pad(s, ' ', left ? width - len : 0);
            break;
        }
        case '%':
            put(s, '%');
            break;
        case 0:
            return;
        default:
            put(s, '%');
            put(s, *fmt);
        }
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    Sink s = {buf, size, 0, -1, {0}, 0};
    format(&s, fmt, ap);
    if (size)
        buf[s.pos < size ? s.pos : size - 1] = 0;
    return (int)s.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static int fdprint(int fd, const char *fmt, va_list ap)
{
    Sink s = {0, 0, 0, fd, {0}, 0};
    format(&s, fmt, ap);
    if (s.n)
        write_all(fd, s.tmp, (size_t)s.n);
    return (int)s.pos;
}

int fprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = fdprint(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = fdprint(1, fmt, ap);
    va_end(ap);
    return n;
}

int read_line(int fd, char *buf, int max)
{
    int n = 0;
    for (;;) {
        char c;
        s64 r = sys_read(fd, &c, 1);
        if (r <= 0)
            return n ? (buf[n] = 0, n) : -1;
        if (c == '\n') {
            buf[n] = 0;
            return n;
        }
        if (n < max - 1)
            buf[n++] = c;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

void print_columns(const char *const *names, const char *const *colors, int n, int width)
{
    int maxlen = 0;
    for (int i = 0; i < n; i++) {
        int l = utf8_width(names[i]);
        if (l > maxlen)
            maxlen = l;
    }
    int colw = maxlen + 2;
    int ncols = width / colw;
    if (ncols < 1)
        ncols = 1;
    int nrows = (n + ncols - 1) / ncols;
    static const char spaces[] = "                                                                ";

    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < ncols; c++) {
            int i = c * nrows + r; /* spaltenweise, wie ls */
            if (i >= n)
                break;
            int l = utf8_width(names[i]);
            size_t bytes = strlen(names[i]);
            if (colors[i][0]) {
                write_all(1, colors[i], strlen(colors[i]));
                write_all(1, names[i], bytes);
                write_all(1, C_RESET, 4);
            } else {
                write_all(1, names[i], bytes);
            }
            if (c + 1 < ncols && (c + 1) * nrows + r < n) {
                for (int pad = colw - l; pad > 0;) {
                    int k = pad > 64 ? 64 : pad;
                    write_all(1, spaces, (size_t)k);
                    pad -= k;
                }
            }
        }
        write_all(1, "\n", 1);
    }
}

int utf8_len_at(const char *s)
{
    unsigned char c = (unsigned char)s[0];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    for (int i = 1; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            return 1; /* abgebrochene Folge */
    return n;
}

int utf8_width(const char *s)
{
    int w = 0;
    while (*s) {
        s += utf8_len_at(s);
        w++;
    }
    return w;
}

static s64 days_from_civil(s64 y, unsigned m, unsigned d)
{
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

u64 date_to_time(const DateTime *dt)
{
    s64 t = days_from_civil(dt->year, (unsigned)dt->month, (unsigned)dt->day) * 86400 + dt->hour * 3600 + dt->min * 60 + dt->sec;
    return t < 0 ? 0 : (u64)t;
}

void time_to_date(u64 t, DateTime *dt)
{
    s64 z = (s64)(t / 86400);
    unsigned rem = (unsigned)(t % 86400);
    dt->wday = (int)((z + 4) % 7);
    z += 719468;
    s64 era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    s64 y = (s64)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    dt->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    dt->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    dt->year = (int)(y + (dt->month <= 2));
    dt->hour = (int)(rem / 3600);
    dt->min = (int)(rem % 3600 / 60);
    dt->sec = (int)(rem % 60);
}

/* ---------- Kommandozeile des Kernels auf dem Boot-Volume ---------- */

static int read_small_file(const char *path, char *buf, int max)
{
    s64 fd = sys_open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = 0;
    s64 r;
    while (n < max - 1 && (r = sys_read((int)fd, buf + n, (u64)(max - 1 - n))) > 0)
        n += (int)r;
    sys_close((int)fd);
    buf[n] = 0;
    return n;
}

/* Setzt in der cmdline-Datei den Schluessel key (z.B. "mode=") auf value; value == NULL entfernt ihn */
int boot_cmdline_set(const char *dir, const char *key, const char *value)
{
    char path[PATH_MAX], old[300], out[300];
    snprintf(path, sizeof(path), "%s/cmdline.txt", dir);
    if (read_small_file(path, old, sizeof(old)) < 0)
        old[0] = 0;

    size_t kl = strlen(key);
    int n = 0;
    out[0] = 0;
    for (char *p = old; *p;) { /* Woerter durch Leerzeichen/Zeilenumbrueche getrennt; das alte Schluesselwort weglassen */
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
            p++;
        char *end = p;
        while (*end && *end != ' ' && *end != '\n' && *end != '\r' && *end != '\t')
            end++;
        int len = (int)(end - p);
        if (len && !(len >= (int)kl && strncmp(p, key, kl) == 0)) {
            if (n + len + 2 >= (int)sizeof(out))
                return -3;
            if (n)
                out[n++] = ' ';
            memcpy(out + n, p, (size_t)len);
            n += len;
            out[n] = 0;
        }
        p = end;
    }
    if (value) {
        int len = (int)(kl + strlen(value));
        if (n + len + 2 >= (int)sizeof(out) || n + len + 2 >= 256) /* der Bootloader liest hoechstens 255 Zeichen */
            return -3;
        if (n)
            out[n++] = ' ';
        n += snprintf(out + n, sizeof(out) - (size_t)n, "%s%s", key, value);
    }
    out[n++] = '\n';
    out[n] = 0;

    s64 fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return (int)fd;
    int rc = write_all((int)fd, out, (size_t)n);
    sys_close((int)fd);
    return rc;
}

/* Boot-Volumes: enthalten \kernel.elf in der Groesse dieses Kernels und einen Ordner \EFI */
int find_boot_volumes(char dirs[][40], int max)
{
    int found = 0;
    MountInfo m;
    VideoInfo vi;
    u64 want = sys_videoinfo(0, &vi) == 0 ? vi.kernel_size : 0;
    for (u64 i = 0; sys_mountinfo(i, &m) == 0 && found < max; i++) {
        if ((m.flags & 2) || m.point[0] != '/')
            continue;
        char p[80];
        Stat st;
        snprintf(p, sizeof(p), "%s/kernel.elf", m.point);
        if (sys_stat(p, &st) != 0 || st.is_dir || (want && st.size != want))
            continue;
        snprintf(p, sizeof(p), "%s/EFI", m.point);
        if (sys_stat(p, &st) != 0 || !st.is_dir)
            continue;
        snprintf(dirs[found++], 40, "%s", m.point);
    }
    return found;
}


char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}
