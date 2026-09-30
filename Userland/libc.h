#ifndef LIBC_H
#define LIBC_H

#include <stdarg.h>
#include "user.h"

/* Kleine C-Bibliothek fuer die User-Programme (Userland/lib/libc.c, wird an jedes Programm gelinkt). */

typedef u64 size_t;
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Speicher und Strings */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void   memset32(void *dst, unsigned int v, size_t count); /* count 32-Bit-Werte */
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strcpy(char *dst, const char *src);
char  *strcat(char *dst, const char *src);
int    atoi(const char *s);
char  *strrchr(const char *s, int c);
int    strcasecmp(const char *a, const char *b);

/* Ausgabe (printf: %d %i %u %x %X %c %s %p %% mit Breite, '-' und '0', Laengen l und ll, %.Ns) */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* Schreibt alles (auch bei Teilschreibvorgaengen). 0 = ok, -1 = Fehler */
int write_all(int fd, const void *buf, size_t n);

/* Liest eine Zeile (ohne '\n') byteweise aus fd. Liefert die Laenge, -1 bei Dateiende ohne Daten. */
int read_line(int fd, char *buf, int max);

/* UTF-8: Dateinamen und Text sind UTF-8, ein Zeichen belegt eine Zelle auf dem Bildschirm */
int utf8_width(const char *s);                 /* Zahl der Zeichen (Zellen) */
int utf8_len_at(const char *s);                /* Laenge des Zeichens an s in Bytes (1-4; ungueltige Bytes zaehlen einzeln) */

/* Zeit: Sekunden seit 1970 <-> Datum. Es ist die Zeit der RTC (keine Zeitzonen-Umrechnung). */
typedef struct {
    int year, month, day, hour, min, sec;
    int wday; /* 0 = Sonntag */
} DateTime;
void time_to_date(u64 t, DateTime *dt);
u64  date_to_time(const DateTime *dt);

/* \cmdline.txt des Boot-Volumes (gilt ab dem naechsten Start). find_boot_volumes sucht Volumes mit \kernel.elf
 * (Groesse wie der laufende Kernel) und \EFI; boot_cmdline_set setzt "key" (z.B. "kbd=") auf value bzw. entfernt ihn
 * (value = NULL). 0 = ok, sonst Fehlercode. */
int find_boot_volumes(char dirs[][40], int max);
int boot_cmdline_set(const char *dir, const char *key, const char *value);

/* ANSI-Farben (die Konsole versteht diese Folgen; in Pipes und Dateien nur ausgeben, wenn sys_isatty(1) != 0) */
#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_DIM     "\x1b[90m"
#define C_RED     "\x1b[1;31m"
#define C_GREEN   "\x1b[1;32m"
#define C_YELLOW  "\x1b[1;33m"
#define C_BLUE    "\x1b[1;34m"
#define C_MAGENTA "\x1b[1;35m"
#define C_CYAN    "\x1b[1;36m"
#define C_WHITE   "\x1b[1;37m"

/* Gibt n Namen spaltenweise aus (wie ls). colors[i] ist eine Farbfolge oder ""; width = Zeilenbreite in Zeichen. */
void print_columns(const char *const *names, const char *const *colors, int n, int width);

#endif
