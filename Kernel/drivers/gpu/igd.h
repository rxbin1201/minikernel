#ifndef IGD_H
#define IGD_H

#include <stdint.h>
#include "boot_info.h"

/* Intel-Grafik im Prozessor (IGD, PCI 00:02.0), Generation 9: Skylake, Kaby Lake, Coffee Lake (UHD Graphics 630
 * u.a.), Comet Lake. Stufe 1: erkennen und nur lesen, was die UEFI-Firmware eingerichtet hat (Pipes, Ebenen,
 * Anschluesse) und welche Pipe den GOP-Framebuffer zeigt. Es wird nichts geschrieben.
 * Registerangaben nach Intels "Programmer's Reference Manual" fuer Skylake/Kaby Lake (Vol. 2c) und dem Linux-i915. */

typedef struct {
    int      present;    /* Intel-GPU gefunden */
    int      gen9;       /* bekannte Gen9-GPU: nur dann werden Register gelesen */
    uint16_t device;
    const char *name;
    uint64_t mmio;       /* BAR0 (GTTMMADR): Register ab 0, globale GTT ab der Haelfte */
    uint64_t mmio_size;
    uint64_t aperture;   /* BAR2 (GMADR): CPU-Fenster in den Grafikspeicher (ueber die GTT) */
    int      scanout_pipe; /* Pipe (0 = A), die den GOP-Framebuffer zeigt, -1 = unbekannt */
    uint32_t scanout_surf; /* deren PLANE_SURF (Adresse im Grafik-Adressraum) */
} IgdInfo;

void igd_init(const BootInfo *info);
const IgdInfo *igd_info(void);

/* Stufe 2, nur auf Befehl (igdtest): zweiter Bildpuffer ueber die GGTT, Umschalten per PLANE_SURF, zurueck.
 * Ausgaben ins Kernel-Log. 0 = funktioniert, < 0 = Abbruch oder Fehler. */
int igd_flip_test(void);

/* Stufe 2, nur auf Befehl (igdtest cursor): Hardware-Mauszeiger einschalten, 3 s im Kreis bewegen, wieder aus */
int igd_cursor_test(void);

/* Stufe 3, nur auf Befehl (igdtest blit): Blitter-Engine starten, fuellen/kopieren pruefen, Geschwindigkeit messen,
 * sichtbare Rechtecke und Scrollen; danach alles zurueck (igd_blt.c) */
int igd_blit_test(void);

/* igdtest info: Zaehler der Bild-Updates seit dem Start und Messung der Kopierwege fuer ganze Bilder (Kernel-Log) */
int igd_info_report(void);

/* Fest eingebaut (ohne "noigd" in der Kommandozeile): Hardware-Mauszeiger und Doppelpufferung */
int  igd_cursor_available(void);
void igd_cursor_move(int x, int y, int visible); /* Spitze des Pfeils bei (x, y) */

/* Grafikmodus: Rechteck anzeigen. Ein ganzes Bild kommt in den verdeckten Puffer und wird beim Bildwechsel
 * umgeschaltet. 1 = erledigt, 0 = nicht zustaendig (dann wie bisher in den Framebuffer). */
int  igd_gfx_blit(const uint32_t *src, uint32_t pitch, int x, int y, int w, int h);
void igd_gfx_end(void); /* Grafikmodus endet: wieder den Framebuffer der Firmware anzeigen */

#endif
