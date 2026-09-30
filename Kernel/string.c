#include "string.h"
#include <stdint.h>

/* Ohne Optimierung/Loop-Idiom-Erkennung, sonst wuerde GCC die Schleifen wieder in memset/memcpy-Aufrufe umwandeln. */
#define NO_LOOP_IDIOM __attribute__((optimize("no-tree-loop-distribute-patterns")))

/* Kopieren und Fuellen mit den String-Befehlen der CPU: 8 Byte je Schritt (rep movsq/stosq), den Rest byteweise.
 * Das ist um ein Vielfaches schneller als eine Byte-Schleife, vor allem beim Schreiben in den Framebuffer. */
NO_LOOP_IDIOM void *memset(void *dst, int c, size_t n)
{
    uint64_t v = (uint8_t)c * 0x0101010101010101ULL;
    void *d = dst;
    size_t q = n >> 3, r = n & 7;
    __asm__ __volatile__("rep stosq" : "+D"(d), "+c"(q) : "a"(v) : "memory");
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(r) : "a"(v) : "memory");
    return dst;
}

NO_LOOP_IDIOM void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;
    const void *s = src;
    size_t q = n >> 3, r = n & 7;
    __asm__ __volatile__("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(r) : : "memory");
    return dst;
}

NO_LOOP_IDIOM void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        memcpy(d, s, n); /* vorwaerts kopieren ist bei d < s immer sicher */
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
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

NO_LOOP_IDIOM int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
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

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return c == 0 ? (char *)s : 0;
}
