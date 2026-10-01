/* Desktop: Ausschalten und Neu starten (Logo-Menue) mit Rueckfrage
 *
 * Nach dem Bestaetigen bekommen alle Programme die Bitte, sich zu beenden (wie beim Schliessen-Knopf). Sind nach drei
 * Sekunden alle weg, schaltet der Desktop aus bzw. startet neu (der Kernel schreibt vorher die Platten-Caches). Bleibt
 * ein Programm offen (z.B. Malen mit ungespeichertem Bild, es fragt dann selbst nach), fragt der Desktop, ob trotzdem
 * ausgeschaltet werden soll. */

#include "desktop.h"

enum { D_NONE, D_CONFIRM, D_FORCE };

int        dialog_kind;
static int dialog_action;  /* A_POWEROFF oder A_RESTART */
static int dialog_hover = -1;
static int power_action;   /* laeuft: Programme werden beendet */
static s64 power_deadline;

static void box(int *x, int *y, int *w, int *h)
{
    *w = U(380);
    *h = U(214);
    *x = (W - *w) / 2;
    *y = (H - *h) / 2 - U(40);
}

/* Knopf 0 = Abbrechen, 1 = Ausfuehren */
static void button_rect(int i, int *bx, int *by, int *bw, int *bh)
{
    int x, y, w, h, pad = U(18);
    box(&x, &y, &w, &h);
    *bw = (w - 3 * pad) / 2;
    *bh = U(32);
    *bx = x + pad + i * (*bw + pad);
    *by = y + h - pad - *bh;
}

void damage_dialog(void)
{
    damage_all(); /* der ganze Bildschirm wird abgedunkelt */
}

void dialog_open(int action)
{
    dialog_action = action;
    dialog_kind = D_CONFIRM;
    dialog_hover = -1;
    damage_dialog();
}

static void dialog_close(void)
{
    dialog_kind = D_NONE;
    damage_dialog();
}

/* Ein-/Aus-Symbol: Ring mit Luecke oben und Strich */
static void power_icon(Surface *s, float cx, float cy, float r, u32 c)
{
    const int n = 20;
    for (int i = 0; i < n; i++) {
        float a0 = 0.55f + (6.2831853f - 1.1f) * i / n, a1 = 0.55f + (6.2831853f - 1.1f) * (i + 1) / n;
        gfx_capsule(s, cx + ui_sin(a0) * r, cy - ui_cos(a0) * r, cx + ui_sin(a1) * r, cy - ui_cos(a1) * r, r * 0.22f, c, 255);
    }
    gfx_capsule(s, cx, cy - r * 1.15f, cx, cy - r * 0.15f, r * 0.22f, c, 255);
}

void draw_dialog(void)
{
    if (!dialog_kind)
        return;
    Surface *s = &gfx_screen;
    int x, y, w, h;
    box(&x, &y, &w, &h);
    gfx_blend_fill(s, 0, 0, W, H, 0x000000, 70);
    gfx_shadow(s, x, y + U(6), w, h, U(14), U(30), 90);
    gfx_round_rect(s, x, y, w, h, U(14), 0xF6F6F8, 252);
    gfx_round_frame(s, x, y, w, h, U(14), 0x000000, 30);
    int off = dialog_action == A_POWEROFF;
    u32 accent = dialog_kind == D_FORCE ? 0xFF3B30 : C_ACCENT;
    float cx = x + w * 0.5f, cy = y + U(44);
    gfx_disc(s, cx, cy, U(24), accent, 255);
    power_icon(s, cx, cy + U(2), U(10), 0xFFFFFF);
    const char *title, *l1, *l2, *ok;
    if (dialog_kind == D_CONFIRM) {
        title = off ? "Computer ausschalten?" : "Computer neu starten?";
        l1 = "Alle Programme werden beendet.";
        l2 = "Ungespeicherte \xC3\x84nderungen fragen sie selbst nach.";
        ok = off ? "Ausschalten" : "Neu starten";
    } else {
        title = "Nicht alle Programme wurden beendet";
        l1 = "Ein Programm ist noch offen (z.B. mit";
        l2 = "ungespeicherten \xC3\x84nderungen).";
        ok = off ? "Trotzdem aus" : "Trotzdem neu";
    }
    int ty = y + U(80);
    text_draw(s, font_bold, U(15), x + (w - text_width(font_bold, U(15), title)) / 2, ty, title, C_TEXT);
    ty += text_height(font_bold, U(15)) + U(4);
    text_draw(s, font_ui, FS, x + (w - text_width(font_ui, FS, l1)) / 2, ty, l1, C_TEXT2);
    ty += text_height(font_ui, FS);
    text_draw(s, font_ui, FS, x + (w - text_width(font_ui, FS, l2)) / 2, ty, l2, C_TEXT2);
    for (int i = 0; i < 2; i++) {
        int bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        u32 bg = i ? accent : 0xE3E3E8, fg = i ? 0xFFFFFF : C_TEXT;
        if (i == dialog_hover)
            bg = gfx_mix(bg, 0x000000, 30);
        gfx_round_rect(s, bx, by, bw, bh, U(8), bg, 255);
        const char *t = i ? ok : "Abbrechen";
        text_draw(s, font_bold, FS, bx + (bw - text_width(font_bold, FS, t)) / 2, by + (bh - text_height(font_bold, FS)) / 2,
                  t, fg);
    }
}

static void power_now(void)
{
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used)
            close_win_now(&wins[i]); /* wer noch laeuft, wird beendet */
    apps_quit();
    sys_power(dialog_action == A_POWEROFF ? 0 : 1);
    power_action = 0; /* sollte nicht zurueckkommen */
}

static void confirm(void)
{
    if (dialog_kind == D_FORCE) {
        dialog_close();
        power_now();
        return;
    }
    dialog_close();
    power_action = dialog_action;
    power_deadline = now_us + 3000000;
    for (int i = nord - 1; i >= 0; i--) /* alle bitten, sich zu beenden */
        close_win(order[i]);
}

static void cancel(void)
{
    power_action = 0;
    dialog_close();
}

void power_tick(void)
{
    if (!power_action || dialog_kind)
        return;
    if (nord == 0) { /* alle Fenster sind zu */
        power_now();
        return;
    }
    if (now_us > power_deadline) { /* jemand bleibt offen: nachfragen */
        dialog_kind = D_FORCE;
        dialog_hover = -1;
        damage_dialog();
    }
}

void dialog_mouse(int px, int py, int down)
{
    int hit = -1;
    for (int i = 0; i < 2; i++) {
        int bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        if (px >= bx && px < bx + bw && py >= by && py < by + bh)
            hit = i;
    }
    if (down) {
        if (hit == 0)
            cancel();
        else if (hit == 1)
            confirm();
        return;
    }
    if (hit != dialog_hover) {
        dialog_hover = hit;
        int x, y, w, h;
        box(&x, &y, &w, &h);
        damage(x, y, w, h);
    }
}

void dialog_key(int k)
{
    if (k == '\n')
        confirm();
    else if (k == 0x1B)
        cancel();
}
