#ifndef KEYBOARD_H
#define KEYBOARD_H

/* PS/2-Tastatur (Scancode-Set 1) ueber IRQ 1 / IOAPIC. Das Layout (us/de/uk) stellt keymap.h ein. */
int keyboard_init(void);

/* Von Tastatur-Treibern (PS/2, USB) aufgerufen: liefert ein Zeichen bzw. einen Sondertastencode (siehe tty.h) an die
 * Eingabe. Behandelt Ctrl-C (Vordergrundgruppe beenden). */
void keyboard_deliver(unsigned char c);

/* Taste mit Umschalttasten (mods: 1 Shift, 2 Alt, 4 Strg, siehe KEY_MODS in tty.h): Hat ein Grafikprogramm den
 * Bildschirm, kommen sie mit; sonst wie bisher (Shift + Bild hoch/runter blaettert in der Konsole). */
void keyboard_deliver_mods(int mods, unsigned char k);

/* Gerade gedrueckte Umschalttasten aller Tastaturen (1 Shift, 2 Alt, 4 Strg), z.B. fuer Strg+Klick */
int keyboard_mods(void);
void keyboard_usb_mods(unsigned char hid_mods); /* USB-Tastaturen melden ihren Stand (HID-Modifier-Byte) */

/* Zwischenablage (console.h) als Eingabe einspeisen (Ctrl-V, rechte Maustaste). Ctrl-C bei markiertem Text kopiert. */
void keyboard_paste(void);

/* Shift + Bild hoch/runter blaettert im Verlauf der Konsole, Shift + Pos1/Ende springt an den Anfang/das Ende.
 * Liefert 1, wenn die Taste dafuer benutzt wurde (dann nicht weiterreichen). key = Sondertastencode (tty.h). */
int keyboard_scroll_key(unsigned char key, int shift);

/* Naechstes getipptes Zeichen oder -1, wenn keins bereitliegt. */
int keyboard_getchar(void);

/* Scancode (Set 1, Make-Code) fuer ein Zeichen oder -1; *needs_shift = 1, wenn Shift noetig ist. */
int keyboard_scancode_for(char c, int *needs_shift);

/* Nur fuer Tests: schickt ein Scancode-Byte ueber den 8042 so, als kaeme es von der Tastatur. */
void keyboard_inject_scancode(unsigned char code);

#endif
