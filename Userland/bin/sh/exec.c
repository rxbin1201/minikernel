/* Shell: Umleitungen, Jobs, Ausfuehren */

#include "libc.h"
#include "malloc.h"
#include "sh.h"

static int  next_job_id = 1;

/* ======================================================================================================================
 * Umleitungen
 * ==================================================================================================================== */

/* Fuehrt die Umleitungen aus. save != NULL: vorher die Deskriptoren 0..2 sichern (fuer eingebaute Befehle). 0 = ok */
static int apply_redirs(Redir *r, int save[3])
{
    for (; r; r = r->next) {
        if (save && r->fd <= 2 && save[r->fd] < 0)
            save[r->fd] = (int)sys_dup(r->fd);
        if (r->type == R_DUP) {
            sys_dup2(r->dupfd, r->fd);
            continue;
        }
        char *path = expand_single(r->target);
        int flags = r->type == R_IN ? O_RDONLY : O_WRONLY | O_CREAT | (r->type == R_APPEND ? O_APPEND : O_TRUNC);
        s64 fd = sys_open(path, flags);
        if (fd < 0) {
            fprintf(2, "%ssh: '%s': %s (Fehler %d)%s\n", ERRC, path,
                    r->type == R_IN ? "nicht gefunden" : "kann nicht geschrieben werden", (int)fd, RSTC);
            u_free(path);
            return -1;
        }
        u_free(path);
        if ((int)fd != r->fd) {
            sys_dup2((int)fd, r->fd);
            sys_close((int)fd);
        }
    }
    return 0;
}

static void restore_redirs(int save[3])
{
    for (int k = 0; k < 3; k++) {
        if (save[k] >= 0) {
            sys_dup2(save[k], k);
            sys_close(save[k]);
            save[k] = -1;
        }
    }
}

/* ---------- Jobs ---------- */

static Job *job_add(int pgid, const int *pids, int npids, const char *desc)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) {
            Job *j = &jobs[i];
            memset(j, 0, sizeof(*j));
            j->used = 1;
            j->id = next_job_id++;
            j->pgid = pgid;
            j->npids = npids;
            for (int k = 0; k < npids; k++)
                j->pids[k] = pids[k];
            snprintf(j->desc, sizeof(j->desc), "%s", desc);
            return j;
        }
    }
    return NULL;
}

/* Sammelt beendete Hintergrundjobs ein und meldet sie */
void reap_jobs(void)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (!j->used)
            continue;
        int alive = 0, state = 0;
        for (int k = 0; k < j->npids; k++) {
            if (!j->pids[k])
                continue;
            int code = 0;
            s64 r = sys_wait_nohang(j->pids[k], &code);
            if (r == ERR_AGAIN) {
                alive = 1;
            } else {
                if (r > state)
                    state = (int)r;
                j->pids[k] = 0;
            }
        }
        if (!alive) {
            if (interactive)
                printf("%s[%d]  %s%s%s  %s\n", tty_out ? C_DIM : "", j->id, tty_out ? (state ? C_RED : C_GREEN) : "",
                       state == 1 ? "Abgestuerzt" : state == 2 ? "Abgebrochen" : "Fertig", tty_out ? C_RESET : "",
                       j->desc);
            j->used = 0;
        }
    }
}

Job *job_find(const char *arg)
{
    if (arg) {
        int id = atoi(arg[0] == '%' ? arg + 1 : arg);
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used && jobs[i].id == id)
                return &jobs[i];
        return NULL;
    }
    Job *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used && (!best || jobs[i].id > best->id))
            best = &jobs[i];
    return best;
}

/* Wartet auf alle Prozesse des Jobs; mit fg = 1 bekommt er das Terminal (Ctrl-C) */
int job_wait(Job *j, int fg)
{
    if (fg)
        sys_tty_fg(j->pgid);
    int status = 0, interrupted = 0;
    for (int k = 0; k < j->npids; k++) {
        if (!j->pids[k])
            continue;
        int code = 0;
        s64 st = sys_wait(j->pids[k], &code);
        if (st == 2)
            interrupted = 1;
        status = st > 0 ? 128 + (int)st : code;
        j->pids[k] = 0;
    }
    if (fg)
        sys_tty_fg(0);
    if (interrupted)
        write_all(1, "^C\n", 3);
    j->used = 0;
    return status;
}

/* ======================================================================================================================
 * Ausfuehren
 * ==================================================================================================================== */

int interrupted;             /* eine Vordergrundgruppe wurde mit Strg+C beendet: den Rest der Eingabe abbrechen */

int brk, cont, ret_flag, ret_status, loop_depth, func_depth;

static int stop_requested(void) { return interrupted || brk || cont || ret_flag; }

static void append_arg(char *out, int *np, int max, const char *a)
{
    int n = *np;
    int quote = a[0] == 0 || strchr(a, ' ') != NULL || strchr(a, '\t') != NULL;
    char q = strchr(a, '"') ? '\'' : '"';
    if (n && n < max - 1)
        out[n++] = ' ';
    if (quote && n < max - 1)
        out[n++] = q;
    for (; *a && n < max - 3; a++)
        out[n++] = *a;
    if (quote && n < max - 2)
        out[n++] = q;
    out[n] = 0;
    *np = n;
}

/* Sucht ein Programm: mit '/' im Namen wie angegeben, sonst in den Verzeichnissen von PATH; jeweils auch mit ".sh" */
int resolve_command(const char *name, char *path, int max)
{
    static const char *const suffix[] = {"", ".sh"};
    Stat st;
    if (strchr(name, '/')) {
        for (int k = 0; k < 2; k++) {
            snprintf(path, (size_t)max, "%s%s", name, suffix[k]);
            if (sys_stat(path, &st) == 0 && !st.is_dir)
                return 0;
        }
        return -1;
    }
    char pathv[256];
    snprintf(pathv, sizeof(pathv), "%s", var_get("PATH"));
    for (char *d = pathv; d;) {
        char *next = strchr(d, ':');
        if (next)
            *next++ = 0;
        if (*d) {
            for (int k = 0; k < 2; k++) {
                snprintf(path, (size_t)max, "%s/%s%s", d, name, suffix[k]);
                if (sys_stat(path, &st) == 0 && !st.is_dir)
                    return 0;
            }
        }
        d = next;
    }
    return -1;
}

static int is_script(const char *path)
{
    size_t l = strlen(path);
    if (l > 3 && strcmp(path + l - 3, ".sh") == 0)
        return 1;
    s64 fd = sys_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    char m[2];
    int ok = sys_read((int)fd, m, 2) == 2 && m[0] == '#' && m[1] == '!';
    sys_close((int)fd);
    return ok;
}

/* Programm starten (im Kindprozess). Kehrt nicht zurueck. */
static void exec_external(int argc, char **argv)
{
    char path[512];
    static char cmdline[CMD_MAX]; /* im Kindprozess, kurz vor exec: statisch statt auf dem Stack */
    if (resolve_command(argv[0], path, sizeof(path)) != 0) {
        fprintf(2, "%ssh: %s: Befehl nicht gefunden%s\n", ERRC, argv[0], RSTC);
        sys_exit(127);
    }
    int n = 0;
    cmdline[0] = 0;
    if (is_script(path)) { /* Skripte laufen in einer eigenen Shell */
        append_arg(cmdline, &n, sizeof(cmdline), "sh");
        append_arg(cmdline, &n, sizeof(cmdline), path);
        for (int i = 1; i < argc; i++)
            append_arg(cmdline, &n, sizeof(cmdline), argv[i]);
        sys_exec("/bin/sh", cmdline);
    } else {
        for (int i = 0; i < argc; i++)
            append_arg(cmdline, &n, sizeof(cmdline), argv[i]);
        sys_exec(path, cmdline);
    }
    fprintf(2, "%ssh: %s: kann nicht gestartet werden%s\n", ERRC, argv[0], RSTC);
    sys_exit(126);
}

/* Wartet auf Kindprozesse im Vordergrund (interaktiv mit eigener Prozessgruppe fuer Strg+C) */
static int wait_fg(int *pids, int n, int pgid)
{
    if (interactive && n)
        sys_tty_fg(pgid);
    int status = 0, was_interrupted = 0;
    for (int i = 0; i < n; i++) {
        int code = 0;
        s64 state = sys_wait(pids[i], &code);
        if (state == 1)
            fprintf(2, "%s[Prozess %d abgestuerzt]%s\n", ERRC, pids[i], RSTC);
        if (state == 2)
            was_interrupted = 1;
        if (i == n - 1)
            status = state ? 128 + (int)state : code;
    }
    if (interactive) {
        sys_tty_fg(0);
        if (was_interrupted) {
            write_all(1, "^C\n", 3);
            interrupted = 1;
        }
    }
    return status;
}

static int spawn_external(int argc, char **argv, Redir *redirs)
{
    s64 pid = sys_fork();
    if (pid < 0) {
        fprintf(2, "%ssh: fork fehlgeschlagen (Fehler %d)%s\n", ERRC, (int)pid, RSTC);
        return 1;
    }
    if (pid == 0) {
        if (apply_redirs(redirs, NULL) != 0)
            sys_exit(1);
        exec_external(argc, argv);
    }
    int p = (int)pid;
    if (interactive)
        sys_setpgid(p, p);
    return wait_fg(&p, 1, p);
}

static int call_function(Func *f, int argc, char **argv)
{
    if (func_depth >= 100) {
        fprintf(2, "%ssh: %s: zu tief verschachtelt (Endlos-Rekursion?)%s\n", ERRC, f->name, RSTC);
        return 1;
    }
    char **old_args = pos_args;
    int old_n = npos;
    pos_args = NULL;
    npos = 0;
    set_positional(argc - 1, argv + 1);
    func_depth++;
    int st = exec_node(f->body);
    func_depth--;
    if (ret_flag) {
        ret_flag = 0;
        st = ret_status;
    }
    set_positional(0, NULL);
    u_free(pos_args);
    pos_args = old_args;
    npos = old_n;
    return st;
}

/* Einfacher Befehl. in_child = 1: laeuft schon im Kindprozess (Pipeline): Programme direkt mit exec starten. */
static int run_simple(Node *n, int in_child)
{
    Vec args = {0};
    for (int i = 0; i < n->nwords; i++)
        expand_fields(n->words[i], &args);
    for (int i = 0; i < n->nassign; i++) {
        char *eq = strchr(n->assigns[i], '=');
        char name[32];
        int nl = (int)(eq - n->assigns[i]);
        if (nl > 31)
            nl = 31;
        memcpy(name, n->assigns[i], (size_t)nl);
        name[nl] = 0;
        char *v = expand_single(eq + 1);
        var_set(name, v);
        u_free(v);
    }
    int st = 0;
    if (args.n == 0) { /* nur Zuweisungen und/oder Umleitungen (z.B. "> datei" legt sie an) */
        if (n->redirs) {
            int save[3] = {-1, -1, -1};
            st = apply_redirs(n->redirs, in_child ? NULL : save) != 0;
            if (!in_child)
                restore_redirs(save);
        } else if (n->nassign) {
            st = last_status; /* Status einer $(...)-Ersetzung */
        }
        vec_push(&args, NULL);
        free_strings(&args);
        return st;
    }
    vec_push(&args, NULL);
    int argc = args.n - 1;
    char **argv = (char **)args.v;

    Func *f = func_find(argv[0]);
    if (f || is_builtin(argv[0])) {
        int save[3] = {-1, -1, -1};
        if (apply_redirs(n->redirs, in_child ? NULL : save) != 0) {
            st = 1;
        } else {
            int old_tty = tty_out;
            tty_out = sys_isatty(1) != 0; /* Ausgabe in eine Datei: keine Farben */
            st = f ? call_function(f, argc, argv) : run_builtin(argc, argv);
            tty_out = old_tty;
        }
        if (!in_child)
            restore_redirs(save);
    } else if (in_child) {
        if (apply_redirs(n->redirs, NULL) != 0)
            sys_exit(1);
        exec_external(argc, argv);
    } else {
        st = spawn_external(argc, argv, n->redirs);
    }
    free_strings(&args);
    return st;
}

static void set_status(int st) { last_status = st; }

/* Pipeline im Vorder- oder Hintergrund. desc = Text fuer 'jobs'. */
static int exec_pipe(Node *n, int bg, const char *desc)
{
    if (n->nitems == 1 && !bg) {
        int st = exec_node(n->items[0]);
        return n->negate ? !st : st;
    }
    int pids[MAX_CMDS], started = 0, first = 0, prev_read = -1;
    for (int i = 0; i < n->nitems && i < MAX_CMDS; i++) {
        int fds[2] = {-1, -1};
        if (i < n->nitems - 1 && sys_pipe(fds) < 0) {
            fprintf(2, "%ssh: keine Pipe mehr moeglich%s\n", ERRC, RSTC);
            break;
        }
        s64 pid = sys_fork();
        if (pid < 0) {
            fprintf(2, "%ssh: fork fehlgeschlagen (Fehler %d)%s\n", ERRC, (int)pid, RSTC);
            if (fds[0] >= 0) {
                sys_close(fds[0]);
                sys_close(fds[1]);
            }
            break;
        }
        if (pid == 0) { /* Kind: Pipes verdrahten und die Stufe ausfuehren */
            interactive = 0;
            if (prev_read >= 0) {
                sys_dup2(prev_read, 0);
                sys_close(prev_read);
            } else if (bg) {
                sys_close(0); /* Hintergrundjobs lesen nicht von der Tastatur */
            }
            if (fds[1] >= 0) {
                sys_dup2(fds[1], 1);
                sys_close(fds[1]);
                sys_close(fds[0]);
            }
            Node *stage = n->items[i];
            int st = stage->kind == K_SIMPLE ? run_simple(stage, 1) : exec_node(stage);
            sys_exit(st);
        }
        pids[started++] = (int)pid;
        if (i == 0)
            first = (int)pid;
        if (interactive)
            sys_setpgid((int)pid, first); /* alle Stufen einer Zeile bilden eine Prozessgruppe */
        if (prev_read >= 0)
            sys_close(prev_read);
        if (fds[1] >= 0)
            sys_close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read >= 0)
        sys_close(prev_read);
    if (!started)
        return 1;
    if (bg) {
        Job *j = job_add(first, pids, started, desc ? desc : "");
        if (j && interactive)
            printf("%s[%d] %d%s\n", tty_out ? C_DIM : "", j->id, first, tty_out ? C_RESET : "");
        else if (!j)
            wait_fg(pids, started, first);
        return 0;
    }
    int st = wait_fg(pids, started, first);
    return n->negate ? !st : st;
}

/* Eintrag einer Liste mit '&' */
static int run_background(Node *item, const char *desc)
{
    if (item->kind == K_ANDOR && item->nitems == 1 && item->items[0]->kind == K_PIPE)
        return exec_pipe(item->items[0], 1, desc);
    s64 pid = sys_fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        interactive = 0;
        sys_close(0);
        sys_exit(exec_node(item));
    }
    int p = (int)pid;
    if (interactive)
        sys_setpgid(p, p);
    Job *j = job_add(p, &p, 1, desc);
    if (j && interactive)
        printf("%s[%d] %d%s\n", tty_out ? C_DIM : "", j->id, p, tty_out ? C_RESET : "");
    return 0;
}

static int exec_loop_body(Node *body, int *st)
{
    *st = exec_node(body);
    if (brk) {
        brk--;
        return 1; /* Schleife verlassen */
    }
    if (cont) {
        cont--;
        return cont ? 1 : 0; /* continue n > 1: auch die aeussere Schleife weiterschalten */
    }
    return interrupted || ret_flag;
}

int exec_node(Node *n)
{
    if (!n)
        return 0;
    int st = 0;
    switch (n->kind) {
    case K_LIST:
        for (int i = 0; i < n->nitems && !stop_requested(); i++) {
            if (n->ops[i] == '&')
                st = run_background(n->items[i], n->texts[i]);
            else
                st = exec_node(n->items[i]);
            set_status(st);
        }
        return st;
    case K_ANDOR:
        st = exec_node(n->items[0]);
        set_status(st);
        for (int i = 1; i < n->nitems && !stop_requested(); i++) {
            char op = n->ops[i - 1];
            if ((op == 'a' && st == 0) || (op == 'o' && st != 0)) {
                st = exec_node(n->items[i]);
                set_status(st);
            }
        }
        return st;
    case K_PIPE:
        return exec_pipe(n, 0, NULL);
    case K_SIMPLE:
        st = run_simple(n, 0);
        set_status(st);
        return st;
    case K_FUNC: {
        Func *f = func_find(n->name);
        for (int i = 0; !f && i < 64; i++)
            if (!funcs[i].name[0])
                f = &funcs[i];
        if (!f) {
            fprintf(2, "%ssh: zu viele Funktionen%s\n", ERRC, RSTC);
            return 1;
        }
        snprintf(f->name, sizeof(f->name), "%s", n->name);
        f->body = n->body;
        arena->keep = 1; /* der Baum muss erhalten bleiben */
        return 0;
    }
    default:
        break;
    }

    /* zusammengesetzte Befehle: Umleitungen gelten fuer den ganzen Block */
    int save[3] = {-1, -1, -1};
    if (n->redirs && apply_redirs(n->redirs, save) != 0) {
        restore_redirs(save);
        return 1;
    }
    switch (n->kind) {
    case K_IF: {
        int done = 0;
        for (int k = 0; k < n->nconds && !done && !stop_requested(); k++) {
            if (exec_node(n->conds[k]) == 0) {
                st = exec_node(n->bodies[k]);
                done = 1;
            }
        }
        if (!done && !stop_requested())
            st = n->els ? exec_node(n->els) : 0;
        break;
    }
    case K_WHILE:
        loop_depth++;
        for (;;) {
            int c = exec_node(n->cond);
            if (stop_requested() || ((c == 0) == n->until))
                break;
            if (exec_loop_body(n->body, &st))
                break;
        }
        loop_depth--;
        break;
    case K_FOR: {
        Vec items = {0};
        if (n->has_in) {
            for (int i = 0; i < n->nlist; i++)
                expand_fields(n->list[i], &items);
        } else {
            for (int i = 0; i < npos; i++)
                vec_push(&items, xstrdup(pos_args[i]));
        }
        loop_depth++;
        for (int i = 0; i < items.n && !stop_requested(); i++) {
            var_set(n->var, (char *)items.v[i]);
            if (exec_loop_body(n->body, &st))
                break;
        }
        loop_depth--;
        free_strings(&items);
        break;
    }
    case K_BRACE:
        st = exec_node(n->body);
        break;
    case K_SUB: {
        s64 pid = sys_fork();
        if (pid == 0) {
            interactive = 0;
            sys_exit(exec_node(n->body));
        }
        int code = 0;
        s64 state = pid > 0 ? sys_wait((int)pid, &code) : 1;
        st = pid < 0 ? 1 : state ? 128 + (int)state : code;
        break;
    }
    default:
        break;
    }
    restore_redirs(save);
    set_status(st);
    return st;
}

/* Liest eine Datei und fuehrt sie aus */
int run_script(const char *path)
{
    static int depth;
    if (depth >= 8) {
        fprintf(2, "%ssh: Skripte zu tief verschachtelt%s\n", ERRC, RSTC);
        return 1;
    }
    Stat st;
    s64 fd = sys_stat(path, &st) == 0 && !st.is_dir ? sys_open(path, O_RDONLY) : -1;
    if (fd < 0) {
        fprintf(2, "%ssh: %s: nicht gefunden%s\n", ERRC, path, RSTC);
        return 127;
    }
    char *text = xmalloc(st.size + 1);
    u64 got = 0;
    while (got < st.size) {
        s64 r = sys_read((int)fd, text + got, st.size - got);
        if (r <= 0)
            break;
        got += (u64)r;
    }
    sys_close((int)fd);
    text[got] = 0;
    for (u64 i = 0; i < got; i++) /* Windows-Zeilenenden */
        if (text[i] == '\r' && text[i + 1] == '\n')
            text[i] = ' ';

    Arena *saved = arena;
    Arena *a = xmalloc(sizeof(Arena));
    memset(a, 0, sizeof(*a));
    arena = a;
    Node *n = parse_text(text);
    int status;
    if (!n) {
        if (parse_incomplete)
            fprintf(2, "%ssh: %s: unerwartetes Dateiende (fehlt 'fi', 'done', '}' oder ein Anfuehrungszeichen?)%s\n", ERRC,
                    path, RSTC);
        else
            fprintf(2, "%ssh: %s: %s%s\n", ERRC, path, parse_msg, RSTC);
        status = 2;
    } else {
        depth++;
        status = exec_node(n);
        depth--;
    }
    if (!a->keep) {
        arena_free(a);
        u_free(a);
    }
    arena = saved;
    u_free(text);
    return status;
}
