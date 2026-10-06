#include "gfx.h"
#include "ui.h"

/* taskmgr: Task-Manager im Stil der Aktivitaetsanzeige von macOS. Zwei Ansichten:
 *   Prozesse  Tabelle (Name, PID, % CPU, CPU-Zeit, Threads, Speicher), Klick auf eine Spalte sortiert (nochmal:
 *             andersherum), Auswahl mit Maus oder Pfeiltasten, "Beenden" bzw. Entf beendet den Prozess (mit Rueckfrage).
 *             Unten: CPU-Last (Benutzer/System) als Verlauf und belegter Arbeitsspeicher.
 *   Leistung  Verlauf jeder CPU und des Arbeitsspeichers ueber die letzte Minute.
 * Gemessen wird jede Sekunde: % CPU = Timer-Ticks des Prozesses seit der letzten Messung durch die vergangene Zeit
 * (100 % = ein Kern, wie bei macOS). Tab wechselt die Ansicht, Esc/q beendet. */

#define MAXP   256
#define MAXCPU 64
#define HIST   60

typedef struct {
    unsigned pid, ppid, threads;
    char     name[32];
    u64      ticks, mem, shm;
    int      cpu10; /* % CPU * 10 */
} Proc;

enum { COL_NAME, COL_PID, COL_CPU, COL_TIME, COL_THR, COL_MEM, NCOL };
static const char *col_name[NCOL] = {"Prozessname", "PID", "% CPU", "CPU-Zeit", "Threads", "Speicher"};
static int col_w[NCOL]; /* Breite der Spalten (die erste bekommt den Rest) */

static Proc procs[MAXP];
static int  nproc;
static struct {
    unsigned pid;
    u64      ticks;
} prev[MAXP];
static int  nprev;
static s64  last_t;

static int  ncpu;
static u64  cpu_prev[MAXCPU][3];               /* user, kernel, idle */
static unsigned char   hist_user[MAXCPU + 1][HIST], hist_sys[MAXCPU + 1][HIST]; /* [ncpu] = alle zusammen, in % */
static unsigned char   hist_mem[HIST];
static int  hist_n;                            /* gefuellte Eintraege (hoechstens HIST) */
static int  cur_user, cur_sys;                 /* letzte Messung, alle CPUs, in % */
static SysInfo si;

static int      view;            /* 0 Prozesse, 1 Leistung */
static int      sort_col = COL_CPU, sort_desc = 1;
static unsigned sel_pid;         /* 0 = keine Auswahl */
static int      top_row;         /* erste sichtbare Zeile */
static unsigned confirm_pid;     /* Rueckfrage "beenden?" offen */
static char     confirm_name[32];
static char     note[64];        /* Meldung in der Werkzeugleiste (z.B. Beenden fehlgeschlagen) */
static s64      note_until;
static unsigned self_pid;

/* ---------- Messen ---------- */

static int cmp(const Proc *a, const Proc *b)
{
    s64 d = 0;
    switch (sort_col) {
    case COL_NAME: d = strcmp(a->name, b->name); return sort_desc ? -(int)d : (int)d;
    case COL_PID: d = (s64)a->pid - (s64)b->pid; break;
    case COL_CPU: d = a->cpu10 - b->cpu10; break;
    case COL_TIME: d = (s64)a->ticks - (s64)b->ticks; break;
    case COL_THR: d = (s64)a->threads - (s64)b->threads; break;
    case COL_MEM: d = (s64)a->mem - (s64)b->mem; break;
    }
    if (!d)
        d = (s64)b->pid - (s64)a->pid; /* gleich: feste Reihenfolge, sonst springen die Zeilen */
    return sort_desc ? (d > 0 ? -1 : 1) : (d > 0 ? 1 : -1);
}

static void sort_procs(void)
{
    for (int i = 1; i < nproc; i++)
        for (int k = i; k > 0 && cmp(&procs[k], &procs[k - 1]) < 0; k--) {
            Proc t = procs[k];
            procs[k] = procs[k - 1];
            procs[k - 1] = t;
        }
}

static void push_hist(unsigned char *h, int v)
{
    if (hist_n == HIST)
        memmove(h, h + 1, HIST - 1);
    h[hist_n == HIST ? HIST - 1 : hist_n] = (unsigned char)(v < 0 ? 0 : v > 100 ? 100 : v);
}

static void sample(void)
{
    s64 t = sys_ticks(), dt = last_t ? t - last_t : 0;
    last_t = t;

    nproc = 0;
    ProcInfo pi;
    for (u64 i = 0; nproc < MAXP && sys_procinfo(i, &pi) == 0; i++) {
        if (pi.state)
            continue; /* beendet, wartet nur aufs Abholen */
        Proc *p = &procs[nproc++];
        p->pid = pi.pid;
        p->ppid = pi.ppid;
        p->threads = pi.threads;
        memcpy(p->name, pi.name, sizeof(p->name));
        p->name[sizeof(p->name) - 1] = 0;
        p->ticks = pi.cpu_ticks;
        p->mem = pi.mem_bytes;
        p->shm = pi.shm_bytes;
        p->cpu10 = 0;
        for (int k = 0; k < nprev && dt > 0; k++)
            if (prev[k].pid == p->pid) {
                p->cpu10 = (int)((p->ticks - prev[k].ticks) * 1000 / (u64)dt);
                break;
            }
    }
    nprev = nproc;
    for (int i = 0; i < nproc; i++) {
        prev[i].pid = procs[i].pid;
        prev[i].ticks = procs[i].ticks;
    }
    sort_procs();

    u64 su = 0, sk = 0, st = 0;
    CpuInfo ci;
    int n = 0;
    for (; n < MAXCPU && sys_cpuinfo((u64)n, &ci) == 0; n++) {
        u64 du = ci.ticks_user - cpu_prev[n][0], dk = ci.ticks_kernel - cpu_prev[n][1], di = ci.ticks_idle - cpu_prev[n][2];
        int first = !cpu_prev[n][0] && !cpu_prev[n][1] && !cpu_prev[n][2];
        cpu_prev[n][0] = ci.ticks_user;
        cpu_prev[n][1] = ci.ticks_kernel;
        cpu_prev[n][2] = ci.ticks_idle;
        u64 tot = du + dk + di;
        if (first || !tot)
            du = dk = 0, tot = 1;
        push_hist(hist_user[n], (int)(du * 100 / tot));
        push_hist(hist_sys[n], (int)(dk * 100 / tot));
        su += du;
        sk += dk;
        st += tot;
    }
    ncpu = n;
    cur_user = (int)(su * 100 / st);
    cur_sys = (int)(sk * 100 / st);
    push_hist(hist_user[MAXCPU], cur_user);
    push_hist(hist_sys[MAXCPU], cur_sys);

    if (sys_sysinfo(&si) != 0)
        memset(&si, 0, sizeof(si));
    push_hist(hist_mem, si.mem_total ? (int)((si.mem_total - si.mem_free) * 100 / si.mem_total) : 0);
    if (hist_n < HIST)
        hist_n++;
}

/* ---------- Texte ---------- */

static void fmt_mem(char *out, int max, u64 b)
{
    const u64 KB = 1024, MB = KB * 1024, GB = MB * 1024;
    if (b >= GB)
        snprintf(out, max, "%llu,%02llu GB", (unsigned long long)(b / GB), (unsigned long long)(b % GB * 100 / GB));
    else if (b >= 10 * MB)
        snprintf(out, max, "%llu,%llu MB", (unsigned long long)(b / MB), (unsigned long long)(b % MB * 10 / MB));
    else if (b >= MB)
        snprintf(out, max, "%llu,%02llu MB", (unsigned long long)(b / MB), (unsigned long long)(b % MB * 100 / MB));
    else
        snprintf(out, max, "%llu KB", (unsigned long long)(b / KB));
}

static void fmt_time(char *out, int max, u64 ticks) /* "1:23,45" (Minuten:Sekunden,Hundertstel) */
{
    u64 cs = ticks, s = cs / 100;
    if (s >= 3600)
        snprintf(out, max, "%llu:%02llu:%02llu", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60),
                 (unsigned long long)(s % 60));
    else
        snprintf(out, max, "%llu:%02llu,%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60),
                 (unsigned long long)(cs % 100));
}

static int app_icon(const char *name)
{
    static const struct {
        const char *n;
        int         icon;
    } map[] = {
        {"term", ICON_TERM},   {"files", ICON_FILES},   {"calc", ICON_CALC},       {"clock", ICON_CLOCK},
        {"about", ICON_ABOUT}, {"paint", ICON_PAINT},   {"snake", ICON_SNAKE},     {"tetris", ICON_TETRIS},
        {"textedit", ICON_EDIT}, {"edit", ICON_EDIT},   {"music", ICON_MUSIC},     {"settings", ICON_SETTINGS},
        {"view", ICON_IMAGE},  {"textview", ICON_TEXT}, {"taskmgr", ICON_TASKS},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(name, map[i].n) == 0)
            return map[i].icon;
    return ICON_NONE;
}

/* ---------- Zeichnen ---------- */

static int TB, HDR, FOOT; /* Werkzeugleiste, Tabellenkopf, unterer Bereich */

static void layout(int w)
{
    TB = U(48);
    HDR = U(28);
    FOOT = U(124);
    col_w[COL_PID] = U(64);
    col_w[COL_CPU] = U(70);
    col_w[COL_TIME] = U(92);
    col_w[COL_THR] = U(70);
    col_w[COL_MEM] = U(100);
    int rest = w - U(16);
    for (int i = 1; i < NCOL; i++)
        rest -= col_w[i];
    col_w[COL_NAME] = rest < U(120) ? U(120) : rest;
}

static int col_x(int c) /* linke Kante der Spalte */
{
    int x = U(8);
    for (int i = 0; i < c; i++)
        x += col_w[i];
    return x;
}

static void text_right(Surface *s, Font *f, int size, int xr, int y, const char *t, u32 c)
{
    text_draw(s, f, size, xr - text_width(f, size, t), y, t, c);
}

static void text_center(Surface *s, Font *f, int size, int cx, int y, const char *t, u32 c)
{
    text_draw(s, f, size, cx - text_width(f, size, t) / 2, y, t, c);
}

/* Knopf; Rueckgabe ueber geom, damit Klicks dieselben Masse benutzen */
typedef struct {
    int x, y, w, h;
} Box;
static Box btn_kill, seg[2], dlg_cancel, dlg_ok;

static int in_box(const Box *b, int x, int y) { return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h; }

static void button(Surface *s, const Box *b, const char *t, int enabled, int primary)
{
    u32 bg = primary ? C_ACCENT : 0xFFFFFF, fg = primary ? 0xFFFFFF : enabled ? C_TEXT : 0xB0B0B5;
    gfx_round_rect(s, b->x, b->y, b->w, b->h, U(6), bg, 255);
    if (!primary)
        gfx_round_frame(s, b->x, b->y, b->w, b->h, U(6), 0xC8C8CC, 255);
    int th = text_height(font_ui, FS);
    text_center(s, font_ui, FS, b->x + b->w / 2, b->y + (b->h - th) / 2, t, fg);
}

static void toolbar(Surface *s)
{
    gfx_gradient(s, 0, 0, s->w, TB, C_TITLE_TOP, C_TITLE_BOT);
    gfx_fill(s, 0, TB - 1, s->w, 1, C_HAIRLINE);
    int bh = U(28), by = (TB - bh) / 2;
    btn_kill = (Box){U(12), by, text_width(font_ui, FS, "Beenden") + U(28), bh};
    button(s, &btn_kill, "Beenden", view == 0 && sel_pid != 0, 0);

    /* Umschalter in der Mitte */
    int sw = U(104), sx = s->w / 2 - sw;
    gfx_round_rect(s, sx, by, sw * 2, bh, U(7), 0xE3E3E6, 255);
    for (int i = 0; i < 2; i++) {
        seg[i] = (Box){sx + i * sw, by, sw, bh};
        if (view == i) {
            gfx_round_rect(s, seg[i].x + U(2), by + U(2), sw - U(4), bh - U(4), U(6), 0xFFFFFF, 255);
            gfx_round_frame(s, seg[i].x + U(2), by + U(2), sw - U(4), bh - U(4), U(6), 0xD0D0D4, 255);
        }
        text_center(s, view == i ? font_bold : font_ui, FS, seg[i].x + sw / 2, by + (bh - text_height(font_ui, FS)) / 2,
                    i ? "Leistung" : "Prozesse", C_TEXT);
    }

    char t[64];
    if (note[0] && sys_ticks() < note_until)
        snprintf(t, sizeof(t), "%s", note);
    else
        snprintf(t, sizeof(t), "%d Prozesse", nproc);
    text_right(s, font_ui, FS_SMALL, s->w - U(14), (TB - text_height(font_ui, FS_SMALL)) / 2, t, C_TEXT2);
}

static int table_rows(const Surface *s) { return (s->h - TB - HDR - FOOT) / ROW_H; }

static void table(Surface *s)
{
    int y = TB, th = text_height(font_ui, FS);
    gfx_fill(s, 0, y, s->w, HDR, 0xFAFAFA);
    gfx_fill(s, 0, y + HDR - 1, s->w, 1, C_HAIRLINE);
    for (int c = 0; c < NCOL; c++) {
        int x = col_x(c), ty = y + (HDR - th) / 2;
        char t[48];
        snprintf(t, sizeof(t), "%s%s", col_name[c], c == sort_col ? (sort_desc ? " \xE2\x86\x93" : " \xE2\x86\x91") : "");
        if (c == COL_NAME)
            text_draw(s, font_bold, FS_SMALL, x + U(30), ty, t, c == sort_col ? C_TEXT : C_TEXT2);
        else
            text_right(s, font_bold, FS_SMALL, x + col_w[c] - U(8), ty, t, c == sort_col ? C_TEXT : C_TEXT2);
        if (c)
            gfx_fill(s, x, y + U(6), 1, HDR - U(12), C_HAIRLINE);
    }

    int rows = table_rows(s);
    if (top_row > nproc - rows)
        top_row = nproc - rows;
    if (top_row < 0)
        top_row = 0;
    int ry = y + HDR;
    for (int i = 0; i < rows; i++) {
        int k = top_row + i, yy = ry + i * ROW_H;
        u32 bg = (i & 1) ? 0xF5F5F7 : 0xFFFFFF;
        if (k < nproc && procs[k].pid == sel_pid)
            bg = C_ACCENT;
        gfx_fill(s, 0, yy, s->w, ROW_H, bg);
        if (k >= nproc)
            continue;
        const Proc *p = &procs[k];
        int sel = p->pid == sel_pid;
        u32 fg = sel ? 0xFFFFFF : C_TEXT, fg2 = sel ? 0xFFFFFF : C_TEXT2;
        int ty = yy + (ROW_H - th) / 2, isz = ROW_H - U(6);
        int icon = app_icon(p->name);
        if (icon != ICON_NONE)
            ui_app_icon(s, icon, col_x(0) + U(4), yy + U(3), isz);
        else
            gfx_round_rect(s, col_x(0) + U(4) + isz / 4, yy + U(3) + isz / 4, isz / 2, isz / 2, U(3),
                           sel ? 0xFFFFFF : 0xC7C7CC, sel ? 160 : 255);
        char t[48];
        snprintf(t, sizeof(t), "%s%s", p->name, p->pid == self_pid ? " (dieses Fenster)" : "");
        text_draw(s, font_ui, FS, col_x(0) + U(30), ty, t, fg);
        snprintf(t, sizeof(t), "%u", p->pid);
        text_right(s, font_ui, FS, col_x(COL_PID) + col_w[COL_PID] - U(8), ty, t, fg2);
        snprintf(t, sizeof(t), "%d,%d", p->cpu10 / 10, p->cpu10 % 10);
        text_right(s, font_ui, FS, col_x(COL_CPU) + col_w[COL_CPU] - U(8), ty, t, p->cpu10 >= 500 && !sel ? 0xD70015 : fg);
        fmt_time(t, sizeof(t), p->ticks);
        text_right(s, font_ui, FS, col_x(COL_TIME) + col_w[COL_TIME] - U(8), ty, t, fg2);
        snprintf(t, sizeof(t), "%u", p->threads);
        text_right(s, font_ui, FS, col_x(COL_THR) + col_w[COL_THR] - U(8), ty, t, fg2);
        fmt_mem(t, sizeof(t), p->mem);
        text_right(s, font_ui, FS, col_x(COL_MEM) + col_w[COL_MEM] - U(8), ty, t, fg);
    }
    if (nproc > rows)
        ui_scrollbar(s, s->w - U(10), ry, rows * ROW_H, nproc, rows, top_row);
    /* Rest bis zum unteren Bereich */
    int used = ry + rows * ROW_H;
    gfx_fill(s, 0, used, s->w, s->h - FOOT - used, 0xFFFFFF);
}

/* Verlauf: Benutzer (blau) und System (rot) uebereinander, wie bei macOS; sys = 0: nur eine Kurve */
static void graph(Surface *s, int x, int y, int w, int h, const unsigned char *user, const unsigned char *sys, u32 c1, u32 c2)
{
    gfx_round_rect(s, x, y, w, h, U(5), 0x1C1C1E, 255);
    for (int g = 1; g < 4; g++)
        gfx_fill(s, x + U(3), y + h * g / 4, w - U(6), 1, 0x2C2C2E);
    int iw = w - U(6), ih = h - U(6), ix = x + U(3), iy = y + U(3);
    for (int px = 0; px < iw; px++) {
        int idx = px * HIST / iw - (HIST - hist_n); /* neueste rechts */
        if (idx < 0)
            continue;
        int u = user[idx] * ih / 100, k = sys ? sys[idx] * ih / 100 : 0;
        if (k)
            gfx_fill(s, ix + px, iy + ih - k, 1, k, c2);
        if (u)
            gfx_fill(s, ix + px, iy + ih - k - u, 1, u, c1);
    }
}

static void footer(Surface *s)
{
    int y = s->h - FOOT, th = text_height(font_ui, FS_SMALL);
    gfx_fill(s, 0, y, s->w, FOOT, 0xF5F5F7);
    gfx_fill(s, 0, y, s->w, 1, C_HAIRLINE);
    int pad = U(14), cy = y + pad;

    /* links: Zahlen */
    char t[64];
    const char *lab[3] = {"Benutzer:", "System:", "Leerlauf:"};
    int val[3] = {cur_user, cur_sys, 100 - cur_user - cur_sys};
    u32 col[3] = {0x0A84FF, 0xFF453A, C_TEXT2};
    int lx = pad, vx = pad + U(150);
    for (int i = 0; i < 3; i++) {
        text_draw(s, font_ui, FS_SMALL, lx, cy + i * (th + U(6)), lab[i], C_TEXT);
        snprintf(t, sizeof(t), "%d %%", val[i]);
        text_right(s, font_bold, FS_SMALL, vx, cy + i * (th + U(6)), t, col[i]);
    }
    int threads = 0;
    for (int i = 0; i < nproc; i++)
        threads += (int)procs[i].threads;
    snprintf(t, sizeof(t), "Threads: %d", threads);
    text_draw(s, font_ui, FS_SMALL, lx, cy + 3 * (th + U(6)), t, C_TEXT2);

    /* Mitte: CPU-Last */
    int gx = vx + U(28), gw = (s->w - gx) / 2 - U(30);
    if (gw < U(80))
        gw = U(80);
    text_center(s, font_bold, FS_SMALL, gx + gw / 2, y + U(6), "CPU-Last", C_TEXT);
    graph(s, gx, y + U(10) + th, gw, FOOT - th - U(22), hist_user[MAXCPU], hist_sys[MAXCPU], 0x0A84FF, 0xFF453A);

    /* rechts: Arbeitsspeicher */
    int mx = gx + gw + U(30), mw = s->w - mx - pad;
    if (mw < U(120))
        return;
    u64 used = si.mem_total - si.mem_free;
    text_draw(s, font_bold, FS_SMALL, mx, cy, "Arbeitsspeicher", C_TEXT);
    char a[24], b[24];
    fmt_mem(a, sizeof(a), used);
    fmt_mem(b, sizeof(b), si.mem_total);
    snprintf(t, sizeof(t), "%s von %s belegt", a, b);
    text_draw(s, font_ui, FS_SMALL, mx, cy + th + U(6), t, C_TEXT2);
    int bar_y = cy + 2 * (th + U(6)) + U(4), bar_h = U(12);
    gfx_round_rect(s, mx, bar_y, mw, bar_h, bar_h / 2, 0xE0E0E4, 255);
    int fw = si.mem_total ? (int)((u64)mw * used / si.mem_total) : 0;
    u32 mc = used * 100 > si.mem_total * 90 ? 0xFF453A : used * 100 > si.mem_total * 70 ? 0xFF9F0A : 0x30D158;
    if (fw > bar_h)
        gfx_round_rect(s, mx, bar_y, fw, bar_h, bar_h / 2, mc, 255);
    fmt_mem(a, sizeof(a), si.mem_free);
    snprintf(t, sizeof(t), "%s frei", a);
    text_draw(s, font_ui, FS_SMALL, mx, bar_y + bar_h + U(6), t, C_TEXT2);
}

static void performance(Surface *s)
{
    int y0 = TB + U(16), pad = U(16), th = text_height(font_bold, FS_SMALL);
    gfx_fill(s, 0, TB, s->w, s->h - TB, 0xFFFFFF);
    int cols = ncpu <= 2 ? ncpu : ncpu <= 4 ? 2 : 4;
    if (cols < 1)
        cols = 1;
    int rows = (ncpu + cols - 1) / cols;
    int mem_h = U(150);
    int cell_w = (s->w - pad * (cols + 1)) / cols;
    int cell_h = (s->h - y0 - mem_h - pad * 2) / (rows ? rows : 1) - pad;
    if (cell_h > U(150))
        cell_h = U(150);
    if (cell_h < U(50))
        cell_h = U(50);
    char t[48];
    for (int i = 0; i < ncpu; i++) {
        int x = pad + (i % cols) * (cell_w + pad), y = y0 + (i / cols) * (cell_h + pad);
        int u = hist_n ? hist_user[i][hist_n - 1] : 0, k = hist_n ? hist_sys[i][hist_n - 1] : 0;
        snprintf(t, sizeof(t), "CPU %d", i);
        text_draw(s, font_bold, FS_SMALL, x, y, t, C_TEXT);
        snprintf(t, sizeof(t), "%d %%", u + k);
        text_right(s, font_ui, FS_SMALL, x + cell_w, y, t, C_TEXT2);
        graph(s, x, y + th + U(4), cell_w, cell_h - th - U(4), hist_user[i], hist_sys[i], 0x0A84FF, 0xFF453A);
    }
    int my = y0 + rows * (cell_h + pad) + U(4);
    if (my + U(60) > s->h)
        return;
    char a[24], b[24];
    fmt_mem(a, sizeof(a), si.mem_total - si.mem_free);
    fmt_mem(b, sizeof(b), si.mem_total);
    text_draw(s, font_bold, FS_SMALL, pad, my, "Arbeitsspeicher", C_TEXT);
    snprintf(t, sizeof(t), "%s von %s", a, b);
    text_right(s, font_ui, FS_SMALL, s->w - pad, my, t, C_TEXT2);
    int gh = s->h - my - th - U(4) - pad;
    if (gh > mem_h)
        gh = mem_h;
    graph(s, pad, my + th + U(4), s->w - 2 * pad, gh, hist_mem, 0, 0x30D158, 0);
}

static void dialog(Surface *s)
{
    for (int y = 0; y < s->h; y++) /* abdunkeln */
        for (int x = 0; x < s->w; x++)
            s->px[y * s->w + x] = gfx_mix(s->px[y * s->w + x], 0x000000, 70);
    int w = U(360), h = U(150), x = (s->w - w) / 2, y = (s->h - h) / 2;
    gfx_shadow(s, x, y, w, h, U(12), U(16), 90);
    gfx_round_rect(s, x, y, w, h, U(12), 0xFFFFFF, 255);
    char t[80];
    snprintf(t, sizeof(t), "\xE2\x80\x9E%s\xE2\x80\x9C beenden?", confirm_name);
    text_center(s, font_bold, FS, x + w / 2, y + U(20), t, C_TEXT);
    const char *info = strcmp(confirm_name, "desktop") == 0 ? "Damit schlie\xC3\x9Ft sich die ganze Oberfl\xC3\xA4" "che."
                       : "Nicht gespeicherte \xC3\x84nderungen gehen verloren.";
    text_center(s, font_ui, FS_SMALL, x + w / 2, y + U(20) + text_height(font_bold, FS) + U(8), info, C_TEXT2);
    int bw = (w - U(48)) / 2, bh = U(30), by = y + h - bh - U(16);
    dlg_cancel = (Box){x + U(16), by, bw, bh};
    dlg_ok = (Box){x + w - U(16) - bw, by, bw, bh};
    button(s, &dlg_cancel, "Abbrechen", 1, 0);
    button(s, &dlg_ok, "Beenden", 1, 1);
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    layout(s->w);
    toolbar(s);
    if (view == 0) {
        table(s);
        footer(s);
    } else {
        performance(s);
    }
    if (confirm_pid)
        dialog(s);
    gfx_present_all();
}

/* ---------- Bedienen ---------- */

static int sel_index(void)
{
    for (int i = 0; i < nproc; i++)
        if (procs[i].pid == sel_pid)
            return i;
    return -1;
}

static void ensure_visible(void)
{
    int i = sel_index(), rows = table_rows(&gfx_screen);
    if (i < 0)
        return;
    if (i < top_row)
        top_row = i;
    if (i >= top_row + rows)
        top_row = i - rows + 1;
}

static void ask_kill(void)
{
    int i = sel_index();
    if (i < 0)
        return;
    confirm_pid = procs[i].pid;
    snprintf(confirm_name, sizeof(confirm_name), "%s", procs[i].name);
}

static void do_kill(void)
{
    unsigned pid = confirm_pid;
    confirm_pid = 0;
    s64 r = sys_kill((int)pid);
    if (r < 0)
        snprintf(note, sizeof(note), "%s l\xC3\xA4sst sich nicht beenden (%lld)", confirm_name, (long long)r);
    else
        snprintf(note, sizeof(note), "%s beendet", confirm_name);
    note_until = sys_ticks() + 300;
    if (r >= 0) {
        sel_pid = 0;
        sys_sleep_ms(50); /* der Prozess raeumt sich auf: dann fehlt er schon in der Liste */
        sample();
    }
}

static void click(int x, int y)
{
    Surface *s = &gfx_screen;
    if (confirm_pid) {
        if (in_box(&dlg_ok, x, y))
            do_kill();
        else if (in_box(&dlg_cancel, x, y))
            confirm_pid = 0;
        return;
    }
    for (int i = 0; i < 2; i++)
        if (in_box(&seg[i], x, y)) {
            view = i;
            return;
        }
    if (view != 0)
        return;
    if (in_box(&btn_kill, x, y)) {
        ask_kill();
        return;
    }
    if (y >= TB && y < TB + HDR) { /* Spaltenkopf: sortieren */
        for (int c = 0; c < NCOL; c++)
            if (x >= col_x(c) && x < col_x(c) + col_w[c]) {
                if (sort_col == c)
                    sort_desc = !sort_desc;
                else
                    sort_col = c, sort_desc = c != COL_NAME && c != COL_PID;
                sort_procs();
            }
        return;
    }
    int ry = TB + HDR, rows = table_rows(s);
    if (y >= ry && y < ry + rows * ROW_H) {
        int k = top_row + (y - ry) / ROW_H;
        sel_pid = k < nproc ? procs[k].pid : 0;
    }
}

static void keypress(int k)
{
    if (confirm_pid) {
        if (k == 0x1B)
            confirm_pid = 0;
        else if (k == '\n' || k == '\r')
            do_kill();
        return;
    }
    int base = KEY_BASE(k);
    if (base == '\t') {
        view = !view;
        return;
    }
    if (view != 0)
        return;
    int i = sel_index();
    if (k == KEY_UP || k == KEY_DOWN) {
        i = i < 0 ? 0 : i + (k == KEY_UP ? -1 : 1);
        if (i >= 0 && i < nproc)
            sel_pid = procs[i].pid;
        ensure_visible();
    } else if (k == KEY_DEL && sel_pid) {
        ask_kill();
    }
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ui_setup(0);
    if (gfx_open_window_ex(U(760), U(560), "Task-Manager", GFX_RESIZABLE) != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    self_pid = (unsigned)sys_getpid();
    sample();
    draw();
    s64 next = sys_ticks() + 100;
    for (;;) {
        Event e;
        s64 wait = (next - sys_ticks()) * 10;
        int got = gfx_wait(&e, wait < 1 ? 1 : (int)wait);
        int redraw = 0;
        if (got) {
            if (e.type == EV_CLOSE || (e.type == EV_KEY && !confirm_pid && (e.key == 0x1B || e.key == 'q')))
                break;
            if (e.type == EV_KEY) {
                keypress(e.key);
                redraw = 1;
            } else if (e.type == EV_DOWN && e.button == 1) {
                click(e.x, e.y);
                redraw = 1;
            } else if (e.type == EV_WHEEL && view == 0 && !confirm_pid) {
                top_row -= e.wheel * 3;
                redraw = 1;
            } else if (e.type == EV_RESIZE) {
                redraw = 1;
            }
        }
        if (sys_ticks() >= next) {
            next = sys_ticks() + 100;
            sample();
            redraw = 1;
        }
        if (redraw)
            draw();
    }
    gfx_close();
    sys_exit(0);
}
