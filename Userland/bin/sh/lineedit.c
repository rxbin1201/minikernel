/* Shell: Zeileneditor mit Verlauf und Tab-Vervollstaendigung */

#include "libc.h"
#include "malloc.h"
#include "sh.h"

/* ---------- Zeileneditor ---------- */

/* Zahl der Zeichen (UTF-8) in den ersten n Bytes */
static int u8count(const char *s, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
}

/* Zeichnet die ganze Zeile neu: Cursor an den Anfang, Prompt + Text, Reste der laengeren alten Zeile ueberschreiben,
 * dann den Cursor an die Bearbeitungsposition zurueckfahren. prev_len = Zeichen (nicht Bytes) der alten Zeile. */
static void redraw(const char *prompt, const char *buf, int len, int pos, int prev_len)
{
    char out[LINE_MAX * 3 + 256];
    int n = 0;
    out[n++] = '\r';
    for (const char *p = prompt; *p && n < 200; p++)
        out[n++] = *p;
    for (int i = 0; i < len; i++)
        out[n++] = buf[i];
    int chars = u8count(buf, len);
    int extra = prev_len > chars ? prev_len - chars : 0;
    for (int i = 0; i < extra; i++)
        out[n++] = ' ';
    for (int i = 0; i < chars - u8count(buf, pos) + extra; i++)
        out[n++] = '\b';
    write_all(1, out, (size_t)n);
}

void hist_add(const char *line)
{
    if (!line[0] || (hist_count && strcmp(history[hist_count - 1], line) == 0))
        return;
    if (hist_count == HIST_MAX) {
        memmove(history[0], history[1], sizeof(history[0]) * (HIST_MAX - 1));
        hist_count--;
    }
    snprintf(history[hist_count++], LINE_MAX, "%s", line);
}

/* ---------- Tab-Vervollstaendigung ---------- */

static const char *const builtin_names[] = {"cd", "pwd", "exit", "history", "help", "set", "unset", "export", "jobs",
                                            "fg", "wait", "source", "clear", "true", "false", "echo", "test", "read",
                                            "shift", "return", "break", "continue", "type", "if", "then", "elif", "else",
                                            "fi", "while", "until", "for", "do", "done", "function", 0};

static char        cand_pool[8192];

static int         cand_used, ncand;

static const char *cand[MAX_CAND];

static char        cand_dir[MAX_CAND];

static int lower_c(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int prefix_ci(const char *name, const char *prefix)
{
    for (; *prefix; name++, prefix++)
        if (lower_c((unsigned char)*name) != lower_c((unsigned char)*prefix))
            return 0;
    return 1;
}

static void cand_add(const char *name, int is_dir)
{
    for (int i = 0; i < ncand; i++)
        if (strcmp(cand[i], name) == 0 && cand_dir[i] == is_dir)
            return;
    size_t l = strlen(name) + 1;
    if (ncand >= MAX_CAND || cand_used + (int)l > (int)sizeof(cand_pool))
        return;
    memcpy(cand_pool + cand_used, name, l);
    cand[ncand] = cand_pool + cand_used;
    cand_dir[ncand] = (char)is_dir;
    ncand++;
    cand_used += (int)l;
}

static void cand_sort(void)
{
    for (int i = 1; i < ncand; i++) {
        const char *n = cand[i];
        char d = cand_dir[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(cand[j], n) > 0) {
            cand[j + 1] = cand[j];
            cand_dir[j + 1] = cand_dir[j];
            j--;
        }
        cand[j + 1] = n;
        cand_dir[j + 1] = d;
    }
}

static void gather_dir(const char *dir, const char *base, int dirs_only_files)
{
    char path[LINE_MAX];
    snprintf(path, sizeof(path), "%s", dir[0] ? dir : ".");
    size_t l = strlen(path);
    if (l > 1 && path[l - 1] == '/')
        path[l - 1] = 0;
    DirEnt ent;
    for (u64 i = 0; sys_readdir(path, i, &ent) == 0; i++) {
        if (dirs_only_files && ent.is_dir)
            continue;
        if (prefix_ci(ent.name, base))
            cand_add(ent.name, (int)ent.is_dir);
    }
}

static int needs_quote(const char *s)
{
    for (; *s; s++)
        if (strchr(" \t|<>;&'\"$#\\", *s))
            return 1;
    return 0;
}

/* Vervollstaendigt das Wort vor dem Cursor. 0 = nichts zu tun, 1 = Zeile geaendert, 2 = mehrdeutig ohne Aenderung
 * (der Aufrufer zeigt die Liste). Danach steht die Kandidatenliste in cand[0..ncand). */
static int complete(char *buf, int *lenp, int *posp, int max)
{
    int len = *lenp, pos = *posp;
    int q = 0, in_word = 0, wstart = pos, expect_cmd = 1, is_cmd = 1;

    for (int i = 0; i < pos; i++) { /* Anfang des aktuellen Wortes und ob es ein Befehl ist */
        char c = buf[i];
        if (q) {
            if (c == q)
                q = 0;
            continue;
        }
        if (c == ' ' || c == '\t') {
            in_word = 0;
            continue;
        }
        if (c == '|' || c == ';' || c == '&') {
            in_word = 0;
            expect_cmd = 1;
            continue;
        }
        if (c == '<' || c == '>') {
            in_word = 0;
            expect_cmd = 0;
            continue;
        }
        if (!in_word) {
            in_word = 1;
            wstart = i;
            is_cmd = expect_cmd;
            expect_cmd = 0;
        }
        if (c == '\\' && i + 1 < pos)
            i++;
        else if (c == '\'' || c == '"')
            q = c;
    }
    if (!in_word) {
        wstart = pos;
        is_cmd = expect_cmd;
    }

    char part[LINE_MAX]; /* das Wort ohne Anfuehrungszeichen */
    int pn = 0, qq = 0;
    for (int i = wstart; i < pos && pn < (int)sizeof(part) - 1; i++) {
        char c = buf[i];
        if (qq) {
            if (c == qq)
                qq = 0;
            else
                part[pn++] = c;
        } else if (c == '\'' || c == '"') {
            qq = c;
        } else if (c == '\\' && i + 1 < pos) {
            part[pn++] = buf[++i];
        } else {
            part[pn++] = c;
        }
    }
    part[pn] = 0;

    ncand = cand_used = 0;
    char *slash = strrchr(part, '/');
    int dirlen = slash ? (int)(slash - part) + 1 : 0;
    const char *base = part + dirlen;
    if (is_cmd && !slash) {
        for (int i = 0; builtin_names[i]; i++)
            if (prefix_ci(builtin_names[i], base))
                cand_add(builtin_names[i], 0);
        char pathv[160];
        snprintf(pathv, sizeof(pathv), "%s", var_get("PATH"));
        for (char *d = pathv; d;) {
            char *next = strchr(d, ':');
            if (next)
                *next++ = 0;
            if (*d)
                gather_dir(d, base, 1);
            d = next;
        }
    } else {
        char dir[LINE_MAX];
        memcpy(dir, part, (size_t)dirlen);
        dir[dirlen] = 0;
        gather_dir(dir, base, 0);
    }
    if (!ncand)
        return 0;
    cand_sort();

    int lcp = (int)strlen(cand[0]);
    for (int i = 1; i < ncand; i++) {
        int k = 0;
        while (k < lcp && lower_c((unsigned char)cand[0][k]) == lower_c((unsigned char)cand[i][k]))
            k++;
        lcp = k;
    }

    char full[LINE_MAX];
    memcpy(full, part, (size_t)dirlen);
    memcpy(full + dirlen, cand[0], (size_t)lcp);
    full[dirlen + lcp] = 0;

    char repl[LINE_MAX + 8];
    int rn = 0, quote = needs_quote(full);
    char qc = (strchr(full, '"') || strchr(full, '$')) ? '\'' : '"';
    if (quote)
        repl[rn++] = qc;
    for (const char *f = full; *f && rn < (int)sizeof(repl) - 4; f++)
        repl[rn++] = *f;
    if (ncand == 1) {
        if (cand_dir[0]) {
            repl[rn++] = '/';
        } else {
            if (quote)
                repl[rn++] = qc;
            repl[rn++] = ' ';
        }
    }

    int rawlen = pos - wstart;
    if (rn == rawlen && memcmp(buf + wstart, repl, (size_t)rn) == 0)
        return ncand > 1 ? 2 : 0;
    if (len - rawlen + rn >= max)
        return 0;
    memmove(buf + wstart + rn, buf + pos, (size_t)(len - pos + 1));
    memcpy(buf + wstart, repl, (size_t)rn);
    *lenp = len - rawlen + rn;
    *posp = wstart + rn;
    return 1;
}

static void show_candidates(int commands)
{
    static char disp[8192 + MAX_CAND * 2];
    const char *names[MAX_CAND], *colors[MAX_CAND];
    int used = 0;
    for (int i = 0; i < ncand; i++) {
        size_t l = strlen(cand[i]);
        if (used + (int)l + 2 > (int)sizeof(disp))
            break;
        memcpy(disp + used, cand[i], l);
        int end = used + (int)l;
        if (cand_dir[i])
            disp[end++] = '/';
        disp[end++] = 0;
        names[i] = disp + used;
        colors[i] = cand_dir[i] ? C_BLUE : commands ? C_GREEN : "";
        used = end;
    }
    write_all(1, "\n", 1);
    print_columns(names, colors, ncand, term_width);
}

/* Liest eine Zeile im Rohmodus. Liefert die Laenge, -1 bei Ctrl-D auf leerer Zeile, -2 bei Ctrl-C. */
int edit_line(const char *prompt, char *buf, int max)
{
    int len = 0, pos = 0, h = hist_count;
    char saved[LINE_MAX];
    saved[0] = 0;
    buf[0] = 0;

    sys_tty_mode(1);
    redraw(prompt, buf, 0, 0, 0);
    for (;;) {
        unsigned char c;
        if (sys_read(0, &c, 1) <= 0) {
            sys_tty_mode(0);
            return -1;
        }
        int prev_len = u8count(buf, len), changed = 1;

        if (c == '\n') {
            write_all(1, "\n", 1);
            sys_tty_mode(0);
            return len;
        } else if (c == 3) { /* Ctrl-C */
            write_all(1, "^C\n", 3);
            buf[0] = 0;
            sys_tty_mode(0);
            return -2;
        } else if (c == 4 && len == 0) { /* Ctrl-D */
            sys_tty_mode(0);
            return -1;
        } else if (c == '\t') {
            int r = complete(buf, &len, &pos, max);
            if (r == 2) {
                show_candidates(0);
                prev_len = 0;
            }
            changed = r != 0;
            if (r == 2 || (r == 0 && ncand > 1))
                changed = 1;
        } else if ((c == 0x7F || c == '\b') && pos > 0) {
            int st = pos - 1; /* ein UTF-8-Zeichen zurueck */
            while (st > 0 && ((unsigned char)buf[st] & 0xC0) == 0x80)
                st--;
            memmove(buf + st, buf + pos, (size_t)(len - pos + 1));
            len -= pos - st;
            pos = st;
        } else if ((c == KEY_DEL || c == 4) && pos < len) {
            int l = utf8_len_at(buf + pos);
            memmove(buf + pos, buf + pos + l, (size_t)(len - pos - l + 1));
            len -= l;
        } else if (c == KEY_LEFT && pos > 0) {
            pos--;
            while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80)
                pos--;
        } else if (c == KEY_RIGHT && pos < len) {
            pos += utf8_len_at(buf + pos);
        } else if (c == KEY_HOME) {
            pos = 0;
        } else if (c == KEY_END) {
            pos = len;
        } else if (c == KEY_UP && h > 0) {
            if (h == hist_count)
                snprintf(saved, sizeof(saved), "%s", buf); /* die angefangene Zeile merken */
            h--;
            snprintf(buf, (size_t)max, "%s", history[h]);
            len = pos = (int)strlen(buf);
        } else if (c == KEY_DOWN && h < hist_count) {
            h++;
            snprintf(buf, (size_t)max, "%s", h == hist_count ? saved : history[h]);
            len = pos = (int)strlen(buf);
        } else if (((c >= 32 && c < 127) || (c >= 0x80 && c < 0xF5)) && len < max - 1) { /* auch UTF-8 (eingefuegt) */
            memmove(buf + pos + 1, buf + pos, (size_t)(len - pos + 1));
            buf[pos++] = (char)c;
            len++;
        } else {
            changed = 0;
        }
        if (changed)
            redraw(prompt, buf, len, pos, prev_len);
    }
}
