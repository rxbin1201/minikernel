/* Selbsttests: Ton (Intel HD Audio + Mischer). In QEMU haengt eine intel-hda mit hda-output (Makefile, SOUND=none
 * spielt in Echtzeit ins Leere). Gezaehlt wird, was der Mischer von jeder Stimme genommen hat. */

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
    title("Ton (HD Audio, Mischer)");
    if (!hda_present()) {
        kprintf("  keine Soundkarte gefunden (QEMU mit SOUND=off gestartet?), uebersprungen\n");
        return;
    }
    for (int i = 0; i < FRAMES; i++) /* Rechteck mit 1 kHz, leise */
        tone[2 * i] = tone[2 * i + 1] = (int16_t)((i / 24) % 2 ? 4000 : -4000);

    check("Unzulaessige Abtastrate wird abgelehnt", hda_open(PID_A, 4000, 2) == HDA_ERR_FORMAT);
    check("Schreiben ohne Oeffnen wird abgelehnt", hda_write(PID_A, tone, 64) == HDA_ERR_NOTOPEN);
    check("48 kHz Stereo laesst sich oeffnen", hda_open(PID_A, 48000, 2) == 0);

    uint64_t t0 = time_ms();
    int64_t w = hda_write(PID_A, tone, sizeof(tone));
    hda_drain(PID_A);
    uint64_t ms = time_ms() - t0, mixed = hda_played(PID_A);
    kprintf("  0,3 s Ton: %ld Bytes geschrieben, %lu gemischt, %lu ms\n", (long)w, (unsigned long)mixed, (unsigned long)ms);
    check("Alles geschrieben und gemischt", w == (int64_t)sizeof(tone) && mixed == sizeof(tone));
    /* QEMU liest der Ausgabe etwas voraus: gemessen ueber die DMA-Position also etwas unter 0,3 s */
    check("Die Wiedergabe laeuft in Echtzeit (etwa 0,3 s)", ms >= 150 && ms <= 1500);

    /* zwei Stimmen gleichzeitig, die zweite mit 22,05 kHz Mono (der Kernel rechnet um) */
    int ok = hda_open(PID_B, 22050, 1) == 0;
    check("Ein zweites Programm kann gleichzeitig spielen", ok);
    t0 = time_ms();
    int64_t wa = hda_write(PID_A, tone, sizeof(tone));
    int64_t wb = hda_write(PID_B, tone, 22050 * 2 * 3 / 10); /* 0,3 s Mono */
    hda_drain(PID_A);
    hda_drain(PID_B);
    ms = time_ms() - t0;
    uint64_t ma = hda_played(PID_A) - mixed, mb = hda_played(PID_B);
    kprintf("  gleichzeitig: A %lu Bytes gemischt, B (22,05 kHz Mono) %lu Bytes, %lu ms\n", (unsigned long)ma,
            (unsigned long)mb, (unsigned long)ms);
    check("Beide Stimmen vollstaendig gemischt", wa == (int64_t)sizeof(tone) && ma == sizeof(tone) &&
                                                 wb == 22050 * 2 * 3 / 10 && mb >= sizeof(tone) - 64 && mb <= sizeof(tone) + 64);
    check("Gleichzeitig statt nacheinander (unter 0,6 s + Vorlauf)", ms < 600 + 300);
    check("Lautstaerke der Stimme setzen", hda_voice_volume(PID_B, 50) == 50);
    hda_close(PID_A);
    hda_close(PID_B);
    check("Nach dem Schliessen keine Stimme mehr", hda_write(PID_A, tone, 64) == HDA_ERR_NOTOPEN);
    check("Gesamtlautstaerke setzen und abfragen", hda_volume(55) == 55 && hda_volume(-1) == 55 && hda_volume(80) == 80);
}
