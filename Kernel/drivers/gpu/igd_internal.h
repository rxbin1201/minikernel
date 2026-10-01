#ifndef IGD_INTERNAL_H
#define IGD_INTERNAL_H

#include <stdint.h>
#include "drivers/gpu/igd.h"
#include "drivers/pci.h"

/* Gemeinsam zwischen igd.c (Erkennung, Anzeige, Mauszeiger, Doppelpufferung) und igd_blt.c (Blitter-Engine) */

#define IGD_PTE_VALID 1ULL

extern IgdInfo            igd_state;
extern volatile uint8_t  *igd_regs;
extern volatile uint64_t *igd_ggtt;          /* globale Grafik-Seitentabelle */
extern uint32_t           igd_ggtt_entries;

uint32_t igd_rd(uint32_t off);
void     igd_wr(uint32_t off, uint32_t v);
void     igd_clflush(uint64_t addr, uint64_t len);   /* CPU-Cache zurueckschreiben/verwerfen (GPU liest am Cache vorbei) */
void     igd_ggtt_flush(void);                        /* geaenderte GGTT-Eintraege uebernehmen */
int      igd_ggtt_claim(uint32_t base, uint32_t pages, uint64_t *saved); /* 0 = frei, alte Eintraege gesichert */
int      igd_preflight(const char *what);             /* Vorpruefung fuer igdtest (0 = weiter) */
uint32_t igd_underrun_begin(int pipe);                 /* FIFO-Unterlauf festhalten lassen; Ergebnis: alte Maske */
int      igd_underrun_end(int pipe, uint32_t imr);     /* 1 = Unterlauf seit begin */
int      igd_dpcd_read(int port, uint32_t addr, uint8_t *buf, int len); /* igd_dp.c: DPCD per AUX, len <= 16 */
int      igd_dpcd_write(int port, uint32_t addr, const uint8_t *buf, int len); /* igd_dp.c: 0 = ok */
int      igd_dp_edid(int port, uint8_t *edid);         /* igd_dp.c: EDID per AUX (256 Byte Platz); Bloecke, 0 = keine */
void     igd_irq_init(const PciDevice *d);              /* igd_irq.c: Bildwechsel-Interrupt einrichten */
void     igd_cursor_reapply(void);                     /* igd.c: Zeiger-Ebene nach einem Anschlusswechsel */
void     igd_edid_dump(const uint8_t *edid, int blocks, uint32_t limit_khz, const char *note); /* igd_mode.c */

/* Blitter im Dauerbetrieb (igd_blt.c): Bild-Updates aus dem Speicher des laufenden Programms */
int  igd_blt_init(void);          /* beim Start; 0 = Blitter uebernimmt */
int  igd_blt_on(void);
int  igd_blt_sync(void);          /* warten, bis alles kopiert ist (vor Schreiben der CPU); 0 = ok */
/* 1 = in Auftrag gegeben (wait = 1: ganzes Bild, kehrt erst zurueck, wenn es fertig ist), 0 = die CPU muss kopieren */
int  igd_blt_copy_user(uint32_t dst, const uint32_t *src, uint32_t pitch, int x, int y, int w, int h, int wait);
void igd_blt_report(void);        /* igdtest info */

/* Doppelpufferung (igd.c): A = Framebuffer der Firmware, B = zweiter Puffer im RAM */
extern int       igd_flip_ready;
extern uint32_t  igd_scr_w, igd_scr_h, igd_scr_stride;
extern uint32_t  igd_surf_a, igd_surf_b;
extern uint8_t  *igd_buf_a, *igd_buf_b;

#endif
