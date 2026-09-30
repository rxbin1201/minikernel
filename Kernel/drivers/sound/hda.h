#ifndef HDA_H
#define HDA_H

#include <stdint.h>

/* Intel High Definition Audio (PCI-Klasse 04.03): Controller, Codecs, Wiedergabe ueber einen Ausgabe-Stream.
 * Ein Prozess zur Zeit "besitzt" die Ausgabe (hda_open ... hda_close); endet er, gibt process.c sie frei.
 * Abgespielt wird immer 16 Bit PCM; umrechnen (8/24/32 Bit, Mono, andere Raten) ist Sache des Programms (play). */

#define HDA_ERR_NODEV   (-1) /* kein Controller / kein Ausgang */
#define HDA_ERR_BUSY    (-2) /* ein anderer Prozess spielt gerade */
#define HDA_ERR_FORMAT  (-3) /* diese Abtastrate / Kanalzahl kann der Codec nicht */
#define HDA_ERR_NOTOPEN (-4)

void    hda_init(void);
int     hda_present(void);
int     hda_open(uint32_t pid, uint32_t rate, uint32_t channels);   /* 16 Bit, 1-2 Kanaele */
int64_t hda_write(uint32_t pid, const void *buf, uint64_t len);     /* blockiert, bis alles im Puffer ist */
int     hda_drain(uint32_t pid);                                    /* wartet, bis alles gespielt ist */
void    hda_close(uint32_t pid);                                    /* sofort aus (auch bei Prozessende) */
int     hda_volume(int percent);                                    /* 0-100, < 0 = nur abfragen; Ergebnis: jetzt */
uint64_t hda_played(uint32_t pid);                                  /* seit hda_open gespielte Bytes */

#endif
