#ifndef IGD_INTERNAL_H
#define IGD_INTERNAL_H

#include <stdint.h>
#include "drivers/gpu/igd.h"

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
void     igd_edid_dump(const uint8_t *edid, int blocks, uint32_t limit_khz, const char *note); /* igd_mode.c */

/* Doppelpufferung (igd.c): A = Framebuffer der Firmware, B = zweiter Puffer im RAM */
extern int       igd_flip_ready;
extern uint32_t  igd_scr_w, igd_scr_h, igd_scr_stride;
extern uint32_t  igd_surf_a, igd_surf_b;
extern uint8_t  *igd_buf_a, *igd_buf_b;

#endif
