#include "keymap.h"
#include "keyboard.h"
#include "string.h"
#include "utf8.h"

/* Tabellen: Index = HID-Usage (0x00..0x67), Werte = Unicode-Zeichen; 0 = nichts.
 * Tottasten stehen als DEAD_* (Private-Use-Bereich) in der Tabelle. */

#define NKEYS 0x68
#define DEAD_CIRC  0xE001 /* ^ */
#define DEAD_ACUTE 0xE002 /* ´ */
#define DEAD_GRAVE 0xE003 /* ` */

typedef struct {
    const char *name;
    uint16_t    normal[NKEYS], shift[NKEYS], altgr[NKEYS];
    int         ctrl_alt_is_altgr;
} Layout;

/* Gemeinsamer Teil aller Layouts: Buchstaben, Ziffernblock, Steuertasten (US-Belegung, die Layouts ueberschreiben) */
static void base(Layout *l)
{
    for (int i = 0; i < 26; i++) {
        l->normal[0x04 + i] = (uint16_t)('a' + i);
        l->shift[0x04 + i] = (uint16_t)('A' + i);
    }
    static const char digits[] = "1234567890", sdig[] = "!@#$%^&*()";
    for (int i = 0; i < 10; i++) {
        l->normal[0x1E + i] = (uint16_t)digits[i];
        l->shift[0x1E + i] = (uint16_t)sdig[i];
    }
    static const uint8_t ctl_keys[] = {0x28, 0x29, 0x2A, 0x2B, 0x2C};
    static const char ctl_chars[] = {'\n', 0x1B, '\b', '\t', ' '};
    for (int i = 0; i < 5; i++)
        l->normal[ctl_keys[i]] = l->shift[ctl_keys[i]] = (uint16_t)ctl_chars[i];
    /* Satzzeichen (US) */
    static const uint8_t pk[] = {0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x64};
    static const char pn[] = "-=[]\\\\;'`,./\\", ps[] = "_+{}||:\"~<>?|";
    for (int i = 0; i < 13; i++) {
        l->normal[pk[i]] = (uint16_t)pn[i];
        l->shift[pk[i]] = (uint16_t)ps[i];
    }
    /* Ziffernblock (NumLock an) */
    static const uint8_t kk[] = {0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63};
    static const char kc[] = "/*-+\n1234567890.";
    for (int i = 0; i < 16; i++)
        l->normal[kk[i]] = l->shift[kk[i]] = (uint16_t)kc[i];
}

static Layout lay_us, lay_de, lay_uk;
static Layout *current;
static uint32_t dead; /* wartende Tottaste */

static void set(Layout *l, uint8_t k, uint16_t n, uint16_t s, uint16_t a)
{
    l->normal[k] = n;
    l->shift[k] = s;
    if (a)
        l->altgr[k] = a;
}

static void init_layouts(void)
{
    if (current)
        return;
    lay_us.name = "us";
    base(&lay_us);

    /* Deutsch (QWERTZ, ISO) */
    lay_de.name = "de";
    lay_de.ctrl_alt_is_altgr = 1;
    base(&lay_de);
    set(&lay_de, 0x1C, 'z', 'Z', 0);                    /* Y-Position */
    set(&lay_de, 0x1D, 'y', 'Y', 0);                    /* Z-Position */
    static const char sdig[] = "!\"\xA7$%&/()=";         /* § als Latin-1 */
    for (int i = 0; i < 10; i++)
        lay_de.shift[0x1E + i] = (uint8_t)sdig[i];
    lay_de.altgr[0x1F] = 0xB2;                          /* ² */
    lay_de.altgr[0x20] = 0xB3;                          /* ³ */
    lay_de.altgr[0x24] = '{';
    lay_de.altgr[0x25] = '[';
    lay_de.altgr[0x26] = ']';
    lay_de.altgr[0x27] = '}';
    set(&lay_de, 0x2D, 0xDF, '?', '\\');                /* ß ? \ */
    set(&lay_de, 0x2E, DEAD_ACUTE, DEAD_GRAVE, 0);      /* ´ ` */
    set(&lay_de, 0x2F, 0xFC, 0xDC, 0);                  /* ü Ü */
    set(&lay_de, 0x30, '+', '*', '~');
    set(&lay_de, 0x31, '#', '\'', 0);
    set(&lay_de, 0x32, '#', '\'', 0);
    set(&lay_de, 0x33, 0xF6, 0xD6, 0);                  /* ö Ö */
    set(&lay_de, 0x34, 0xE4, 0xC4, 0);                  /* ä Ä */
    set(&lay_de, 0x35, DEAD_CIRC, 0xB0, 0);             /* ^ ° */
    set(&lay_de, 0x36, ',', ';', 0);
    set(&lay_de, 0x37, '.', ':', 0);
    set(&lay_de, 0x38, '-', '_', 0);
    set(&lay_de, 0x64, '<', '>', '|');
    lay_de.altgr[0x14] = '@';                           /* q */
    lay_de.altgr[0x08] = 0x20AC;                        /* e: € */
    lay_de.altgr[0x10] = 0xB5;                          /* m: µ */

    /* Britisch (ISO) */
    lay_uk.name = "uk";
    base(&lay_uk);
    lay_uk.shift[0x1F] = '"';
    lay_uk.shift[0x20] = 0xA3;                          /* £ */
    lay_uk.altgr[0x21] = 0x20AC;                        /* € */
    set(&lay_uk, 0x34, '\'', '@', 0);
    set(&lay_uk, 0x31, '#', '~', 0);
    set(&lay_uk, 0x32, '#', '~', 0);
    set(&lay_uk, 0x35, '`', 0xAC, 0xA6);                /* ` ¬ ¦ */
    set(&lay_uk, 0x64, '\\', '|', 0);
    lay_uk.altgr[0x08] = 0xE9;                          /* é */
    lay_uk.altgr[0x18] = 0xFA;                          /* ú */
    lay_uk.altgr[0x0C] = 0xED;                          /* í */
    lay_uk.altgr[0x12] = 0xF3;                          /* ó */
    lay_uk.altgr[0x04] = 0xE1;                          /* á */

    current = &lay_us;
}

int keymap_set(const char *name)
{
    init_layouts();
    Layout *all[] = {&lay_us, &lay_de, &lay_uk};
    for (int i = 0; i < 3; i++) {
        if (name && strcmp(name, all[i]->name) == 0) {
            current = all[i];
            dead = 0;
            return 0;
        }
    }
    return -1;
}

const char *keymap_name(void)
{
    init_layouts();
    return current->name;
}

const char *keymap_list(void)
{
    return "us de uk";
}

static void emit(uint32_t cp)
{
    char b[4];
    int n = utf8_encode(cp, b);
    for (int i = 0; i < n; i++)
        keyboard_deliver((unsigned char)b[i]);
}

static uint32_t dead_char(uint32_t d)
{
    return d == DEAD_CIRC ? '^' : d == DEAD_ACUTE ? 0xB4 : '`';
}

/* Tottaste + Buchstabe; 0 = nicht kombinierbar */
static uint32_t compose(uint32_t d, uint32_t c)
{
    static const char vowels[] = "aeiouAEIOU";
    static const uint16_t circ[] = {0xE2, 0xEA, 0xEE, 0xF4, 0xFB, 0xC2, 0xCA, 0xCE, 0xD4, 0xDB};
    static const uint16_t acute[] = {0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA};
    static const uint16_t grave[] = {0xE0, 0xE8, 0xEC, 0xF2, 0xF9, 0xC0, 0xC8, 0xCC, 0xD2, 0xD9};
    if (d == DEAD_ACUTE && c == 'y')
        return 0xFD;
    if (d == DEAD_ACUTE && c == 'Y')
        return 0xDD;
    for (int i = 0; vowels[i]; i++)
        if ((uint32_t)vowels[i] == c)
            return d == DEAD_CIRC ? circ[i] : d == DEAD_ACUTE ? acute[i] : grave[i];
    return 0;
}

int keymap_key(uint8_t usage, int shift, int ctrl, int altgr, int caps)
{
    init_layouts();
    if (usage >= NKEYS)
        return 0;
    if (altgr == 2) { /* linke Alt-Taste: nur zusammen mit Strg (deutsch) wie AltGr */
        if (ctrl && current->ctrl_alt_is_altgr) {
            ctrl = 0;
            altgr = 1;
        } else {
            altgr = 0;
        }
    }
    uint32_t c;
    if (altgr) {
        c = current->altgr[usage];
        if (!c)
            return 1; /* AltGr + Taste ohne Belegung: nichts */
    } else {
        c = shift ? current->shift[usage] : current->normal[usage];
    }
    if (!c)
        return 0;

    if (caps && !altgr) { /* Feststelltaste: Buchstaben (auch Umlaute) umkehren */
        uint32_t lower = current->normal[usage], upper = current->shift[usage];
        if (uni_upper(lower) == upper && lower != upper)
            c = shift ? lower : upper;
    }
    if (ctrl && !altgr) {
        uint32_t l = current->normal[usage];
        if (l >= 'a' && l <= 'z') { /* Strg+Buchstabe = Steuerzeichen (Strg-C = 3 ...) */
            dead = 0;
            keyboard_deliver((unsigned char)(l - 'a' + 1));
        }
        return 1;
    }

    if (c >= DEAD_CIRC && c <= DEAD_GRAVE) {
        if (dead) { /* zweimal: das Zeichen selbst */
            emit(dead_char(dead));
            dead = 0;
            if (c == DEAD_CIRC || c == DEAD_ACUTE || c == DEAD_GRAVE) {
                /* die zweite Tottaste gibt ebenfalls ihr Zeichen aus (wie unter Windows: ^^) */
                emit(dead_char(c));
            }
            return 1;
        }
        dead = c;
        return 1;
    }
    if (dead) {
        uint32_t d = dead;
        dead = 0;
        if (c == ' ') {
            emit(dead_char(d));
            return 1;
        }
        uint32_t comp = compose(d, c);
        if (comp) {
            emit(comp);
            return 1;
        }
        emit(dead_char(d)); /* nicht kombinierbar: beide Zeichen */
    }
    emit(c);
    return 1;
}

uint8_t keymap_ps2_to_usage(uint8_t sc)
{
    static const uint8_t t[0x60] = {
        /* 00 */ 0, 0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,
        /* 10 */ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30, 0x28, 0, 0x04, 0x16,
        /* 20 */ 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x35, 0, 0x31, 0x1D, 0x1B, 0x06, 0x19,
        /* 30 */ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0, 0x55, 0, 0x2C, 0, 0, 0, 0, 0, 0,
        /* 40 */ 0, 0, 0, 0, 0, 0, 0, 0x5F, 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,
        /* 50 */ 0x5A, 0x5B, 0x62, 0x63, 0, 0, 0x64, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    return sc < 0x60 ? t[sc] : 0;
}
