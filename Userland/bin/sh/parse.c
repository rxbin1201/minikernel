/* Shell: Zerlegen in Woerter und Operatoren, Syntaxbaum */

#include "libc.h"
#include "malloc.h"
#include "sh.h"

static Node *parse_list(void);

/* ======================================================================================================================
 * Zerlegen: Woerter und Operatoren
 * ==================================================================================================================== */

enum { T_EOF, T_NL, T_WORD, T_SEMI, T_AMP, T_AND, T_OR, T_PIPE, T_LPAREN, T_RPAREN, T_REDIR };

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

int         parse_incomplete, parse_failed;

char        parse_msg[160];

static int is_meta(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')' || c == ' ' || c == '\t' ||
           c == '\n' || c == 0;
}

/* Hinter einer Klammer (s[i] == '(' bzw. '{'): Position hinter der passenden schliessenden Klammer, -1 = unvollstaendig */
int skip_group(const char *s, int i, char open, char close)
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

static Node *new_node(int kind)
{
    Node *n = aalloc(sizeof(Node));
    n->kind = kind;
    return n;
}

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
Node *parse_text(const char *text)
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
