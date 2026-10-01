#ifndef TTY_H
#define TTY_H

#include <stdint.h>

/* Terminal: Tastatur als stdin, Konsole (Bildschirm + Serial) als stdout.
 *
 * Zeilenmodus (Standard): Zeichen werden angezeigt (Echo), Backspace loescht, Enter schliesst die Zeile ab und
 * read() liefert sie. Ctrl-D am Zeilenanfang = Dateiende. Ctrl-C: siehe unten.
 * Rohmodus (fuer Shells/Editoren): read() liefert Zeichen sofort und ohne Echo; Sondertasten kommen als Codes >= 0x80.
 *
 * Ctrl-C beendet alle Prozesse der Vordergrundgruppe (tty_set_fg). Ist keine gesetzt, wird 0x03 als Zeichen geliefert
 * (im Zeilenmodus bricht es die angefangene Zeile ab). */

#define KEY_UP    0xF5
#define KEY_DOWN  0xF6
#define KEY_LEFT  0xF7
#define KEY_RIGHT 0xF8
#define KEY_HOME  0xF9
#define KEY_END   0xFA
#define KEY_DEL   0xFB
#define KEY_PGUP  0xFC
#define KEY_PGDN  0xFD
/* Linke Alt-Taste + Taste: dieses Byte, dann die Taste (Kleinbuchstabe, Ziffer, Tab, Leertaste oder Sondertaste
 * oben). Mit Shift KEY_ALT_SHIFT. Beide sind in UTF-8 nie gueltig; der Zeilenmodus und das rohe Lesen der Konsole
 * verwerfen sie samt Taste - auswerten tut sie nur, wer SYS_GETCHAR liest (Grafikprogramme, der Desktop). */
#define KEY_ALT       0xFE
#define KEY_ALT_SHIFT 0xFF

int64_t tty_read(void *buf, uint64_t len);        /* blockiert; ERR_INTR, wenn der Prozess gekillt wird */
int64_t tty_write(const void *buf, uint64_t len);

void     tty_set_mode(int raw);
void     tty_set_fg(uint32_t pgid);
uint32_t tty_get_fg(void);

/* Aus dem Tastatur-Interrupt: Ctrl-C. 1 = Vordergrundgruppe wurde gekillt (Zeichen nicht weitergeben). */
int tty_interrupt(void);

#endif
