#include "core/fdobj.h"
#include "arch/x86_64/cpu.h"
#include "mm/heap.h"
#include "net/net.h"
#include "core/process.h"
#include "lib/string.h"
#include "core/tty.h"

#define PIPE_SIZE 4096

typedef struct Pipe {
    uint8_t  buf[PIPE_SIZE];
    uint32_t head, tail, count;
    int      readers, writers; /* Anzahl der FdObj (nicht der Deskriptoren) an den beiden Enden */
} Pipe;

/* ---------- Anlegen / Zaehlen ---------- */

static FdObj *alloc_obj(FdKind kind)
{
    FdObj *o = kcalloc(1, sizeof(*o));
    if (o) {
        o->kind = kind;
        o->refs = 1;
    }
    return o;
}

FdObj *fdobj_new_console(void)
{
    return alloc_obj(FD_CONSOLE);
}

FdObj *fdobj_new_file(const FsFile *file)
{
    FdObj *o = alloc_obj(FD_FILE);
    if (o)
        o->file = *file;
    return o;
}

FdObj *fdobj_new_udp(struct UdpSock *s)
{
    FdObj *o = alloc_obj(FD_UDP);
    if (o)
        o->udp = s;
    return o;
}

FdObj *fdobj_new_tcp(struct TcpConn *c)
{
    FdObj *o = alloc_obj(FD_TCP);
    if (o)
        o->tcp = c;
    return o;
}

int fdobj_new_pipe(FdObj **read_end, FdObj **write_end)
{
    Pipe *p = kcalloc(1, sizeof(*p));
    FdObj *r = alloc_obj(FD_PIPE_R), *w = alloc_obj(FD_PIPE_W);
    if (!p || !r || !w) {
        kfree(p);
        kfree(r);
        kfree(w);
        return -1;
    }
    p->readers = p->writers = 1;
    r->pipe = w->pipe = p;
    *read_end = r;
    *write_end = w;
    return 0;
}

FdObj *fdobj_ref(FdObj *o)
{
    uint64_t f = irq_save();
    o->refs++;
    irq_restore(f);
    return o;
}

void fdobj_unref(FdObj *o)
{
    uint64_t f = irq_save();
    int last = --o->refs == 0;
    irq_restore(f);
    if (!last)
        return;

    if (o->kind == FD_FILE) {
        fs_close(&o->file);
    } else if (o->kind == FD_UDP) {
        udp_close(o->udp);
    } else if (o->kind == FD_TCP) {
        tcp_close(o->tcp);
    } else if (o->kind == FD_PIPE_R || o->kind == FD_PIPE_W) {
        Pipe *p = o->pipe;
        f = irq_save();
        if (o->kind == FD_PIPE_R)
            p->readers--;
        else
            p->writers--;
        int dead = p->readers == 0 && p->writers == 0;
        irq_restore(f);
        if (dead)
            kfree(p);
    }
    kfree(o);
}

/* ---------- Pipe ---------- */

static int64_t pipe_read(Pipe *p, uint8_t *out, uint64_t len)
{
    for (;;) {
        uint64_t f = irq_save();
        if (p->count > 0) {
            uint64_t n = 0;
            while (n < len && p->count > 0) {
                out[n++] = p->buf[p->tail];
                p->tail = (p->tail + 1) % PIPE_SIZE;
                p->count--;
            }
            irq_restore(f);
            return (int64_t)n;
        }
        int eof = p->writers == 0;
        irq_restore(f);
        if (eof)
            return 0;
        if (process_wait_tick() != 0)
            return ERR_INTR;
    }
}

static int64_t pipe_write(Pipe *p, const uint8_t *in, uint64_t len)
{
    uint64_t done = 0;
    while (done < len) {
        uint64_t f = irq_save();
        if (p->readers == 0) {
            irq_restore(f);
            return done ? (int64_t)done : ERR_PIPE;
        }
        while (done < len && p->count < PIPE_SIZE) {
            p->buf[p->head] = in[done++];
            p->head = (p->head + 1) % PIPE_SIZE;
            p->count++;
        }
        int full = done < len;
        irq_restore(f);
        if (full && process_wait_tick() != 0)
            return done ? (int64_t)done : ERR_INTR;
    }
    return (int64_t)done;
}

/* ---------- allgemeine Zugriffe ---------- */

int64_t fdobj_read(FdObj *o, void *buf, uint64_t len)
{
    switch (o->kind) {
    case FD_CONSOLE:
        return tty_read(buf, len);
    case FD_FILE:
        return fs_read(&o->file, buf, len);
    case FD_PIPE_R:
        return pipe_read(o->pipe, buf, len);
    case FD_UDP:
        return udp_recvfrom(o->udp, buf, len > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)len, 0, 0, NET_WAIT_FOREVER);
    case FD_TCP:
        return tcp_recv(o->tcp, buf, len);
    default:
        return ERR_BADF;
    }
}

int64_t fdobj_write(FdObj *o, const void *buf, uint64_t len)
{
    switch (o->kind) {
    case FD_CONSOLE:
        return tty_write(buf, len);
    case FD_FILE:
        return fs_write(&o->file, buf, len);
    case FD_PIPE_W:
        return pipe_write(o->pipe, buf, len);
    case FD_TCP:
        return tcp_send(o->tcp, buf, len);
    default:
        return ERR_BADF;
    }
}

int64_t fdobj_available(FdObj *o)
{
    if (o->kind == FD_UDP)
        return udp_pending(o->udp);
    if (o->kind == FD_TCP)
        return tcp_pending(o->tcp);
    if (o->kind == FD_PIPE_W) { /* Schreibende: freier Platz (so viel geht ohne Warten hinein), -1 = kein Leser mehr */
        uint64_t f = irq_save();
        int64_t r = o->pipe->readers == 0 ? -1 : (int64_t)(PIPE_SIZE - o->pipe->count);
        irq_restore(f);
        return r;
    }
    if (o->kind != FD_PIPE_R)
        return o->kind == FD_FILE ? 1 : 0;
    uint64_t f = irq_save();
    int64_t r = o->pipe->count ? (int64_t)o->pipe->count : (o->pipe->writers == 0 ? -1 : 0);
    irq_restore(f);
    return r;
}

int64_t fdobj_seek(FdObj *o, int64_t offset, int whence)
{
    if (o->kind != FD_FILE)
        return ERR_SPIPE;
    return fs_seek(&o->file, offset, whence);
}
