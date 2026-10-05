#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include "boot_info.h"

/* Textkonsole auf dem GOP-Framebuffer (32 bpp, Pixelformat 0x00RRGGBB), 8x16-Font, mit Scrolling.
 * Verarbeitet \n, \r, \t und \b sowie ANSI-Farbfolgen (ESC[31m, ESC[1;34m, ESC[0m, ESC[2J, ESC[H, ESC[K). */
void console_init(const BootFramebuffer *fb);
void console_putc(char c);

/* Legt ein Abbild des Bildschirms im RAM an (nach heap_init): schnelleres Scrollen, kein Lesen mehr vom Framebuffer. */
void console_enable_shadow(void);
void console_clear(void);
void console_set_color(uint32_t fg, uint32_t bg);

/* Fuer Selbsttests: Cursorposition (in Zeichen) und Pixel aus dem Framebuffer lesen. */
void     console_get_cursor(uint32_t *col, uint32_t *row);
uint32_t console_rows(void);
uint32_t console_cols(void);
uint32_t console_width_px(void);
uint32_t console_height_px(void);

/* Schriftvergroesserung (1 = 8x16, 2 = 16x32 ...); bei sehr hohen Aufloesungen stellt console_init selbst 2 ein. Loescht den Bildschirm. */
void     console_set_scale(uint32_t scale);
uint32_t console_scale(void);

/* Nach einem Moduswechsel: neue sichtbare Groesse (hoechstens die vom Start, gleiche Zeilenlaenge). Die Schrift passt
 * sich an, wenn sie nicht per scale= festgelegt ist; die letzten Zeilen bleiben stehen. 0 = ok, -1 = geht nicht
 * (z.B. zeichnet gerade ein Grafikprogramm) */
int console_resize(uint32_t w, uint32_t h);
int console_gfx_active(void); /* ein Programm hat den Bildschirm (SYS_GFX) */

/* Verlauf: aus dem Bild gescrollte Zeilen bleiben erhalten. console_scroll_request ist aus Interrupts/anderen Threads
 * aufrufbar (positiv = zurueck in die Vergangenheit); ein Konsolen-Thread (console_start_thread, nach sched_init)
 * blaettert dann. Neue Ausgabe laesst die Ansicht stehen; ein Tastendruck springt zurueck zum Ende. */
void     console_start_thread(void);
void     console_scroll_request(int lines);
void     console_live_request(void);  /* zurueck zum Ende (bei einem Tastendruck), aus Interrupts aufrufbar */
void     console_kick(void);          /* Konsolen-Thread wecken (z.B. Mauszeiger neu zeichnen), aus Interrupts aufrufbar */
void     console_view_scroll(int64_t delta); /* sofort blaettern (nicht aus Interrupts) */
void     console_view_live(void);
uint32_t console_history_lines(void);
uint64_t console_view_offset(void);
uint32_t console_debug_char(uint32_t col, uint32_t row); /* Zeichen im sichtbaren Bild (Selbsttests) */

/* Markieren mit der Maus (Pixelpositionen), Kopieren in die Zwischenablage und Einfuegen */
void        console_select_press(int x, int y);   /* linke Taste gedrueckt */
void        console_select_move(int x, int y);    /* bei gedrueckter Taste bewegt */
void        console_select_release(void);
void        console_select_word(int x, int y);    /* Doppelklick: Wort (bis zum naechsten Leerzeichen) */
void        console_select_line(int x, int y);    /* Dreifachklick: ganze Zeile */
int         console_has_selection(void);
void        console_select_clear(void);
uint32_t    console_copy_selection(void);         /* Markierung -> Zwischenablage; Laenge, 0 = nichts markiert */
#define CONSOLE_CLIP_MAX (4u << 20) /* Zwischenablage: hoechstens 4 MiB */
const char *console_clipboard(uint32_t *len);
int         console_clipboard_set(const char *text, uint32_t len); /* 0, -1 = zu gross oder kein Speicher */

/* Mauszeiger (nur Framebuffer, nicht im Abbild): Position der Spitze in Pixeln; visible = 0 blendet ihn aus */
void     console_cursor_set(int x, int y, int visible);
/* Groesse des Mauszeigers in Prozent (0 = nach der Schriftgroesse wie bisher, sonst 100-250); gilt fuer den
 * Hardware-Zeiger (igd.c), den Zeiger der Konsole und - ueber SYS_GFX 6 abgefragt - den der Grafikbibliothek */
/* Startanimation (splash.c) statt der Meldungen: die Konsole schreibt dann nur ins Abbild (dmesg zeigt alles). Endet,
 * wenn ein Programm den Bildschirm nimmt oder etwas ausgibt (console_splash_end(0): leere Konsole) oder bei einem
 * Fehler des Kernels (console_splash_end(1): mit allen Meldungen) */
void     console_splash_start(void);
void     console_splash_progress(int percent); /* Ladebalken: so weit ist der Start (steigt nur) */
void     console_splash_end(int show_log);
int      console_splash_active(void);
void     console_cursor_size(uint32_t pct);
uint32_t console_cursor_pct(void);
void     console_set_tick(void (*hook)(void));

/* Grafikmodus: ein Programm uebernimmt den Bildschirm (die Konsole schreibt dann nur in ihr Abbild und kommt beim
 * Freigeben unveraendert zurueck). acquire: 0 = ok, -2 = belegt. */
int      console_gfx_acquire(uint32_t pid);
void     console_gfx_release(uint32_t pid);
int      console_gfx_owner(uint32_t pid);
void     console_gfx_blit(const uint32_t *src, uint32_t src_pitch, int x, int y, int w, int h); /* wird alle 10 ms vom Konsolen-Thread aufgerufen */
uint32_t console_debug_fb_pixel(uint32_t x, uint32_t y); /* liest den Framebuffer (langsam, nur Tests) */
uint32_t console_read_pixel(uint32_t x, uint32_t y);
void     console_repaint(void); /* Framebuffer komplett aus dem Abbild neu schreiben */

#endif
