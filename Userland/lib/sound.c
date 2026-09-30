#include "sound.h"
#include "libc.h"

/* siehe sound.h. Ohne FPU: Sinus nach Bhaskara in Ganzzahlen, Phase 0-65535 = eine Periode. */

#define SND_RATE 48000
#define SND_MAX  (SND_RATE / 2) /* ein Ton hoechstens 0,5 s */

static int   snd_ok;
static short snd_buf[SND_MAX];

int snd_open(void)
{
    snd_ok = sys_audio(0, SND_RATE, 1) == 0;
    return snd_ok ? 0 : -1;
}

static int snd_sin(unsigned phase)
{
    long long t = phase & 0x7FFF, p = 0x8000;
    int v = (int)(16 * t * (p - t) * 32767 / (5 * p * p - 4 * t * (p - t)));
    return (phase & 0x8000) ? -v : v;
}

static void snd_out(int frames)
{
    for (int done = 0; done < frames;) {
        s64 r = sys_audio(1, (u64)(snd_buf + done), (u64)(frames - done) * 2);
        if (r <= 0) {
            snd_ok = 0;
            return;
        }
        done += (int)(r / 2);
    }
}

void snd_tone(int hz, int ms, int volume)
{
    if (!snd_ok || ms <= 0)
        return;
    int frames = SND_RATE / 1000 * ms;
    if (frames > SND_MAX)
        frames = SND_MAX;
    int amp = 12000 * (volume < 0 ? 0 : volume > 100 ? 100 : volume) / 100;
    int attack = SND_RATE / 1000 * 3, release = SND_RATE / 1000 * 12; /* gegen Knacken */
    if (attack + release > frames)
        attack = release = frames / 2;
    unsigned phase = 0, step = (unsigned)(((unsigned long long)hz << 16) / SND_RATE);
    for (int i = 0; i < frames; i++) {
        int env = 256;
        if (i < attack)
            env = i * 256 / (attack ? attack : 1);
        else if (i >= frames - release)
            env = (frames - i) * 256 / (release ? release : 1);
        snd_buf[i] = (short)(snd_sin(phase) * amp / 32767 * env / 256);
        phase = (phase + step) & 0xFFFF;
    }
    snd_out(frames);
}

void snd_rest(int ms)
{
    if (!snd_ok || ms <= 0)
        return;
    int frames = SND_RATE / 1000 * ms;
    if (frames > SND_MAX)
        frames = SND_MAX;
    memset(snd_buf, 0, (u64)frames * 2);
    snd_out(frames);
}

void snd_close(void)
{
    if (snd_ok)
        sys_audio(3, 0, 0);
    snd_ok = 0;
}
