#ifndef STRING_H
#define STRING_H

#include <stddef.h>

/* GCC kann auch im Freestanding-Modus Aufrufe an diese Funktionen erzeugen. */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char  *strchr(const char *s, int c);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dst, const char *src, size_t n); /* fuellt wie ueblich bis n mit 0 auf */

#endif
