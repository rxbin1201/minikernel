#ifndef HDA_H
#define HDA_H

#include <stdint.h>

/* Intel High Definition Audio (PCI-Klasse 04.03): Controller, Codecs, Wiedergabe ueber einen Ausgabe-Stream.
 * Bis zu 8 Prozesse spielen gleichzeitig: jeder hat eine Stimme (hda_open ... hda_close), ein Mischer im Kernel
 * addiert sie; endet ein Prozess, gibt process.c seine Stimme frei. Programme liefern 16 Bit PCM, Mono oder Stereo,
 * 8-192 kHz; die Umrechnung auf die Mischrate (48 kHz) macht der Kernel. */

#define HDA_ERR_NODEV   (-1) /* kein Controller / kein Ausgang */
#define HDA_ERR_BUSY    (-2) /* alle Stimmen belegt */
#define HDA_ERR_FORMAT  (-3) /* Abtastrate / Kanalzahl nicht unterstuetzt */
#define HDA_ERR_NOTOPEN (-4)

void    hda_init(void);
int     hda_present(void);
int     hda_open(uint32_t pid, uint32_t rate, uint32_t channels);   /* 16 Bit, 1-2 Kanaele, 8000-192000 Hz */
int64_t hda_write(uint32_t pid, const void *buf, uint64_t len);     /* blockiert, bis alles im Puffer ist */
int     hda_drain(uint32_t pid);                                    /* wartet, bis alles gespielt ist */
void    hda_close(uint32_t pid);                                    /* Stimme frei (auch bei Prozessende) */
int     hda_volume(int percent);                                    /* Gesamtlautstaerke 0-100, < 0 = nur abfragen */
int     hda_voice_volume(uint32_t pid, int percent);                /* Lautstaerke der eigenen Stimme */
uint64_t hda_played(uint32_t pid);                                  /* seit hda_open gemischte Bytes (48 kHz Stereo) */

/* Ausgabe waehlen (Ton-Menue im Desktop): Ausgaenge der Soundkarte (Lautsprecher, Kopfhoerer, Line-Out) und - wenn
 * eine Soundbar/Kopfhoerer per Bluetooth bereit ist - Bluetooth. Gleiches Layout wie AudioOutput in user.h. */
#define HDA_OUT_BT 100
typedef struct {
    char    name[24];
    uint8_t kind;    /* 0 Soundkarte, 1 Bluetooth */
    uint8_t plugged; /* an der Buchse steckt etwas (Bluetooth: verbunden) */
    uint8_t on;      /* der Ton kommt hier heraus (bzw. kaeme, wenn gerade nichts spielt) */
    uint8_t pad;
    int32_t id;      /* fuer hda_output_select */
} HdaOutput;
int hda_output_info(unsigned i, HdaOutput *o); /* 0 oder -1 am Ende */
int hda_output_select(int sel);                /* -1 automatisch, id eines Ausgangs */
int hda_output_get(void);

#endif
