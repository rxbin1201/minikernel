#ifndef USERLAND_BIN_SH_SH_H
#define USERLAND_BIN_SH_SH_H

/* Gemeinsame Typen, Zustand und Funktionen der Shell (Userland/bin/sh/). */

#include "libc.h"
#include "malloc.h"

/* Shell.
 *   Zeileneditor: Pfeiltasten, Pos1/Ende, Entf, Backspace, Verlauf (Hoch/Runter), Tab = Vervollstaendigen, Strg+C bricht
 *                 ab, Strg+D beendet. Unvollstaendige Befehle (if ... ohne fi) werden auf weiteren Zeilen fortgesetzt.
 *   Syntax:       a | b   a < d   a > d   a >> d   2> d   2>&1   a ; b   a && b   a || b   a &   ! a   # Kommentar
 *                 '...' woertlich, "..." mit Ersetzungen, \x
 *   Bloecke:      if/elif/else/fi, while/until ... do ... done, for x in ...; do ... done, name() { ...; }, { ...; }, ( ... )
 *   Ersetzungen:  $NAME ${NAME} ${NAME:-vorgabe} ${NAME:=vorgabe} ${#NAME} $? $$ $# $0..$9 $@ $* ~ $(befehl) `befehl`
 *                 $((rechnung)) (ganze Zahlen: + - * / % Vergleiche && || !), Platzhalter * ? [abc] (im letzten Pfadteil)
 *   Eingebaut:    cd pwd exit echo test [ read set export unset shift return break continue type source . history
 *                 clear jobs fg wait true false : help
 *   Skripte:      "sh datei [args]", "./datei" bzw. jede Datei, die mit "#!" beginnt oder auf ".sh" endet.
 * Interaktiv bildet jede Befehlszeile eine Prozessgruppe; Strg+C beendet die laufende Gruppe (und bricht Schleifen ab),
 * nicht die Shell. Variablen gelten nur in der Shell (es gibt keine Umgebung fuer Programme). */

#define LINE_MAX  512
#define MAX_CMDS  16
#define HIST_MAX  32
#define MAX_VARS  256
#define MAX_JOBS  8
#define MAX_CAND  128

typedef struct {
    int  used, id, pgid, npids;
    int  pids[MAX_CMDS];
    char desc[80];
} Job;

#define ERRC (tty_err ? C_RED : "")
#define RSTC (tty_err ? C_RESET : "")

typedef struct Chunk {
    struct Chunk *next;
    u64 used, cap, pad;
} Chunk;

typedef struct {
    Chunk *head;
    int    keep; /* enthaelt Funktionsdefinitionen: nicht freigeben */
} Arena;

/* Wachsende Liste von Zeigern (auf dem Heap, wird am Ende in die Arena kopiert) */
typedef struct {
    void **v;
    int    n, cap;
} Vec;

typedef struct {
    char  name[32];
    char *value;
} Var;

static inline int is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

static inline int is_ident(char c)       { return is_ident_start(c) || (c >= '0' && c <= '9'); }

typedef struct Node Node;

typedef struct {
    char  name[32];
    Node *body;
} Func;

enum { R_IN, R_OUT, R_APPEND, R_DUP };

enum { K_SIMPLE, K_PIPE, K_ANDOR, K_LIST, K_IF, K_WHILE, K_FOR, K_BRACE, K_SUB, K_FUNC };

typedef struct Redir {
    int   type, fd, dupfd;
    char *target;
    struct Redir *next;
} Redir;

struct Node {
    int    kind;
    Redir *redirs;
    char **words;   int nwords;    /* K_SIMPLE */
    char **assigns; int nassign;
    Node **items;   int nitems;    /* K_PIPE, K_ANDOR, K_LIST */
    char  *ops;                    /* K_ANDOR: 'a' = &&, 'o' = ||; K_LIST: ';' oder '&' hinter dem Eintrag */
    char **texts;                  /* K_LIST: Quelltext jedes Eintrags (fuer 'jobs') */
    int    negate;                 /* K_PIPE: ! */
    Node **conds, **bodies; int nconds; Node *els; /* K_IF */
    Node  *cond, *body; int until; /* K_WHILE */
    char  *var; char **list; int nlist, has_in; /* K_FOR */
    char  *name;                   /* K_FUNC */
};

/* Zeichen mit Markierung: 0 = unquotiert (Platzhalter wirksam), 1 = quotiert, 2 = aus unquotierter Ersetzung
 * (wird an Leerzeichen getrennt), 4 = erzwungene Trennstelle ("$@"), 5 = leeres quotiertes Wort ("") */
typedef struct {
    char          *s;
    unsigned char *m;
    int            len, cap;
} SB;

/* ---------- main.c ---------- */

extern char history[HIST_MAX][LINE_MAX];
extern int  hist_count;
extern Job  jobs[MAX_JOBS];
extern int  tty_out, tty_err, term_width;

/* ---------- vars.c: Speicher, Variablen, Funktionen ---------- */

extern Arena *arena;
extern Var    vars[MAX_VARS];
extern char **pos_args;   /* $1 .. $npos */
extern int    npos;
extern char  *arg0;
extern Func   funcs[64];

void       *xmalloc(u64 n);
char       *xstrdup(const char *s);
void       *aalloc(u64 n);
void        arena_free(Arena *a);
char       *astrndup(const char *s, int n);
void        vec_push(Vec *v, void *p);
void      **vec_finish(Vec *v);
const char *var_get(const char *name);
void        var_set(const char *name, const char *value);
void        var_unset(const char *name);
int         is_name(const char *w);
int         is_assignment(const char *w);
void        set_positional(int argc, char **argv);
Func       *func_find(const char *name);

/* ---------- parse.c ---------- */

extern int  parse_incomplete, parse_failed;
extern char parse_msg[160];

int   skip_group(const char *s, int i, char open, char close);
Node *parse_text(const char *text);

/* ---------- expand.c ---------- */

extern int last_status;
extern int interactive;

void  sb_put(SB *b, char c, unsigned char mask);
void  sb_free(SB *b);
s64   str_to_num(const char *s);
char *expand_single(const char *w);
void  expand_fields(const char *w, Vec *out);
void  free_strings(Vec *v);

/* ---------- lineedit.c ---------- */

void hist_add(const char *line);
int  edit_line(const char *prompt, char *buf, int max);

/* ---------- exec.c ---------- */

extern int interrupted; /* eine Vordergrundgruppe wurde mit Strg+C beendet: den Rest der Eingabe abbrechen */
extern int brk, cont, ret_flag, ret_status, loop_depth, func_depth;

void reap_jobs(void);
Job *job_find(const char *arg);
int  job_wait(Job *j, int fg);
int  resolve_command(const char *name, char *path, int max);
int  exec_node(Node *n);
int  run_script(const char *path);

/* ---------- builtins.c ---------- */

int is_builtin(const char *name);
int run_builtin(int argc, char **argv);

#endif
