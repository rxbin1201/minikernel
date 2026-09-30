/* Shell: Speicher fuer den Syntaxbaum, Variablen, Positionsparameter, Funktionen */

#include "libc.h"
#include "malloc.h"
#include "sh.h"

/* ======================================================================================================================
 * Speicher: Arena fuer den Syntaxbaum (wird nach der Ausfuehrung als Ganzes freigegeben, ausser er enthaelt Funktionen)
 * ==================================================================================================================== */

void *xmalloc(u64 n)
{
    void *p = u_malloc(n ? n : 1);
    if (!p) {
        fprintf(2, "sh: kein Speicher mehr\n");
        sys_exit(1);
    }
    return p;
}

char *xstrdup(const char *s)
{
    size_t l = strlen(s);
    char *p = xmalloc(l + 1);
    memcpy(p, s, l + 1);
    return p;
}

Arena *arena;

void *aalloc(u64 n)
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

void arena_free(Arena *a)
{
    while (a->head) {
        Chunk *n = a->head->next;
        u_free(a->head);
        a->head = n;
    }
}

char *astrndup(const char *s, int n)
{
    char *p = aalloc((u64)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

void vec_push(Vec *v, void *p)
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

void **vec_finish(Vec *v)
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

Var   vars[MAX_VARS];

char **pos_args;   /* $1 .. $npos */

int    npos;

char  *arg0 = "sh";

const char *var_get(const char *name)
{
    for (int i = 0; i < MAX_VARS; i++)
        if (vars[i].name[0] && strcmp(vars[i].name, name) == 0)
            return vars[i].value;
    return "";
}

void var_set(const char *name, const char *value)
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

void var_unset(const char *name)
{
    for (int i = 0; i < MAX_VARS; i++) {
        if (vars[i].name[0] && strcmp(vars[i].name, name) == 0) {
            u_free(vars[i].value);
            vars[i].value = 0;
            vars[i].name[0] = 0;
        }
    }
}

int is_name(const char *w)
{
    if (!is_ident_start(*w))
        return 0;
    while (is_ident(*w))
        w++;
    return *w == 0;
}

/* "NAME=wert" */
int is_assignment(const char *w)
{
    if (!is_ident_start(*w))
        return 0;
    while (is_ident(*w))
        w++;
    return *w == '=';
}

/* Setzt $1.. (kopiert die Zeichenketten) */
void set_positional(int argc, char **argv)
{
    for (int i = 0; i < npos; i++)
        u_free(pos_args[i]);
    u_free(pos_args);
    npos = argc;
    pos_args = xmalloc(sizeof(char *) * (u64)(argc + 1));
    for (int i = 0; i < argc; i++)
        pos_args[i] = xstrdup(argv[i]);
}

Func funcs[64];

Func *func_find(const char *name)
{
    for (int i = 0; i < 64; i++)
        if (funcs[i].name[0] && strcmp(funcs[i].name, name) == 0)
            return &funcs[i];
    return NULL;
}
