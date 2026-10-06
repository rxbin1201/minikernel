#ifndef SBC_H
#define SBC_H

#include <stdint.h>

/* SBC-Encoder (Bluetooth A2DP, Pflicht-Codec), nur Ganzzahlen: 8 Teilbaender, 16 Bloecke, Stereo (zwei getrennt
 * kodierte Kanaele), Bitzuteilung "Loudness". Ein Frame = 128 Stereo-Samples. */

#define SBCENC_FREQ_16000 0
#define SBCENC_FREQ_32000 1
#define SBCENC_FREQ_44100 2
#define SBCENC_FREQ_48000 3
#define SBCENC_SAMPLES    128 /* je Kanal und Frame */

typedef struct {
    int     freq, bitpool;
    int32_t x[2][80]; /* letzte 80 Eingangswerte je Kanal (Analysefilter) */
} SbcEnc;

void     sbcenc_init(SbcEnc *e, int freq, int bitpool);
uint32_t sbcenc_frame_len(const SbcEnc *e);
/* 128 Stereo-Samples (L, R abwechselnd) -> ein Frame; Ergebnis: Laenge in Byte */
uint32_t sbcenc_encode(SbcEnc *e, const int16_t *pcm, uint8_t *out);

#endif
