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

static char history[HIST_MAX][LINE_MAX];
static int  hist_count;
static Job  jobs[MAX_JOBS];
static int  next_job_id = 1;
static int  tty_out, tty_err, term_width = 80;

#define ERRC (tty_err ? C_RED : "")
#define RSTC (tty_err ? C_RESET : "")

/* ======================================================================================================================
 * Speicher: Arena fuer den Syntaxbaum (wird nach der Ausfuehrung als Ganzes freigegeben, ausser er enthaelt Funktionen)
 * ==================================================================================================================== */

static void *xmalloc(u64 n)
{
    void *p = u_malloc(n ? n : 1);
    if (!p) {
        fprintf(2, "sh: kein Speicher mehr\n");
        sys_exit(1);
    }
    return p;
}

static char *xstrdup(const char *s)
{
    size_t l = strlen(s);
    char *p = xmalloc(l + 1);
    memcpy(p, s, l + 1);
    return p;
}

typedef struct Chunk {
    struct Chunk *next;
    u64 used, cap, pad;
} Chunk;

typedef struct {
    Chunk *head;
    int    keep; /* enthaelt Funktionsdefinitionen: nicht freigeben */
} Arena;

static Arena *arena;

static void *aalloc(u64 n)
{
    n = (n + 15) & ~15ULL;
    Chunk *c = arena->head;
    if (!c || c->used + n > c->cap) {
        u64 cap = n > 16384 ? n : 16384;
        c = xmalloc(sizeof(Chunk) + cap);
        c->next = arena->head;
        c->used = 0;
        c->cap = cap;
        arena->head = c;
    }
    void *p = (char *)(c + 1) + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

static void arena_free(Arena *a)
{
    while (a->head) {
        Chunk *n = a->head->next;
        u_free(a->head);
        a->head = n;
    }
}

static char *astrndup(const char *s, int n)
{
    char *p = aalloc((u64)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

/* Wachsende Liste von Zeigern (auf dem Heap, wird am Ende in die Arena kopiert) */
typedef struct {
    void **v;
    int    n, cap;
} Vec;

static void vec_push(Vec *v, void *p)
{
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 8;
        void **nv = xmalloc(sizeof(void *) * (u64)cap);
        if (v->n)
            memcpy(nv, v->v, sizeof(void *) * (u64)v->n);
        u_free(v->v);
        v->v = nv;
        v->cap = cap;
    }
    v->v[v->n++] = p;
}

static void **vec_finish(Vec *v)
{
    void **a = aalloc(sizeof(void *) * (u64)(v->n + 1));
    if (v->n)
        memcpy(a, v->v, sizeof(void *) * (u64)v->n);
    u_free(v->v);
    v->v = 0;
    return a;
}

/* ======================================================================================================================
 * Variablen, Positionsparameter, Funktionen
 * ==================================================================================================================== */

typedef struct {
    char  name[32];
    char *value;
} Var;

static Var   vars[MAX_VARS];
static char **pos_args;   /* $1 .. $npos */
static int    npos;
static char  *arg0 = "sh";

static const char *var_get(const char *name)
{
    for (int i = 0; i < MAX_VARS; i++)
        if (vars[i].name[0] && strcmp(vars[i].name, name) == 0)
            return vars[i].value;
    return "";
}

static void var_set(const char *name, const char *value)
{
    Var *free_slot = NULL;
    for (int i = 0; i < MAX_VARS; i++) {
        if (vars[i].name[0] && strcmp(vars[i].name, name) == 0) {
            char *v = xstrdup(value);
            u_free(vars[i].value);
            vars[i].value = v;
            return;
        }
        if (!vars[i].name[0] && !free_slot)
            free_slot = &vars[i];
    }
    if (!free_slot) {
        fprintf(2, "%ssh: zu viele Variablen%s\n", ERRC, RSTC);
        return;
    }
    snprintf(free_slot->name, sizeof(free_slot->name), "%s", name);
    free_slot->value = xstrdup(value);
}

static void var_unset(const char *name)
{
    for (int i = 0; i < MAX_VARS; i++) {
        if (vars[i].name[0] && strcmp(vars[i].name, name) == 0) {
            u_free(vars[i].value);
            vars[i].value = 0;
            vars[i].name[0] = 0;
        }
    }
}

static int is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_ident(char c)       { return is_ident_start(c) || (c >= '0' && c <= '9'); }

static int is_name(const char *w)
{
    if (!is_ident_start(*w))
        return 0;
    while (is_ident(*w))
        w++;
    return *w == 0;
}

/* "NAME=wert" */
static int is_assignment(const char *w)
{
    if (!is_ident_start(*w))
        return 0;
    while (is_ident(*w))
        w++;
    return *w == '=';
}

/* Setzt $1.. (kopiert die Zeichenketten) */
static void set_positional(int argc, char **argv)
{
    for (int i = 0; i < npos; i++)
        u_free(pos_args[i]);
    u_free(pos_args);
    npos = argc;
    pos_args = xmalloc(sizeof(char *) * (u64)(argc + 1));
    for (int i = 0; i < argc; i++)
        pos_args[i] = xstrdup(argv[i]);
}

typedef struct Node Node;

typedef struct {
    char  name[32];
    Node *body;
} Func;

static Func funcs[64];

static Func *func_find(const char *name)
{
    for (int i = 0; i < 64; i++)
        if (funcs[i].name[0] && strcmp(funcs[i].name, name) == 0)
            return &funcs[i];
    return NULL;
}

/* ======================================================================================================================
 * Zerlegen: Woerter und Operatoren
 * ==================================================================================================================== */

enum { T_EOF, T_NL, T_WORD, T_SEMI, T_AMP, T_AND, T_OR, T_PIPE, T_LPAREN, T_RPAREN, T_REDIR };
enum { R_IN, R_OUT, R_APPEND, R_DUP };

typedef struct {
    int   type;
    char *text;               /* T_WORD: Rohtext (mit Anfuehrungszeichen) */
    int   rtype, rfd, rdup;   /* T_REDIR */
    int   line;
    int   start, end;         /* Lage im Quelltext */
} Tok;

static const char *src;
static int         sp, src_line;
static Tok         tok;
static int         parse_incomplete, parse_failed;
static char        parse_msg[160];

static int is_meta(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')' || c == ' ' || c == '\t' ||
           c == '\n' || c == 0;
}

/* Hinter einer Klammer (s[i] == '(' bzw. '{'): Position hinter der passenden schliessenden Klammer, -1 = unvollstaendig */
static int skip_group(const char *s, int i, char open, char close)
{
    int depth = 0;
    for (int j = i; s[j]; j++) {
        char c = s[j];
        if (c == '\\' && s[j + 1]) {
            j++;
        } else if (c == '\'') {
            j++;
            while (s[j] && s[j] != '\'')
                j++;
            if (!s[j])
                return -1;
        } else if (c == '"') {
            j++;
            while (s[j] && s[j] != '"') {
                if (s[j] == '\\' && s[j + 1])
                    j++;
                j++;
            }
            if (!s[j])
                return -1;
        } else if (c == open) {
            depth++;
        } else if (c == close) {
            if (--depth == 0)
                return j + 1;
        }
    }
    return -1;
}

/* Ende eines Wortes ab i; -1 = unvollstaendig (offene Anfuehrungszeichen oder Klammern) */
static int scan_word(const char *s, int i)
{
    while (!is_meta(s[i])) {
        char c = s[i];
        if (c == '\\') {
            if (!s[i + 1])
                return i + 1;
            i += 2;
        } else if (c == '\'') {
            i++;
            while (s[i] && s[i] != '\'')
                i++;
            if (!s[i])
                return -1;
            i++;
        } else if (c == '"') {
            i++;
            while (s[i] && s[i] != '"') {
                if (s[i] == '\\' && s[i + 1]) {
                    i += 2;
                    continue;
                }
                if (s[i] == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) {
                    int e = skip_group(s, i + 1, s[i + 1], s[i + 1] == '(' ? ')' : '}');
                    if (e < 0)
                        return -1;
                    i = e;
                    continue;
                }
                i++;
            }
            if (!s[i])
                return -1;
            i++;
        } else if (c == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) {
            int e = skip_group(s, i + 1, s[i + 1], s[i + 1] == '(' ? ')' : '}');
            if (e < 0)
                return -1;
            i = e;
        } else if (c == '`') {
            i++;
            while (s[i] && s[i] != '`')
                i++;
            if (!s[i])
                return -1;
            i++;
        } else {
            i++;
        }
    }
    return i;
}

static void lex(void)
{
    for (;;) {
        while (src[sp] == ' ' || src[sp] == '\t')
            sp++;
        if (src[sp] == '\\' && src[sp + 1] == '\n') { /* Fortsetzungszeile */
            sp += 2;
            src_line++;
            continue;
        }
        if (src[sp] == '#') { /* Kommentar */
            while (src[sp] && src[sp] != '\n')
                sp++;
        }
        break;
    }
    memset(&tok, 0, sizeof(tok));
    tok.line = src_line;
    tok.start = sp;
    const char *s = src + sp;
    char c = *s;
    if (!c) {
        tok.type = T_EOF;
        return;
    }
    if (c == '\n') {
        tok.type = T_NL;
        sp++;
        src_line++;
        tok.end = sp;
        return;
    }
    /* Umleitung mit Nummer: 2> 2>> 2>&1 */
    int k = 0, fd = -1;
    while (s[k] >= '0' && s[k] <= '9')
        k++;
    if (k > 0 && (s[k] == '<' || s[k] == '>')) {
        fd = 0;
        for (int i = 0; i < k; i++)
            fd = fd * 10 + (s[i] - '0');
        s += k;
        sp += k;
        c = *s;
    }
    if (c == '<' || c == '>') {
        tok.type = T_REDIR;
        tok.rfd = fd >= 0 ? fd : (c == '<' ? 0 : 1);
        if (c == '>' && s[1] == '>') {
            tok.rtype = R_APPEND;
            sp += 2;
        } else if (s[1] == '&' && s[2] >= '0' && s[2] <= '9') {
            tok.rtype = R_DUP;
            tok.rdup = s[2] - '0';
            sp += 3;
        } else {
            tok.rtype = c == '<' ? R_IN : R_OUT;
            sp += 1;
        }
        tok.end = sp;
        return;
    }
    if (fd >= 0) { /* doch keine Umleitung: Ziffern gehoeren zum Wort */
        sp -= k;
        s -= k;
        c = *s;
    }
    if (c == '&' && s[1] == '&') { tok.type = T_AND; sp += 2; }
    else if (c == '|' && s[1] == '|') { tok.type = T_OR; sp += 2; }
    else if (c == '&') { tok.type = T_AMP; sp++; }
    else if (c == '|') { tok.type = T_PIPE; sp++; }
    else if (c == ';') { tok.type = T_SEMI; sp++; }
    else if (c == '(') { tok.type = T_LPAREN; sp++; }
    else if (c == ')') { tok.type = T_RPAREN; sp++; }
    else {
        int e = scan_word(src, sp);
        if (e < 0) {
            parse_incomplete = 1;
            tok.type = T_EOF;
            return;
        }
        tok.type = T_WORD;
        tok.text = astrndup(src + sp, e - sp);
        for (int i = sp; i < e; i++) /* Fortsetzungszeilen im Wort mitzaehlen */
            if (src[i] == '\n')
                src_line++;
        sp = e;
    }
    tok.end = sp;
}

static int tok_is(const char *kw)
{
    return tok.type == T_WORD && strcmp(tok.text, kw) == 0;
}

static void parse_error(const char *what)
{
    if (parse_failed || parse_incomplete)
        return;
    if (tok.type == T_EOF) { /* es fehlt noch etwas: bei der Eingabe weiterlesen */
        parse_incomplete = 1;
        return;
    }
    parse_failed = 1;
    const char *t = tok.type == T_WORD ? tok.text : tok.type == T_NL ? "Zeilenende" : tok.type == T_SEMI ? ";" :
                    tok.type == T_AMP ? "&" : tok.type == T_AND ? "&&" : tok.type == T_OR ? "||" :
                    tok.type == T_PIPE ? "|" : tok.type == T_LPAREN ? "(" : tok.type == T_RPAREN ? ")" : "Umleitung";
    snprintf(parse_msg, sizeof(parse_msg), "Zeile %d: %s (bei '%s')", tok.line, what, t);
}

static int failed(void) { return parse_failed || parse_incomplete; }

/* ======================================================================================================================
 * Syntaxbaum
 * ==================================================================================================================== */

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

static Node *new_node(int kind)
{
    Node *n = aalloc(sizeof(Node));
    n->kind = kind;
    return n;
}

static Node *parse_list(void);
static Node *parse_command(void);

static void skip_newlines(void)
{
    while (tok.type == T_NL)
        lex();
}

static int at_terminator(void)
{
    if (tok.type == T_EOF || tok.type == T_RPAREN)
        return 1;
    if (tok.type != T_WORD)
        return 0;
    static const char *const kw[] = {"then", "else", "elif", "fi", "do", "done", "}", 0};
    for (int i = 0; kw[i]; i++)
        if (strcmp(tok.text, kw[i]) == 0)
            return 1;
    return 0;
}

static void expect_word(const char *kw)
{
    if (tok_is(kw))
        lex();
    else {
        char m[64];
        snprintf(m, sizeof(m), "'%s' erwartet", kw);
        parse_error(m);
    }
}

/* Umleitungen hinter einem Befehl; 0 = ok */
static int parse_redirs(Redir **list)
{
    Redir **tail = list;
    while (*tail)
        tail = &(*tail)->next;
    while (tok.type == T_REDIR) {
        Redir *r = aalloc(sizeof(Redir));
        r->type = tok.rtype;
        r->fd = tok.rfd;
        r->dupfd = tok.rdup;
        lex();
        if (r->type != R_DUP) {
            if (tok.type != T_WORD) {
                parse_error("Dateiname fuer die Umleitung fehlt");
                return -1;
            }
            r->target = tok.text;
            lex();
        }
        *tail = r;
        tail = &r->next;
    }
    return 0;
}

static Node *parse_simple(void)
{
    Node *n = new_node(K_SIMPLE);
    Vec words = {0}, assigns = {0};
    for (;;) {
        if (tok.type == T_REDIR) {
            if (parse_redirs(&n->redirs) != 0)
                break;
        } else if (tok.type == T_WORD) {
            if (words.n == 0 && is_assignment(tok.text))
                vec_push(&assigns, tok.text);
            else
                vec_push(&words, tok.text);
            lex();
        } else {
            break;
        }
    }
    n->nwords = words.n;
    n->nassign = assigns.n;
    n->words = (char **)vec_finish(&words);
    n->assigns = (char **)vec_finish(&assigns);
    if (!n->nwords && !n->nassign && !n->redirs)
        parse_error("Befehl erwartet");
    return n;
}

static Node *parse_if(void)
{
    Node *n = new_node(K_IF);
    Vec conds = {0}, bodies = {0};
    lex(); /* if */
    for (;;) {
        Node *c = parse_list();
        expect_word("then");
        Node *b = parse_list();
        if (failed())
            break;
        vec_push(&conds, c);
        vec_push(&bodies, b);
        if (tok_is("elif")) {
            lex();
            continue;
        }
        if (tok_is("else")) {
            lex();
            n->els = parse_list();
        }
        expect_word("fi");
        break;
    }
    n->nconds = conds.n;
    n->conds = (Node **)vec_finish(&conds);
    n->bodies = (Node **)vec_finish(&bodies);
    return n;
}

static Node *parse_while(int until)
{
    Node *n = new_node(K_WHILE);
    n->until = until;
    lex();
    n->cond = parse_list();
    expect_word("do");
    n->body = parse_list();
    expect_word("done");
    return n;
}

static Node *parse_for(void)
{
    Node *n = new_node(K_FOR);
    lex(); /* for */
    if (tok.type != T_WORD || !is_name(tok.text)) {
        parse_error("Variablenname hinter 'for' erwartet");
        return n;
    }
    n->var = tok.text;
    lex();
    skip_newlines();
    Vec list = {0};
    if (tok_is("in")) {
        n->has_in = 1;
        lex();
        while (tok.type == T_WORD) {
            vec_push(&list, tok.text);
            lex();
        }
    }
    n->nlist = list.n;
    n->list = (char **)vec_finish(&list);
    if (tok.type == T_SEMI || tok.type == T_NL)
        lex();
    skip_newlines();
    expect_word("do");
    n->body = parse_list();
    expect_word("done");
    return n;
}

static Node *parse_command(void)
{
    Node *n = NULL;
    if (tok.type == T_WORD) {
        if (tok_is("if"))
            n = parse_if();
        else if (tok_is("while"))
            n = parse_while(0);
        else if (tok_is("until"))
            n = parse_while(1);
        else if (tok_is("for"))
            n = parse_for();
        else if (tok_is("{")) {
            n = new_node(K_BRACE);
            lex();
            n->body = parse_list();
            expect_word("}");
        } else if (tok_is("function")) { /* function name { ... } */
            lex();
            if (tok.type != T_WORD || !is_name(tok.text)) {
                parse_error("Funktionsname erwartet");
                return new_node(K_SIMPLE);
            }
            n = new_node(K_FUNC);
            n->name = tok.text;
            lex();
            if (tok.type == T_LPAREN) {
                lex();
                if (tok.type != T_RPAREN)
                    parse_error("')' erwartet");
                lex();
            }
            skip_newlines();
            n->body = parse_command();
            return n;
        } else if (is_name(tok.text)) { /* name() { ... } ? */
            int save_sp = sp, save_line = src_line;
            Tok save = tok;
            lex();
            if (tok.type == T_LPAREN) {
                lex();
                if (tok.type == T_RPAREN) {
                    lex();
                    skip_newlines();
                    n = new_node(K_FUNC);
                    n->name = save.text;
                    n->body = parse_command();
                    return n;
                }
            }
            sp = save_sp;
            src_line = save_line;
            tok = save;
        }
    } else if (tok.type == T_LPAREN) {
        n = new_node(K_SUB);
        lex();
        n->body = parse_list();
        if (tok.type == T_RPAREN)
            lex();
        else
            parse_error("')' erwartet");
    }
    if (!n)
        return parse_simple();
    parse_redirs(&n->redirs); /* Umleitungen hinter if/while/for/{ } */
    return n;
}

static Node *parse_pipeline(void)
{
    Node *n = new_node(K_PIPE);
    Vec cmds = {0};
    if (tok_is("!")) {
        n->negate = 1;
        lex();
    }
    vec_push(&cmds, parse_command());
    while (!failed() && tok.type == T_PIPE) {
        lex();
        skip_newlines();
        vec_push(&cmds, parse_command());
    }
    n->nitems = cmds.n;
    n->items = (Node **)vec_finish(&cmds);
    return n;
}

static Node *parse_andor(void)
{
    Node *n = new_node(K_ANDOR);
    Vec items = {0};
    char ops[64];
    int nops = 0;
    vec_push(&items, parse_pipeline());
    while (!failed() && (tok.type == T_AND || tok.type == T_OR) && nops < 63) {
        ops[nops++] = tok.type == T_AND ? 'a' : 'o';
        lex();
        skip_newlines();
        vec_push(&items, parse_pipeline());
    }
    n->nitems = items.n;
    n->items = (Node **)vec_finish(&items);
    n->ops = astrndup(ops, nops);
    return n;
}

/* Folge von Befehlen bis zu einem Schluesselwort (then, fi, done, ...), ')' oder dem Ende */
static Node *parse_list(void)
{
    Node *n = new_node(K_LIST);
    Vec items = {0}, texts = {0};
    char ops[256];
    int nops = 0;
    skip_newlines();
    while (!failed() && !at_terminator()) {
        int start = tok.start;
        Node *it = parse_andor();
        if (failed())
            break;
        int end = tok.start;
        char op = ';';
        if (tok.type == T_AMP) {
            op = '&';
            end = tok.start;
            lex();
        } else if (tok.type == T_SEMI || tok.type == T_NL) {
            lex();
        } else if (!at_terminator()) {
            parse_error("unerwartetes Zeichen");
            break;
        }
        while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\n' || src[end - 1] == ';'))
            end--;
        vec_push(&items, it);
        vec_push(&texts, astrndup(src + start, end - start));
        if (nops < 255)
            ops[nops++] = op;
        skip_newlines();
    }
    n->nitems = items.n;
    n->items = (Node **)vec_finish(&items);
    n->texts = (char **)vec_finish(&texts);
    n->ops = astrndup(ops, nops);
    return n;
}

/* Zerlegt einen ganzen Text. NULL bei Fehler (parse_failed) oder wenn noch etwas fehlt (parse_incomplete). */
static Node *parse_text(const char *text)
{
    src = text;
    sp = 0;
    src_line = 1;
    parse_incomplete = parse_failed = 0;
    parse_msg[0] = 0;
    lex();
    Node *n = parse_list();
    if (!failed() && tok.type != T_EOF)
        parse_error("unerwartetes Zeichen");
    if (failed())
        return NULL;
    return n;
}

/* ======================================================================================================================
 * Ersetzen: Variablen, $(befehl), $((rechnung)), ~, Platzhalter
 * ==================================================================================================================== */

/* Zeichen mit Markierung: 0 = unquotiert (Platzhalter wirksam), 1 = quotiert, 2 = aus unquotierter Ersetzung
 * (wird an Leerzeichen getrennt), 4 = erzwungene Trennstelle ("$@"), 5 = leeres quotiertes Wort ("") */
typedef struct {
    char          *s;
    unsigned char *m;
    int            len, cap;
} SB;

static void sb_put(SB *b, char c, unsigned char mask)
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

static void sb_free(SB *b)
{
    u_free(b->s);
    u_free(b->m);
    memset(b, 0, sizeof(*b));
}

static int last_status;
static int interactive;
static int exec_node(Node *n);
static void expand_into(const char *w, int *i, SB *b, int quoted);

/* ---- Rechnen: $((...)) ---- */

static const char *ap;
static int         aerr;

static s64 arith_or(void);

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

static s64 str_to_num(const char *s)
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
static char *expand_single(const char *w)
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
    char word[512];
    int n = len < 511 ? len : 511;
    memcpy(word, s, (size_t)n);
    word[n] = 0;
    char *slash = strrchr(word, '/');
    char dir[512];
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
static void expand_fields(const char *w, Vec *out)
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

static void free_strings(Vec *v)
{
    for (int i = 0; i < v->n; i++)
        u_free(v->v[i]);
    u_free(v->v);
    memset(v, 0, sizeof(*v));
}

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

static void hist_add(const char *line)
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
static int edit_line(const char *prompt, char *buf, int max)
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
static void reap_jobs(void)
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

static Job *job_find(const char *arg)
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
static int job_wait(Job *j, int fg)
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

static int interrupted;             /* eine Vordergrundgruppe wurde mit Strg+C beendet: den Rest der Eingabe abbrechen */
static int brk, cont, ret_flag, ret_status, loop_depth, func_depth;

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
static int resolve_command(const char *name, char *path, int max)
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
    char path[512], cmdline[1024];
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

static int run_builtin(int argc, char **argv);
static int is_builtin(const char *name);

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
    int pids[16], started = 0, first = 0, prev_read = -1;
    for (int i = 0; i < n->nitems && i < 16; i++) {
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

static int exec_node(Node *n)
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
static int run_script(const char *path)
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

/* ======================================================================================================================
 * Eingebaute Befehle
 * ==================================================================================================================== */

static const char *const builtin_list[] = {"cd", "pwd", "exit", "history", "help", "set", "unset", "export", "jobs", "fg",
                                           "wait", "source", ".", "clear", "true", "false", ":", "echo", "test", "[",
                                           "read", "shift", "return", "break", "continue", "type", 0};

static int is_builtin(const char *name)
{
    for (int i = 0; builtin_list[i]; i++)
        if (strcmp(name, builtin_list[i]) == 0)
            return 1;
    return 0;
}

/* ---- test / [ ---- */

static int t_expr(char **a, int n);

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
           "            mouse  poweroff  reboot  (siehe 'ls /bin')\n"
           "%sGrafik:%s     desktop (Fenster, Terminal, Dateien, Rechner, Uhr)  paint [bild.bmp]  view bild.bmp  snake  tetris\n"
           "%sNetzwerk:%s   ifconfig [-a | dhcp | 192.168.1.50/24 gw 192.168.1.1]  ping name  nslookup name  ntp [-n]\n"
           "            udp send ziel port text / udp listen port [-e]  lspci [-v]\n"
           "%sSyntax:%s     a | b   a > d   a >> d   a < d   a 2> d   a 2>&1   a ; b   a && b   a || b   a &   # Kommentar\n"
           "            'woertlich'  \"mit $VAR\"  NAME=wert  $NAME ${NAME:-vorgabe}  $?  $1 $# $@  $(befehl)  $((1+2))  *.txt\n"
           "%sBloecke:%s    if befehl; then ...; elif ...; else ...; fi      while befehl; do ...; done   (until ebenso)\n"
           "            for x in a b c; do ...; done      name() { ...; }      { ...; } > datei      ( ... )\n"
           "%sSkripte:%s    sh datei [args]  oder ./datei.sh  (Beispiel: /etc/demo.sh)\n"
           "%sTasten:%s     Tab = vervollstaendigen, Pfeile, Hoch/Runter = Verlauf, Strg+C = abbrechen, Strg+D = beenden\n",
           h, r, h, r, h, r, h, r, h, r, h, r, h, r, h, r);
}

static int run_builtin(int argc, char **argv)
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
        char cwd[128], prompt[200], line[LINE_MAX];
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
