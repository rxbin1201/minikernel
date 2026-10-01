/* Benannte Dienste, siehe service.h */

#include "core/service.h"
#include "arch/x86_64/spinlock.h"
#include "core/fdobj.h"
#include "core/process.h"
#include "core/syscall.h"
#include "lib/string.h"

#define MAX_SERVICES 8
#define MAX_PENDING  8

typedef struct {
    FdObj   *r, *w; /* Enden fuer den Dienst */
    uint32_t pid;   /* wer sich verbunden hat */
} Conn;

typedef struct {
    char     name[SERVICE_NAME_MAX];
    uint32_t owner; /* 0 = frei */
    Conn     q[MAX_PENDING];
    int      head, count;
} Service;

static Service  services[MAX_SERVICES];
static Spinlock svc_lock = SPINLOCK_INIT("service");

static Service *find(const char *name)
{
    for (int i = 0; i < MAX_SERVICES; i++)
        if (services[i].owner && strcmp(services[i].name, name) == 0)
            return &services[i];
    return 0;
}

int64_t service_register(const char *name)
{
    if (!name[0] || strlen(name) >= SERVICE_NAME_MAX)
        return ERR_INVAL;
    uint32_t me = process_pid(process_current());
    int64_t r = ERR_NOMEM;
    uint64_t f = spin_lock(&svc_lock);
    if (find(name)) {
        r = ERR_EXIST;
    } else {
        for (int i = 0; i < MAX_SERVICES; i++)
            if (!services[i].owner) {
                memset(&services[i], 0, sizeof(services[i]));
                strncpy(services[i].name, name, SERVICE_NAME_MAX);
                services[i].owner = me;
                r = 0;
                break;
            }
    }
    spin_unlock(&svc_lock, f);
    return r;
}

/* Dienst entfernen; wartende Verbindungen werden (ausserhalb des Locks) geschlossen */
static void drop(Service *s, Conn *out, int *n)
{
    for (int i = 0; i < s->count; i++)
        out[(*n)++] = s->q[(s->head + i) % MAX_PENDING];
    s->owner = 0;
    s->count = 0;
}

static void close_conns(Conn *c, int n)
{
    for (int i = 0; i < n; i++) {
        fdobj_unref(c[i].r);
        fdobj_unref(c[i].w);
    }
}

int64_t service_unregister(const char *name)
{
    Conn c[MAX_PENDING];
    int n = 0;
    int64_t r = ERR_NOENT;
    uint64_t f = spin_lock(&svc_lock);
    Service *s = find(name);
    if (s && s->owner == process_pid(process_current())) {
        drop(s, c, &n);
        r = 0;
    }
    spin_unlock(&svc_lock, f);
    close_conns(c, n);
    return r;
}

void service_owner_exit(uint32_t pid)
{
    Conn c[MAX_SERVICES * MAX_PENDING];
    int n = 0;
    uint64_t f = spin_lock(&svc_lock);
    for (int i = 0; i < MAX_SERVICES; i++)
        if (services[i].owner == pid)
            drop(&services[i], c, &n);
    spin_unlock(&svc_lock, f);
    close_conns(c, n);
}

int64_t service_connect(const char *name, int fds[2])
{
    Process *p = process_current();
    FdObj *a_r, *a_w, *b_r, *b_w; /* a: Dienst -> Programm, b: Programm -> Dienst */
    if (fdobj_new_pipe(&a_r, &a_w) != 0)
        return ERR_NOMEM;
    if (fdobj_new_pipe(&b_r, &b_w) != 0) {
        fdobj_unref(a_r);
        fdobj_unref(a_w);
        return ERR_NOMEM;
    }
    int64_t r = ERR_NOENT;
    uint64_t f = spin_lock(&svc_lock);
    Service *s = find(name);
    if (s && s->count == MAX_PENDING) {
        r = ERR_AGAIN;
    } else if (s) {
        Conn *c = &s->q[(s->head + s->count++) % MAX_PENDING];
        c->r = b_r;
        c->w = a_w;
        c->pid = process_pid(p);
        r = 0;
    }
    spin_unlock(&svc_lock, f);
    if (r != 0) {
        fdobj_unref(a_r);
        fdobj_unref(a_w);
        fdobj_unref(b_r);
        fdobj_unref(b_w);
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
    Conn c;
    int64_t r = ERR_AGAIN;
    uint64_t f = spin_lock(&svc_lock);
    Service *s = find(name);
    if (!s || s->owner != process_pid(p)) {
        r = ERR_NOENT;
    } else if (s->count) {
        c = s->q[s->head];
        s->head = (s->head + 1) % MAX_PENDING;
        s->count--;
        r = 0;
    }
    spin_unlock(&svc_lock, f);
    if (r != 0)
        return r;
    out[0] = process_fd_install(p, c.r);
    out[1] = process_fd_install(p, c.w);
    out[2] = (int)c.pid;
    if (out[0] < 0 || out[1] < 0) {
        if (out[0] >= 0)
            process_fd_close(p, out[0]);
        if (out[1] >= 0)
            process_fd_close(p, out[1]);
        return ERR_NOMEM;
    }
    return 0;
}
