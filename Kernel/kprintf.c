#include "kprintf.h"
#include "console.h"
#include "cpu.h"
#include "serial.h"
#include <stdint.h>

/* Ausgabe-Senke: pro Zeichen aufgerufen. Damit teilen sich kprintf und ksnprintf denselben Formatierer. */
typedef struct {
    void  (*put)(char c, void *ctx);
    void   *ctx;
    size_t  count; /* Anzahl erzeugter Zeichen (auch ueber die Puffergrenze hinaus) */
} Sink;

static void emit(Sink *s, char c)
{
    s->put(c, s->ctx);
    s->count++;
}

static void emit_repeat(Sink *s, char c, int n)
{
    while (n-- > 0)
        emit(s, c);
}

typedef struct {
    int left, zero, plus, space, alt;
    int width, precision; /* precision < 0: nicht angegeben */
} Spec;

static void format_number(Sink *s, const Spec *sp, uint64_t v, int is_signed, int base, int upper, int is_ptr)
{
    char digits[32];
    int  n = 0;
    char prefix[3];
    int  plen = 0;
    int  negative = 0;

    if (is_signed && (int64_t)v < 0) {
        negative = 1;
        v = -v;
    }

    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[n++] = set[v % base];
        v /= base;
    } while (v);

    if (negative)
        prefix[plen++] = '-';
    else if (is_signed && sp->plus)
        prefix[plen++] = '+';
    else if (is_signed && sp->space)
        prefix[plen++] = ' ';

    if (is_ptr || (sp->alt && base == 16 && !(n == 1 && digits[0] == '0'))) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }

    int zeros = 0;
    if (sp->precision >= 0) {
        if (sp->precision > n)
            zeros = sp->precision - n;
    } else if (sp->zero && !sp->left) {
        int total = plen + n;
        if (sp->width > total)
            zeros = sp->width - total;
    }

    int len = plen + zeros + n;
    int pad = sp->width > len ? sp->width - len : 0;

    if (!sp->left)
        emit_repeat(s, ' ', pad);
    for (int i = 0; i < plen; i++)
        emit(s, prefix[i]);
    emit_repeat(s, '0', zeros);
    while (n)
        emit(s, digits[--n]);
    if (sp->left)
        emit_repeat(s, ' ', pad);
}

static void format_string(Sink *s, const Spec *sp, const char *str)
{
    if (!str)
        str = "(null)";
    int len = 0;
    while (str[len] && (sp->precision < 0 || len < sp->precision))
        len++;

    int pad = sp->width > len ? sp->width - len : 0;
    if (!sp->left)
        emit_repeat(s, ' ', pad);
    for (int i = 0; i < len; i++)
        emit(s, str[i]);
    if (sp->left)
        emit_repeat(s, ' ', pad);
}

static void vformat(Sink *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(s, *fmt);
            continue;
        }
        fmt++;

        Spec sp = {0, 0, 0, 0, 0, 0, -1};

        /* Flags */
        for (;; fmt++) {
            if      (*fmt == '-') sp.left = 1;
            else if (*fmt == '0') sp.zero = 1;
            else if (*fmt == '+') sp.plus = 1;
            else if (*fmt == ' ') sp.space = 1;
            else if (*fmt == '#') sp.alt = 1;
            else break;
        }

        /* Breite */
        if (*fmt == '*') {
            sp.width = va_arg(ap, int);
            if (sp.width < 0) {
                sp.left = 1;
                sp.width = -sp.width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                sp.width = sp.width * 10 + (*fmt++ - '0');
        }

        /* Genauigkeit */
        if (*fmt == '.') {
            fmt++;
            sp.precision = 0;
            if (*fmt == '*') {
                sp.precision = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    sp.precision = sp.precision * 10 + (*fmt++ - '0');
            }
        }

        /* Laenge: 0 = int, 1 = long, 2 = long long/size_t/..., -1 = short, -2 = char */
        int len = 0;
        if (*fmt == 'h') {
            len = -1;
            fmt++;
            if (*fmt == 'h') { len = -2; fmt++; }
        } else if (*fmt == 'l') {
            len = 1;
            fmt++;
            if (*fmt == 'l') { len = 2; fmt++; }
        } else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            len = 2;
            fmt++;
        }

        char conv = *fmt;
        if (!conv)
            break;

        switch (conv) {
        case 'd':
        case 'i': {
            int64_t v;
            if      (len >= 1)  v = len == 1 ? va_arg(ap, long) : va_arg(ap, long long);
            else                v = va_arg(ap, int);
            if (len == -1) v = (short)v;
            if (len == -2) v = (signed char)v;
            format_number(s, &sp, (uint64_t)v, 1, 10, 0, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v;
            if      (len >= 1)  v = len == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned long long);
            else                v = va_arg(ap, unsigned int);
            if (len == -1) v = (unsigned short)v;
            if (len == -2) v = (unsigned char)v;
            format_number(s, &sp, v, 0, conv == 'u' ? 10 : 16, conv == 'X', 0);
            break;
        }
        case 'p':
            format_number(s, &sp, (uint64_t)(uintptr_t)va_arg(ap, void *), 0, 16, 0, 1);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = sp.width > 1 ? sp.width - 1 : 0;
            if (!sp.left)
                emit_repeat(s, ' ', pad);
            emit(s, c);
            if (sp.left)
                emit_repeat(s, ' ', pad);
            break;
        }
        case 's':
            format_string(s, &sp, va_arg(ap, const char *));
            break;
        case '%':
            emit(s, '%');
            break;
        default: /* unbekannt: unveraendert ausgeben */
            emit(s, '%');
            emit(s, conv);
        }
    }
}

/* ---- kprintf: Serial + Konsole ---- */

void kputc(char c)
{
    static int esc; /* ANSI-Farbfolgen gehen nur an die Konsole, nicht in das serielle Log */
    console_putc(c);
    if (esc == 0 && c == 0x1B) {
        esc = 1;
        return;
    }
    if (esc == 1) {
        esc = c == '[' ? 2 : 0;
        return;
    }
    if (esc == 2) {
        if (c >= '@' && c <= '~')
            esc = 0;
        return;
    }
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);
}

static void put_console(char c, void *ctx)
{
    (void)ctx;
    kputc(c);
}

int kvprintf(const char *fmt, va_list ap)
{
    Sink s = {put_console, 0, 0};
    /* Ganze Ausgabe am Stueck, damit sich Zeilen verschiedener Threads/Handler nicht vermischen */
    uint64_t flags = irq_save();
    vformat(&s, fmt, ap);
    irq_restore(flags);
    return (int)s.count;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* ---- ksnprintf: in Puffer ---- */

typedef struct {
    char  *buf;
    size_t size;
    size_t pos;
} BufCtx;

static void put_buf(char c, void *ctx)
{
    BufCtx *b = ctx;
    if (b->pos + 1 < b->size)
        b->buf[b->pos] = c;
    b->pos++;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    BufCtx b = {buf, size, 0};
    Sink s = {put_buf, &b, 0};
    vformat(&s, fmt, ap);
    if (size)
        buf[b.pos < size ? b.pos : size - 1] = 0;
    return (int)s.count;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
