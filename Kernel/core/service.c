/* Benannte Dienste, siehe service.h */

#include "core/service.h"
#include "arch/x86_64/spinlock.h"
#include "core/fdobj.h"
#include "core/process.h"
#include "core/syscall.h"
#include "lib/string.h"
#include "mm/heap.h"

/* Dienste und wartende Verbindungen liegen einzeln auf dem Heap (frueher feste Tabellen: 8 Dienste mit je 8 wartenden
 * Verbindungen). Die Obergrenzen fangen nur Ausreisser ab. Alles unter svc_lock; Pipes und Speicher werden ausserhalb
 * davon freigegeben. */
#define SERVICE_LIMIT 256 /* Dienste im System */
#define PENDING_LIMIT 256 /* wartende Verbindungen je Dienst */

typedef struct Conn {
    struct Conn *next;
    FdObj       *r, *w; /* Enden fuer den Dienst */
    uint32_t     pid;   /* wer sich verbunden hat */
} Conn;

typedef struct Service {
    struct Service *next;
    char            name[SERVICE_NAME_MAX];
    uint32_t        owner;
    Conn           *head, *tail; /* wartende Verbindungen, aelteste zuerst */
    int             count;
} Service;

static Service *services;
static int      nservices;
static Spinlock svc_lock = SPINLOCK_INIT("service");

static Service *find(const char *name)
{
    for (Service *s = services; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return s;
    return 0;
}

/* Verbindungen schliessen und freigeben (ausserhalb des Locks) */
static void close_conns(Conn *c)
{
    while (c) {
        Conn *next = c->next;
        fdobj_unref(c->r);
        fdobj_unref(c->w);
        kfree(c);
        c = next;
    }
}

int64_t service_register(const char *name)
{
    if (!name[0] || strlen(name) >= SERVICE_NAME_MAX)
        return ERR_INVAL;
    Service *n = kcalloc(1, sizeof(*n));
    if (!n)
        return ERR_NOMEM;
    strncpy(n->name, name, SERVICE_NAME_MAX - 1);
    n->owner = process_pid(process_current());
    int64_t r = 0;
    uint64_t f = spin_lock(&svc_lock);
    if (find(name)) {
        r = ERR_EXIST;
    } else if (nservices >= SERVICE_LIMIT) {
        r = ERR_NOMEM;
    } else {
        n->next = services;
        services = n;
        nservices++;
    }
    spin_unlock(&svc_lock, f);
    if (r != 0)
        kfree(n);
    return r;
}

/* Dienst aus der Liste nehmen (unter dem Lock); Dienst und Verbindungen gibt der Aufrufer danach frei */
static void unlink_service(Service *s)
{
    for (Service **pp = &services; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            nservices--;
            return;
        }
}

int64_t service_unregister(const char *name)
{
    int64_t r = ERR_NOENT;
    uint64_t f = spin_lock(&svc_lock);
    Service *s = find(name);
    if (s && s->owner == process_pid(process_current())) {
        unlink_service(s);
        r = 0;
    } else {
        s = 0;
    }
    spin_unlock(&svc_lock, f);
    if (s) {
        close_conns(s->head);
        kfree(s);
    }
    return r;
}

void service_owner_exit(uint32_t pid)
{
    for (;;) { /* je Durchgang einen Dienst des Prozesses (meist hat er keinen oder einen) */
        uint64_t f = spin_lock(&svc_lock);
        Service *s = services;
        while (s && s->owner != pid)
            s = s->next;
        if (s)
            unlink_service(s);
        spin_unlock(&svc_lock, f);
        if (!s)
            return;
        close_conns(s->head);
        kfree(s);
    }
}

int64_t service_connect(const char *name, int fds[2])
{
    Process *p = process_current();
    FdObj *a_r, *a_w, *b_r, *b_w; /* a: Dienst -> Programm, b: Programm -> Dienst */
    Conn *c = kcalloc(1, sizeof(*c));
    if (!c)
        return ERR_NOMEM;
    if (fdobj_new_pipe(&a_r, &a_w) != 0) {
        kfree(c);
        return ERR_NOMEM;
    }
    if (fdobj_new_pipe(&b_r, &b_w) != 0) {
        fdobj_unref(a_r);
        fdobj_unref(a_w);
        kfree(c);
        return ERR_NOMEM;
    }
    c->r = b_r;
    c->w = a_w;
    c->pid = process_pid(p);
    int64_t r = ERR_NOENT;
    uint64_t f = spin_lock(&svc_lock);
    Service *s = find(name);
    if (s && s->count >= PENDING_LIMIT) {
        r = ERR_AGAIN;
    } else if (s) {
        if (s->tail)
            s->tail->next = c;
        else
            s->head = c;
        s->tail = c;
        s->count++;
        r = 0;
    }
    spin_unlock(&svc_lock, f);
    if (r != 0) {
        fdobj_unref(a_r);
        fdobj_unref(a_w);
        fdobj_unref(b_r);
        fdobj_unref(b_w);
        kfree(c);
        return r;
    }
    /* Die Enden des Dienstes liegen in der Warteschlange; die eigenen kommen in die Deskriptortabelle */
    fds[0] = process_fd_install(p, a_r);
    fds[1] = process_fd_install(p, b_w);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0)
            process_fd_close(p, fds[0]);
        if (fds[1] >= 0)
            process_fd_close(p, fds[1]);
        return ERR_NOMEM; /* der Dienst sieht beim Annehmen eine schon geschlossene Verbindung */
    }
    return 0;
}

int64_t service_accept(const char *name, int out[3])
{
    Process *p = process_current();
    Conn *c = 0;
    int64_t r = ERR_AGAIN;
    uint64_t f = spin_lock(&svc_lock);
    Service *s = find(name);
    if (!s || s->owner != process_pid(p)) {
        r = ERR_NOENT;
    } else if (s->head) {
        c = s->head;
        s->head = c->next;
        if (!s->head)
            s->tail = 0;
        s->count--;
        r = 0;
    }
    spin_unlock(&svc_lock, f);
    if (r != 0)
        return r;
    FdObj *cr = c->r, *cw = c->w;
    out[2] = (int)c->pid;
    kfree(c);
    out[0] = process_fd_install(p, cr);
    out[1] = process_fd_install(p, cw);
    if (out[0] < 0 || out[1] < 0) {
        if (out[0] >= 0)
            process_fd_close(p, out[0]);
        if (out[1] >= 0)
            process_fd_close(p, out[1]);
        return ERR_NOMEM;
    }
    return 0;
}
