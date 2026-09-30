#include "utf8.h"

uint32_t utf8_next(const char **sp)
{
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t c = s[0];
    if (c == 0)
        return 0;
    int extra;
    uint32_t cp, min;
    if (c < 0x80) {
        *sp += 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        extra = 1; cp = c & 0x1F; min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2; cp = c & 0x0F; min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3; cp = c & 0x07; min = 0x10000;
    } else {
        *sp += 1;
        return UNI_REPLACEMENT;
    }
    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *sp += 1;
            return UNI_REPLACEMENT;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *sp += 1;
        return UNI_REPLACEMENT;
    }
    *sp += extra + 1;
    return cp;
}

int utf8_encode(uint32_t cp, char *out)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = UNI_REPLACEMENT;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_valid(const char *s)
{
    for (;;) {
        const char *before = s;
        uint32_t cp = utf8_next(&s);
        if (cp == 0)
            return 1;
        if (cp == UNI_REPLACEMENT && s == before + 1)
            return 0; /* ungueltige Folge (ein echtes U+FFFD waere 3 Bytes lang) */
    }
}

int utf16_encode(uint32_t cp, uint16_t out[2])
{
    if (cp < 0x10000) {
        out[0] = (uint16_t)cp;
        return 1;
    }
    cp -= 0x10000;
    out[0] = (uint16_t)(0xD800 | (cp >> 10));
    out[1] = (uint16_t)(0xDC00 | (cp & 0x3FF));
    return 2;
}

int utf16_to_utf8(const uint16_t *u, int count, char *out, int max)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        uint32_t c = u[i];
        if (c == 0 || c == 0xFFFF)
            break;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < count && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = UNI_REPLACEMENT; /* einzelnes Ersatzzeichen */
        }
        char tmp[4];
        int l = utf8_encode(c, tmp);
        if (n + l > max - 1)
            break; /* an einer Zeichengrenze abschneiden */
        for (int k = 0; k < l; k++)
            out[n++] = tmp[k];
    }
    out[n] = 0;
    return n;
}

int utf8_to_utf16(const char *s, uint16_t *out, int max)
{
    int n = 0;
    for (;;) {
        const char *before = s;
        uint32_t cp = utf8_next(&s);
        if (cp == 0)
            return n;
        if (cp == UNI_REPLACEMENT && s == before + 1)
            return -1;
        uint16_t u[2];
        int l = utf16_encode(cp, u);
        if (n + l > max)
            return -1;
        for (int k = 0; k < l; k++)
            out[n++] = u[k];
    }
}

uint32_t uni_upper(uint32_t c)
{
    if (c < 0x80)
        return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c == 0xB5)
        return 0x39C;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return c - 0x20;
    if (c == 0xFF)
        return 0x178;
    if (c >= 0x100 && c <= 0x17F) { /* Latin Erweitert-A: meist abwechselnd Gross/Klein */
        if (c == 0x131)
            return 'I';
        if (c == 0x17F)
            return 'S';
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
            return (c & 1) ? c - 1 : c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c & 1) ? c : c - 1;
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3C9) /* Griechisch */
        return c == 0x3C2 ? 0x3A3 : c - 0x20;
    if (c >= 0x430 && c <= 0x44F) /* Kyrillisch */
        return c - 0x20;
    if (c >= 0x450 && c <= 0x45F)
        return c - 0x50;
    return c;
}
