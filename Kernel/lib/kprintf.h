#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

/* Gibt ein Zeichen auf Serial UND Framebuffer-Konsole aus. */
void kputc(char c);      /* Meldung des Kernels (mit kprintf_quiet nicht auf dem Bildschirm) */
void kputc_user(char c); /* Ausgabe eines Programms (Konsole): immer auf den Bildschirm */
int  kprintf_quiet(int on); /* 1 = Meldungen nur seriell und in dmesg; liefert den alten Wert */

/* Unterstuetzt: %d %i %u %x %X %p %c %s %% mit Flags '-' '0' '+' ' ' '#', Breite, Genauigkeit (auch '*')
 * und Laengen hh h l ll z j t. Gleiche Semantik wie printf; kein Gleitkomma. */
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int kvprintf(const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
