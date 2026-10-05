#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>

/* Startbild (splash.c): schwarzer Grund, das Startlogo der Firmware (BGRT) an seiner Stelle, darunter ein
 * Ladebalken wie bei macOS - direkt in den Framebuffer. Gesteuert von der Konsole (console_splash_*): solange es steht,
 * schreibt die Konsole nur ins RAM-Abbild. Fortschritt in Promille. */
void splash_logo(const void *bmp, uint64_t size, uint32_t x, uint32_t y, uint32_t scr_w, uint32_t scr_h);
void splash_draw(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h, int permille); /* ganzes Bild */
void splash_bar(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h, int permille);  /* nur der Balken */

#endif
