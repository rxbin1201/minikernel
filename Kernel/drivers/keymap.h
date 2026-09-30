#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>

/* Tastaturlayouts (us, de, uk) fuer PS/2- und USB-Tastaturen. Beide Treiber melden die Taste als HID-Usage-Code
 * (Position der Taste, wie bei USB; PS/2-Scancodes werden umgerechnet), das Layout macht daraus das Zeichen.
 * Unterstuetzt: Shift, Feststelltaste (auch fuer Umlaute), AltGr (rechte Alt-Taste, beim deutschen Layout auch
 * Strg+Alt), Strg+Buchstabe (Steuerzeichen), Tottasten ^ ´ ` (deutsch: ^ + a = â, ´ + e = é; Leertaste = das Zeichen selbst).
 * Die Zeichen gehen als UTF-8 an keyboard_deliver. */

#define KEYMAP_NAME_MAX 8

int         keymap_set(const char *name);   /* 0 = ok, -1 = unbekannt */
const char *keymap_name(void);
const char *keymap_list(void);              /* "us de uk" */

/* Eine gedrueckte Taste (HID-Usage 0x04..0x38, 0x54..0x64). altgr: 1 = rechte Alt-Taste, 2 = nur linke Alt-Taste.
 * Liefert 1, wenn die Taste zum Layout gehoert. */
int keymap_key(uint8_t usage, int shift, int ctrl, int altgr, int caps);

/* PS/2 Scancode-Set 1 (Make-Code ohne 0xE0) -> HID-Usage, 0 = keine Zeichentaste */
uint8_t keymap_ps2_to_usage(uint8_t scancode);

#endif
