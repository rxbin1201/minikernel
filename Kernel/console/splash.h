#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>

/* Startanimation (splash.c): Logo und drehender Punktkreis, direkt in den Framebuffer. Gesteuert von der Konsole
 * (console_splash_start/_end): solange sie laeuft, schreibt die Konsole nur ins RAM-Abbild. */
void splash_draw(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h);              /* ganzes Bild */
void splash_tick(volatile uint32_t *fb, uint32_t pitch, uint32_t w, uint32_t h, uint64_t ms); /* nur der Punktkreis */

#endif
