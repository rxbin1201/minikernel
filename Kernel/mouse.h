#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "syscall.h" /* MouseInfo */

/* Maus: zeigt einen Cursor auf dem Bildschirm (sobald eine USB-Maus angeschlossen ist) und stellt Position/Tasten den
 * Programmen bereit (SYS_MOUSE). Das Mausrad blaettert im Verlauf der Konsole. Treiber: USB HID Boot-Protokoll
 * (Kernel/usb_mouse.c). Nicht unterstuetzt: PS/2-Mause und Touchpads, Mause ohne Boot-Protokoll. */

void mouse_init(void); /* nach console_start_thread und usb_init aufrufen */

/* Von Treibern aufgerufen (aus dem USB-Thread): Tastenzustand (Bit 0 links, 1 rechts, 2 Mitte), Bewegung in
 * Geraete-Einheiten (dy > 0 = nach unten), Rad (> 0 = weg vom Benutzer = nach oben). */
void mouse_report(uint8_t buttons, int dx, int dy, int wheel);
/* Absolute Zeigegeraete: Position als Bruchteil des Bildschirms (0..65535 = links/oben bis rechts/unten) */
void mouse_report_abs(uint8_t buttons, uint32_t fx, uint32_t fy, int wheel);

int  mouse_attached(void);
void mouse_click_reset(void);
/* Ein Programm uebernimmt die Maus (pid) bzw. gibt sie zurueck (0): dann markiert/blaettert/fuegt die Konsole nicht
 * selbst ein, das Programm liest alles ueber SYS_MOUSE. Endet das Programm, faellt die Maus an die Konsole zurueck. */
void mouse_set_owner(uint32_t pid);
void mouse_owner_exit(uint32_t pid); /* Mehrfachklick-Zaehlung zuruecksetzen (Selbsttests) */
void mouse_get(MouseInfo *out); /* setzt den aufgelaufenen Radwert zurueck */

/* Teil des Konsolen-Threads: bringt den Cursor auf den neuesten Stand */
void mouse_tick(void);

/* Von usb_mouse.c: Zahl der angeschlossenen (noch vorhandenen) Mause */
int  usb_mouse_count(void);

#endif
