#include "gfx.h"
#include "malloc.h"
#include "minimp3.h" /* nur die Deklarationen; die Implementierung steht in mp3dec.c */
#include "ui.h"

/* music [ordner | datei]: Musik-Programm fuer den Desktop - MP3 und WAV eines Ordners.
 *   Oben der laufende Titel (Titel und Interpret aus ID3-Tags, sonst der Dateiname), Fortschritt (anklicken oder
 *   ziehen: springen), Zurueck / Abspielen-Pause / Weiter, Zufall und Wiederholen; darunter die Titelliste
 *   (Doppelklick oder Enter spielt), unten Ordner wechseln und die Lautstaerke dieses Programms.
 *   Tasten: Leertaste Abspielen/Pause, Pfeil links/rechts 10 s zurueck/vor, hoch/runter Auswahl, n/p naechster/voriger.
 * Gespielt wird ueber eine eigene Stimme im Mischer des Kernels (SYS_AUDIO), in kleinen Portionen (je etwa 0,25 s
 * Vorrat), damit die Oberflaeche nie wartet. Eine MP3-Datei ist eingeblendet (sys_mmap_file): Decoder und Kopf-
 * Durchlauf lesen direkt aus ihr, ohne Puffer und Umkopieren, Springen setzt nur die Position. Nebenbei liest das
 * Programm alle Frame-Koepfe (ohne zu dekodieren): daraus die genaue Laenge und eine Sprungtabelle, damit Springen
 * auch ohne feste Bitrate genau landet. Fuer die Pause merkt sich das Programm die zuletzt geschickten
 * Abtastwerte: was im Kernel noch nicht gespielt war, wird beim Fortsetzen noch einmal geschickt (keine Luecke). */

/* ---------------------------------------------------------------------------------------------------------------------
 * Titelliste
 * ------------------------------------------------------------------------------------------------------------------- */

enum { K_WAV = 1, K_MP3 = 2 };

typedef struct {
    char name[256];
    char title[96], artist[96];
    int  kind;
    int  secs;    /* Laenge in Sekunden, -1 = unbekannt */
    int  scanned; /* Tags und Laenge gelesen */
} Track;

static char   folder[PATH_MAX] = "/disk";
static Track *tracks;
static int    ntracks, sel = -1, scroll;

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static int track_path(int i, char *out, size_t max)
{
    int n = snprintf(out, max, "%s%s%s", folder, strcmp(folder, "/") == 0 ? "" : "/", tracks[i].name);
    return n < 0 || (size_t)n >= max ? -1 : 0;
}

static void load_folder(void)
{
    u_free(tracks);
    tracks = 0;
    ntracks = 0;
    int cap = 32;
    Track *t = u_malloc(sizeof(Track) * (u64)cap);
    DirEnt de;
    for (u64 i = 0; sys_readdir(folder, i, &de) == 0; i++) {
        if (de.is_dir || !(ends_with(de.name, ".mp3") || ends_with(de.name, ".wav")))
            continue;
        if (ntracks == cap) {
            Track *nt = u_malloc(sizeof(Track) * (u64)cap * 2);
            memcpy(nt, t, sizeof(Track) * (u64)ntracks);
            u_free(t);
            t = nt;
            cap *= 2;
        }
        Track *k = &t[ntracks++];
        memset(k, 0, sizeof(*k));
        snprintf(k->name, sizeof(k->name), "%s", de.name);
        k->kind = ends_with(de.name, ".mp3") ? K_MP3 : K_WAV;
        k->secs = -1;
        snprintf(k->title, sizeof(k->title), "%s", de.name); /* bis die Tags gelesen sind: Dateiname ohne Endung */
        char *dot = strrchr(k->title, '.');
        if (dot)
            *dot = 0;
    }
    for (int i = 1; i < ntracks; i++) { /* nach Dateinamen sortieren */
        Track x = t[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(t[j].name, x.name) > 0) {
            t[j + 1] = t[j];
            j--;
        }
        t[j + 1] = x;
    }
    tracks = t;
    sel = ntracks ? 0 : -1;
    scroll = 0;
}

/* ---------- Tags: ID3v2 (TIT2, TPE1), ID3v1 ---------- */

static void put_utf8(char *out, int max, int *n, unsigned cp)
{
    if (cp < 0x80) {
        if (*n + 1 < max) out[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*n + 2 < max) {
            out[(*n)++] = (char)(0xC0 | cp >> 6);
            out[(*n)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (*n + 3 < max) {
        out[(*n)++] = (char)(0xE0 | cp >> 12);
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

/* Textrahmen (Kodierung im ersten Byte) nach UTF-8 */
static void id3_text(const unsigned char *d, int len, char *out, int max)
{
    int n = 0;
    if (len < 1)
        return;
    int enc = d[0];
    d++;
    len--;
    if (enc == 1 || enc == 2) { /* UTF-16 mit BOM bzw. Big-Endian */
        int be = enc == 2, i = 0;
        if (enc == 1 && len >= 2) {
            be = d[0] == 0xFE && d[1] == 0xFF;
            i = 2;
        }
        for (; i + 1 < len; i += 2) {
            unsigned cp = be ? (unsigned)d[i] << 8 | d[i + 1] : (unsigned)d[i + 1] << 8 | d[i];
            if (!cp)
                break;
            if (cp >= 0xD800 && cp < 0xE000) /* ausserhalb der BMP: weglassen */
                continue;
            put_utf8(out, max, &n, cp);
        }
    } else {
        for (int i = 0; i < len && d[i]; i++) {
            if (enc == 3) {
                if (n + 1 < max)
                    out[n++] = (char)d[i];
            } else {
                put_utf8(out, max, &n, d[i]); /* ISO-8859-1 */
            }
        }
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }
static unsigned syncsafe(const unsigned char *p) { return (unsigned)(p[0] & 0x7F) << 21 | (unsigned)(p[1] & 0x7F) << 14 | (unsigned)(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }
static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* ID3v2 im Puffer: Titel/Interpret; Ergebnis: Laenge des Tags (0 = keiner) */
static unsigned id3v2(const unsigned char *b, int n, Track *t)
{
    if (n < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3')
        return 0;
    int ver = b[3];
    unsigned size = syncsafe(b + 6), total = 10 + size + ((b[5] & 0x10) ? 10 : 0);
    if (ver < 3 || ver > 4)
        return total;
    int p = 10;
    if (b[5] & 0x40) /* erweiterter Kopf */
        p += ver == 4 ? (int)syncsafe(b + 10) : (int)be32(b + 10) + 4;
    while (p + 10 <= n && p + 10 <= (int)total && b[p]) {
        unsigned fs = ver == 4 ? syncsafe(b + p + 4) : be32(b + p + 4);
        if (fs > (unsigned)(n - p - 10))
            break;
        const unsigned char *d = b + p + 10;
        if (!memcmp(b + p, "TIT2", 4))
            id3_text(d, (int)fs, t->title, sizeof(t->title));
        else if (!memcmp(b + p, "TPE1", 4))
            id3_text(d, (int)fs, t->artist, sizeof(t->artist));
        p += 10 + (int)fs;
    }
    return total;
}

static void id3v1_field(const unsigned char *d, int len, char *out, int max)
{
    int n = 0;
    for (int i = 0; i < len && d[i]; i++)
        put_utf8(out, max, &n, d[i]);
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static unsigned char scanbuf[65536];

static int wav_info(int fd, unsigned *rate, unsigned *ch, unsigned *bits, unsigned *fmt, u64 *data_bytes, s64 *data_start);

/* Tags und Laenge eines Titels (einmal, nach und nach im Leerlauf) */
static void scan_track(int i)
{
    Track *t = &tracks[i];
    t->scanned = 1;
    char p[PATH_MAX];
    if (track_path(i, p, sizeof(p)))
        return;
    s64 fd = sys_open(p, O_RDONLY);
    if (fd < 0)
        return;
    if (t->kind == K_WAV) {
        unsigned rate, ch, bits, fmt;
        u64 bytes;
        s64 start;
        if (wav_info((int)fd, &rate, &ch, &bits, &fmt, &bytes, &start) == 0 && rate && ch && bits)
            t->secs = (int)(bytes / (ch * (bits / 8)) / rate);
        sys_close((int)fd);
        return;
    }
    s64 size = sys_lseek((int)fd, 0, 2);
    sys_lseek((int)fd, 0, 0);
    s64 n = sys_read((int)fd, scanbuf, sizeof(scanbuf));
    if (n < 0)
        n = 0;
    char title[96] = "", artist[96] = "";
    Track tmp = *t;
    tmp.title[0] = tmp.artist[0] = 0;
    unsigned tag = id3v2(scanbuf, (int)n, &tmp);
    snprintf(title, sizeof(title), "%s", tmp.title);
    snprintf(artist, sizeof(artist), "%s", tmp.artist);
    if (!title[0] && size > 128) { /* ID3v1 am Ende */
        unsigned char v1[128];
        sys_lseek((int)fd, size - 128, 0);
        if (sys_read((int)fd, v1, 128) == 128 && !memcmp(v1, "TAG", 3)) {
            id3v1_field(v1 + 3, 30, title, sizeof(title));
            id3v1_field(v1 + 33, 30, artist, sizeof(artist));
        }
    }
    if (title[0])
        snprintf(t->title, sizeof(t->title), "%s", title);
    snprintf(t->artist, sizeof(t->artist), "%s", artist);
    /* Laenge: erster Frame hinter dem Tag; "Xing"/"Info" nennt die Zahl der Frames, sonst ueber die Bitrate */
    sys_lseek((int)fd, tag, 0);
    n = sys_read((int)fd, scanbuf, sizeof(scanbuf));
    if (n < 0)
        n = 0;
    sys_close((int)fd);
    static mp3dec_t d;
    static short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_init(&d);
    mp3dec_frame_info_t info;
    int off = 0;
    for (int tries = 0; tries < 8 && off < n; tries++) {
        mp3dec_decode_frame(&d, scanbuf + off, (int)n - off, pcm, &info);
        if (!info.frame_bytes)
            break;
        const unsigned char *fr = scanbuf + off + info.frame_offset;
        int fl = info.frame_bytes;
        for (int k = 0; k + 12 < fl; k++) /* VBR-Kopf? */
            if ((fr[k] == 'X' && fr[k + 1] == 'i' && fr[k + 2] == 'n' && fr[k + 3] == 'g') ||
                (fr[k] == 'I' && fr[k + 1] == 'n' && fr[k + 2] == 'f' && fr[k + 3] == 'o')) {
                unsigned flags = be32(fr + k + 4);
                if ((flags & 1) && info.hz) {
                    unsigned frames = be32(fr + k + 8), spf = info.layer == 1 ? 384 : (info.hz < 32000 && info.layer == 3) ? 576 : 1152;
                    t->secs = (int)((u64)frames * spf / (unsigned)info.hz);
                    return;
                }
                break;
            }
        if (info.bitrate_kbps) {
            t->secs = (int)((u64)(size - (s64)tag) * 8 / 1000 / (u64)info.bitrate_kbps);
            return;
        }
        off += info.frame_offset + info.frame_bytes;
    }
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Wiedergabe
 * ------------------------------------------------------------------------------------------------------------------- */

#define HIST   131072 /* zuletzt geschickte Frames (Stereo): fuer eine Pause ohne Luecke (mehr als der Vorrat) */
#define CHUNK  2048

static int    cur = -1, playing, shuffle, repeat;
static int    pfd = -1, pkind, rate;
static int    voice;          /* Stimme offen */
static u64    base_in;        /* Quell-Frames vor dem Oeffnen dieser Stimme */
static u64    in_written;     /* seitdem geschickt */
static u64    total_frames;   /* Laenge (WAV genau, MP3 geschaetzt) */
static u64    paused_pos;     /* Position beim Anhalten */
static u64    backlog;        /* beim Fortsetzen noch einmal zu schickende Frames */
static short  hist[HIST * 2];
static u64    hist_n;         /* Zahl der Frames, die je in hist kamen (Ringindex = hist_n % HIST) */
static int    volume = 80;
static char   status[160];
static s64    status_t;
static char   fmt_line[96];
static int    no_sound;

/* WAV */
static unsigned w_ch, w_bits, w_fmt, w_frame;
static s64      w_start;
static u64      w_left;
static unsigned char w_in[CHUNK * 8 * 4];
/* MP3 */
#define WINDOW 65536             /* so viel sieht der Decoder je Frame */
static mp3dec_t             dec;
static const unsigned char *mdata; /* eingeblendete Datei (m_size Bytes) */
static u64                  mpos;  /* naechster Frame */
static s64                  m_start, m_size;
static short         pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static short         out[MINIMP3_MAX_SAMPLES_PER_FRAME > CHUNK * 2 ? MINIMP3_MAX_SAMPLES_PER_FRAME : CHUNK * 2];
static u64           skip_smp;  /* nach einem Sprung: so viele Abtastwerte noch verwerfen */
static int           seeked;    /* in diesem Titel wurde gesprungen (Ende sagt dann nichts ueber die Laenge) */

/* Kopf-Durchlauf (MP3): Sprungpunkte je 16 Frames, genaue Laenge */
typedef struct {
    u64 off, smp;
} SeekPt;
static SeekPt       *seek_tab;
static int           nseek, capseek, scanning, scan_done;
static u64           scan_smp, scan_frames, spos; /* spos: Stelle des Durchlaufs in der Datei */
static mp3dec_t      sdec;

static void say(const char *m)
{
    snprintf(status, sizeof(status), "%s", m);
    status_t = sys_time_us();
}

static int wav_info(int fd, unsigned *r, unsigned *ch, unsigned *bits, unsigned *fmt, u64 *data_bytes, s64 *data_start)
{
    unsigned char h[12];
    if (sys_read(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return -1;
    int have_fmt = 0;
    for (int guard = 0; guard < 64; guard++) {
        unsigned char c[8];
        if (sys_read(fd, c, 8) != 8)
            return -2;
        unsigned len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char f[40];
            unsigned n = len < sizeof(f) ? len : sizeof(f);
            if (sys_read(fd, f, n) != (s64)n)
                return -2;
            if (len > n)
                sys_lseek(fd, (s64)(len - n), 1);
            if (len & 1)
                sys_lseek(fd, 1, 1);
            *fmt = le16(f);
            *ch = le16(f + 2);
            *r = le32(f + 4);
            *bits = le16(f + 14);
            if (*fmt == 0xFFFE && n >= 26)
                *fmt = le16(f + 24);
            have_fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt)
                return -3;
            *data_bytes = len;
            *data_start = sys_lseek(fd, 0, 1);
            return 0;
        } else {
            sys_lseek(fd, (s64)(len + (len & 1)), 1);
        }
    }
    return -2;
}

/* 32-Bit-Gleitkomma (als Bits) -> 16 Bit */
static int float_to_s16(unsigned bits)
{
    int sign = (bits >> 31) & 1, exp = (int)((bits >> 23) & 0xFF) - 127;
    unsigned mant = (bits & 0x7FFFFF) | 0x800000;
    if (((bits >> 23) & 0xFF) == 0 || exp < -16)
        return 0;
    int v = exp >= 0 ? 32767 : (int)(((long long)mant * 32767) >> (23 - exp));
    if (v > 32767)
        v = 32767;
    return sign ? -v : v;
}

static int wav_sample(const unsigned char *p)
{
    switch (w_bits) {
    case 8: return ((int)p[0] - 128) << 8;
    case 16: return (short)le16(p);
    case 24: return (int)(((unsigned)p[0] << 8 | (unsigned)p[1] << 16 | (unsigned)p[2] << 24)) >> 16;
    case 32: return w_fmt == 3 ? float_to_s16(le32(p)) : (int)le32(p) >> 16;
    default: return 0;
    }
}

static void close_voice(void)
{
    if (voice)
        sys_audio(3, 0, 0);
    voice = 0;
}

static int open_voice(void)
{
    s64 r = sys_audio(0, (u64)rate, 2);
    if (r < 0) {
        no_sound = r == ERR_NOSYS;
        say(r == ERR_NOSYS ? "Keine Soundkarte gefunden" : r == ERR_AGAIN ? "Zu viele Programme spielen gerade Ton"
                                                                         : "Dieses Format kann die Soundkarte nicht");
        return -1;
    }
    voice = 1;
    sys_audio(6, (u64)volume, 0);
    in_written = 0;
    return 0;
}

static u64 played_in(void) /* in dieser Stimme schon gespielte Quell-Frames */
{
    if (!voice)
        return 0;
    s64 mixed = sys_audio(5, 0, 0); /* Bytes bei 48 kHz Stereo */
    u64 p = mixed > 0 ? (u64)mixed / 4 * (u64)rate / 48000 : 0;
    return p < in_written ? p : in_written;
}

static u64 position(void)
{
    return playing ? base_in + played_in() : paused_pos;
}

static void send(const short *st, u64 frames, int remember) /* Stereo an die Stimme (und ins Gedaechtnis) */
{
    u64 bytes = frames * 4, done = 0;
    while (done < bytes) {
        s64 r = sys_audio(1, (u64)((const char *)st + done), bytes - done);
        if (r <= 0)
            break;
        done += (u64)r;
    }
    in_written += frames;
    if (remember)
        for (u64 i = 0; i < frames; i++) {
            u64 k = hist_n++ % HIST;
            hist[k * 2] = st[i * 2];
            hist[k * 2 + 1] = st[i * 2 + 1];
        }
}

static void scan_stop(void)
{
    scanning = 0;
}

static void stop_file(void)
{
    close_voice();
    if (pfd >= 0)
        sys_close(pfd);
    pfd = -1;
    if (mdata)
        sys_munmap((void *)mdata, (u64)m_size);
    mdata = 0;
    playing = 0;
    scan_stop();
}

/* Naechster Frame ab *pos in der eingeblendeten Datei (dec: Decoder, pcm 0 = nur Kopf). Liefert die Abtastwerte und
 * setzt *pos dahinter; info->frame_bytes = 0: keiner mehr bis zum Ende. */
static int next_frame(mp3dec_t *d, u64 *pos, short *out_pcm, mp3dec_frame_info_t *info)
{
    while (*pos < (u64)m_size) {
        u64 left = (u64)m_size - *pos;
        int s = mp3dec_decode_frame(d, mdata + *pos, (int)(left < WINDOW ? left : WINDOW), out_pcm, info);
        if (info->frame_bytes) {
            *pos += (u64)info->frame_bytes; /* schliesst uebersprungenen Muell vor dem Frame ein (frame_offset) */
            return s;
        }
        if (left <= WINDOW)
            break;
        *pos += WINDOW / 2; /* im Fenster kein Frame: dahinter weitersuchen */
    }
    *pos = (u64)m_size;
    info->frame_bytes = 0;
    return 0;
}

/* Ein Stueck des Kopf-Durchlaufs (hoechstens 64 KiB je Aufruf, damit Oberflaeche und Ton nicht warten) */
static int buffered_enough(void);
static void scan_step(void)
{
    if (!scanning || !buffered_enough())
        return;
    u64 stop = spos + WINDOW;
    while (spos < stop) {
        mp3dec_frame_info_t info;
        int smp = next_frame(&sdec, &spos, 0, &info);
        if (!info.frame_bytes)
            break;
        if (scan_frames % 16 == 0) {
                if (nseek == capseek) {
                    int c = capseek ? capseek * 2 : 256;
                    SeekPt *n = u_malloc(sizeof(SeekPt) * (u64)c);
                    if (nseek)
                        memcpy(n, seek_tab, sizeof(SeekPt) * (u64)nseek);
                    u_free(seek_tab);
                    seek_tab = n;
                    capseek = c;
                }
            seek_tab[nseek].off = spos - (u64)info.frame_bytes + (u64)info.frame_offset; /* Anfang des Frames */
            seek_tab[nseek++].smp = scan_smp;
        }
        scan_smp += (u64)smp;
        scan_frames++;
    }
    if (spos >= (u64)m_size) { /* fertig: jetzt ist die Laenge genau */
        scan_stop();
        scan_done = 1;
        if (scan_smp && rate) {
            total_frames = scan_smp;
            if (cur >= 0)
                tracks[cur].secs = (int)(scan_smp / (u64)rate);
        }
    }
}

static void scan_start(void)
{
    nseek = 0;
    scan_done = 0;
    scan_smp = scan_frames = 0;
    spos = (u64)m_start;
    mp3dec_init(&sdec);
    scanning = 1;
}

static void start_track(int i, int autoplay);

static void next_track(int dir, int from_end)
{
    if (!ntracks)
        return;
    int n;
    if (shuffle && ntracks > 1) {
        static u64 rng = 0x9E3779B97F4A7C15ULL;
        rng ^= (u64)sys_time_us();
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        n = (int)(rng % (u64)(ntracks - 1));
        if (n >= cur)
            n++;
    } else {
        n = cur + dir;
        if (n >= ntracks || n < 0) {
            if (!repeat && from_end) { /* Ende der Liste */
                stop_file();
                cur = -1;
                fmt_line[0] = 0;
                return;
            }
            n = (n + ntracks) % ntracks;
        }
    }
    start_track(n, 1);
}

/* Datei auf Position target (Quell-Frames) stellen */
static void seek_to(u64 target)
{
    if (pfd < 0)
        return;
    if (total_frames && target > total_frames)
        target = total_frames;
    if (pkind == K_WAV) {
        sys_lseek(pfd, w_start + (s64)(target * w_frame), 0);
        w_left = total_frames > target ? total_frames - target : 0;
    } else {
        s64 off;
        skip_smp = 0;
        if (scan_done && nseek) { /* genau: Sprungpunkt etwas vor dem Ziel (Bit-Reservoir), den Rest verwerfen */
            int k = nseek - 1;
            while (k > 0 && seek_tab[k].smp + 10 * 1152 > target) /* Vorlauf: bis zu 10 Frames ohne Ton */
                k--;
            off = (s64)seek_tab[k].off;
            skip_smp = target > seek_tab[k].smp ? target - seek_tab[k].smp : 0;
        } else { /* Durchlauf noch nicht fertig: anteilig nach Bytes */
            off = m_start + (total_frames ? (s64)((u64)(m_size - m_start) * target / total_frames) : 0);
        }
        mpos = (u64)off;
        mp3dec_init(&dec);
    }
    seeked = 1;
    hist_n = 0;
    backlog = 0;
    base_in = target;
    paused_pos = target;
    if (playing) {
        close_voice();
        if (open_voice() != 0)
            playing = 0;
    } else {
        in_written = 0;
    }
}

static void start_track(int i, int autoplay)
{
    stop_file();
    cur = i;
    sel = i;
    char p[PATH_MAX];
    if (i < 0 || i >= ntracks || track_path(i, p, sizeof(p)) || (pfd = (int)sys_open(p, O_RDONLY)) < 0) {
        say("Datei nicht lesbar");
        return;
    }
    if (!tracks[i].scanned) {
        scan_track(i);
        sys_lseek(pfd, 0, 0);
    }
    pkind = tracks[i].kind;
    base_in = in_written = paused_pos = backlog = hist_n = skip_smp = 0;
    seeked = 0;
    if (pkind == K_WAV) {
        unsigned r;
        u64 bytes;
        if (wav_info(pfd, &r, &w_ch, &w_bits, &w_fmt, &bytes, &w_start) != 0 || !w_ch || !r || (w_fmt != 1 && w_fmt != 3) ||
            (w_bits != 8 && w_bits != 16 && w_bits != 24 && w_bits != 32) || r < 8000 || r > 192000) {
            say("Diese WAV-Datei wird nicht unterst\xC3\xBCtzt");
            stop_file();
            return;
        }
        rate = (int)r;
        w_frame = w_ch * (w_bits / 8);
        total_frames = w_left = bytes / w_frame;
        snprintf(fmt_line, sizeof(fmt_line), "WAV  \xC2\xB7  %u,%u kHz  \xC2\xB7  %u Bit  \xC2\xB7  %s", r / 1000, r % 1000 / 100,
                 w_bits, w_ch == 1 ? "Mono" : "Stereo");
    } else {
        m_size = sys_lseek(pfd, 0, 2);
        s64 map = m_size > 0 ? sys_mmap_file(pfd, (u64)m_size, 0, 0) : ERR_INVAL;
        if (map < 0) {
            say("Datei l\xC3\xA4sst sich nicht einblenden");
            stop_file();
            return;
        }
        mdata = (const unsigned char *)map;
        m_start = 0;
        if (m_size >= 10 && mdata[0] == 'I' && mdata[1] == 'D' && mdata[2] == '3')
            m_start = 10 + (s64)syncsafe(mdata + 6) + ((mdata[5] & 0x10) ? 10 : 0);
        if (m_start >= m_size)
            m_start = 0;
        mpos = (u64)m_start;
        mp3dec_init(&dec);
        /* ersten Frame suchen: Rate und Format (nur Kopf ansehen, die Position bleibt) */
        mp3dec_frame_info_t info;
        info.hz = 0;
        u64 look = mpos;
        for (int tries = 0; tries < 64 && !info.hz; tries++) {
            next_frame(&dec, &look, 0, &info);
            if (!info.frame_bytes)
                break;
        }
        if (!info.hz) {
            say("Keine MP3-Daten gefunden");
            stop_file();
            return;
        }
        rate = info.hz;
        total_frames = tracks[i].secs > 0 ? (u64)tracks[i].secs * (u64)rate : 0;
        snprintf(fmt_line, sizeof(fmt_line), "MP3  \xC2\xB7  %d,%d kHz  \xC2\xB7  %d kbit/s  \xC2\xB7  %s", info.hz / 1000,
                 info.hz % 1000 / 100, info.bitrate_kbps, info.channels == 1 ? "Mono" : "Stereo");
        mp3dec_init(&dec); /* das Ansehen hat nichts verbraucht; Decoder frisch fuer den Anfang */
        scan_start();      /* nebenbei: genaue Laenge und Sprungtabelle */
    }
    status[0] = 0;
    if (autoplay && open_voice() == 0)
        playing = 1;
}

static void pause_play(void)
{
    if (!playing)
        return;
    u64 pl = played_in();
    u64 unplayed = in_written - pl;
    if (unplayed > HIST)
        unplayed = HIST;
    if (unplayed > hist_n)
        unplayed = hist_n;
    paused_pos = base_in + in_written - unplayed;
    backlog = unplayed;
    close_voice();
    playing = 0;
}

static void resume_play(void)
{
    if (playing)
        return;
    if (pfd < 0) {
        start_track(cur >= 0 ? cur : sel >= 0 ? sel : 0, 1);
        return;
    }
    if (open_voice() != 0)
        return;
    base_in = paused_pos;
    playing = 1;
    if (backlog) { /* was beim Anhalten noch im Kernel lag, noch einmal schicken */
        static short tmp[CHUNK * 2];
        u64 start = hist_n - backlog;
        while (backlog) {
            u64 n = backlog < CHUNK ? backlog : CHUNK;
            for (u64 k = 0; k < n; k++) {
                u64 h = (start + k) % HIST;
                tmp[k * 2] = hist[h * 2];
                tmp[k * 2 + 1] = hist[h * 2 + 1];
            }
            send(tmp, n, 0);
            start += n;
            backlog -= n;
        }
    }
}

/* Eine Portion dekodieren und schicken; 0 = Titel zu Ende */
static int produce(void)
{
    if (pkind == K_WAV) {
        if (!w_left)
            return 0;
        u64 want = w_left < CHUNK ? w_left : CHUNK;
        s64 got = sys_read(pfd, w_in, want * w_frame);
        if (got <= 0)
            return 0;
        u64 frames = (u64)got / w_frame;
        w_left -= frames;
        for (u64 k = 0; k < frames; k++) {
            const unsigned char *p = w_in + k * w_frame;
            int l = wav_sample(p), r = w_ch > 1 ? wav_sample(p + w_bits / 8) : l;
            out[k * 2] = (short)l;
            out[k * 2 + 1] = (short)r;
        }
        send(out, frames, 1);
        return 1;
    }
    for (;;) {
        mp3dec_frame_info_t info;
        int s = next_frame(&dec, &mpos, pcm, &info);
        if (!info.frame_bytes)
            return 0; /* Ende der Datei */
        if (!s) { /* noch kein Hauptdaten-Vorrat (nach einem Sprung): der Frame zaehlt trotzdem fuer das Ziel */
            if (skip_smp) {
                u64 spf = info.layer == 1 ? 384 : (info.hz < 32000 && info.layer == 3) ? 576 : 1152;
                skip_smp = skip_smp > spf ? skip_smp - spf : 0;
            }
            continue;
        }
        if (skip_smp) { /* nach einem genauen Sprung: bis zum Ziel verwerfen */
            if (skip_smp >= (u64)s) {
                skip_smp -= (u64)s;
                continue;
            }
            int k = (int)skip_smp;
            skip_smp = 0;
            int ch = info.channels;
            memmove(pcm, pcm + k * ch, (u64)(s - k) * (u64)ch * 2);
            s -= k;
        }
        if (info.hz != rate) { /* Rate wechselt mitten im Titel (selten): neue Stimme */
            base_in += in_written;
            close_voice();
            rate = info.hz;
            if (open_voice() != 0)
                return 0;
        }
        if (info.channels == 2) {
            send(pcm, (u64)s, 1);
        } else {
            for (int k = 0; k < s; k++)
                out[k * 2] = out[k * 2 + 1] = pcm[k];
            send(out, (u64)s, 1);
        }
        return 1;
    }
}

/* Vorrat in der Stimme auf etwa 1 s halten (blockiert nie lange): so viel darf das Programm stocken - etwa wenn eine
 * Datei zum ersten Mal von einem langsamen Datentraeger gelesen wird -, ohne dass eine Luecke im Ton entsteht */
#define FEED_AHEAD(r) ((u64)(r))      /* 1 s */
#define SCAN_ABOVE(r) ((u64)(r) / 2)  /* Nebenarbeit (Kopf-Durchlauf, Tags) nur mit mindestens 0,5 s Vorrat */

static int buffered_enough(void)
{
    return !playing || in_written - played_in() >= SCAN_ABOVE(rate);
}

static void feed(void)
{
    if (!playing)
        return;
    for (int guard = 0; guard < 64 && in_written - played_in() < FEED_AHEAD(rate); guard++) {
        if (!produce()) {
            /* Titel zu Ende: erst ausspielen lassen, dann weiter */
            if (in_written - played_in() > (u64)rate / 50)
                return;
            u64 real = base_in + in_written;
            if (cur >= 0 && pkind == K_MP3 && real && !seeked && !scan_done) /* ohne Sprung: jetzt ist die Laenge bekannt */
                tracks[cur].secs = (int)(real / (u64)rate);
            next_track(1, 1);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Aussehen
 * ------------------------------------------------------------------------------------------------------------------- */

#define C_MUSIC 0xFA2D48

static int head_h(void) { return U(178); }
static int foot_h(void) { return U(40); }
static int list_y(void) { return head_h() + U(26); }
static int visible(void) { int v = (gfx_screen.h - list_y() - foot_h()) / ROW_H; return v < 1 ? 1 : v; }
static int text_x(void) { return U(166); }
static int bar_x0(void) { return text_x(); }
static int bar_x1(void) { return gfx_screen.w - U(26); }
static int bar_y(void) { return U(96); }
static int ctl_y(void) { return U(140); }
static int ctl_cx(void) { return (bar_x0() + bar_x1()) / 2; }
static int vol_x0(void) { return gfx_screen.w - U(170); }
static int vol_x1(void) { return gfx_screen.w - U(24); }

/* Dreieck mit weichen Kanten (4 Abtastzeilen je Pixelzeile) */
static void tri(Surface *s, float x0, float y0, float x1, float y1, float x2, float y2, u32 c)
{
    float minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    float miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int bx = (int)minx, bw = (int)maxx - bx + 2;
    if (bw > 256)
        return;
    float ex[3][4] = {{x0, y0, x1, y1}, {x1, y1, x2, y2}, {x2, y2, x0, y0}};
    for (int py = (int)miny; py <= (int)maxy; py++) {
        float cov[260];
        for (int k = 0; k < bw; k++)
            cov[k] = 0;
        for (int sub = 0; sub < 4; sub++) {
            float sy = py + (sub + 0.5f) / 4, xl = 1e9f, xr = -1e9f;
            for (int e = 0; e < 3; e++) {
                float ax = ex[e][0], ay = ex[e][1], bx2 = ex[e][2], by = ex[e][3];
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                    float x = ax + (sy - ay) * (bx2 - ax) / (by - ay);
                    if (x < xl) xl = x;
                    if (x > xr) xr = x;
                }
            }
            if (xl > xr)
                continue;
            for (int k = 0; k < bw; k++) {
                float a = bx + k, b = a + 1, l = xl > a ? xl : a, r = xr < b ? xr : b;
                if (r > l)
                    cov[k] += (r - l) / 4;
            }
        }
        for (int k = 0; k < bw; k++)
            if (cov[k] > 0.01f)
                gfx_blend_fill(s, bx + k, py, 1, 1, c, (int)(cov[k] * 255));
    }
}

static void fmt_time(u64 secs, char *out, size_t max)
{
    snprintf(out, max, "%llu:%02llu", (unsigned long long)(secs / 60), (unsigned long long)(secs % 60));
}

static void note(Surface *s, float x, float y, float sz, u32 c) /* Notenzeichen (zwei Noten mit Balken) */
{
    gfx_disc(s, x + sz * 0.28f, y + sz * 0.78f, sz * 0.13f, c, 255);
    gfx_disc(s, x + sz * 0.70f, y + sz * 0.70f, sz * 0.13f, c, 255);
    gfx_capsule(s, x + sz * 0.39f, y + sz * 0.78f, x + sz * 0.39f, y + sz * 0.22f, sz * 0.06f, c, 255);
    gfx_capsule(s, x + sz * 0.81f, y + sz * 0.70f, x + sz * 0.81f, y + sz * 0.14f, sz * 0.06f, c, 255);
    gfx_capsule(s, x + sz * 0.39f, y + sz * 0.24f, x + sz * 0.81f, y + sz * 0.16f, sz * 0.10f, c, 255);
}

static int hover = -1, drag_bar, drag_vol, bar_preview = -1;

enum { B_PREV = 1, B_PLAY, B_NEXT, B_SHUF, B_REP, B_FOLDER };

static void btn_rect(int b, int *x, int *y, int *w, int *h)
{
    int cx = ctl_cx(), cy = ctl_y();
    *h = U(36);
    *y = cy - *h / 2;
    switch (b) {
    case B_PREV: *w = U(40); *x = cx - U(70) - *w / 2; break;
    case B_PLAY: *w = U(48); *h = U(48); *y = cy - U(24); *x = cx - U(24); break;
    case B_NEXT: *w = U(40); *x = cx + U(70) - *w / 2; break;
    case B_SHUF: *w = U(78); *x = cx - U(190) - *w / 2; break;
    case B_REP: *w = U(110); *x = cx + U(190) - *w / 2; break;
    default:
        *w = text_width(font_ui, FS, "Ordner \xE2\x80\xA6") + U(22);
        *h = U(26);
        *x = U(12);
        *y = gfx_screen.h - foot_h() + (foot_h() - *h) / 2;
    }
}

static int btn_at(int px, int py)
{
    for (int b = B_PREV; b <= B_FOLDER; b++) {
        int x, y, w, h;
        btn_rect(b, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h)
            return b;
    }
    return -1;
}

static void pill(Surface *s, int b, const char *label, int on)
{
    int x, y, w, h;
    btn_rect(b, &x, &y, &w, &h);
    y += U(6);
    h -= U(12);
    gfx_round_rect(s, x, y, w, h, h / 2, on ? C_MUSIC : hover == b ? 0xE4E4EA : 0xFFFFFF, 255);
    if (!on)
        gfx_round_frame(s, x, y, w, h, h / 2, 0x000000, 30);
    text_draw(s, font_ui, FS_SMALL, x + (w - text_width(font_ui, FS_SMALL, label)) / 2, y + (h - text_height(font_ui, FS_SMALL)) / 2,
              label, on ? 0xFFFFFF : C_TEXT);
}

/* Prompt: anderer Ordner */
static int  prompt;
static char field[256];
static int  flen;

static void draw(void)
{
    Surface *s = &gfx_screen;
    int W = s->w, H = s->h;
    gfx_fill(s, 0, 0, W, H, C_WINDOW);

    /* --- jetzt laeuft --- */
    gfx_gradient(s, 0, 0, W, head_h(), 0xFBFBFD, 0xF1F1F5);
    gfx_fill(s, 0, head_h() - 1, W, 1, 0xE2E2E6);
    int ax = U(22), ay = U(24), as = U(124);
    gfx_shadow(s, ax, ay + U(3), as, as, U(14), U(16), 60);
    gfx_round_rect_grad(s, ax, ay, as, as, U(14), 0xFF6B81, 0xB43CF0, 255);
    note(s, ax + as * 0.2f, ay + as * 0.2f, as * 0.6f, 0xFFFFFF);
    int tx = text_x();
    const char *title = cur >= 0 ? tracks[cur].title : "Nichts ausgew\xC3\xA4hlt";
    const char *artist = cur >= 0 ? (tracks[cur].artist[0] ? tracks[cur].artist : "Unbekannter Interpret") : "Doppelklick auf einen Titel spielt ihn";
    gfx_set_clip(tx, 0, W - tx - U(20), head_h());
    text_draw(s, font_bold, U(19), tx, U(26), title, C_TEXT);
    text_draw(s, font_ui, U(14), tx, U(26) + text_height(font_bold, U(19)), artist, C_TEXT2);
    if (cur >= 0)
        text_draw(s, font_ui, FS_SMALL, tx, U(26) + text_height(font_bold, U(19)) + text_height(font_ui, U(14)), fmt_line, 0x9A9AA0);
    gfx_no_clip();

    /* Fortschritt */
    u64 pos = bar_preview >= 0 ? (u64)bar_preview : position();
    u64 tot = total_frames;
    int bx0 = bar_x0(), bx1 = bar_x1(), by = bar_y(), bh = U(6);
    gfx_round_rect(s, bx0, by, bx1 - bx0, bh, bh / 2, 0xDADAE0, 255);
    if (cur >= 0 && tot && rate) {
        u64 p = pos > tot ? tot : pos;
        int fx = bx0 + (int)((u64)(bx1 - bx0) * p / tot);
        if (fx > bx0 + 1)
            gfx_round_rect(s, bx0, by, fx - bx0, bh, bh / 2, C_MUSIC, 255);
        gfx_disc(s, (float)fx, by + bh * 0.5f, U(7), 0xFFFFFF, 255);
        gfx_ring(s, (float)fx, by + bh * 0.5f, U(7), 1.2f, 0xC8C8CE, 255);
        char a[16], b[16], c2[20];
        fmt_time(p / (u64)rate, a, sizeof(a));
        fmt_time((tot - p) / (u64)rate, b, sizeof(b));
        snprintf(c2, sizeof(c2), "-%s", b);
        text_draw(s, font_ui, FS_SMALL, bx0, by + U(12), a, C_TEXT2);
        text_draw(s, font_ui, FS_SMALL, bx1 - text_width(font_ui, FS_SMALL, c2), by + U(12), c2, C_TEXT2);
    } else if (cur >= 0 && rate) {
        char a[16];
        fmt_time(pos / (u64)rate, a, sizeof(a));
        text_draw(s, font_ui, FS_SMALL, bx0, by + U(12), a, C_TEXT2);
    }

    /* Knoepfe */
    int cx = ctl_cx(), cy = ctl_y();
    float u = U(1);
    u32 ic = C_TEXT;
    for (int dir = -1; dir <= 1; dir += 2) { /* zurueck / weiter: zwei Dreiecke */
        float bxc = cx + dir * U(70);
        if (hover == (dir < 0 ? B_PREV : B_NEXT))
            gfx_disc(s, bxc, (float)cy, U(19), 0xE6E6EC, 255);
        for (int k = 0; k < 2; k++) {
            float o = (k == 0 ? -9 : 0) * u * dir;
            if (dir > 0)
                tri(s, bxc + o, cy - 8 * u, bxc + o, cy + 8 * u, bxc + o + 10 * u, (float)cy, ic);
            else
                tri(s, bxc - o, cy - 8 * u, bxc - o, cy + 8 * u, bxc - o - 10 * u, (float)cy, ic);
        }
    }
    gfx_disc(s, (float)cx, (float)cy, U(24), hover == B_PLAY ? gfx_mix(C_MUSIC, 0x000000, 30) : C_MUSIC, 255);
    if (playing) {
        gfx_round_rect(s, cx - (int)(7 * u), cy - (int)(9 * u), (int)(5 * u), (int)(18 * u), (int)(2 * u), 0xFFFFFF, 255);
        gfx_round_rect(s, cx + (int)(2 * u), cy - (int)(9 * u), (int)(5 * u), (int)(18 * u), (int)(2 * u), 0xFFFFFF, 255);
    } else {
        tri(s, cx - 6 * u, cy - 10 * u, cx - 6 * u, cy + 10 * u, cx + 11 * u, (float)cy, 0xFFFFFF);
    }
    pill(s, B_SHUF, "Zufall", shuffle);
    pill(s, B_REP, "Wiederholen", repeat);

    /* --- Titelliste --- */
    int ly = list_y(), hy = head_h();
    gfx_fill(s, 0, hy, W, U(26), 0xFAFAFA);
    gfx_fill(s, 0, ly - 1, W, 1, 0xE5E5E5);
    int ty = hy + (U(26) - text_height(font_bold, FS_SMALL)) / 2;
    int c_title = U(54), c_artist = W * 55 / 100, c_dur = W - U(70);
    text_draw(s, font_bold, FS_SMALL, U(18), ty, "#", C_TEXT2);
    text_draw(s, font_bold, FS_SMALL, c_title, ty, "Titel", C_TEXT2);
    text_draw(s, font_bold, FS_SMALL, c_artist, ty, "Interpret", C_TEXT2);
    text_draw(s, font_bold, FS_SMALL, c_dur, ty, "Dauer", C_TEXT2);
    int vis = visible() + 1, lh = H - ly - foot_h();
    gfx_set_clip(0, ly, W, lh);
    for (int r = 0; r < vis && scroll + r < ntracks; r++) {
        int i = scroll + r, ry = ly + r * ROW_H;
        Track *t = &tracks[i];
        if (i == sel)
            gfx_round_rect(s, U(6), ry + 1, W - U(12), ROW_H - 2, U(5), 0xE9E9EE, 255);
        else if (r % 2)
            gfx_fill(s, 0, ry, W, ROW_H, 0xF8F8FA);
        int iy = ry + (ROW_H - text_height(font_ui, FS)) / 2, me = i == cur;
        u32 tc = me ? C_MUSIC : C_TEXT;
        if (me && playing) { /* kleine springende Balken */
            s64 now = sys_time_us() / 1000;
            for (int k = 0; k < 3; k++) {
                int hgt = U(4) + (int)((now / (110 + 37 * k) + k * 3) % 7) * U(1) * 2;
                gfx_fill(s, U(16) + k * U(5), ry + ROW_H / 2 + U(7) - hgt, U(3), hgt, C_MUSIC);
            }
        } else {
            char num[8];
            snprintf(num, sizeof(num), "%d", i + 1);
            text_draw(s, font_ui, FS, U(18), iy, num, me ? C_MUSIC : 0xA0A0A6);
        }
        gfx_set_clip(c_title, ly, c_artist - c_title - U(10), lh);
        text_draw(s, me ? font_bold : font_ui, FS, c_title, iy, t->title, tc);
        gfx_set_clip(c_artist, ly, c_dur - c_artist - U(10), lh);
        text_draw(s, font_ui, FS, c_artist, iy, t->artist[0] ? t->artist : "\xE2\x80\x94", C_TEXT2);
        gfx_set_clip(0, ly, W, lh);
        char d[16] = "";
        if (t->secs >= 0)
            fmt_time((u64)t->secs, d, sizeof(d));
        text_draw(s, font_ui, FS, c_dur, iy, d, C_TEXT2);
    }
    if (!ntracks)
        text_draw(s, font_ui, FS, (W - text_width(font_ui, FS, "Keine MP3- oder WAV-Dateien in diesem Ordner")) / 2, ly + U(30),
                  "Keine MP3- oder WAV-Dateien in diesem Ordner", 0xAEAEB2);
    gfx_no_clip();
    ui_scrollbar(s, W, ly, lh, ntracks, visible(), scroll);

    /* --- unten: Ordner, Lautstaerke --- */
    int fy = H - foot_h();
    gfx_fill(s, 0, fy, W, foot_h(), 0xF7F7F9);
    gfx_fill(s, 0, fy, W, 1, 0xE2E2E6);
    int fx, fyb, fw, fh;
    btn_rect(B_FOLDER, &fx, &fyb, &fw, &fh);
    gfx_round_rect(s, fx, fyb, fw, fh, U(6), hover == B_FOLDER ? 0xE4E4EA : 0xFFFFFF, 255);
    gfx_round_frame(s, fx, fyb, fw, fh, U(6), 0x000000, 30);
    text_draw(s, font_ui, FS, fx + U(11), fyb + (fh - text_height(font_ui, FS)) / 2, "Ordner \xE2\x80\xA6", C_TEXT);
    char info[300];
    if (status[0] && sys_time_us() - status_t < 6000000)
        snprintf(info, sizeof(info), "%s", status);
    else
        snprintf(info, sizeof(info), "%s  \xC2\xB7  %d %s", folder, ntracks, ntracks == 1 ? "Titel" : "Titel");
    gfx_set_clip(fx + fw + U(10), fy, vol_x0() - U(40) - fx - fw - U(10), foot_h());
    text_draw(s, font_ui, FS_SMALL, fx + fw + U(12), fy + (foot_h() - text_height(font_ui, FS_SMALL)) / 2, info, C_TEXT2);
    gfx_no_clip();
    /* Lautstaerke: Lautsprecher + Regler */
    float sx = vol_x0() - U(22), sy = fy + foot_h() * 0.5f;
    gfx_round_rect(s, (int)(sx - 5 * u), (int)(sy - 3 * u), (int)(4 * u), (int)(6 * u), 1, 0x6E6E73, 255);
    tri(s, sx - 2 * u, sy - 3 * u, sx - 2 * u, sy + 3 * u, sx + 4 * u, sy, 0x6E6E73);
    tri(s, sx - 2 * u, sy - 3 * u, sx + 4 * u, sy - 7 * u, sx + 4 * u, sy + 7 * u, 0x6E6E73);
    tri(s, sx - 2 * u, sy - 3 * u, sx + 4 * u, sy + 7 * u, sx - 2 * u, sy + 3 * u, 0x6E6E73);
    int vx0 = vol_x0(), vx1 = vol_x1(), vy = (int)sy - U(2), vf = vx0 + (vx1 - vx0) * volume / 100;
    gfx_round_rect(s, vx0, vy, vx1 - vx0, U(4), U(2), 0xD4D4DA, 255);
    gfx_round_rect(s, vx0, vy, vf - vx0 > U(4) ? vf - vx0 : U(4), U(4), U(2), 0x8E8E93, 255);
    gfx_disc(s, (float)vf, vy + U(2), U(7), 0xFFFFFF, 255);
    gfx_ring(s, (float)vf, vy + U(2), U(7), 1.2f, 0xC0C0C6, 255);

    /* Prompt: Ordner waehlen */
    if (prompt) {
        int pw = U(440) < W - U(20) ? U(440) : W - U(20), ph = U(150), px = (W - pw) / 2, py = U(60);
        gfx_blend_fill(s, 0, 0, W, H, 0x000000, 40);
        gfx_shadow(s, px, py + U(4), pw, ph, U(12), U(24), 80);
        gfx_round_rect(s, px, py, pw, ph, U(12), 0xF6F6F8, 255);
        gfx_round_frame(s, px, py, pw, ph, U(12), 0x000000, 30);
        text_draw(s, font_bold, U(15), px + U(20), py + U(16), "Ordner mit Musik", C_TEXT);
        int fyy = py + U(16) + text_height(font_bold, U(15)) + U(8), fww = pw - U(40), fhh = U(28);
        gfx_round_rect(s, px + U(20), fyy, fww, fhh, U(6), 0xFFFFFF, 255);
        gfx_round_frame(s, px + U(20), fyy, fww, fhh, U(6), C_ACCENT, 200);
        int tw = text_width(font_ui, FS, field), tx2 = px + U(30);
        gfx_set_clip(px + U(24), fyy, fww - U(8), fhh);
        text_draw(s, font_ui, FS, tx2, fyy + (fhh - text_height(font_ui, FS)) / 2, field, C_TEXT);
        gfx_fill(s, tx2 + tw + 1, fyy + (fhh - text_height(font_ui, FS)) / 2, 2, text_height(font_ui, FS), C_ACCENT);
        gfx_no_clip();
        text_draw(s, font_ui, FS_SMALL, px + U(20), fyy + fhh + U(8), "Enter: \xC3\xB6" "ffnen  \xC2\xB7  Esc: abbrechen", C_TEXT2);
    }
    gfx_present_all();
}

/* ---------------------------------------------------------------------------------------------------------------------
 * Eingaben
 * ------------------------------------------------------------------------------------------------------------------- */

static void keep_visible(void)
{
    int v = visible();
    if (sel >= 0 && sel < scroll) scroll = sel;
    if (sel >= scroll + v) scroll = sel - v + 1;
    if (scroll > ntracks - v) scroll = ntracks - v;
    if (scroll < 0) scroll = 0;
}

static void toggle(void)
{
    if (playing)
        pause_play();
    else
        resume_play();
}

static u64 bar_target(int px)
{
    int x0 = bar_x0(), x1 = bar_x1();
    if (px < x0) px = x0;
    if (px > x1) px = x1;
    return total_frames * (u64)(px - x0) / (u64)(x1 - x0);
}

static void set_volume(int px)
{
    int v = (px - vol_x0()) * 100 / (vol_x1() - vol_x0());
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    if (voice)
        sys_audio(6, (u64)volume, 0);
}

static void open_folder(const char *p)
{
    Stat st;
    if (sys_stat(p, &st) != 0 || !st.is_dir) {
        say("Ordner nicht gefunden");
        return;
    }
    stop_file();
    cur = -1;
    fmt_line[0] = 0;
    snprintf(folder, sizeof(folder), "%s", p);
    load_folder();
}

static void key(int k)
{
    if (prompt) {
        if (k == 0x1B) {
            prompt = 0;
        } else if (k == '\n') {
            prompt = 0;
            open_folder(field);
        } else if (k == '\b' || k == 0x7F) {
            while (flen > 0 && (field[flen - 1] & 0xC0) == 0x80)
                flen--;
            if (flen > 0)
                flen--;
            field[flen] = 0;
        } else if (k >= 32 && k < 0xF5 && flen < (int)sizeof(field) - 1) {
            field[flen++] = (char)k;
            field[flen] = 0;
        }
        return;
    }
    int b = KEY_BASE(k);
    if (k & KEY_MOD_ALT)
        return;
    if (b == ' ' && !(k & ~0xFF))
        toggle();
    else if (b == KEY_LEFT || b == KEY_RIGHT) {
        if (pfd >= 0 && rate) {
            u64 p = position(), d = (u64)rate * 10;
            seek_to(b == KEY_LEFT ? (p > d ? p - d : 0) : p + d);
        }
    } else if (b == KEY_UP && sel > 0) {
        sel--;
        keep_visible();
    } else if (b == KEY_DOWN && sel + 1 < ntracks) {
        sel++;
        keep_visible();
    } else if (k == '\n' && sel >= 0) {
        start_track(sel, 1);
    } else if (k == 'n') {
        next_track(1, 0);
    } else if (k == 'p') {
        next_track(-1, 0);
    }
}

void _start(int argc, char **argv)
{
    char want_file[256] = "";
    if (argc > 1) {
        Stat st;
        if (sys_stat(argv[1], &st) == 0 && st.is_dir) {
            snprintf(folder, sizeof(folder), "%s", argv[1]);
        } else { /* eine Datei: ihr Ordner, und sie gleich spielen */
            const char *sl = strrchr(argv[1], '/');
            if (sl) {
                int n = (int)(sl - argv[1]);
                snprintf(folder, sizeof(folder), "%.*s", n ? n : 1, argv[1]);
                snprintf(want_file, sizeof(want_file), "%s", sl + 1);
            }
        }
    }
    ui_setup(0);
    if (gfx_open_window_ex(U(760), U(560), "Musik", GFX_RESIZABLE) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    load_folder();
    if (want_file[0])
        for (int i = 0; i < ntracks; i++)
            if (strcmp(tracks[i].name, want_file) == 0) {
                start_track(i, 1);
                keep_visible();
            }
    draw();
    s64 last_draw = sys_time_us();
    s64 last_click = 0;
    int last_row = -1, scan_next = 0;
    for (;;) {
        Event e;
        int got = gfx_wait(&e, playing ? 10 : 100), changed = 0;
        if (got) {
            int nev = 0;
            do {
                changed = 1;
                if (e.type == EV_CLOSE) {
                    stop_file();
                    gfx_close();
                    sys_exit(0);
                } else if (e.type == EV_KEY) {
                    key(e.key);
                } else if (e.type == EV_DOWN && e.button == 1 && !prompt) {
                    int b = btn_at(e.x, e.y);
                    if (b == B_PLAY) toggle();
                    else if (b == B_PREV) {
                        if (pfd >= 0 && rate && position() > (u64)rate * 3)
                            seek_to(0); /* erst an den Anfang, wie ueblich */
                        else
                            next_track(-1, 0);
                    } else if (b == B_NEXT) next_track(1, 0);
                    else if (b == B_SHUF) shuffle = !shuffle;
                    else if (b == B_REP) repeat = !repeat;
                    else if (b == B_FOLDER) {
                        prompt = 1;
                        snprintf(field, sizeof(field), "%s", folder);
                        flen = (int)strlen(field);
                    } else if (e.y >= bar_y() - U(10) && e.y < bar_y() + U(16) && e.x >= bar_x0() - U(8) && e.x <= bar_x1() + U(8) &&
                               pfd >= 0 && total_frames) {
                        drag_bar = 1;
                        bar_preview = (int)bar_target(e.x);
                    } else if (e.y >= gfx_screen.h - foot_h() && e.x >= vol_x0() - U(10)) {
                        drag_vol = 1;
                        set_volume(e.x);
                    } else if (e.y >= list_y() && e.y < gfx_screen.h - foot_h()) {
                        int i = scroll + (e.y - list_y()) / ROW_H;
                        if (i < ntracks) {
                            s64 now = sys_ticks();
                            if (i == last_row && now - last_click < 40) {
                                start_track(i, 1);
                                last_click = 0;
                            } else {
                                last_click = now;
                            }
                            last_row = i;
                            sel = i;
                        }
                    }
                } else if (e.type == EV_MOVE) {
                    if (drag_bar)
                        bar_preview = (int)bar_target(e.x);
                    else if (drag_vol)
                        set_volume(e.x);
                    else {
                        int h = prompt ? -1 : btn_at(e.x, e.y);
                        if (h == hover)
                            changed = 0;
                        hover = h;
                    }
                } else if (e.type == EV_UP) {
                    if (drag_bar && bar_preview >= 0)
                        seek_to((u64)bar_preview);
                    drag_bar = drag_vol = 0;
                    bar_preview = -1;
                } else if (e.type == EV_WHEEL) {
                    scroll -= e.wheel * 3;
                    if (scroll > ntracks - visible()) scroll = ntracks - visible();
                    if (scroll < 0) scroll = 0;
                } else if (e.type == EV_RESIZE) {
                    keep_visible();
                }
            } while (++nev < 64 && gfx_poll(&e));
        }
        feed();
        scan_step();
        if (!got && scan_next < ntracks && buffered_enough()) { /* im Leerlauf: Tags und Laengen nachlesen */
            while (scan_next < ntracks && tracks[scan_next].scanned)
                scan_next++;
            if (scan_next < ntracks) {
                scan_track(scan_next++);
                changed = 1;
            }
        }
        s64 now = sys_time_us();
        if (playing && now - last_draw > 150000) /* Fortschritt und Balken */
            changed = 1;
        if (status[0] && now - status_t > 6000000 && now - status_t < 6200000)
            changed = 1;
        if (changed) {
            draw();
            last_draw = now;
        }
    }
}
