/* Shell: Ersetzen (Variablen, $(befehl), $((rechnung)), ~, Platzhalter) */

#include "libc.h"
#include "malloc.h"
#include "sh.h"

static s64 arith_or(void);
static void expand_into(const char *w, int *i, SB *b, int quoted);

/* ======================================================================================================================
 * Ersetzen: Variablen, $(befehl), $((rechnung)), ~, Platzhalter
 * ==================================================================================================================== */

void sb_put(SB *b, char c, unsigned char mask)
{
    if (b->len + 1 >= b->cap) {
        int cap = b->cap ? b->cap * 2 : 64;
        char *s = xmalloc((u64)cap);
        unsigned char *m = xmalloc((u64)cap);
        if (b->len) {
            memcpy(s, b->s, (size_t)b->len);
            memcpy(m, b->m, (size_t)b->len);
        }
        u_free(b->s);
        u_free(b->m);
        b->s = s;
        b->m = m;
        b->cap = cap;
    }
    b->s[b->len] = c;
    b->m[b->len] = mask;
    b->len++;
}

static void sb_puts(SB *b, const char *s, unsigned char mask)
{
    for (; *s; s++)
        sb_put(b, *s, mask);
}

void sb_free(SB *b)
{
    u_free(b->s);
    u_free(b->m);
    memset(b, 0, sizeof(*b));
}

int last_status;

int interactive;

/* ---- Rechnen: $((...)) ---- */

static const char *ap;

static int         aerr;

static void askip(void)
{
    while (*ap == ' ' || *ap == '\t' || *ap == '\n')
        ap++;
}

static s64 parse_num(const char **p)
{
    s64 v = 0;
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (*(*p)++ - '0');
    return v;
}

s64 str_to_num(const char *s)
{
    while (*s == ' ')
        s++;
    int neg = 0;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    s64 v = parse_num(&s);
    return neg ? -v : v;
}

static s64 arith_primary(void)
{
    askip();
    if (*ap == '(') {
        ap++;
        s64 v = arith_or();
        askip();
        if (*ap == ')')
            ap++;
        else
            aerr = 1;
        return v;
    }
    if (*ap == '-') { ap++; return -arith_primary(); }
    if (*ap == '+') { ap++; return arith_primary(); }
    if (*ap == '!') { ap++; return !arith_primary(); }
    if (*ap >= '0' && *ap <= '9')
        return parse_num(&ap);
    if (is_ident_start(*ap)) {
        char name[32];
        int n = 0;
        while (is_ident(*ap) && n < 31)
            name[n++] = *ap++;
        name[n] = 0;
        return str_to_num(var_get(name));
    }
    aerr = 1;
    return 0;
}

static s64 arith_mul(void)
{
    s64 v = arith_primary();
    for (;;) {
        askip();
        char c = *ap;
        if (c != '*' && c != '/' && c != '%')
            return v;
        ap++;
        s64 r = arith_primary();
        if ((c == '/' || c == '%') && r == 0) {
            aerr = 2;
            return 0;
        }
        v = c == '*' ? v * r : c == '/' ? v / r : v % r;
    }
}

static s64 arith_add(void)
{
    s64 v = arith_mul();
    for (;;) {
        askip();
        if (*ap == '+' && ap[1] != '+') { ap++; v += arith_mul(); }
        else if (*ap == '-') { ap++; v -= arith_mul(); }
        else return v;
    }
}

static s64 arith_rel(void)
{
    s64 v = arith_add();
    for (;;) {
        askip();
        if (ap[0] == '<' && ap[1] == '=') { ap += 2; v = v <= arith_add(); }
        else if (ap[0] == '>' && ap[1] == '=') { ap += 2; v = v >= arith_add(); }
        else if (ap[0] == '<') { ap++; v = v < arith_add(); }
        else if (ap[0] == '>') { ap++; v = v > arith_add(); }
        else return v;
    }
}

static s64 arith_eq(void)
{
    s64 v = arith_rel();
    for (;;) {
        askip();
        if (ap[0] == '=' && ap[1] == '=') { ap += 2; v = v == arith_rel(); }
        else if (ap[0] == '!' && ap[1] == '=') { ap += 2; v = v != arith_rel(); }
        else return v;
    }
}

static s64 arith_and(void)
{
    s64 v = arith_eq();
    for (;;) {
        askip();
        if (ap[0] == '&' && ap[1] == '&') { ap += 2; s64 r = arith_eq(); v = v && r; }
        else return v;
    }
}

static s64 arith_or(void)
{
    s64 v = arith_and();
    for (;;) {
        askip();
        if (ap[0] == '|' && ap[1] == '|') { ap += 2; s64 r = arith_and(); v = v || r; }
        else return v;
    }
}

static s64 arith_eval(const char *expr, int *err)
{
    ap = expr;
    aerr = 0;
    s64 v = arith_or();
    askip();
    if (*ap)
        aerr = 1;
    *err = aerr;
    return v;
}

/* ---- $(befehl): in einem Kindprozess ausfuehren und die Ausgabe einsammeln ---- */

static char *run_capture(const char *cmd)
{
    int fds[2];
    if (sys_pipe(fds) < 0)
        return xstrdup("");
    s64 pid = sys_fork();
    if (pid < 0) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        return xstrdup("");
    }
    if (pid == 0) {
        sys_close(fds[0]);
        sys_dup2(fds[1], 1);
        sys_close(fds[1]);
        interactive = 0;
        Arena a = {0};
        arena = &a;
        Node *n = parse_text(cmd);
        if (!n) {
            fprintf(2, "%ssh: $(...): %s%s\n", ERRC, parse_msg[0] ? parse_msg : "unvollstaendig", RSTC);
            sys_exit(2);
        }
        sys_exit(exec_node(n));
    }
    sys_close(fds[1]);
    int cap = 256, len = 0;
    char *out = xmalloc((u64)cap);
    for (;;) {
        if (len + 128 >= cap) {
            char *n = xmalloc((u64)cap * 2);
            memcpy(n, out, (size_t)len);
            u_free(out);
            out = n;
            cap *= 2;
        }
        s64 r = sys_read(fds[0], out + len, (u64)(cap - len - 1));
        if (r <= 0)
            break;
        len += (int)r;
    }
    sys_close(fds[0]);
    int code = 0;
    s64 st = sys_wait((int)pid, &code);
    last_status = st ? 128 + (int)st : code;
    while (len > 0 && out[len - 1] == '\n') /* Zeilenumbrueche am Ende entfallen */
        len--;
    out[len] = 0;
    return out;
}

/* Wort ohne Trennen und Platzhalter ersetzen (fuer Zuweisungen, Umleitungsziele, Rechnungen) */
char *expand_single(const char *w)
{
    SB b = {0};
    int i = 0;
    while (w[i])
        expand_into(w, &i, &b, 0);
    char *r = xmalloc((u64)b.len + 1);
    int n = 0;
    for (int k = 0; k < b.len; k++)
        if (b.m[k] != 5 && b.m[k] != 4)
            r[n++] = b.s[k];
        else if (b.m[k] == 4)
            r[n++] = ' ';
    r[n] = 0;
    sb_free(&b);
    return r;
}

static void put_value(SB *b, const char *v, int quoted)
{
    sb_puts(b, v, quoted ? 1 : 2);
}

/* Ersetzt das Konstrukt ab w[*i] == '$' */
static void expand_dollar(const char *w, int *i, SB *b, int quoted)
{
    int j = *i + 1;
    char tmp[32];
    if (w[j] == '(' && w[j + 1] == '(') { /* $((rechnung)) */
        int e = skip_group(w, j, '(', ')');
        if (e < 0) {
            *i = (int)strlen(w);
            return;
        }
        char *inner = xmalloc((u64)(e - j));
        int n = e - j - 4;
        if (n < 0)
            n = 0;
        memcpy(inner, w + j + 2, (size_t)n);
        inner[n] = 0;
        char *ex = expand_single(inner);
        int err;
        s64 v = arith_eval(ex, &err);
        if (err)
            fprintf(2, "%ssh: Rechenfehler in '%s'%s%s\n", ERRC, ex, err == 2 ? " (Division durch 0)" : "", RSTC);
        u_free(inner);
        u_free(ex);
        snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
        put_value(b, tmp, quoted);
        *i = e;
        return;
    }
    if (w[j] == '(') { /* $(befehl) */
        int e = skip_group(w, j, '(', ')');
        if (e < 0) {
            *i = (int)strlen(w);
            return;
        }
        char *cmd = xmalloc((u64)(e - j));
        memcpy(cmd, w + j + 1, (size_t)(e - j - 2));
        cmd[e - j - 2] = 0;
        char *out = run_capture(cmd);
        put_value(b, out, quoted);
        u_free(cmd);
        u_free(out);
        *i = e;
        return;
    }
    if (w[j] == '{') { /* ${NAME}, ${#NAME}, ${NAME:-wort}, ${NAME:=wort} */
        int e = skip_group(w, j, '{', '}');
        if (e < 0) {
            *i = (int)strlen(w);
            return;
        }
        char inner[256];
        int n = e - j - 2 < 255 ? e - j - 2 : 255;
        memcpy(inner, w + j + 1, (size_t)n);
        inner[n] = 0;
        *i = e;
        if (inner[0] == '#' && inner[1]) {
            const char *v = var_get(inner + 1);
            snprintf(tmp, sizeof(tmp), "%d", utf8_width(v));
            put_value(b, tmp, quoted);
            return;
        }
        char *op = strchr(inner, ':');
        char name[64];
        int nl = op ? (int)(op - inner) : n;
        if (nl > 63)
            nl = 63;
        memcpy(name, inner, (size_t)nl);
        name[nl] = 0;
        const char *v;
        if (strcmp(name, "?") == 0) {
            snprintf(tmp, sizeof(tmp), "%d", last_status);
            v = tmp;
        } else if (name[0] >= '1' && name[0] <= '9') {
            int k = atoi(name);
            v = k <= npos ? pos_args[k - 1] : "";
        } else {
            v = var_get(name);
        }
        if (op && (op[1] == '-' || op[1] == '=') && !v[0]) {
            char *def = expand_single(op + 2);
            if (op[1] == '=')
                var_set(name, def);
            put_value(b, def, quoted);
            u_free(def);
            return;
        }
        put_value(b, v, quoted);
        return;
    }
    const char *v = NULL;
    if (is_ident_start(w[j])) {
        char name[64];
        int n = 0;
        while (is_ident(w[j]) && n < 63)
            name[n++] = w[j++];
        name[n] = 0;
        v = var_get(name);
    } else if (w[j] == '?') {
        snprintf(tmp, sizeof(tmp), "%d", last_status);
        v = tmp;
        j++;
    } else if (w[j] == '$') {
        snprintf(tmp, sizeof(tmp), "%d", (int)sys_getpid());
        v = tmp;
        j++;
    } else if (w[j] == '#') {
        snprintf(tmp, sizeof(tmp), "%d", npos);
        v = tmp;
        j++;
    } else if (w[j] == '0') {
        v = arg0;
        j++;
    } else if (w[j] >= '1' && w[j] <= '9') {
        int k = w[j] - '0';
        v = k <= npos ? pos_args[k - 1] : "";
        j++;
    } else if (w[j] == '@' || w[j] == '*') {
        for (int k = 0; k < npos; k++) {
            if (k)
                sb_put(b, ' ', quoted ? (w[j] == '@' ? 4 : 1) : 2);
            put_value(b, pos_args[k], quoted);
        }
        if (quoted && !npos && w[j] == '*')
            sb_put(b, 0, 5);
        *i = j + 1;
        return;
    } else {
        sb_put(b, '$', quoted ? 1 : 0); /* einzelnes '$' */
        *i = j;
        return;
    }
    put_value(b, v, quoted);
    *i = j;
}

/* Verarbeitet ein Stueck des Wortes ab w[*i] */
static void expand_into(const char *w, int *i, SB *b, int quoted)
{
    char c = w[*i];
    (void)quoted;
    if (c == '~' && *i == 0 && (w[1] == '/' || w[1] == 0)) {
        sb_puts(b, var_get("HOME"), 1);
        (*i)++;
    } else if (c == '\\') {
        if (w[*i + 1] == '\n') {
            *i += 2;
        } else if (w[*i + 1]) {
            sb_put(b, w[*i + 1], 1);
            *i += 2;
        } else {
            (*i)++;
        }
    } else if (c == '\'') {
        sb_put(b, 0, 5);
        (*i)++;
        while (w[*i] && w[*i] != '\'')
            sb_put(b, w[(*i)++], 1);
        if (w[*i])
            (*i)++;
    } else if (c == '"') {
        sb_put(b, 0, 5);
        (*i)++;
        while (w[*i] && w[*i] != '"') {
            char d = w[*i];
            if (d == '\\' && (w[*i + 1] == '$' || w[*i + 1] == '"' || w[*i + 1] == '\\' || w[*i + 1] == '`')) {
                sb_put(b, w[*i + 1], 1);
                *i += 2;
            } else if (d == '\\' && w[*i + 1] == '\n') {
                *i += 2;
            } else if (d == '$') {
                expand_dollar(w, i, b, 1);
            } else if (d == '`') {
                int e = *i + 1;
                while (w[e] && w[e] != '`')
                    e++;
                char *cmd = xmalloc((u64)(e - *i));
                memcpy(cmd, w + *i + 1, (size_t)(e - *i - 1));
                cmd[e - *i - 1] = 0;
                char *out = run_capture(cmd);
                sb_puts(b, out, 1);
                u_free(cmd);
                u_free(out);
                *i = w[e] ? e + 1 : e;
            } else {
                sb_put(b, d, 1);
                (*i)++;
            }
        }
        if (w[*i])
            (*i)++;
    } else if (c == '$') {
        expand_dollar(w, i, b, 0);
    } else if (c == '`') {
        int e = *i + 1;
        while (w[e] && w[e] != '`')
            e++;
        char *cmd = xmalloc((u64)(e - *i));
        memcpy(cmd, w + *i + 1, (size_t)(e - *i - 1));
        cmd[e - *i - 1] = 0;
        char *out = run_capture(cmd);
        sb_puts(b, out, 2);
        u_free(cmd);
        u_free(out);
        *i = w[e] ? e + 1 : e;
    } else {
        sb_put(b, c, 0);
        (*i)++;
    }
}

/* ---- Platzhalter: * ? [abc] ---- */

static int glob_match(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p)
                return 1;
            for (; *s; s++)
                if (glob_match(p, s))
                    return 1;
            return glob_match(p, s);
        }
        if (!*s)
            return 0;
        if (*p == '?') {
            s += utf8_len_at(s);
            p++;
            continue;
        }
        if (*p == '[') {
            const char *q = p + 1;
            int neg = *q == '!' || *q == '^', ok = 0;
            if (neg)
                q++;
            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2])
                        ok = 1;
                    q += 3;
                } else {
                    if (*s == *q)
                        ok = 1;
                    q++;
                }
            }
            if (!*q) { /* keine schliessende Klammer: '[' woertlich */
                if (*s != '[')
                    return 0;
                p++;
                s++;
                continue;
            }
            if (ok == neg)
                return 0;
            p = q + 1;
            s++;
            continue;
        }
        if (*p != *s)
            return 0;
        p++;
        s++;
    }
    return *s == 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void sort_strings(char **v, int n)
{
    for (int i = 1; i < n; i++) {
        char *x = v[i];
        int j = i - 1;
        while (j >= 0 && cmp_str(&v[j], &x) > 0) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
}

/* Feld mit unquotierten Platzhaltern gegen die Dateien im Verzeichnis pruefen. Liefert die Zahl der Treffer. */
static int glob_field(const char *s, const unsigned char *m, int len, Vec *out)
{
    int wild = 0;
    for (int k = 0; k < len; k++)
        if ((m[k] == 0 || m[k] == 2) && (s[k] == '*' || s[k] == '?' || s[k] == '['))
            wild = 1;
    if (!wild)
        return 0;
    char word[PATH_MAX];
    int n = len < 511 ? len : 511;
    memcpy(word, s, (size_t)n);
    word[n] = 0;
    char *slash = strrchr(word, '/');
    char dir[PATH_MAX];
    const char *pat;
    if (slash) {
        int dl = (int)(slash - word);
        memcpy(dir, word, (size_t)dl);
        dir[dl] = 0;
        if (!dl) {
            dir[0] = '/';
            dir[1] = 0;
        }
        pat = slash + 1;
        if (strchr(dir, '*') || strchr(dir, '?'))
            return 0; /* Platzhalter nur im letzten Teil des Pfades */
    } else {
        dir[0] = 0;
        pat = word;
    }
    DirEnt ent;
    int found = 0, first = out->n;
    for (u64 i = 0; sys_readdir(dir[0] ? dir : ".", i, &ent) == 0; i++) {
        if (ent.name[0] == '.' && pat[0] != '.')
            continue;
        if (!glob_match(pat, ent.name))
            continue;
        char full[768];
        if (slash)
            snprintf(full, sizeof(full), "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", ent.name);
        else
            snprintf(full, sizeof(full), "%s", ent.name);
        vec_push(out, xstrdup(full));
        found++;
    }
    if (found)
        sort_strings((char **)out->v + first, found);
    return found;
}

/* Ersetzt ein Wort komplett: trennt an Leerzeichen aus Ersetzungen und wendet Platzhalter an. Haengt die Felder an out an
 * (Zeichenketten auf dem Heap). */
void expand_fields(const char *w, Vec *out)
{
    SB b = {0};
    int i = 0;
    while (w[i])
        expand_into(w, &i, &b, 0);

    SB f = {0};
    int present = 0;
    for (int k = 0; k <= b.len; k++) {
        int end = k == b.len;
        int split = !end && ((b.m[k] == 2 && (b.s[k] == ' ' || b.s[k] == '\t' || b.s[k] == '\n')) || b.m[k] == 4);
        if (end || split) {
            if (present || f.len) {
                if (!glob_field(f.s, f.m, f.len, out)) {
                    char *s = xmalloc((u64)f.len + 1);
                    memcpy(s, f.s, (size_t)f.len);
                    s[f.len] = 0;
                    vec_push(out, s);
                }
            }
            f.len = 0;
            present = 0;
            if (!end && b.m[k] == 4)
                present = 1; /* "$@": auch leere Parameter bleiben Felder */
            continue;
        }
        if (b.m[k] == 5) {
            present = 1;
            continue;
        }
        sb_put(&f, b.s[k], b.m[k]);
    }
    sb_free(&f);
    sb_free(&b);
}

void free_strings(Vec *v)
{
    for (int i = 0; i < v->n; i++)
        u_free(v->v[i]);
    u_free(v->v);
    memset(v, 0, sizeof(*v));
}
