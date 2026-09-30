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

/* Stufe 4, Teil 1 (igdtest edid): nur lesen. Monitordaten (EDID) ueber DDC, aktueller Zeitablauf und Takt (igd_mode.c) */
int igd_edid_test(void);

/* Stufe 4, Teil 2 (igdtest scale): Testbilder in kleinerer Aufloesung, vom Skalierer der Pipe hochgerechnet; zurueck */
int igd_scale_test(void);

/* Stufe 4, Teil 3 (igdtest mode): die per HDMI moeglichen Modi aus der EDID je einige Sekunden setzen, dann zurueck */
int igd_mode_test(void);

/* DisplayPort, Teil 1 (igdtest dp): nur lesen. Auf den Ports B-D per AUX nach DP-Geraeten suchen, DPCD und EDID (igd_dp.c) */
int igd_dp_test(void);

/* DisplayPort, Teil 2 (igdtest dpmode): Modi des Monitors auf der bestehenden DP-Verbindung setzen, dann zurueck */
int igd_dpmode_test(void);

/* DisplayPort, Teil 3 (igdtest dptrain): Verbindung abschalten und selbst neu einmessen (Link-Training), Bild wieder an */
int igd_dptrain_test(void);

/* Anschluesse (igdtest output [b|c|d]): anzeigen; mit Port das Bild von Grund auf dorthin legen, 12 s, zurueck */
int igd_output_test(int port);

/* Bildwechsel-Interrupt (igd_irq.c): zaehlt die Bildwechsel der angezeigten Pipe und weckt Wartende */
int      igd_vblank_ok(void);
uint64_t igd_vblank_count(void);
int      igd_wait_vblank(int timeout_ms); /* 1 = Bildwechsel kam, 0 = Zeit abgelaufen oder kein Interrupt */
int      igd_vblank_test(void);           /* igdtest vblank */

/* Fest eingebaut: Modi des Monitors (EDID, per HDMI moeglich, hoechstens so gross wie der Framebuffer der Firmware)
 * im Betrieb umschalten; die Konsole passt sich an. hz in 1/100 Hz (0 = hoechste Bildrate bzw. beim Setzen egal);
 * w = 0 setzt den Modus der Firmware. */
#define IGD_MODE_NODRIVER (-1)
#define IGD_MODE_NOMODE   (-2)
#define IGD_MODE_BUSY     (-3) /* ein Grafikprogramm laeuft */
#define IGD_MODE_FAILED   (-4)
int  igd_mode_count(void);
int  igd_mode_info(int i, uint32_t *w, uint32_t *h, uint32_t *hz, int *current);
int  igd_mode_set(uint32_t w, uint32_t h, uint32_t hz);
void igd_modes_boot(void); /* aus igd_init: Modi einsammeln, "igdmode=" anwenden */

/* Fest eingebaut (ohne "noigd" in der Kommandozeile): Hardware-Mauszeiger und Doppelpufferung */
int  igd_cursor_available(void);
void igd_cursor_move(int x, int y, int visible); /* Spitze des Pfeils bei (x, y) */

/* Grafikmodus: Rechteck anzeigen. Ein ganzes Bild kommt in den verdeckten Puffer und wird beim Bildwechsel
 * umgeschaltet. 1 = erledigt, 0 = nicht zustaendig (dann wie bisher in den Framebuffer). */
int  igd_gfx_blit(const uint32_t *src, uint32_t pitch, int x, int y, int w, int h);
void igd_gfx_end(void); /* Grafikmodus endet: wieder den Framebuffer der Firmware anzeigen */

#endif
