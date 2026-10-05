/* Shell: Hauptprogramm (Ueberblick ueber Syntax und eingebaute Befehle: sh.h) */

#include "libc.h"
#include "malloc.h"
#include "sh.h"

char history[HIST_MAX][LINE_MAX];

int  hist_count;

Job  jobs[MAX_JOBS];

int  tty_out, tty_err, term_width = 80;

/* ======================================================================================================================
 * Hauptprogramm
 * ==================================================================================================================== */

/* Fuehrt einen Text aus (sh -c, eine Eingabezeile). Arena bleibt erhalten, wenn Funktionen definiert wurden. */
static int run_text(const char *text)
{
    Arena *a = xmalloc(sizeof(Arena));
    memset(a, 0, sizeof(*a));
    Arena *saved = arena;
    arena = a;
    Node *n = parse_text(text);
    int st;
    if (!n) {
        fprintf(2, "%ssh: %s%s\n", ERRC, parse_incomplete ? "unvollstaendige Eingabe" : parse_msg, RSTC);
        st = 2;
    } else {
        st = exec_node(n);
    }
    if (!a->keep) {
        arena_free(a);
        u_free(a);
    }
    arena = saved;
    return st;
}

void _start(int argc, char **argv)
{
    tty_out = sys_isatty(1) != 0;
    tty_err = sys_isatty(2) != 0;
    var_set("PATH", "/bin:/disk/bin:.");
    var_set("HOME", "/");
    set_positional(0, NULL);

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) { /* sh -c "befehle" [name args...] */
        if (argc > 3) {
            arg0 = argv[3];
            set_positional(argc - 4, argv + 4);
        }
        sys_exit(run_text(argv[2]));
    }
    if (argc >= 2) { /* sh skript [args] */
        arg0 = argv[1];
        set_positional(argc - 2, argv + 2);
        sys_exit(run_script(argv[1]));
    }

    interactive = 1;
    s64 t = sys_isatty(0);
    if (t && (t >> 16) > 0)
        term_width = (int)(t >> 16);
    sys_setpgid(0, 0); /* eigene Prozessgruppe, damit Ctrl-C bei Kindern nicht die Shell trifft */
    sys_tty_fg(0);
    printf("\n%sMini-Shell%s (PID %d). '%shelp%s' zeigt die Befehle, Tab vervollstaendigt.\n", tty_out ? C_CYAN : "",
           tty_out ? C_RESET : "", (int)sys_getpid(), tty_out ? C_GREEN : "", tty_out ? C_RESET : "");

    Stat st;
    if (sys_stat("/etc/profile", &st) == 0 && !st.is_dir)
        run_script("/etc/profile");

    SB acc = {0}; /* bisher eingegebene Zeilen eines noch unvollstaendigen Befehls (if ... fi ueber mehrere Zeilen) */
    for (;;) {
        char cwd[PATH_MAX], prompt[PATH_MAX + 64], line[LINE_MAX];
        if (!acc.len) {
            reap_jobs();
            if (sys_getcwd(cwd, sizeof(cwd)) < 0)
                snprintf(cwd, sizeof(cwd), "?");
            snprintf(prompt, sizeof(prompt), "%s%s%s%s> %s", tty_out ? C_CYAN : "", cwd, tty_out ? C_RESET : "",
                     tty_out ? (last_status ? C_RED : C_GREEN) : "", tty_out ? C_RESET : "");
        } else {
            snprintf(prompt, sizeof(prompt), "%s... %s", tty_out ? C_DIM : "", tty_out ? C_RESET : "");
        }
        if (tty_out && (sys_cursor() >> 16) != 0) /* die letzte Ausgabe endete ohne Zeilenumbruch: nicht ueberschreiben */
            write_all(1, "\n", 1);
        int len = edit_line(prompt, line, sizeof(line));
        if (len == -2) { /* Strg+C: Eingabe verwerfen */
            acc.len = 0;
            continue;
        }
        if (len < 0) {
            printf("exit\n");
            sys_exit(last_status);
        }
        if (len > 0)
            hist_add(line);
        for (int i = 0; i < len; i++)
            sb_put(&acc, line[i], 0);
        sb_put(&acc, '\n', 0);
        sb_put(&acc, 0, 0);
        acc.len--;

        Arena *a = xmalloc(sizeof(Arena));
        memset(a, 0, sizeof(*a));
        arena = a;
        Node *n = parse_text(acc.s);
        if (!n && parse_incomplete) { /* es fehlt noch etwas: weitere Zeile lesen */
            arena_free(a);
            u_free(a);
            continue;
        }
        acc.len = 0;
        if (!n) {
            fprintf(2, "%ssh: %s%s\n", ERRC, parse_msg, RSTC);
            last_status = 2;
        } else {
            interrupted = brk = cont = ret_flag = 0;
            last_status = exec_node(n);
        }
        if (!a->keep) {
            arena_free(a);
            u_free(a);
        }
    }
}
