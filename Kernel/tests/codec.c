/* Selbsttests: SBC-Encoder (drivers/bt/sbc.c) - Vergleichswert auf dem Host erzeugt, wo dieselben Frames mit dem
 * Decoder der Referenz-Bibliothek (BlueZ libsbc) dekodiert wurden (Verstaerkung 1,000, Sinus mit 68 dB Abstand) */

#include "drivers/bt/sbc.h"
#include "lib/crypto.h"
#include "lib/string.h"
#include "tests/selftest.h"

void test_sbc(void)
{
    title("SBC-Encoder (Bluetooth-Audio)");
    static int16_t pcm[20 * 128 * 2];
    for (int i = 0; i < 20 * 128; i++) { /* Saegezahn links, Dreieck rechts */
        pcm[i * 2] = (int16_t)(((i * 37) % 2000 - 1000) * 16);
        int t = (i * 11) % 1000;
        pcm[i * 2 + 1] = (int16_t)((t < 500 ? t : 1000 - t) * 40 - 10000);
    }
    static SbcEnc e;
    sbcenc_init(&e, SBCENC_FREQ_48000, 53);
    static uint8_t out[20 * 128];
    uint32_t n = 0, first = 0;
    for (int f = 0; f < 20; f++) {
        uint32_t len = sbcenc_encode(&e, pcm + f * 256, out + n);
        if (!f)
            first = len;
        n += len;
    }
    check("Frame: 118 Byte (48 kHz, Stereo, 16 Bloecke, 8 Baender, Bitpool 53), Kopf 9C F9 35",
          first == 118 && sbcenc_frame_len(&e) == 118 && out[0] == 0x9C && out[1] == 0xF9 && out[2] == 53);
    uint8_t h[20];
    sha1(out, n, h);
    static const uint8_t want[20] = {0xd4, 0x84, 0xa3, 0x29, 0x2e, 0xf6, 0x1f, 0xce, 0x36, 0x50,
                                     0x91, 0xbc, 0x7a, 0x07, 0xc8, 0x3b, 0x0b, 0x5c, 0x43, 0x42};
    check("20 Frames bitgenau wie auf dem Host (dort vom Referenz-Decoder angenommen)", n == 2360 && memcmp(h, want, 20) == 0);
}
