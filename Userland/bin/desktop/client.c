/* Desktop: die Programme hinter den Fenstern (eigene Prozesse, Protokoll in winproto.h)
 *
 * launch_app() startet ein Programm mit zwei Pipes (Deskriptor 3 und 4); Programme, die anders gestartet wurden (z.B.
 * im Terminal), verbinden sich ueber den Dienst "desktop" (SYS_SERVICE) und bekommen ebenso zwei Pipes. Ein Programm
 * meldet sich mit WP_CREATE und zeichnet in
 * geteilten Speicher; das Fenster erscheint mit dem ersten fertigen Bild (sonst sieht man beim Oeffnen kurz Schwarz).
 * Danach kopiert der Desktop bei jedem WP_DAMAGE den geaenderten Teil in das Fensterbild. Aendert sich die Groesse,
 * bekommt das Programm WP_RESIZE und antwortet mit einem neuen Puffer (WP_BUFFER); bis dahin bleibt der alte sichtbar.
 * Der Desktop wartet nie auf ein Programm: geschrieben wird nur, wenn die Pipe Platz hat, gelesen nur ganze
 * Nachrichten. */

#include "desktop.h"

#define MAXPEND 32 /* gestartete Programme, deren Fenster noch nicht da ist */
#define MAXREAP 32

typedef struct {
    int   used, pid, in, out, action;
    int   created;  /* WP_CREATE ist da, es fehlt noch das erste Bild */
    WpMsg create;
    s64   t0;       /* Start (Mikrosekunden) */
    char  name[32];
} Pending;

static Pending pend[MAXPEND];
static int     reap[MAXREAP]; /* beendete oder zu beendende Programme, die noch abgeholt werden muessen */

static void add_reap(int pid)
{
    for (int i = 0; i < MAXREAP; i++)
        if (!reap[i]) {
            reap[i] = pid;
            return;
        }
    int code;
    sys_wait(pid, &code); /* Liste voll (sollte nicht vorkommen): dann eben warten */
}

/* Nachricht schicken, wenn Platz ist (sonst verwerfen: ein haengendes Programm haelt den Desktop nicht auf) */
static void send_msg(int fd, const WpMsg *m)
{
    if (fd >= 0 && sys_fdavail(fd) >= (s64)sizeof(*m))
        write_all(fd, m, sizeof(*m));
}

/* 1 = Nachricht gelesen, 0 = (noch) keine ganze, -1 = Programm hat die Pipe geschlossen */
static int read_msg(int fd, WpMsg *m)
{
    s64 n = sys_fdavail(fd);
    if (n < 0)
        return -1;
    if (n < (s64)sizeof(*m))
        return 0;
    u64 got = 0;
    while (got < sizeof(*m)) {
        s64 r = sys_read(fd, (char *)m + got, sizeof(*m) - got);
        if (r <= 0)
            return -1;
        got += (u64)r;
    }
    return 1;
}

static Pending *free_pending(void)
{
    for (int i = 0; i < MAXPEND; i++)
        if (!pend[i].used)
            return &pend[i];
    return 0;
}

static void add_pending(Pending *p, int pid, int in, int out, int action, const char *name)
{
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->pid = pid;
    p->in = in;
    p->out = out;
    p->action = action;
    p->t0 = now_us;
    snprintf(p->name, sizeof(p->name), "%s", name);
}

/* Bekannte Programme: Symbol und Name fuer die Taskleiste */
static const struct {
    const char *prog, *name;
    int         action;
} known[] = {
    {"term", "Terminal", A_TERM},   {"files", "Dateien", A_FILES},       {"calc", "Rechner", A_CALC},
    {"clock", "Uhr", A_CLOCK},      {"about", "Info", A_ABOUT},          {"paint", "Malen", A_PAINT},
    {"snake", "Snake", A_SNAKE},    {"tetris", "Tetris", A_TETRIS},      {"textview", "Textansicht", ICON_TEXT},
    {"view", "Bildansicht", ICON_IMAGE},  {"textedit", "Texteditor", A_EDIT},   {"music", "Musik", A_MUSIC},
};

/* Programme, die sich selbst melden (aus dem Terminal gestartet): annehmen und begruessen */
void apps_accept(void)
{
    int c[3];
    Pending *p;
    while ((p = free_pending()) && sys_service_accept("desktop", c) == 0) {
        WpMsg hello = {WP_HELLO, W, H, ui_pct, WP_MAGIC, 0, 0, 0, {0}};
        write_all(c[1], &hello, sizeof(hello));
        char prog[32] = "Programm";
        ProcInfo pi;
        for (u64 i = 0; sys_procinfo(i, &pi) == 0; i++)
            if ((int)pi.pid == c[2])
                snprintf(prog, sizeof(prog), "%s", pi.name);
        int action = ICON_NONE;
        const char *name = prog;
        for (u64 k = 0; k < sizeof(known) / sizeof(known[0]); k++)
            if (strcmp(known[k].prog, prog) == 0) {
                action = known[k].action;
                name = known[k].name;
            }
        add_pending(p, c[2], c[0], c[1], action, name);
    }
}

void launch_app(const char *path, const char *cmdline, int action, const char *name)
{
    Pending *p = free_pending();
    if (!p)
        return;
    int ev[2], rq[2];
    if (sys_pipe(ev) < 0)
        return;
    if (sys_pipe(rq) < 0) {
        sys_close(ev[0]);
        sys_close(ev[1]);
        return;
    }
    WpMsg hello = {WP_HELLO, W, H, ui_pct, WP_MAGIC, 0, 0, 0, {0}};
    write_all(ev[1], &hello, sizeof(hello)); /* liegt bereit, bevor das Programm nachsieht */
    s64 pid = sys_fork();
    if (pid == 0) {
        /* Die beiden Enden auf 3 und 4 legen. Liegt das Schreibende schon auf 3, erst wegkopieren (frueher ging das
         * ueber die festen Nummern 30 und 31 - ab etwa 13 Fenstern lagen dort schon Pipes anderer Fenster) */
        int out = rq[1] == WP_FD_IN ? (int)sys_dup(rq[1]) : rq[1];
        sys_dup2(ev[0], WP_FD_IN);
        sys_dup2(out, WP_FD_OUT);
        sys_closefrom(WP_FD_OUT + 1); /* keine Pipes anderer Fenster erben (es koennen Hunderte sein) */
        sys_setpgid(0, 0);
        sys_exec(path, cmdline);
        sys_exit(127);
    }
    sys_close(ev[0]);
    sys_close(rq[1]);
    if (pid < 0) {
        sys_close(ev[1]);
        sys_close(rq[0]);
        return;
    }
    add_pending(p, (int)pid, rq[0], ev[1], action, name);
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

/* Datei oder Ordner mit dem passenden Programm oeffnen (Wunsch eines Programms, z.B. der Dateien); leerer Pfad: ein
 * neues, leeres Dokument im Texteditor */
void open_path(const char *path)
{
    char cmd[WP_PATH_MAX + 32];
    Stat st;
    if (!path[0]) {
        launch_app("/bin/textedit", "textedit", A_EDIT, "Texteditor");
        return;
    }
    if (sys_stat(path, &st) != 0 || strchr(path, '"'))
        return;
    if (st.is_dir) {
        snprintf(cmd, sizeof(cmd), "files \"%s\"", path);
        launch_app("/bin/files", cmd, A_FILES, "Dateien");
    } else if (ends_with(path, ".mp3") || ends_with(path, ".wav")) {
        snprintf(cmd, sizeof(cmd), "music \"%s\"", path);
        launch_app("/bin/music", cmd, A_MUSIC, "Musik");
    } else if (ends_with(path, ".bmp")) {
        snprintf(cmd, sizeof(cmd), "view \"%s\"", path);
        launch_app("/bin/view", cmd, ICON_IMAGE, "Bildansicht");
    } else { /* alles andere im Texteditor (Binaerdateien lehnt er mit einer Meldung ab) */
        snprintf(cmd, sizeof(cmd), "textedit \"%s\"", path);
        launch_app("/bin/textedit", cmd, A_EDIT, "Texteditor");
    }
}

static void drop_pending(Pending *p)
{
    sys_close(p->in);
    sys_close(p->out);
    add_reap(p->pid);
    p->used = 0;
}

/* Geteilten Speicher des Programms einblenden; 0 = keiner bzw. zu klein */
static u32 *map_buffer(unsigned id, int w, int h)
{
    s64 a = sys_shm_map(id);
    if (a < 0)
        return 0;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || sys_shm_size(id) < (s64)w * h * 4) {
        sys_shm_unmap((void *)a);
        return 0;
    }
    return (u32 *)a;
}

/* Das Programm hat sein erstes Bild: jetzt das Fenster anlegen */
static void attach(Pending *p)
{
    WpMsg *m = &p->create;
    int cw = m->a, ch = m->b;
    u32 *px = map_buffer((unsigned)m->c, cw, ch);
    m->text[sizeof(m->text) - 1] = 0;
    Win *w = px ? new_window(m->text[0] ? m->text : p->name, cw, ch + ((m->d & WPF_FRAMELESS) ? 0 : TITLE_H)) : 0;
    if (!w) {
        if (px)
            sys_shm_unmap(px);
        sys_kill(p->pid);
        drop_pending(p);
        return;
    }
    w->pid = p->pid;
    w->app = p->action;
    snprintf(w->name, sizeof(w->name), "%s", p->name);
    w->flags = m->d;
    w->app_in = p->in;
    w->app_out = p->out;
    w->app_px = px;
    w->app_w = w->req_w = cw;
    w->app_h = w->req_h = ch;
    p->used = 0;
    WpMsg f = {WP_FOCUS, 1, 0, 0, 0, 0, 0, 0, {0}};
    send_msg(w->app_out, &f);
}

static void poll_pending(Pending *p)
{
    WpMsg m;
    int r;
    while ((r = read_msg(p->in, &m)) > 0) {
        if (m.type == WP_CREATE && !p->created) {
            p->create = m;
            p->created = 1;
        } else if (m.type == WP_DAMAGE && p->created) {
            attach(p);
            return;
        } else if (m.type == WP_WANT_FRAME) { /* noch ohne Fenster: gleich weiterzeichnen lassen */
            WpMsg f = {WP_FRAME, 0, 0, 0, 0, 0, 0, 0, {0}};
            send_msg(p->out, &f);
        }
    }
    if (r < 0) { /* ohne Fenster beendet */
        drop_pending(p);
        return;
    }
    if (p->created && now_us - p->t0 > 500000) /* zeichnet nichts: Fenster trotzdem zeigen */
        attach(p);
}

static void poll_window(Win *w)
{
    WpMsg m;
    int r;
    while ((r = read_msg(w->app_in, &m)) > 0) {
        if (m.type == WP_DAMAGE) {
            if (m.c > 0 && m.d > 0)
                win_dirty(w, m.a, win_th(w) + m.b, m.c, m.d);
        } else if (m.type == WP_TITLE) {
            m.text[sizeof(m.text) - 1] = 0;
            snprintf(w->title, sizeof(w->title), "%s", m.text);
            if (win_th(w))
                win_dirty(w, 0, 0, w->w, TITLE_H);
        } else if (m.type == WP_WINCMD) {
            window_cmd(w, m.a);
        } else if (m.type == WP_SETTINGS) {
            settings_changed();
            if (m.a == 1)
                dialog_open(A_RESTART);
        } else if (m.type == WP_SETMODE) {
            change_mode(m.a, m.b, m.c);
        } else if (m.type == WP_WANT_FRAME) {
            w->app_frame = 1;
        } else if (m.type == WP_BUFFER) { /* neuer Puffer nach WP_RESIZE */
            u32 *px = map_buffer((unsigned)m.c, m.a, m.b);
            if (px) {
                if (w->app_px)
                    sys_shm_unmap(w->app_px);
                w->app_px = px;
                w->app_w = m.a;
                w->app_h = m.b;
                win_dirty_all(w);
            }
        } else if (m.type == WP_OPEN) { /* Pfad in Stuecken */
            if (m.a >= 0 && m.c >= 0 && m.c <= 32 && m.a + m.c < WP_PATH_MAX) {
                memcpy(w->open_path + m.a, m.text, (size_t)m.c);
                if (m.b) {
                    w->open_path[m.a + m.c] = 0;
                    open_path(w->open_path);
                }
            }
        }
    }
    if (r < 0) { /* Programm hat das Fenster zugemacht oder ist beendet */
        w->app_done = 1;
        close_win(w);
    }
}

void apps_poll(void)
{
    apps_accept();
    for (int i = 0; i < MAXPEND; i++)
        if (pend[i].used)
            poll_pending(&pend[i]);
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used && !wins[i].app_done)
            poll_window(&wins[i]);
    for (int i = 0; i < MAXREAP; i++) {
        int code;
        if (reap[i] && sys_wait_nohang(reap[i], &code) != ERR_AGAIN)
            reap[i] = 0;
    }
}

/* Nach dem Bildwechsel: wer auf das naechste Bild wartet, darf zeichnen; gesammelte Mausbewegungen und neue Groessen
 * schicken (beim Ziehen hoechstens eine Groesse je Bild) */
void apps_frame(void)
{
    for (int i = 0; i < MAXW; i++) {
        Win *w = &wins[i];
        if (!w->used || w->app_done)
            continue;
        if (w->app_move) {
            WpMsg m = {WP_INPUT, EV_MOVE, 0, w->app_mx, w->app_my, 0, 0, 0, {0}};
            send_msg(w->app_out, &m);
            w->app_move = 0;
        }
        int cw = w->w, ch = w->h - win_th(w), zoom = w->zoomed != 0;
        if ((w->flags & WPF_RESIZABLE) && (cw != w->req_w || ch != w->req_h || zoom != w->req_zoom)) {
            WpMsg m = {WP_RESIZE, cw, ch, zoom, 0, 0, 0, 0, {0}};
            send_msg(w->app_out, &m);
            w->req_w = cw;
            w->req_h = ch;
            w->req_zoom = zoom;
        }
        if (w->app_frame) {
            WpMsg m = {WP_FRAME, 0, 0, 0, 0, 0, 0, 0, {0}};
            send_msg(w->app_out, &m);
            w->app_frame = 0;
        }
    }
}

/* Ereignis an das Programm; Mauskoordinaten werden relativ zum Inhalt */
void app_input(Win *w, int type, int key, int x, int y, int button, int wheel)
{
    if (w->app_done)
        return;
    int cx, cy, cw, ch;
    content_rect(w, &cx, &cy, &cw, &ch);
    if (type == EV_MOVE) { /* hoechstens eine Bewegung je Bild */
        w->app_mx = x - cx;
        w->app_my = y - cy;
        w->app_move = 1;
        return;
    }
    if (w->app_move) { /* Reihenfolge erhalten: erst die Bewegung, dann der Klick */
        WpMsg mv = {WP_INPUT, EV_MOVE, 0, w->app_mx, w->app_my, 0, 0, 0, {0}};
        send_msg(w->app_out, &mv);
        w->app_move = 0;
    }
    WpMsg m = {WP_INPUT, type, key, x - cx, y - cy, button, wheel, 0, {0}};
    send_msg(w->app_out, &m);
}

void app_focus(Win *w, int on)
{
    if (w->app_done)
        return;
    WpMsg m = {WP_FOCUS, on, 0, 0, 0, 0, 0, 0, {0}};
    send_msg(w->app_out, &m);
}

/* Schliessen-Knopf: das Programm bitten (es darf nachfragen, z.B. "nicht gespeichert"). Wer beim dritten Mal noch
 * laeuft, reagiert offenbar nicht und wird beendet. */
void app_request_close(Win *w)
{
    if (++w->app_close_n >= 3) {
        sys_kill(w->pid);
        return;
    }
    WpMsg m = {WP_CLOSE, 0, 0, 0, 0, 0, 0, 0, {0}};
    send_msg(w->app_out, &m);
}

/* Fenster weg: Pipes und Speicher freigeben; laeuft das Programm noch (Desktop wird beendet), beenden */
void app_free(Win *w)
{
    if (!w->app_done)
        sys_kill(w->pid);
    sys_close(w->app_in);
    sys_close(w->app_out);
    if (w->app_px)
        sys_shm_unmap(w->app_px);
    w->app_px = 0;
    add_reap(w->pid);
}

/* Beim Beenden des Desktops: alles abholen, was noch laeuft */
void apps_quit(void)
{
    for (int i = 0; i < MAXPEND; i++)
        if (pend[i].used) {
            sys_kill(pend[i].pid);
            drop_pending(&pend[i]);
        }
    for (int i = 0; i < MAXREAP; i++)
        if (reap[i]) {
            int code;
            sys_kill(reap[i]);
            sys_wait(reap[i], &code);
            reap[i] = 0;
        }
}

/* Inhalt aus dem geteilten Speicher ins Fensterbild (nur der neu zu zeichnende Teil). Ist das Fenster gerade groesser
 * als der Puffer (Groesse wird geaendert, das Programm zeichnet noch), wird der Rest in der Farbe der Ecke unten
 * rechts gefuellt - meist der Hintergrund des Programms. */
void draw_app(Win *w, int x, int y, int cw, int ch)
{
    int aw = w->app_w < cw ? w->app_w : cw, ah = w->app_h < ch ? w->app_h : ch;
    int x0 = x > gfx_clip.x0 ? x : gfx_clip.x0, x1 = x + aw < gfx_clip.x1 ? x + aw : gfx_clip.x1;
    int y0 = y > gfx_clip.y0 ? y : gfx_clip.y0, y1 = y + ah < gfx_clip.y1 ? y + ah : gfx_clip.y1;
    if (w->app_px && x0 < x1) /* kopieren und dabei Byte 3 auf 255 (deckend, fuer das Mischen auf der GPU) */
        for (int yy = y0; yy < y1; yy++) {
            u32 *d = tgt->px + (u64)yy * (u64)tgt->w + (u64)x0;
            const u32 *s = w->app_px + (u64)(yy - y) * (u64)w->app_w + (u64)(x0 - x);
            for (int i = 0; i < x1 - x0; i++)
                d[i] = s[i] | 0xFF000000u;
        }
    u32 fill = (w->app_px && w->app_w && w->app_h ? w->app_px[(u64)w->app_h * (u64)w->app_w - 1] & 0xFFFFFF : C_WINDOW) |
               0xFF000000u;
    if (aw < cw)
        gfx_fill(tgt, x + aw, y, cw - aw, ch, fill);
    if (ah < ch)
        gfx_fill(tgt, x, y + ah, aw, ch - ah, fill);
}
