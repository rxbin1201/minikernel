#include "libc.h"
#include "malloc.h"

/* edit [datei]: Texteditor fuer die Konsole.
 *
 *   Tasten:  Pfeile, Pos1/Ende, Bild hoch/runter, Entf, Backspace, Tab (4 Leerzeichen), Enter
 *            Strg+S speichern   Strg+Q beenden (bei Aenderungen zweimal)   Strg+F suchen (Enter = weitersuchen)
 *            Strg+G gehe zu Zeile   Strg+A alles markieren   Strg+C kopieren   Strg+X ausschneiden
 *            Strg+V einfuegen   Strg+K Zeile ausschneiden   Strg+D Zeile duplizieren
 *   Maus:    Klick setzt den Cursor, Ziehen markiert, Doppelklick markiert ein Wort, Rad scrollt,
 *            rechte Taste fuegt ein
 * Text ist UTF-8 (Umlaute). Windows-Zeilenenden (CRLF) bleiben beim Speichern erhalten. */

#define TABW    8
#define GUTTER_MIN 4

typedef struct {
    char *s;
    int   len, cap;
} Line;

static Line *L;
static int   nl, lcap;
static int   cy, cx;          /* Cursor: Zeile, Byte in der Zeile */
static int   want;            /* gewuenschte Anzeigespalte beim Hoch/Runter */
static int   top, left;       /* erste sichtbare Zeile, erste sichtbare Spalte */
static int   sel;             /* Markierung aktiv: von (ay, ax) bis zum Cursor */
static int   ay, ax;
static int   modified, crlf, trailing_nl = 1;
static char  filename[PATH_MAX];
static int   rows = 25, cols = 80, cell_w = 8, cell_h = 16;
static char  msg[200];
static int   msg_err;
static int   quit_armed;      /* Strg+Q bei Aenderungen: einmal gedrueckt */
static char  last_find[128];

/* ---------- Zeilen ---------- */

static void *xalloc(u64 n)
{
    void *p = u_malloc(n);
    if (!p) {
        write_all(1, "\x1b[2J\x1b[H", 7);
        fprintf(2, "edit: kein Speicher mehr\n");
        sys_exit(1);
    }
    return p;
}

static void line_reserve(Line *l, int need)
{
    if (need <= l->cap)
        return;
    int cap = l->cap ? l->cap : 16;
    while (cap < need)
        cap *= 2;
    char *n = xalloc((u64)cap);
    if (l->len)
        memcpy(n, l->s, (size_t)l->len);
    u_free(l->s);
    l->s = n;
    l->cap = cap;
}

static void line_insert_bytes(Line *l, int at, const char *b, int n)
{
    line_reserve(l, l->len + n);
    memmove(l->s + at + n, l->s + at, (size_t)(l->len - at));
    memcpy(l->s + at, b, (size_t)n);
    l->len += n;
}

static void line_delete_bytes(Line *l, int at, int n)
{
    memmove(l->s + at, l->s + at + n, (size_t)(l->len - at - n));
    l->len -= n;
}

/* Fuegt an Position idx eine leere Zeile ein */
static void lines_insert(int idx)
{
    if (nl == lcap) {
        int cap = lcap ? lcap * 2 : 64;
        Line *n = xalloc(sizeof(Line) * (u64)cap);
        if (nl)
            memcpy(n, L, sizeof(Line) * (u64)nl);
        u_free(L);
        L = n;
        lcap = cap;
    }
    memmove(L + idx + 1, L + idx, sizeof(Line) * (u64)(nl - idx));
    L[idx].s = 0;
    L[idx].len = L[idx].cap = 0;
    nl++;
}

static void lines_delete(int idx)
{
    u_free(L[idx].s);
    memmove(L + idx, L + idx + 1, sizeof(Line) * (u64)(nl - idx - 1));
    nl--;
}

/* ---------- UTF-8 und Spalten ---------- */

static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int next_pos(const Line *l, int x)
{
    if (x >= l->len)
        return l->len;
    x++;
    while (x < l->len && is_cont(l->s[x]))
        x++;
    return x;
}

static int prev_pos(const Line *l, int x)
{
    if (x <= 0)
        return 0;
    x--;
    while (x > 0 && is_cont(l->s[x]))
        x--;
    return x;
}

/* Anzeigespalte des Bytes x (Tabs bis zur naechsten Achterspalte, ein Zeichen = eine Spalte) */
static int dcol(const Line *l, int x)
{
    int c = 0;
    for (int i = 0; i < x && i < l->len; i++) {
        if (l->s[i] == '\t')
            c = (c / TABW + 1) * TABW;
        else if (!is_cont(l->s[i]))
            c++;
    }
    return c;
}

/* Byte-Position zur Anzeigespalte col (die naechstliegende Zeichengrenze) */
static int x_for_col(const Line *l, int col)
{
    int c = 0, x = 0;
    while (x < l->len) {
        int w = l->s[x] == '\t' ? (c / TABW + 1) * TABW - c : 1;
        if (c + w > col)
            break;
        c += w;
        x = next_pos(l, x);
    }
    return x;
}

static int digits(int n)
{
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

static int gutter(void)
{
    int g = digits(nl) + 1;
    return g < GUTTER_MIN ? GUTTER_MIN : g;
}

static int text_rows(void) { return rows - 2; }
static int text_cols(void) { return cols - 1 - gutter(); } /* letzte Bildschirmspalte frei lassen: sonst scrollt die Konsole */

/* ---------- Markierung ---------- */

/* Normalisierte Markierung: (y1, x1) vor (y2, x2); 0 = keine */
static int sel_range(int *y1, int *x1, int *y2, int *x2)
{
    if (!sel || (ay == cy && ax == cx))
        return 0;
    if (ay < cy || (ay == cy && ax < cx)) {
        *y1 = ay; *x1 = ax; *y2 = cy; *x2 = cx;
    } else {
        *y1 = cy; *x1 = cx; *y2 = ay; *x2 = ax;
    }
    return 1;
}

static int in_sel(int y, int x)
{
    int y1, x1, y2, x2;
    if (!sel_range(&y1, &x1, &y2, &x2))
        return 0;
    if (y < y1 || y > y2)
        return 0;
    if (y == y1 && x < x1)
        return 0;
    if (y == y2 && x >= x2)
        return 0;
    return 1;
}

/* Markierten Text in einen neuen Puffer (Laenge in *n) */
static char *sel_text(int *n)
{
    int y1, x1, y2, x2;
    *n = 0;
    if (!sel_range(&y1, &x1, &y2, &x2))
        return 0;
    u64 total = 1;
    for (int y = y1; y <= y2; y++)
        total += (u64)L[y].len + 1;
    char *t = xalloc(total);
    int k = 0;
    for (int y = y1; y <= y2; y++) {
        int a = y == y1 ? x1 : 0, b = y == y2 ? x2 : L[y].len;
        memcpy(t + k, L[y].s + a, (size_t)(b - a));
        k += b - a;
        if (y != y2)
            t[k++] = '\n';
    }
    t[k] = 0;
    *n = k;
    return t;
}

static void delete_selection(void)
{
    int y1, x1, y2, x2;
    if (!sel_range(&y1, &x1, &y2, &x2)) {
        sel = 0;
        return;
    }
    if (y1 == y2) {
        line_delete_bytes(&L[y1], x1, x2 - x1);
    } else {
        L[y1].len = x1;
        line_insert_bytes(&L[y1], x1, L[y2].s + x2, L[y2].len - x2);
        for (int y = y2; y > y1; y--)
            lines_delete(y);
    }
    cy = y1;
    cx = x1;
    sel = 0;
    modified = 1;
}

/* ---------- Bearbeiten ---------- */

static void set_want(void) { want = dcol(&L[cy], cx); }

static void insert_text(const char *t, int n)
{
    if (sel)
        delete_selection();
    for (int i = 0; i < n;) {
        if (t[i] == '\r') {
            i++;
            continue;
        }
        if (t[i] == '\n') { /* Zeile teilen */
            lines_insert(cy + 1);
            line_insert_bytes(&L[cy + 1], 0, L[cy].s + cx, L[cy].len - cx);
            L[cy].len = cx;
            cy++;
            cx = 0;
            i++;
            continue;
        }
        int j = i;
        while (j < n && t[j] != '\n' && t[j] != '\r')
            j++;
        line_insert_bytes(&L[cy], cx, t + i, j - i);
        cx += j - i;
        i = j;
    }
    modified = 1;
    set_want();
}

static void backspace(void)
{
    if (sel) {
        delete_selection();
    } else if (cx > 0) {
        int p = prev_pos(&L[cy], cx);
        line_delete_bytes(&L[cy], p, cx - p);
        cx = p;
        modified = 1;
    } else if (cy > 0) { /* mit der Zeile davor verbinden */
        int pl = L[cy - 1].len;
        line_insert_bytes(&L[cy - 1], pl, L[cy].s, L[cy].len);
        lines_delete(cy);
        cy--;
        cx = pl;
        modified = 1;
    }
    set_want();
}

static void delete_char(void)
{
    if (sel) {
        delete_selection();
    } else if (cx < L[cy].len) {
        line_delete_bytes(&L[cy], cx, next_pos(&L[cy], cx) - cx);
        modified = 1;
    } else if (cy + 1 < nl) {
        line_insert_bytes(&L[cy], cx, L[cy + 1].s, L[cy + 1].len);
        lines_delete(cy + 1);
        modified = 1;
    }
    set_want();
}

static void copy_selection(int cut)
{
    int n;
    char *t = sel_text(&n);
    if (!t) {
        snprintf(msg, sizeof(msg), "Nichts markiert (Maus ziehen, Doppelklick oder Strg+A)");
        return;
    }
    sys_clipboard_set(t, (u64)n);
    u_free(t);
    if (cut)
        delete_selection();
    snprintf(msg, sizeof(msg), "%s (%d Bytes)", cut ? "Ausgeschnitten" : "Kopiert", n);
}

static void cut_line(void)
{
    sel = 0;
    Line *l = &L[cy];
    char *t = xalloc((u64)l->len + 2);
    memcpy(t, l->s, (size_t)l->len);
    t[l->len] = '\n';
    sys_clipboard_set(t, (u64)l->len + 1);
    u_free(t);
    if (nl > 1) {
        lines_delete(cy);
        if (cy >= nl)
            cy = nl - 1;
    } else {
        l->len = 0;
    }
    cx = 0;
    modified = 1;
    set_want();
    snprintf(msg, sizeof(msg), "Zeile ausgeschnitten (Strg+V fuegt sie ein)");
}

static void dup_line(void)
{
    sel = 0;
    lines_insert(cy + 1);
    line_insert_bytes(&L[cy + 1], 0, L[cy].s, L[cy].len);
    cy++;
    modified = 1;
}

static void paste_clipboard(void)
{
    s64 n = sys_clipboard_get(0, 0); /* erst die Laenge (bis 4 MiB), dann der Inhalt */
    char *buf = n > 0 ? u_malloc((u64)n) : 0;
    if (buf && (n = sys_clipboard_get(buf, (u64)n)) > 0)
        insert_text(buf, (int)n);
    u_free(buf);
}

/* ---------- Bewegen ---------- */

static void move_vert(int d)
{
    cy += d;
    if (cy < 0)
        cy = 0;
    if (cy >= nl)
        cy = nl - 1;
    cx = x_for_col(&L[cy], want);
}

static void ensure_visible(void)
{
    int tr = text_rows(), tc = text_cols();
    if (cy < top)
        top = cy;
    if (cy >= top + tr)
        top = cy - tr + 1;
    int c = dcol(&L[cy], cx);
    if (c < left)
        left = c;
    if (c >= left + tc)
        left = c - tc + 1;
}

/* ---------- Dateien ---------- */

static void append_line(const char *b, int n)
{
    lines_insert(nl);
    if (n > 0)
        line_insert_bytes(&L[nl - 1], 0, b, n);
}

static int load(const char *path)
{
    Stat st;
    if (sys_stat(path, &st) != 0) {
        lines_insert(0);
        snprintf(msg, sizeof(msg), "Neue Datei");
        return 0;
    }
    if (st.is_dir) {
        fprintf(2, "edit: '%s' ist ein Verzeichnis\n", path);
        return -1;
    }
    s64 fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "edit: '%s' kann nicht geoeffnet werden (Fehler %d)\n", path, (int)fd);
        return -1;
    }
    char *data = xalloc(st.size + 1);
    u64 got = 0;
    s64 r;
    while (got < st.size && (r = sys_read((int)fd, data + got, st.size - got)) > 0)
        got += (u64)r;
    sys_close((int)fd);

    /* in Zeilen zerlegen; ein abschliessendes \n erzeugt keine leere letzte Zeile (wird beim Speichern wieder angehaengt) */
    trailing_nl = got == 0 || data[got - 1] == '\n';
    u64 start = 0;
    for (u64 i = 0; i < got; i++) {
        if (data[i] == '\n') {
            u64 end = i;
            if (end > start && data[end - 1] == '\r') {
                end--;
                crlf = 1;
            }
            append_line(data + start, (int)(end - start));
            start = i + 1;
        }
    }
    if (start < got || nl == 0)
        append_line(data + start, (int)(got - start));
    u_free(data);
    snprintf(msg, sizeof(msg), "%d Zeilen gelesen%s", nl, crlf ? " (Windows-Zeilenenden)" : "");
    return 0;
}

static int save(void)
{
    s64 fd = sys_open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(msg, sizeof(msg), "Speichern fehlgeschlagen: %s (Fehler %d%s)", filename, (int)fd,
                 fd == -30 ? ", Datentraeger nur lesbar" : fd == -22 ? ", ungueltiger Name" : "");
        msg_err = 1;
        return -1;
    }
    u64 bytes = 0;
    int ok = 1;
    for (int y = 0; y < nl && ok; y++) {
        if (L[y].len && write_all((int)fd, L[y].s, (size_t)L[y].len) != 0)
            ok = 0;
        bytes += (u64)L[y].len;
        if (y + 1 < nl || trailing_nl) {
            if (write_all((int)fd, crlf ? "\r\n" : "\n", crlf ? 2 : 1) != 0)
                ok = 0;
            bytes += crlf ? 2 : 1;
        }
    }
    sys_close((int)fd);
    if (!ok) {
        snprintf(msg, sizeof(msg), "Fehler beim Schreiben (Datentraeger voll?)");
        msg_err = 1;
        return -1;
    }
    modified = 0;
    snprintf(msg, sizeof(msg), "Gespeichert: %s (%d Zeilen, %llu Bytes)", filename, nl, (unsigned long long)bytes);
    return 0;
}

/* ---------- Anzeige ---------- */

static char  *frame;        /* ganze Ausgabe eines Bildes */
static int    frame_len, frame_cap;
static char **prev_rows;    /* was zuletzt in jeder Bildschirmzeile stand (nur geaenderte Zeilen neu schreiben) */
static int    force_redraw = 1;

static char rowbuf[8192];
static int  rowlen;

static void rput(const char *s, int n)
{
    if (rowlen + n < (int)sizeof(rowbuf) - 16) {
        memcpy(rowbuf + rowlen, s, (size_t)n);
        rowlen += n;
    }
}
static void rputs(const char *s) { rput(s, (int)strlen(s)); }

static void frame_put(const char *s, int n)
{
    if (frame_len + n > frame_cap) {
        int cap = frame_cap ? frame_cap * 2 : 65536;
        while (cap < frame_len + n)
            cap *= 2;
        char *f = xalloc((u64)cap);
        if (frame_len)
            memcpy(f, frame, (size_t)frame_len);
        u_free(frame);
        frame = f;
        frame_cap = cap;
    }
    memcpy(frame + frame_len, s, (size_t)n);
    frame_len += n;
}

/* Zeile r des Bildschirms festlegen: nur ausgeben, wenn sie sich geaendert hat */
static void emit_row(int r)
{
    rowbuf[rowlen] = 0;
    if (!force_redraw && prev_rows[r] && strcmp(prev_rows[r], rowbuf) == 0)
        return;
    u_free(prev_rows[r]);
    prev_rows[r] = xalloc((u64)rowlen + 1);
    memcpy(prev_rows[r], rowbuf, (size_t)rowlen + 1);
    char pos[24];
    int n = snprintf(pos, sizeof(pos), "\x1b[%d;1H", r + 1);
    frame_put(pos, n);
    frame_put(rowbuf, rowlen);
    frame_put("\x1b[0m\x1b[K", 7);
}

/* Text einer Zeile auf Breite w bringen (UTF-8-Zeichen zaehlen einfach) */
static void rpad(const char *s, int w)
{
    int c = 0;
    for (const char *p = s; *p && c < w; c++) {
        int l = utf8_len_at(p);
        rput(p, l);
        p += l;
    }
    for (; c < w; c++)
        rput(" ", 1);
}

static void render_text_row(int r)
{
    int i = top + r - 1, g = gutter(), tc = text_cols();
    rowlen = 0;
    if (i >= nl) {
        rputs("\x1b[90m~");
        emit_row(r);
        return;
    }
    char num[16];
    snprintf(num, sizeof(num), "%*d ", g - 1, i + 1);
    rputs(i == cy ? "\x1b[33m" : "\x1b[90m");
    rputs(num);
    rputs("\x1b[0m");

    const Line *l = &L[i];
    int col = 0, style = 0; /* 0 normal, 1 markiert, 2 Cursor */
    for (int x = 0; x <= l->len;) {
        int is_end = x == l->len;
        int w = is_end ? 1 : l->s[x] == '\t' ? (col / TABW + 1) * TABW - col : 1;
        int nx = is_end ? x + 1 : next_pos(l, x);
        for (int k = 0; k < w; k++, col++) {
            if (col < left)
                continue;
            if (col >= left + tc)
                break;
            int st = (i == cy && x == cx && k == 0) ? 2 : (!is_end && in_sel(i, x)) ? 1 : 0;
            if (is_end && st != 2)
                break; /* hinter dem Zeilenende nur den Cursor zeichnen */
            if (st != style) {
                rputs(st == 2 ? "\x1b[0m\x1b[7m" : st == 1 ? "\x1b[0m\x1b[44m" : "\x1b[0m");
                style = st;
            }
            if (is_end || l->s[x] == '\t')
                rput(" ", 1);
            else
                rput(l->s + x, nx - x);
        }
        if (col >= left + tc)
            break;
        x = nx;
    }
    emit_row(r);
}

static char prompt_label[40], prompt_text[128];
static int  prompt_mode; /* 0 = keins, 1 = suchen, 2 = gehe zu, 3 = speichern unter */

static void render(void)
{
    frame_len = 0;
    int tc = cols - 1;

    /* Kopfzeile */
    rowlen = 0;
    char left_part[320], right_part[64];
    const char *base = filename[0] ? filename : "(neue Datei)";
    snprintf(left_part, sizeof(left_part), " edit  %s%s", base, modified ? "  [geaendert]" : "");
    snprintf(right_part, sizeof(right_part), "Zeile %d/%d  Spalte %d ", cy + 1, nl, dcol(&L[cy], cx) + 1);
    int lw = utf8_width(left_part), rw = (int)strlen(right_part);
    rputs("\x1b[7m");
    if (lw + rw > tc) {
        rpad(left_part, tc);
    } else {
        rpad(left_part, tc - rw);
        rputs(right_part);
    }
    emit_row(0);

    for (int r = 1; r <= text_rows(); r++)
        render_text_row(r);

    /* Fusszeile: Eingabe, Meldung oder Hilfe */
    rowlen = 0;
    if (prompt_mode) {
        rputs("\x1b[1;33m");
        rputs(prompt_label);
        rputs("\x1b[0m");
        rputs(prompt_text);
        rputs("\x1b[7m \x1b[0m");
    } else if (msg[0]) {
        rputs(msg_err ? "\x1b[1;31m" : "\x1b[1;32m");
        rpad(msg, tc);
    } else {
        rputs("\x1b[36m");
        rpad("^S Speichern  ^Q Beenden  ^F Suchen  ^G Zeile  ^C/^X/^V Kopieren/Ausschn./Einf.  ^K Zeile ausschn.  ^A Alles", tc);
    }
    emit_row(rows - 1);

    force_redraw = 0;
    if (frame_len)
        write_all(1, frame, (size_t)frame_len);
}

/* ---------- Suchen und Eingabezeile ---------- */

static int lower_ascii(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int match_at(const Line *l, int x, const char *q, int qn)
{
    if (x + qn > l->len)
        return 0;
    for (int k = 0; k < qn; k++)
        if (lower_ascii((unsigned char)l->s[x + k]) != lower_ascii((unsigned char)q[k]))
            return 0;
    return 1;
}

/* Sucht q ab dem Cursor (ohne Beachtung der Gross-/Kleinschreibung bei ASCII), am Ende geht es vorn weiter.
 * Der Fund wird markiert, der Cursor steht dahinter (so findet ein zweites Suchen den naechsten). */
static void find_next(const char *q)
{
    int qn = (int)strlen(q);
    if (!qn)
        return;
    for (int step = 0; step <= nl; step++) {
        int y = (cy + step) % nl;
        int from = step == 0 ? cx : 0;
        int to = step == nl ? cx : L[y].len; /* nach einer ganzen Runde: den Anfang der Startzeile */
        for (int x = from; x + qn <= L[y].len && x <= to; x++) {
            if (match_at(&L[y], x, q, qn)) {
                int wrapped = y < cy || step == nl;
                ay = y;
                ax = x;
                cy = y;
                cx = x + qn;
                sel = 1;
                set_want();
                snprintf(msg, sizeof(msg), "Gefunden in Zeile %d%s (Strg+F, Enter = weitersuchen)", y + 1, wrapped ? ", von vorn" : "");
                msg_err = 0;
                return;
            }
        }
    }
    snprintf(msg, sizeof(msg), "'%s' nicht gefunden", q);
    msg_err = 1;
}

static void prompt_start(int mode, const char *label, const char *initial)
{
    prompt_mode = mode;
    snprintf(prompt_label, sizeof(prompt_label), "%s", label);
    snprintf(prompt_text, sizeof(prompt_text), "%s", initial);
}

static void prompt_finish(void)
{
    int mode = prompt_mode;
    prompt_mode = 0;
    msg[0] = 0;
    msg_err = 0;
    if (mode == 1) {
        snprintf(last_find, sizeof(last_find), "%s", prompt_text);
        find_next(last_find);
    } else if (mode == 2) {
        int n = atoi(prompt_text);
        if (n < 1)
            n = 1;
        if (n > nl)
            n = nl;
        sel = 0;
        cy = n - 1;
        cx = 0;
        set_want();
        top = cy - text_rows() / 2 < 0 ? 0 : cy - text_rows() / 2;
    } else if (mode == 3) {
        if (!prompt_text[0])
            return;
        snprintf(filename, sizeof(filename), "%s", prompt_text);
        save();
    }
}

/* Taste in der Eingabezeile. Liefert 1, wenn sie verbraucht wurde. */
static void prompt_key(int c)
{
    int n = (int)strlen(prompt_text);
    if (c == '\n') {
        prompt_finish();
    } else if (c == 0x1B || c == 3 || c == 17) {
        prompt_mode = 0;
        snprintf(msg, sizeof(msg), "Abgebrochen");
        msg_err = 0;
    } else if (c == '\b' || c == 0x7F) {
        while (n > 0 && is_cont(prompt_text[n - 1]))
            n--;
        if (n > 0)
            n--;
        prompt_text[n] = 0;
    } else if (((c >= 32 && c < 127) || (c >= 0x80 && c < 0xF5)) && n < (int)sizeof(prompt_text) - 1) {
        prompt_text[n] = (char)c;
        prompt_text[n + 1] = 0;
    }
}

/* ---------- Tasten ---------- */

static void quit(void)
{
    sys_mousemode(0);
    write_all(1, "\x1b[0m\x1b[2J\x1b[H", 11);
    sys_exit(0);
}

static void key(int c)
{
    if (prompt_mode) {
        prompt_key(c);
        ensure_visible();
        return;
    }
    if (c != 17)
        quit_armed = 0;
    if (c != 17 && c != 3)
        msg_err = 0;
    int keep_msg = 0;

    switch (c) {
    case KEY_UP:    sel = 0; move_vert(-1); break;
    case KEY_DOWN:  sel = 0; move_vert(1); break;
    case KEY_PGUP:  sel = 0; move_vert(-text_rows()); break;
    case KEY_PGDN:  sel = 0; move_vert(text_rows()); break;
    case KEY_LEFT:
        if (sel) {
            int y1, x1, y2, x2;
            if (sel_range(&y1, &x1, &y2, &x2)) {
                cy = y1;
                cx = x1;
            }
            sel = 0;
        } else if (cx > 0) {
            cx = prev_pos(&L[cy], cx);
        } else if (cy > 0) {
            cy--;
            cx = L[cy].len;
        }
        set_want();
        break;
    case KEY_RIGHT:
        if (sel) {
            int y1, x1, y2, x2;
            if (sel_range(&y1, &x1, &y2, &x2)) {
                cy = y2;
                cx = x2;
            }
            sel = 0;
        } else if (cx < L[cy].len) {
            cx = next_pos(&L[cy], cx);
        } else if (cy + 1 < nl) {
            cy++;
            cx = 0;
        }
        set_want();
        break;
    case KEY_HOME: sel = 0; cx = 0; set_want(); break;
    case KEY_END:  sel = 0; cx = L[cy].len; set_want(); break;
    case KEY_DEL:  delete_char(); break;
    case '\b':
    case 0x7F:     backspace(); break;
    case '\t':     insert_text("    ", 4); break;
    case '\n':     insert_text("\n", 1); break;
    case 19: /* Strg+S */
        if (!filename[0])
            prompt_start(3, "Speichern unter: ", "");
        else
            save();
        keep_msg = 1;
        break;
    case 17: /* Strg+Q */
        if (modified && !quit_armed) {
            quit_armed = 1;
            snprintf(msg, sizeof(msg), "Ungespeicherte Aenderungen! Strg+Q nochmal = verwerfen, Strg+S = speichern");
            msg_err = 1;
            keep_msg = 1;
            break;
        }
        quit();
        break;
    case 6: /* Strg+F */
        prompt_start(1, "Suchen: ", last_find);
        break;
    case 7: /* Strg+G */
        prompt_start(2, "Gehe zu Zeile: ", "");
        break;
    case 1: /* Strg+A */
        ay = 0;
        ax = 0;
        cy = nl - 1;
        cx = L[cy].len;
        sel = 1;
        set_want();
        break;
    case 3:  copy_selection(0); keep_msg = 1; break; /* Strg+C */
    case 24: copy_selection(1); keep_msg = 1; break; /* Strg+X */
    case 11: cut_line(); keep_msg = 1; break;        /* Strg+K */
    case 4:  dup_line(); break;                      /* Strg+D */
    case 0x1B: sel = 0; break;
    default:
        if ((c >= 32 && c < 127) || (c >= 0x80 && c < 0xF5)) {
            char ch = (char)c;
            insert_text(&ch, 1);
        }
        break;
    }
    if (!keep_msg && c != 0x1B)
        msg[0] = 0;
    ensure_visible();
}

/* ---------- Maus ---------- */

static MouseInfo mprev;
static int dragging;
static s64 last_click_tick;
static int last_click_y = -1, last_click_x = -1;

/* Bildschirmposition (Pixel) -> Textposition; 0 = ausserhalb des Textbereichs */
static int mouse_pos(const MouseInfo *m, int *y, int *x, int *row_out)
{
    int row = m->y / cell_h, col = m->x / cell_w;
    *row_out = row;
    if (row < 1 || row > text_rows())
        return 0;
    int i = top + row - 1;
    if (i >= nl)
        i = nl - 1;
    int c = col - gutter() + left;
    if (c < 0)
        c = 0;
    *y = i;
    *x = x_for_col(&L[i], c);
    return 1;
}

static int word_char(char c) { return c && c != ' ' && c != '\t'; }

static int mouse_poll(void)
{
    MouseInfo m;
    if (sys_mouse(&m) != 0 || m.events == mprev.events)
        return 0;
    int changed = 1, y, x, row;
    int pressed = m.left_presses != mprev.left_presses; /* auch kurze Klicks zwischen zwei Abfragen */
    int released = !(m.buttons & 1);
    MouseInfo at = m;
    at.x = m.press_x;
    at.y = m.press_y;

    if (pressed && !prompt_mode && mouse_pos(&at, &y, &x, &row)) {
        s64 now = sys_ticks();
        if (y == last_click_y && x == last_click_x && now - last_click_tick < 40) { /* Doppelklick: Wort */
            int a = x, b = x;
            while (a > 0 && word_char(L[y].s[a - 1]))
                a--;
            while (b < L[y].len && word_char(L[y].s[b]))
                b++;
            if (b > a) {
                ay = y;
                ax = a;
                cy = y;
                cx = b;
                sel = 1;
            }
            dragging = 0;
            last_click_tick = 0;
        } else {
            cy = y;
            cx = x;
            ay = y;
            ax = x;
            sel = 0;
            dragging = 1;
            last_click_tick = now;
            last_click_x = x;
            last_click_y = y;
        }
        set_want();
        msg[0] = 0;
    } else if ((m.buttons & 1) && dragging && !pressed) {
        int r = m.y / cell_h;
        if (r < 1 && top > 0) /* ueber den Rand gezogen: mitscrollen */
            top--;
        else if (r > text_rows() && top + text_rows() < nl)
            top++;
        MouseInfo mm = m;
        if (r < 1)
            mm.y = cell_h;
        if (r > text_rows())
            mm.y = text_rows() * cell_h;
        if (mouse_pos(&mm, &y, &x, &row)) {
            cy = y;
            cx = x;
            sel = !(cy == ay && cx == ax);
            set_want();
        }
    }
    if (released)
        dragging = 0;
    if (m.right_presses != mprev.right_presses && !prompt_mode) { /* rechte Taste: einfuegen */
        paste_clipboard();
        ensure_visible();
    }
    if (m.wheel) { /* Rad: scrollen (Cursor bleibt) */
        top -= m.wheel * 3;
        if (top > nl - 1)
            top = nl - 1;
        if (top < 0)
            top = 0;
    }
    mprev = m;
    return changed;
}

/* ---------- Hauptschleife ---------- */

void _start(int argc, char **argv)
{
    s64 t = sys_isatty(1);
    if (!t) {
        fprintf(2, "edit: laeuft nur auf der Konsole\n");
        sys_exit(1);
    }
    cols = (int)(t >> 16);
    rows = (int)(t & 0xFFFF);
    if (cols < 20 || rows < 5) {
        fprintf(2, "edit: Bildschirm zu klein\n");
        sys_exit(1);
    }
    VideoInfo vi;
    for (u64 i = 0; sys_videoinfo(i, &vi) == 0; i++) {
        if (vi.current) {
            cell_w = 8 * (int)vi.scale;
            cell_h = 16 * (int)vi.scale;
        }
    }

    if (argc > 1) {
        snprintf(filename, sizeof(filename), "%s", argv[1]);
        if (load(filename) != 0)
            sys_exit(1);
    } else {
        lines_insert(0);
        snprintf(msg, sizeof(msg), "Neue Datei (Strg+S fragt nach dem Namen)");
    }
    prev_rows = xalloc(sizeof(char *) * (u64)rows);
    memset(prev_rows, 0, sizeof(char *) * (u64)rows);

    sys_tty_fg(0);     /* Strg+C kommt als Zeichen an (Kopieren), statt den Editor zu beenden */
    sys_mousemode(1);  /* die Maus gehoert jetzt dem Editor */
    sys_mouse(&mprev);
    write_all(1, "\x1b[0m\x1b[2J", 8);
    ensure_visible();
    render();

    for (;;) {
        int dirty = 0;
        s64 c;
        while ((c = sys_getchar()) >= 0) {
            key((int)c);
            dirty = 1;
        }
        if (mouse_poll())
            dirty = 1;
        if (dirty)
            render();
        else
            sys_sleep_ms(10);
    }
}
