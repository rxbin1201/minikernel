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
/* Tasten mit Umschalttasten fuer Grafikprogramme (in UTF-8 nie gueltige Bytes):
 *   KEY_ALT, Taste            linke Alt + Taste (Kleinbuchstabe, Ziffer, Tab, Leertaste oder Sondertaste oben)
 *   KEY_MODS, mods, Taste     mods: 1 Shift, 2 Alt, 4 Strg - z.B. Shift + Pfeil (nur, solange ein Grafikprogramm den
 *                             Bildschirm hat; in der Konsole bleibt Shift + Bild hoch/runter das Blaettern)
 * Der Zeilenmodus und das rohe Lesen der Konsole liefern bei KEY_MODS ohne Alt nur die Taste und verwerfen den Rest;
 * auswerten tut das, wer SYS_GETCHAR liest (Grafikbibliothek, Desktop). */
#define KEY_ALT  0xFE
#define KEY_MODS 0xFF

int64_t tty_read(void *buf, uint64_t len);        /* blockiert; ERR_INTR, wenn der Prozess gekillt wird */
int64_t tty_write(const void *buf, uint64_t len);

void     tty_set_mode(int raw);
void     tty_set_fg(uint32_t pgid);
uint32_t tty_get_fg(void);

/* Aus dem Tastatur-Interrupt: Ctrl-C. 1 = Vordergrundgruppe wurde gekillt (Zeichen nicht weitergeben). */
int tty_interrupt(void);

#endif
