#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdint.h>

/* UTF-8 und ein paar Unicode-Hilfen. Dateinamen sind im ganzen Kernel UTF-8; auf FAT/exFAT werden sie als UTF-16 gespeichert. */

#define UNI_REPLACEMENT 0xFFFD

/* Liest ein Zeichen aus einem NUL-terminierten UTF-8-Text und rueckt *s weiter. Ungueltige Folgen liefern
 * UNI_REPLACEMENT (und rücken um 1 Byte vor); am Textende wird 0 geliefert und nicht weitergerueckt. */
uint32_t utf8_next(const char **s);

/* Schreibt ein Zeichen als UTF-8 (1-4 Bytes, kein NUL). Liefert die Zahl der Bytes. */
int utf8_encode(uint32_t cp, char *out);

/* Ist der Text gueltiges UTF-8 (keine Ueberlaenge, keine Surrogate, hoechstens U+10FFFF)? */
int utf8_valid(const char *s);

/* Zeichen -> UTF-16 (Ersatzpaare fuer > U+FFFF). Liefert die Zahl der Einheiten (1 oder 2). */
int utf16_encode(uint32_t cp, uint16_t out[2]);

/* UTF-16-Einheiten (bis count oder bis zum ersten 0/0xFFFF) -> UTF-8, max Bytes inkl. NUL. Liefert die Laenge. */
int utf16_to_utf8(const uint16_t *u, int count, char *out, int max);

/* Text -> UTF-16 (ohne Abschluss). Liefert die Zahl der Einheiten oder -1, wenn es mehr als max sind / ungueltig ist. */
int utf8_to_utf16(const char *s, uint16_t *out, int max);

/* Einfaches Gross-Mapping wie die Standard-Upcase-Tabelle von exFAT (ASCII, Latin-1, Latin Erweitert-A, Griechisch,
 * Kyrillisch). Wird fuer Vergleiche ohne Beachtung der Schreibweise und fuer den exFAT-Namenshash benutzt. */
uint32_t uni_upper(uint32_t cp);

#endif
