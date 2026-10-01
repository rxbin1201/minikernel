/* Shell: eingebaute Befehle */

#include "libc.h"
#include "malloc.h"
#include "sh.h"

/* ======================================================================================================================
 * Eingebaute Befehle
 * ==================================================================================================================== */

static const char *const builtin_list[] = {"cd", "pwd", "exit", "history", "help", "set", "unset", "export", "jobs", "fg",
                                           "wait", "source", ".", "clear", "true", "false", ":", "echo", "test", "[",
                                           "read", "shift", "return", "break", "continue", "type", 0};

int is_builtin(const char *name)
{
    for (int i = 0; builtin_list[i]; i++)
        if (strcmp(name, builtin_list[i]) == 0)
            return 1;
    return 0;
}

static int t_unary(const char *op, const char *x)
{
    Stat st;
    if (strcmp(op, "-z") == 0) return x[0] == 0;
    if (strcmp(op, "-n") == 0) return x[0] != 0;
    int ok = sys_stat(x, &st) == 0;
    if (strcmp(op, "-e") == 0 || strcmp(op, "-r") == 0 || strcmp(op, "-w") == 0 || strcmp(op, "-x") == 0) return ok;
    if (strcmp(op, "-f") == 0) return ok && !st.is_dir;
    if (strcmp(op, "-d") == 0) return ok && st.is_dir;
    if (strcmp(op, "-s") == 0) return ok && st.size > 0;
    return -1;
}

static int t_binary(const char *x, const char *op, const char *y)
{
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return strcmp(x, y) == 0;
    if (strcmp(op, "!=") == 0) return strcmp(x, y) != 0;
    if (strcmp(op, "<") == 0) return strcmp(x, y) < 0;
    if (strcmp(op, ">") == 0) return strcmp(x, y) > 0;
    s64 a = str_to_num(x), b = str_to_num(y);
    if (strcmp(op, "-eq") == 0) return a == b;
    if (strcmp(op, "-ne") == 0) return a != b;
    if (strcmp(op, "-lt") == 0) return a < b;
    if (strcmp(op, "-le") == 0) return a <= b;
    if (strcmp(op, "-gt") == 0) return a > b;
    if (strcmp(op, "-ge") == 0) return a >= b;
    return -1;
}

static int t_expr(char **a, int n)
{
    for (int i = n - 1; i > 0; i--) { /* -o bindet schwaecher als -a */
        if (strcmp(a[i], "-o") == 0) {
            int l = t_expr(a, i), r = t_expr(a + i + 1, n - i - 1);
            return l < 0 || r < 0 ? -1 : (l || r);
        }
    }
    for (int i = n - 1; i > 0; i--) {
        if (strcmp(a[i], "-a") == 0) {
            int l = t_expr(a, i), r = t_expr(a + i + 1, n - i - 1);
            return l < 0 || r < 0 ? -1 : (l && r);
        }
    }
    if (n == 0)
        return 0;
    if (strcmp(a[0], "!") == 0) {
        int r = t_expr(a + 1, n - 1);
        return r < 0 ? -1 : !r;
    }
    if (n == 3 && strcmp(a[0], "(") == 0 && strcmp(a[2], ")") == 0)
        return a[1][0] != 0;
    if (n == 1)
        return a[0][0] != 0;
    if (n == 2)
        return t_unary(a[0], a[1]);
    if (n == 3)
        return t_binary(a[0], a[1], a[2]);
    return -1;
}

static int builtin_test(int argc, char **argv)
{
    int n = argc - 1;
    if (strcmp(argv[0], "[") == 0) {
        if (n < 1 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(2, "%s[: ']' fehlt%s\n", ERRC, RSTC);
            return 2;
        }
        n--;
    }
    int r = t_expr(argv + 1, n);
    if (r < 0) {
        fprintf(2, "%stest: unverstaendlicher Ausdruck%s\n", ERRC, RSTC);
        return 2;
    }
    return r ? 0 : 1;
}

/* ---- echo ---- */

static int builtin_echo(int argc, char **argv)
{
    int newline = 1, escapes = 0, first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        int known = 1;
        for (const char *q = argv[first] + 1; *q; q++)
            if (*q != 'n' && *q != 'e')
                known = 0;
        if (!known)
            break;
        for (const char *q = argv[first] + 1; *q; q++) {
            if (*q == 'n')
                newline = 0;
            else
                escapes = 1;
        }
    }
    SB out = {0};
    for (int i = first; i < argc; i++) {
        if (i > first)
            sb_put(&out, ' ', 0);
        for (const char *s = argv[i]; *s; s++) {
            char c = *s;
            if (escapes && c == '\\' && s[1]) {
                s++;
                switch (*s) {
                case 'e': c = 0x1B; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '\\': c = '\\'; break;
                default:
                    sb_put(&out, '\\', 0);
                    c = *s;
                }
            }
            sb_put(&out, c, 0);
        }
    }
    if (newline)
        sb_put(&out, '\n', 0);
    int r = out.len ? write_all(1, out.s, (size_t)out.len) : 0;
    sb_free(&out);
    return r ? 1 : 0;
}

/* ---- read ---- */

static int builtin_read(int argc, char **argv)
{
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-p") == 0) {
        write_all(1, argv[i + 1], strlen(argv[i + 1]));
        i += 2;
    }
    char line[1024];
    int n = 0, got_any = 0;
    for (;;) {
        char c;
        s64 r = sys_read(0, &c, 1);
        if (r <= 0)
            break;
        got_any = 1;
        if (c == '\n')
            break;
        if (n < (int)sizeof(line) - 1)
            line[n++] = c;
    }
    line[n] = 0;
    if (n && line[n - 1] == '\r')
        line[--n] = 0;
    if (i >= argc) {
        var_set("REPLY", line);
        return got_any ? 0 : 1;
    }
    char *p = line;
    for (; i < argc; i++) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (i == argc - 1) { /* die letzte Variable bekommt den Rest */
            char *e = p + strlen(p);
            while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = 0;
            var_set(argv[i], p);
            break;
        }
        char *e = p;
        while (*e && *e != ' ' && *e != '\t')
            e++;
        char save = *e;
        *e = 0;
        var_set(argv[i], p);
        *e = save;
        p = e;
    }
    return got_any ? 0 : 1;
}

static void print_help(void)
{
    const char *h = tty_out ? C_CYAN : "", *r = tty_out ? C_RESET : "";
    printf("%sEingebaut:%s  cd  pwd  exit [n]  echo [-n] [-e]  test/[ ]  read [-p text] VAR..  set  export N=w  unset N\n"
           "            shift [n]  return [n]  break [n]  continue [n]  type NAME  source datei  history  clear\n"
           "            jobs  fg [n]  wait  true  false  help\n"
           "%sProgramme:%s  ls  cat  less  edit  cp  mv  rm  mkdir  touch  wc  grep  head  tail  sort  uniq  diff  find  du\n"
           "            df  tree  hexdump  cal  uptime  seq  sleep  date  ps  kill  mount  lsusb  keymap  resolution\n"
           "            mouse  cpus  burn  dmesg  poweroff  reboot  (siehe 'ls /bin')\n"
           "%sGrafik:%s     desktop (Fenster, Terminal, Dateien, Rechner, Uhr)  paint [bild.bmp]  view bild.bmp  snake  tetris\n"
           "%sNetzwerk:%s   ifconfig [-a | dhcp | 192.168.1.50/24 gw 192.168.1.1]  ping name  nslookup name  ntp [-n]\n"
           "            udp send ziel port text / udp listen port [-e]  lspci [-v]  wget [-O datei] http://...  netstat\n"
           "%sSyntax:%s     a | b   a > d   a >> d   a < d   a 2> d   a 2>&1   a ; b   a && b   a || b   a &   # Kommentar\n"
           "            'woertlich'  \"mit $VAR\"  NAME=wert  $NAME ${NAME:-vorgabe}  $?  $1 $# $@  $(befehl)  $((1+2))  *.txt\n"
           "%sBloecke:%s    if befehl; then ...; elif ...; else ...; fi      while befehl; do ...; done   (until ebenso)\n"
           "            for x in a b c; do ...; done      name() { ...; }      { ...; } > datei      ( ... )\n"
           "%sSkripte:%s    sh datei [args]  oder ./datei.sh  (Beispiel: /etc/demo.sh)\n"
           "%sTasten:%s     Tab = vervollstaendigen, Pfeile, Hoch/Runter = Verlauf, Strg+C = abbrechen, Strg+D = beenden\n",
           h, r, h, r, h, r, h, r, h, r, h, r, h, r, h, r);
}

int run_builtin(int argc, char **argv)
{
    const char *name = argv[0];
    int n1 = argc > 1 ? atoi(argv[1]) : 1;

    if (strcmp(name, "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : var_get("HOME");
        s64 r = sys_chdir(dir[0] ? dir : "/");
        if (r < 0)
            fprintf(2, "%scd: '%s': kein Verzeichnis (Fehler %d)%s\n", ERRC, dir, (int)r, RSTC);
        return r < 0;
    }
    if (strcmp(name, "pwd") == 0) {
        char cwd[256];
        sys_getcwd(cwd, sizeof(cwd));
        printf("%s\n", cwd);
        return 0;
    }
    if (strcmp(name, "exit") == 0)
        sys_exit(argc > 1 ? atoi(argv[1]) : last_status);
    if (strcmp(name, "true") == 0 || strcmp(name, ":") == 0)
        return 0;
    if (strcmp(name, "false") == 0)
        return 1;
    if (strcmp(name, "echo") == 0)
        return builtin_echo(argc, argv);
    if (strcmp(name, "test") == 0 || strcmp(name, "[") == 0)
        return builtin_test(argc, argv);
    if (strcmp(name, "read") == 0)
        return builtin_read(argc, argv);
    if (strcmp(name, "clear") == 0) {
        write_all(1, "\x1b[2J\x1b[H", 7);
        return 0;
    }
    if (strcmp(name, "history") == 0) {
        for (int i = 0; i < hist_count; i++)
            printf("%s%3d%s  %s\n", tty_out ? C_DIM : "", i + 1, tty_out ? C_RESET : "", history[i]);
        return 0;
    }
    if (strcmp(name, "set") == 0 || strcmp(name, "export") == 0) {
        if (argc == 1) {
            for (int i = 0; i < MAX_VARS; i++)
                if (vars[i].name[0])
                    printf("%s%s%s=%s\n", tty_out ? C_CYAN : "", vars[i].name, tty_out ? C_RESET : "", vars[i].value);
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq && is_assignment(argv[i])) {
                *eq = 0;
                var_set(argv[i], eq + 1);
                *eq = '=';
            }
        }
        return 0;
    }
    if (strcmp(name, "unset") == 0) {
        for (int i = 1; i < argc; i++) {
            var_unset(argv[i]);
            Func *f = func_find(argv[i]);
            if (f)
                f->name[0] = 0;
        }
        return 0;
    }
    if (strcmp(name, "shift") == 0) {
        int k = argc > 1 ? n1 : 1;
        if (k < 0 || k > npos)
            return 1;
        for (int i = 0; i < k; i++)
            u_free(pos_args[i]);
        memmove(pos_args, pos_args + k, sizeof(char *) * (u64)(npos - k));
        npos -= k;
        return 0;
    }
    if (strcmp(name, "return") == 0) {
        ret_flag = 1;
        ret_status = argc > 1 ? n1 : last_status;
        return ret_status;
    }
    if (strcmp(name, "break") == 0 || strcmp(name, "continue") == 0) {
        if (!loop_depth) {
            fprintf(2, "%s%s: nur in einer Schleife moeglich%s\n", ERRC, name, RSTC);
            return 1;
        }
        int k = n1 < 1 ? 1 : n1 > loop_depth ? loop_depth : n1;
        if (name[0] == 'b')
            brk = k;
        else
            cont = k;
        return 0;
    }
    if (strcmp(name, "type") == 0) {
        int rc = 0;
        for (int i = 1; i < argc; i++) {
            char path[512];
            if (func_find(argv[i]))
                printf("%s ist eine Funktion\n", argv[i]);
            else if (is_builtin(argv[i]))
                printf("%s ist eingebaut\n", argv[i]);
            else if (resolve_command(argv[i], path, sizeof(path)) == 0)
                printf("%s ist %s\n", argv[i], path);
            else {
                printf("%s: nicht gefunden\n", argv[i]);
                rc = 1;
            }
        }
        return rc;
    }
    if (strcmp(name, "jobs") == 0) {
        reap_jobs();
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used)
                printf("%s[%d]%s  %sLaeuft%s  %s\n", tty_out ? C_DIM : "", jobs[i].id, tty_out ? C_RESET : "",
                       tty_out ? C_GREEN : "", tty_out ? C_RESET : "", jobs[i].desc);
        return 0;
    }
    if (strcmp(name, "fg") == 0) {
        Job *j = job_find(argc > 1 ? argv[1] : NULL);
        if (!j) {
            fprintf(2, "%sfg: kein solcher Job%s\n", ERRC, RSTC);
            return 1;
        }
        printf("%s\n", j->desc);
        return job_wait(j, 1);
    }
    if (strcmp(name, "wait") == 0) {
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used)
                job_wait(&jobs[i], 0);
        return 0;
    }
    if (strcmp(name, "source") == 0 || strcmp(name, ".") == 0) {
        if (argc < 2) {
            fprintf(2, "%sAufruf: source datei [args]%s\n", ERRC, RSTC);
            return 1;
        }
        char **old_args = pos_args;
        int old_n = npos, with_args = argc > 2;
        if (with_args) {
            pos_args = NULL;
            npos = 0;
            set_positional(argc - 2, argv + 2);
        }
        int st = run_script(argv[1]);
        if (with_args) {
            set_positional(0, NULL);
            u_free(pos_args);
            pos_args = old_args;
            npos = old_n;
        }
        return st;
    }
    if (strcmp(name, "help") == 0) {
        print_help();
        return 0;
    }
    return 127;
}
