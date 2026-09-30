/* MP3 (MPEG-1/2/2.5 Layer III, auch I/II) abspielen oder in WAV umwandeln. Dekodiert wird mit minimp3 von lieff
 * (https://github.com/lieff/minimp3, CC0 / gemeinfrei, unveraendert in Userland/include/minimp3.h). Die Datei wird
 * stueckweise gelesen; jeder Frame (1152 Abtastwerte) geht direkt an den Mischer des Kernels, der Rate und
 * Mono/Stereo selbst umrechnet. */

#include "libc.h"
#include "minimp3.h" /* nur die Deklarationen; die Implementierung steht in mp3dec.c */
#include "play.h"

#define INBUF (64 * 1024)

static unsigned char inbuf[INBUF];
static short         pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static mp3dec_t      dec;

int is_mp3(int fd)
{
    unsigned char h[4];
    int ok = sys_read(fd, h, 4) == 4 &&
             ((h[0] == 'I' && h[1] == 'D' && h[2] == '3') || (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0));
    sys_lseek(fd, 0, 0);
    return ok;
}

/* ID3v2-Tag am Anfang ueberspringen (Groesse "syncsafe": 4 x 7 Bit); Ergebnis: Beginn der Audiodaten */
static s64 skip_id3(int fd)
{
    unsigned char h[10];
    if (sys_read(fd, h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
        s64 size = (s64)(h[6] & 0x7F) << 21 | (s64)(h[7] & 0x7F) << 14 | (s64)(h[8] & 0x7F) << 7 | (h[9] & 0x7F);
        s64 start = 10 + size + ((h[5] & 0x10) ? 10 : 0); /* Fusszeile */
        sys_lseek(fd, start, 0);
        return start;
    }
    sys_lseek(fd, 0, 0);
    return 0;
}

static void wav_header(int out, unsigned rate, unsigned ch, unsigned data_bytes)
{
    unsigned char h[44];
    unsigned blk = ch * 2;
    memcpy(h, "RIFF", 4);
    unsigned v[] = {36 + data_bytes};
    memcpy(h + 4, v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    unsigned fmt[4] = {16, 1u | ch << 16, rate, rate * blk};
    memcpy(h + 16, fmt, 16);
    unsigned short b[2] = {(unsigned short)blk, 16};
    memcpy(h + 32, b, 4);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    write_all(out, h, 44);
}

int play_mp3(const char *file, int fd, const char *wav_out)
{
    s64 size = sys_lseek(fd, 0, 2);
    s64 start = skip_id3(fd), left_in_file = size - start;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    int fill = 0, open_hz = 0, open_ch = 0, out = -1, tty = sys_isatty(1) != 0, last_sec = -1;
    unsigned long long samples = 0, out_bytes = 0;
    unsigned est_total = 0; /* geschaetzte Laenge in s (aus der Bitrate des ersten Frames) */
    if (wav_out) {
        out = (int)sys_open(wav_out, O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) {
            fprintf(2, "play: %s kann nicht angelegt werden\n", wav_out);
            return 1;
        }
        wav_header(out, 44100, 2, 0); /* Platzhalter, am Ende mit den echten Werten ueberschrieben */
    }
    int eof = 0;
    for (;;) {
        if (!eof && fill < INBUF / 2) { /* nachlesen */
            s64 n = sys_read(fd, inbuf + fill, (u64)(INBUF - fill));
            if (n <= 0)
                eof = 1;
            else
                fill += (int)n;
        }
        if (!fill)
            break;
        int s = mp3dec_decode_frame(&dec, inbuf, fill, pcm, &info);
        if (!info.frame_bytes) { /* nichts mehr gefunden */
            if (eof)
                break;
            fill = 0; /* Muell: weiterlesen */
            continue;
        }
        memmove(inbuf, inbuf + info.frame_bytes, (u64)(fill - info.frame_bytes));
        fill -= info.frame_bytes;
        if (!s)
            continue; /* z.B. Xing/Info-Kopf oder noch kein Hauptdaten-Vorrat */
        if (info.hz != open_hz || info.channels != open_ch) {
            if (!open_hz) {
                printf("%s: MPEG Layer %d, %d Hz, %s, %d kbit/s", file, info.layer, info.hz,
                       info.channels == 1 ? "Mono" : "Stereo", info.bitrate_kbps);
                if (info.bitrate_kbps)
                    est_total = (unsigned)(left_in_file * 8 / 1000 / info.bitrate_kbps);
                if (est_total)
                    printf(", etwa %u:%02u", est_total / 60, est_total % 60);
                printf(wav_out ? " -> %s\n" : "\n", wav_out);
            }
            if (!wav_out) {
                if (open_hz)
                    sys_audio(2, 0, 0);
                s64 r = sys_audio(0, (u64)info.hz, (u64)info.channels);
                if (r < 0) {
                    fprintf(2, "play: Tonausgabe: Fehler %lld\n", (long long)r);
                    return 1;
                }
            }
            open_hz = info.hz;
            open_ch = info.channels;
        }
        u64 bytes = (u64)s * (u64)info.channels * 2;
        if (wav_out) {
            if (write_all(out, pcm, bytes) < 0) {
                fprintf(2, "play: Schreiben fehlgeschlagen\n");
                return 1;
            }
            out_bytes += bytes;
        } else {
            for (u64 done = 0; done < bytes;) {
                s64 r = sys_audio(1, (u64)((char *)pcm + done), bytes - done);
                if (r <= 0)
                    return 1;
                done += (u64)r;
            }
        }
        samples += (unsigned long long)s;
        int sec = (int)(samples / (unsigned long long)info.hz);
        if (tty && sec != last_sec) {
            last_sec = sec;
            printf("\r  %d:%02d", sec / 60, sec % 60);
            if (est_total)
                printf(" / %u:%02u", est_total / 60, est_total % 60);
        }
    }
    if (tty)
        printf("\n");
    if (!open_hz) {
        fprintf(2, "play: %s enthaelt keine MP3-Frames\n", file);
        return 1;
    }
    if (wav_out) {
        sys_lseek(out, 0, 0);
        wav_header(out, (unsigned)open_hz, (unsigned)open_ch, (unsigned)out_bytes);
        sys_close(out);
        printf("%llu Abtastwerte je Kanal geschrieben\n", samples);
    } else {
        sys_audio(2, 0, 0);
        sys_audio(3, 0, 0);
    }
    return 0;
}
