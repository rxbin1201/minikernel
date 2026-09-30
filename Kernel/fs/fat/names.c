/* FAT: neue Namen erzeugen (Kurzname, Alias mit ~N, LFN-Eintraege) */

#include "lib/kprintf.h"
#include "lib/string.h"
#include "core/syscall.h" /* ERR_* */
#include "drivers/rtc.h"
#include "lib/utf8.h"
#include "fs/fat/fat_internal.h"

/* ---------- Neue Namen: Kurzname, Alias mit ~N, LFN-Eintraege ---------- */

/* Zeichen, die in einem langen Namen (FAT und exFAT) nicht vorkommen duerfen; nur ASCII wird angelegt */
int fat_name_valid(const char *s, size_t len)
{
    if (len == 0 || len > 255)
        return 0;
    if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
        return 0;
    if (s[len - 1] == ' ' || s[len - 1] == '.' || s[0] == ' ')
        return 0;
    if (!utf8_valid(s))
        return 0;
    const char *p = s;
    for (uint32_t cp; (cp = utf8_next(&p)) != 0;) {
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || (cp < 0x80 && strchr("\\/:*?\"<>|", (int)cp)))
            return 0;
    }
    uint16_t units[256];
    return utf8_to_utf16(s, units, 255) > 0; /* hoechstens 255 UTF-16-Einheiten */
}

static int short_char_ok(char c) /* c ist GROSS */
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '$' || c == '~' ||
           c == '!' || c == '#' || c == '%' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '@' ||
           c == '^' || c == '{' || c == '}';
}

/* Passt der Name in 8.3 (GROSS oder ganz klein pro Teil)? 0 = ja (out = Kurzname, ntres = Klein-Flags), -1 = braucht LFN */
static int short_fits(const char *s, size_t len, uint8_t out[11], uint8_t *ntres)
{
    memset(out, ' ', 11);
    *ntres = 0;
    for (size_t i = 0; i < len; i++)
        if ((uint8_t)s[i] >= 0x80)
            return -1; /* Nicht-ASCII braucht einen langen Namen */
    size_t dot = len;
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    size_t name_len = dot, ext_len = dot < len ? len - dot - 1 : 0;
    if (name_len == 0 || name_len > 8 || ext_len > 3)
        return -1;

    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
    for (size_t i = 0; i < len; i++) {
        if (i == dot)
            continue;
        char c = s[i];
        int in_ext = i > dot;
        if (c >= 'a' && c <= 'z') {
            if (in_ext) ext_lower = 1; else base_lower = 1;
        } else if (c >= 'A' && c <= 'Z') {
            if (in_ext) ext_upper = 1; else base_upper = 1;
        }
        c = upper_c(c);
        if (!short_char_ok(c))
            return -1;
        if (in_ext)
            out[8 + (i - dot - 1)] = (uint8_t)c;
        else
            out[i] = (uint8_t)c;
    }
    if ((base_lower && base_upper) || (ext_lower && ext_upper))
        return -1;
    *ntres = (uint8_t)((base_lower ? 0x08 : 0) | (ext_lower ? 0x10 : 0));
    return 0;
}

/* Gibt es im Verzeichnis schon einen Eintrag mit diesem Kurznamen? 1 = ja, 0 = nein, -1 = Fehler */
static int alias_taken(uint32_t dir, const uint8_t n11[11])
{
    DirIter it;
    fat_it_init(&it, dir);
    DirItem item;
    int r;
    while ((r = fat_dir_next_item(&it, &item)) == 1)
        if (memcmp(item.e.name, n11, 11) == 0)
            return 1;
    return r < 0 ? -1 : 0;
}

/* Erzeugt einen eindeutigen Kurznamen fuer einen langen Namen (wie Windows: ERSTE6~1.TXT) */
static int make_alias(uint32_t dir, const char *s, size_t len, uint8_t out[11])
{
    size_t dot = len;
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    char base[9], ext[4];
    int bn = 0, en = 0, lossy = 0;
    for (size_t i = 0; i < dot; i++) {
        char c = s[i];
        if (c == ' ' || c == '.') {
            lossy = 1;
            continue;
        }
        if ((uint8_t)c >= 0x80) { /* je Zeichen ein '_' (Folgebytes ueberspringen) */
            lossy = 1;
            if (((uint8_t)c & 0xC0) == 0x80)
                continue;
            c = '_';
        }
        c = upper_c(c);
        if (!short_char_ok(c)) {
            c = '_';
            lossy = 1;
        }
        if (bn < 8)
            base[bn++] = c;
        else
            lossy = 1;
    }
    for (size_t i = dot + 1; i < len && dot < len; i++) {
        char c = s[i];
        if (c == ' ') {
            lossy = 1;
            continue;
        }
        if ((uint8_t)c >= 0x80) {
            lossy = 1;
            if (((uint8_t)c & 0xC0) == 0x80)
                continue;
            c = '_';
        }
        c = upper_c(c);
        if (!short_char_ok(c)) {
            c = '_';
            lossy = 1;
        }
        if (en < 3)
            ext[en++] = c;
        else
            lossy = 1;
    }
    if (bn == 0) {
        base[bn++] = '_';
        lossy = 1;
    }

    for (int n = 0; n <= 999999; n++) {
        if (n == 0 && lossy)
            continue; /* ohne Tilde nur, wenn nichts abgeschnitten oder ersetzt wurde (z.B. nur gemischte Gross-/Kleinschreibung) */
        char tail[10];
        int tl = 0, keep = bn;
        if (n > 0) {
            ksnprintf(tail, sizeof(tail), "~%d", n);
            tl = (int)strlen(tail);
            keep = 8 - tl;
            if (keep > bn)
                keep = bn;
        }
        memset(out, ' ', 11);
        for (int i = 0; i < keep; i++)
            out[i] = (uint8_t)base[i];
        for (int i = 0; i < tl; i++)
            out[keep + i] = (uint8_t)tail[i];
        for (int i = 0; i < en; i++)
            out[8 + i] = (uint8_t)ext[i];
        int t = alias_taken(dir, out);
        if (t < 0)
            return ERR_IO;
        if (!t)
            return 0;
    }
    return ERR_EXIST;
}

/* Legt einen Eintrag samt LFN-Eintraegen an (erweitert das Verzeichnis bei Bedarf). 0 = ok */
int fat_dir_add_name(uint32_t dir_cluster, const char *comp, uint8_t attr, uint32_t first_cluster, uint32_t size,
                        DirLoc *loc_out)
{
    size_t len = strlen(comp);
    if (!fat_name_valid(comp, len))
        return ERR_INVAL;
    uint16_t units[256];
    int nunits = utf8_to_utf16(comp, units, 255);
    if (nunits <= 0)
        return ERR_INVAL;

    uint8_t n11[11], ntres = 0;
    int lfn = short_fits(comp, len, n11, &ntres) != 0;
    if (lfn) {
        int r = make_alias(dir_cluster, comp, len, n11);
        if (r != 0)
            return r;
        ntres = 0;
    } else {
        int t = alias_taken(dir_cluster, n11);
        if (t < 0)
            return ERR_IO;
        if (t)
            return ERR_EXIST;
    }
    int parts = lfn ? (nunits + 12) / 13 : 0;
    int need = parts + 1;

    /* Eine Reihe aufeinanderfolgender freier Eintraege suchen (nach dem ersten 0x00 ist alles frei) */
    DirIter it;
    fat_it_init(&it, dir_cluster);
    DirLoc locs[LFN_PARTS + 2];
    DirEntry *e;
    DirLoc l;
    int run = 0, r;
    while ((r = fat_it_next(&it, &e, &l)) == 1) {
        if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
            locs[run++] = l;
            if (run == need)
                break;
        } else {
            run = 0;
        }
    }
    if (r < 0)
        return ERR_IO;
    if (run < need) { /* Verzeichnis ist voll: Cluster anhaengen (nicht moeglich in der festen FAT12/16-Wurzel) */
        if (it.fixed_root)
            return ERR_NOSPC;
        uint32_t last = it.last_cluster;
        while (run < need) {
            uint32_t c = fat_alloc_cluster(last);
            if (!c)
                return ERR_NOSPC;
            if (fat_zero_cluster(c) != 0)
                return ERR_IO;
            last = c;
            for (uint32_t j = 0; j < vol.spc * 16 && run < need; j++) {
                locs[run].lba = cluster_lba(c) + j / 16;
                locs[run].off = (j % 16) * 32;
                run++;
            }
        }
    }

    uint8_t sum = fat_lfn_checksum(n11);
    for (int i = 0; i < parts; i++) { /* LFN-Eintraege, hoechster Teil zuerst */
        int k = parts - i;
        uint8_t raw[32];
        memset(raw, 0, sizeof(raw));
        raw[0] = (uint8_t)(k | (i == 0 ? 0x40 : 0));
        raw[11] = ATTR_LFN;
        raw[13] = sum;
        static const uint8_t offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int j = 0; j < 13; j++) {
            int idx = (k - 1) * 13 + j;
            uint16_t u = idx < nunits ? units[idx] : idx == nunits ? 0x0000 : 0xFFFF;
            put16(raw + offs[j], u);
        }
        if (fat_entry_write(&locs[i], (const DirEntry *)raw) != 0)
            return ERR_IO;
    }

    DirEntry n;
    memset(&n, 0, sizeof(n));
    memcpy(n.name, n11, 11);
    n.attr = attr;
    n.ntres = ntres;
    uint16_t stamp_date, stamp_time;
    dos_now(&stamp_date, &stamp_time);
    n.crt_date = n.acc_date = n.wrt_date = stamp_date;
    n.crt_time = n.wrt_time = stamp_time;
    n.cluster_hi = first_cluster >> 16;
    n.cluster_lo = first_cluster & 0xFFFF;
    n.size = size;
    if (fat_entry_write(&locs[parts], &n) != 0)
        return ERR_IO;
    if (loc_out)
        *loc_out = locs[parts];
    return 0;
}
