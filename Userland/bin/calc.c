#include "gfx.h"
#include "ui.h"

/* calc: Rechner (Festkomma mit 6 Nachkommastellen), Tasten mit Maus oder Tastatur (Ziffern, + - * / , = Enter,
 * Ruecktaste, c oder Esc = loeschen). Unter dem Desktop im Fenster, sonst im Vollbild (q beendet). */

#define FX 1000000LL

static const char *const keys[20] = {"C", "\xC2\xB1", "%", "/", "7", "8", "9", "*", "4", "5", "6", "-",
                                     "1", "2", "3", "+", "0", ".", "\xE2\x86\x90", "="};
static char disp[40];
static s64  acc, cur, dec;
static char op;
static int  fresh = 1;

static void show(s64 v)
{
    int neg = v < 0;
    u64 a = neg ? (u64)-v : (u64)v;
    char frac[8];
    snprintf(frac, sizeof(frac), "%06llu", (unsigned long long)(a % FX));
    int fl = 6;
    while (fl > 0 && frac[fl - 1] == '0')
        frac[--fl] = 0;
    snprintf(disp, sizeof(disp), "%s%llu%s%s", neg ? "-" : "", (unsigned long long)(a / FX), fl ? "," : "", frac);
}

static s64 apply(s64 a, s64 b)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a / 1000 * b / 1000;
    case '/':
        if (!b) {
            snprintf(disp, sizeof(disp), "Fehler");
            return 0;
        }
        return a * 1000 / b * 1000;
    default: return b;
    }
}

static void press(const char *k)
{
    char c = k[0];
    if (c >= '0' && c <= '9') {
        if (fresh) {
            cur = 0;
            dec = 0;
            fresh = 0;
        }
        if (dec) {
            if (dec < FX) {
                dec *= 10;
                cur += (cur < 0 ? -1 : 1) * (c - '0') * (FX / dec);
            }
        } else if (cur < 100000000000LL * FX / 1000) {
            cur = cur * 10 + (cur < 0 ? -1 : 1) * (c - '0') * FX;
        }
        show(cur);
        if (dec && !strchr(disp, ',')) /* Nullen hinter dem Komma beim Tippen zeigen */
            strcat(disp, ",");
    } else if (c == '.' || c == ',') {
        if (fresh) {
            cur = 0;
            fresh = 0;
        }
        if (!dec)
            dec = 1;
        show(cur);
        if (!strchr(disp, ','))
            strcat(disp, ",");
    } else if (c == 'C') {
        cur = acc = 0;
        op = 0;
        dec = 0;
        fresh = 1;
        show(0);
    } else if (strcmp(k, "\xC2\xB1") == 0) {
        cur = -cur;
        show(cur);
    } else if (c == '%') {
        cur = cur / 100;
        show(cur);
    } else if (strcmp(k, "\xE2\x86\x90") == 0) {
        cur = cur / FX / 10 * FX;
        dec = 0;
        show(cur);
    } else if (c == '=') {
        if (op) {
            cur = apply(acc, cur);
            op = 0;
            if (strcmp(disp, "Fehler") != 0)
                show(cur);
        }
        fresh = 1;
        dec = 0;
    } else if (strchr("+-*/", c)) {
        if (op && !fresh)
            cur = apply(acc, cur);
        acc = cur;
        op = c;
        fresh = 1;
        dec = 0;
        show(cur);
    }
}

/* Lage der Tasten: Anzeige oben, darunter 4 x 5 runde Tasten */
static int head(void) { return U(96); }

static void geom(int *x, int *y, int *d, int *gap)
{
    Surface *s = &gfx_screen;
    *gap = U(10);
    int dw = (s->w - 5 * *gap) / 4, dh = (s->h - head() - 6 * *gap) / 5;
    *d = dw < dh ? dw : dh;
    *x = (s->w - 4 * *d - 3 * *gap) / 2;
    *y = head() + *gap;
}

static int button_at(int px, int py)
{
    int x, y, d, gap;
    geom(&x, &y, &d, &gap);
    for (int i = 0; i < 20; i++) {
        int bx = x + (i % 4) * (d + gap), by = y + (i / 4) * (d + gap);
        if (px >= bx && px < bx + d && py >= by && py < by + d)
            return i;
    }
    return -1;
}

static void draw(int pressed)
{
    Surface *s = &gfx_screen;
    gfx_fill(s, 0, 0, s->w, s->h, 0x2C2C2E);
    int dsz = U(44), tw = text_width(font_ui, dsz, disp);
    while (tw > s->w - U(28) && dsz > U(18)) { /* lange Zahlen kleiner */
        dsz -= U(4);
        tw = text_width(font_ui, dsz, disp);
    }
    text_draw(s, font_ui, dsz, s->w - U(16) - tw, U(88) - text_height(font_ui, dsz), disp, 0xFFFFFF);
    int bx0, by0, d, gap;
    geom(&bx0, &by0, &d, &gap);
    for (int i = 0; i < 20; i++) {
        int bx = bx0 + (i % 4) * (d + gap), by = by0 + (i / 4) * (d + gap);
        int isop = i % 4 == 3 || i == 19, top = i < 3;
        u32 c = isop ? 0xFF9F0A : top ? 0xA5A5A5 : 0x505050, tc = top ? 0x000000 : 0xFFFFFF;
        if (isop && op && keys[i][0] == op && fresh) { /* gewaehlter Operator: invertiert */
            c = 0xFFFFFF;
            tc = 0xFF9F0A;
        }
        if (i == pressed) /* gedrueckt: heller */
            c = gfx_mix(c, 0xFFFFFF, 90);
        gfx_disc(s, bx + d * 0.5f, by + d * 0.5f, d * 0.5f, c, 255);
        const char *k = keys[i];
        if (k[0] == '/' && !k[1]) k = "\xC3\xB7";
        else if (k[0] == '*' && !k[1]) k = "\xC3\x97";
        else if (k[0] == '-' && !k[1]) k = "\xE2\x88\x92";
        else if (k[0] == '.' && !k[1]) k = ",";
        int fs = top ? U(20) : U(24), kw = text_width(font_ui, fs, k);
        text_draw(s, font_ui, fs, bx + (d - kw) / 2, by + (d - text_height(font_ui, fs)) / 2, k, tc);
    }
    gfx_present_all();
}

static const char *key_of(int k)
{
    static char one[2];
    if (k == '\b' || k == 0x7F) return "\xE2\x86\x90";
    if (k == 'c' || k == 'C' || k == 0x1B) return "C";
    if (k == ',' || k == '.') return ".";
    if (k == '\n' || k == '=') return "=";
    if ((k >= '0' && k <= '9') || (k && strchr("+-*/%", k))) {
        one[0] = (char)k;
        one[1] = 0;
        return one;
    }
    return 0;
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ui_setup(0);
    if (gfx_open_window(4 * U(58) + 5 * U(10), U(96) + 5 * U(58) + 6 * U(10), "Rechner") != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0); /* Vollbild: Massstab erst jetzt bekannt */
    show(0);
    draw(-1);
    int pressed = -1;
    for (;;) {
        Event e;
        if (!gfx_wait(&e, -1))
            continue;
        if (e.type == EV_CLOSE || (e.type == EV_KEY && e.key == 'q' && !gfx_windowed()))
            break;
        if (e.type == EV_KEY) {
            const char *k = key_of(e.key);
            if (k) {
                press(k);
                draw(-1);
            }
        } else if (e.type == EV_DOWN && e.button == 1) {
            pressed = button_at(e.x, e.y);
            if (pressed >= 0) {
                press(keys[pressed]);
                draw(pressed);
            }
        } else if (e.type == EV_UP && pressed >= 0) {
            pressed = -1;
            draw(-1);
        }
    }
    gfx_close();
    sys_exit(0);
}
