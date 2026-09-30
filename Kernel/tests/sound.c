/* Selbsttests: Ton (Intel HD Audio). In QEMU haengt eine intel-hda mit hda-output (Makefile, SOUND=none spielt
 * in Echtzeit ins Leere). Gezaehlt wird ueber die DMA-Position: alles geschrieben, alles gespielt, in Echtzeit. */

#include "arch/x86_64/apic.h"
#include "drivers/sound/hda.h"
#include "lib/kprintf.h"
#include "tests/selftest.h"

#define PID_A 4243
#define PID_B 4244
#define FRAMES (48000 * 3 / 10) /* 0,3 s bei 48 kHz */

static int16_t tone[FRAMES * 2];

void test_sound(void)
{
    title("Ton (HD Audio)");
    if (!hda_present()) {
        kprintf("  keine Soundkarte gefunden (QEMU mit SOUND=off gestartet?), uebersprungen\n");
        return;
    }
    for (int i = 0; i < FRAMES; i++) /* Rechteck mit 1 kHz, leise */
        tone[2 * i] = tone[2 * i + 1] = (int16_t)((i / 24) % 2 ? 4000 : -4000);

    check("Unbekannte Abtastrate wird abgelehnt", hda_open(PID_A, 12345, 2) == HDA_ERR_FORMAT);
    check("48 kHz Stereo laesst sich oeffnen", hda_open(PID_A, 48000, 2) == 0);
    check("Ein zweiter Prozess bekommt 'belegt'", hda_open(PID_B, 48000, 2) == HDA_ERR_BUSY);
    check("Schreiben ohne Oeffnen wird abgelehnt", hda_write(PID_B, tone, 64) == HDA_ERR_NOTOPEN);

    uint64_t t0 = time_ms();
    int64_t w = hda_write(PID_A, tone, sizeof(tone));
    hda_drain(PID_A);
    uint64_t ms = time_ms() - t0;
    uint64_t played = hda_played(PID_A);
    kprintf("  0,3 s Ton: %ld Bytes geschrieben, %lu gespielt, %lu ms\n", (long)w, (unsigned long)played,
            (unsigned long)ms);
    check("Alles geschrieben und vom Controller gelesen", w == (int64_t)sizeof(tone) && played == sizeof(tone));
    check("Die Wiedergabe laeuft in Echtzeit (0,3 s + Nachlauf)", ms >= 280 && ms <= 1500);
    hda_close(PID_A);

    /* ein zweites Mal hintereinander, andere Rate: der Controller muss wieder am Ringanfang beginnen */
    int ok = hda_open(PID_B, 44100, 2) == 0;
    w = hda_write(PID_B, tone, sizeof(tone) / 3);
    hda_drain(PID_B);
    ok = ok && w == (int64_t)(sizeof(tone) / 3) && hda_played(PID_B) == sizeof(tone) / 3;
    hda_close(PID_B);
    check("Zweite Wiedergabe (44,1 kHz) direkt danach", ok);
    check("Lautstaerke setzen und abfragen", hda_volume(55) == 55 && hda_volume(-1) == 55 && hda_volume(80) == 80);
}
