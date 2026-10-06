#include "core/process.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/smp.h"
#include "core/fdobj.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "lib/kprintf.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "core/sched.h"
#include "lib/string.h"
#include "core/syscall.h"
#include "core/tty.h"
#include "fs/vfs.h"
#include "drivers/mouse.h"
#include "console/console.h"
#include "drivers/sound/hda.h"
#include "arch/x86_64/spinlock.h"
#include "core/service.h"
#include "drivers/gpu/igd.h"
#include "arch/x86_64/apic.h"

/* Tabellen wachsen bei Bedarf; die Obergrenzen fangen nur Ausreisser ab (ein Programm, das endlos Dateien oeffnet,
 * soll nicht den ganzen Kernel-Heap belegen) */
#define PROC_LIMIT        4096 /* Prozesse gleichzeitig (Eintraege bleiben angelegt und werden wiederverwendet) */
#define FD_LIMIT          1024 /* offene Deskriptoren je Prozess (wie ulimit -n unter Linux) */
#define THREAD_LIMIT      1024 /* Threads je Prozess (mit beendeten, noch nicht abgeholten) */
#define VMA_LIMIT         1024 /* eingeblendete Dateien je Prozess (SYS_MMAP_FILE) */
#define SHMMAP_LIMIT      1024 /* eingeblendeter geteilter Speicher je Prozess */
#define SHM_LIMIT         4096 /* geteilte Speicherobjekte im System */
#define TH_CHUNK          16   /* Threads je Block */
#define USER_STACK_PAGES  16   /* anfangs eingeblendet (64 KiB) */
#define USER_STACK_MAX    (8ULL << 20) /* der Stack waechst bei Bedarf (Seitenfehler) bis hierhin */
#define MAX_ARGS          256
#define ARGS_BYTES        PROCESS_CMDLINE_MAX
#define PAGE              4096ULL

extern void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg1, uint64_t arg2) __attribute__((noreturn));
extern void enter_user_regs(const SyscallFrame *r) __attribute__((noreturn));

/* Ein Thread des Prozesses. Platz 0 ist der erste (main), weitere kommen per SYS_THREAD_CREATE. Die Nummer des
 * Platzes ist die Thread-Nummer fuer das Programm. Ein beendeter Thread haelt seinen Platz, bis ihn jemand abholt
 * (SYS_THREAD_JOIN). Alles unter dem BKL. */
typedef struct {
    int          tid;    /* Nummer (bleibt beim Wiederverwenden) */
    Thread      *t;      /* laufender Kernel-Thread, 0 = beendet */
    int          used;
    volatile int done;
    uint64_t     ret;    /* Rueckgabewert (SYS_THREAD_EXIT) */
    int          joiner; /* Platz des Threads, der auf diesen wartet, -1 = keiner */
    uint64_t     futex;  /* Adresse, auf die er wartet (SYS_FUTEX_WAIT), 0 = keine */
    Event        ev;     /* weckt ihn: Futex, join, Ende des Prozesses */
} UThread;

/* Threads liegen in Bloecken, die nie umziehen und nie freigegeben werden: wake_threads liest sie auch aus Interrupts
 * (Strg+C), ohne BKL. Ein neuer Block wird erst fertig gemacht und dann hinten angehaengt. */
typedef struct UThreadChunk {
    struct UThreadChunk *next;
    UThread              t[TH_CHUNK];
} UThreadChunk;

#define FOR_UTHREADS(p, u)                                                                          \
    for (UThreadChunk *c_ = __atomic_load_n(&(p)->threads, __ATOMIC_ACQUIRE); c_;                 \
         c_ = __atomic_load_n(&c_->next, __ATOMIC_ACQUIRE))                                        \
        for (UThread *u = c_->t; u < c_->t + TH_CHUNK; u++)

typedef struct {
    uint64_t    addr;
    struct Shm *obj; /* 0 = Platz frei */
} ShmMap;

/* Ausgeblendete Seiten eines Prozesses mit mehreren Threads: frei erst, wenn jede CPU, auf der gerade ein anderer
 * Thread lief, ihren TLB geleert hat (sonst schriebe er ueber einen alten TLB-Eintrag in einen neu vergebenen Frame) */
typedef struct Retired {
    struct Retired *next;
    uint32_t        mask;                 /* betroffene CPUs */
    uint64_t        snap[SMP_MAX_CPUS];   /* deren tlb_flushes beim Ausblenden */
    struct Shm     *shm;                  /* geteilter Speicher: Referenz bis dahin behalten */
    uint64_t        n;
    uint64_t        frames[];
} Retired;

/* Eine eingeblendete Datei (SYS_MMAP_FILE): die Seiten kommen erst beim ersten Zugriff (Seitenfehler) aus der Datei.
 * Privat: geschriebene Seiten gehoeren nur dem Prozess, die Datei bleibt, wie sie ist. */
typedef struct {
    uint64_t start, end; /* seitenausgerichtet; end = 0: Platz frei */
    uint64_t offset;     /* Stelle in der Datei, die bei start liegt */
    int      writable;
    FsFile   file;       /* eigene Kopie: eigene Position, unabhaengig vom Deskriptor (der darf geschlossen werden) */
} Vma;

struct Process {
    /* bleibt, wenn der Eintrag wiederverwendet wird (alloc_process): Liste und die gewachsenen Tabellen */
    Process      *next_proc;
    UThreadChunk *threads;
    int           nchunks;
    FdObj       **fds;       /* Platz fuer nfds Deskriptoren */
    int           nfds;
    Vma          *vmas;
    int           nvmas;
    ShmMap       *shm;       /* eingeblendeter geteilter Speicher */
    int           nshm;
    /* ab hier beim Wiederverwenden genullt */
    int          used;
    uint32_t     pid, parent, pgid;
    char         name[32];
    char         cwd[VFS_PATH_MAX];
    AddressSpace *as;
    int          nlive;      /* laufende Threads; der letzte raeumt den Prozess ab */
    volatile int exiting;    /* exit, Ausnahme oder kill: alle Threads beenden sich beim naechsten Kernel-Eintritt */
    int          exit_req_code, exit_req_faulted;
    Retired     *retired;
    uint64_t     entry, user_rsp, argv;
    int          argc;
    uint64_t     brk_start, brk_cur, brk_mapped; /* brk_mapped: page-aligned Ende der gemappten Seiten */
    uint64_t     mmap_next;
    volatile int exited;
    volatile int killed;
    int          exit_code;
    int          faulted;
    uint64_t     cpu_ticks;  /* Timer-Ticks aller Threads (je 10 ms; tick_sink in sched.c) */
};

/* Alle Prozess-Eintraege; sie werden nie freigegeben (beendete werden wiederverwendet), damit auch Interrupts
 * (Strg+C) die Liste ohne BKL durchgehen koennen. Neue Eintraege werden hinten angehaengt. */
static Process *proc_head, *proc_tail;
static int      proc_entries;

#define FOR_PROCS(p) \
    for (Process *p = __atomic_load_n(&proc_head, __ATOMIC_ACQUIRE); p; p = __atomic_load_n(&p->next_proc, __ATOMIC_ACQUIRE))

static void shm_release_all(Process *p);
static void tlb_reclaim(Process *p, int all);
static int  self_slot(Process *p);
static void vma_clear_all(Process *p);
static uint32_t next_pid = 1;

/* ---------- User-Speicher ---------- */

/* Stellt sicher, dass die Seite bei va mit mindestens den geforderten Rechten gemappt ist (neue Seiten sind genullt). */
static int map_user_page(AddressSpace *as, uint64_t va, uint64_t flags)
{
    uint64_t phys, old;
    if (as_translate(as, va, &phys, &old)) {
        /* schon von einem anderen Segment angelegt: Rechte vereinigen (schreibbar, wenn eins schreibbar; NX nur, wenn beide NX) */
        uint64_t merged = ((old | flags) & PAGE_WRITE) | (old & flags & PAGE_NX) | PAGE_USER;
        return as_set_flags(as, va, merged);
    }
    uint64_t frame = pmm_alloc_frame();
    if (!frame)
        return -1;
    memset((void *)frame, 0, PAGE);
    if (as_map(as, va, frame, flags | PAGE_USER) != 0) {
        pmm_free_frame(frame);
        return -1;
    }
    return 0;
}

/* Gibt die Seite bei va frei (falls gemappt). */
static void unmap_user_page(AddressSpace *as, uint64_t va)
{
    uint64_t phys, flags;
    if (as_translate(as, va, &phys, &flags)) {
        as_unmap(as, va);
        if (!(flags & PAGE_SHARED)) /* geteilte Frames gehoeren dem Shared-Memory-Objekt */
            pmm_free_frame(phys & ~(PAGE - 1));
    }
}

/* Schreibt in fremden Adressraum ueber die physischen Adressen (Identity-Mapping). */
static int user_write(AddressSpace *as, uint64_t va, const void *src, uint64_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t phys;
        if (!as_translate(as, va, &phys, 0))
            return -1;
        uint64_t chunk = PAGE - (va & (PAGE - 1));
        if (chunk > len)
            chunk = len;
        memcpy((void *)phys, s, chunk);
        va += chunk;
        s += chunk;
        len -= chunk;
    }
    return 0;
}

/* ---------- Minimaler ELF64-Loader ---------- */

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) Elf64Ehdr;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed)) Elf64Phdr;

static int load_elf(Process *p, const uint8_t *elf, uint64_t size)
{
    const Elf64Ehdr *eh = (const Elf64Ehdr *)elf;
    if (size < sizeof(*eh) || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 || eh->ident[5] != 1 || eh->type != 2 || eh->machine != 0x3E ||
        eh->phentsize != sizeof(Elf64Phdr) || eh->phoff + (uint64_t)eh->phnum * sizeof(Elf64Phdr) > size) {
        kprintf("process: kein gueltiges x86-64 ELF (ET_EXEC)\n");
        return -1;
    }
    if (eh->entry < USER_BASE || eh->entry >= USER_END) {
        kprintf("process: Entry %#lx ausserhalb des User-Bereichs\n", eh->entry);
        return -1;
    }

    uint64_t image_end = USER_BASE;
    const Elf64Phdr *ph = (const Elf64Phdr *)(elf + eh->phoff);
    for (unsigned i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
            continue;

        uint64_t va = ph[i].vaddr, end = va + ph[i].memsz;
        if (va < USER_BASE || end > USER_BRK_LIMIT || end < va ||
            ph[i].filesz > ph[i].memsz || ph[i].offset + ph[i].filesz > size) {
            kprintf("process: Segment %u ausserhalb des erlaubten Bereichs\n", i);
            return -1;
        }

        uint64_t flags = ((ph[i].flags & PF_W) ? PAGE_WRITE : 0) | ((ph[i].flags & PF_X) ? 0 : PAGE_NX);
        for (uint64_t a = va & ~(PAGE - 1); a < end; a += PAGE)
            if (map_user_page(p->as, a, flags) != 0)
                return -1;

        for (uint64_t done = 0; done < ph[i].filesz;) {
            uint64_t phys;
            as_translate(p->as, va + done, &phys, 0);
            uint64_t chunk = PAGE - ((va + done) & (PAGE - 1));
            if (chunk > ph[i].filesz - done)
                chunk = ph[i].filesz - done;
            memcpy((void *)phys, elf + ph[i].offset + done, chunk);
            done += chunk;
        }
        if (end > image_end)
            image_end = end;
    }
    p->entry = eh->entry;
    p->brk_start = p->brk_cur = p->brk_mapped = (image_end + PAGE - 1) & ~(PAGE - 1);
    p->mmap_next = USER_MMAP_BASE;

    /* Stack am oberen Ende (darunter waechst er bei Bedarf bis USER_STACK_MAX, siehe process_page_fault) */
    for (unsigned i = 1; i <= USER_STACK_PAGES; i++)
        if (map_user_page(p->as, USER_END - i * PAGE, PAGE_WRITE | PAGE_NX) != 0)
            return -1;
    return 0;
}

/* Zerlegt die Kommandozeile (bis ARGS_BYTES Zeichen, MAX_ARGS Woerter) und legt argv (SysV-artig) am oberen Ende des
 * Stacks ab. Die Puffer liegen auf dem Heap: der Kernel-Stack hat nur 16 KiB. */
static int setup_args(Process *p, const char *cmdline, const char *path)
{
    struct {
        char     buf[ARGS_BYTES];
        char    *argvp[MAX_ARGS];
        uint64_t ptrs[MAX_ARGS + 1];
    } *a = kmalloc(sizeof(*a));
    if (!a)
        return -1;
    char *buf = a->buf, **argvp = a->argvp;
    uint64_t *ptrs = a->ptrs;
    int argc = 0, rc = -1;

    size_t n = 0;
    for (; cmdline[n] && n < ARGS_BYTES - 1; n++)
        buf[n] = cmdline[n];
    buf[n] = 0;

    /* Woerter sind durch Leerzeichen getrennt; '...' und "..." fassen Leerzeichen zu einem Wort zusammen (die
     * Anfuehrungszeichen selbst fallen weg). Das Wort wird an Ort und Stelle zusammengezogen (nur kuerzer, nie laenger). */
    for (char *c = buf; *c && argc < MAX_ARGS;) {
        while (*c == ' ')
            *c++ = 0;
        if (!*c)
            break;
        char *w = c;
        argvp[argc++] = w;
        while (*c && *c != ' ') {
            if (*c == '"' || *c == '\'') {
                char q = *c++;
                while (*c && *c != q)
                    *w++ = *c++;
                if (*c)
                    c++;
            } else {
                *w++ = *c++;
            }
        }
        if (w != c)
            *w = 0; /* verkuerztes Wort abschliessen (steht c auf einem Leerzeichen, macht das die Schleife oben sonst selbst) */
    }
    if (argc == 0)
        argvp[argc++] = (char *)path;

    uint64_t strbytes = 0;
    for (int i = 0; i < argc; i++)
        strbytes += strlen(argvp[i]) + 1;

    uint64_t str_base  = (USER_END - strbytes) & ~15ULL;
    uint64_t argv_addr = (str_base - (uint64_t)(argc + 1) * 8) & ~15ULL;

    uint64_t va = str_base;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argvp[i]) + 1;
        ptrs[i] = va;
        if (user_write(p->as, va, argvp[i], len) != 0)
            goto out;
        va += len;
    }
    ptrs[argc] = 0;
    if (user_write(p->as, argv_addr, ptrs, (uint64_t)(argc + 1) * 8) != 0)
        goto out;

    p->argc = argc;
    p->argv = argv_addr;
    p->user_rsp = argv_addr - 8; /* wie nach einem 'call': rsp + 8 ist 16-Byte-ausgerichtet */
    rc = 0;
out:
    kfree(a);
    return rc;
}

/* ---------- Prozesse ---------- */

static Process *process_find(uint32_t pid)
{
    FOR_PROCS(p)
        if (p->used && p->pid == pid)
            return p;
    return 0;
}

/* ---------- Wachsende Tabellen (mit BKL) ---------- */

/* Tabelle *tab (Eintraege der Groesse size, jetzt *n Stueck) auf mindestens need Eintraege bringen, hoechstens limit;
 * neue Eintraege sind genullt. 0 = ok. */
static int table_grow(void **tab, int *n, int need, int limit, size_t size)
{
    if (need <= *n)
        return 0;
    if (need > limit)
        return -1;
    int cap = *n ? *n : 16;
    while (cap < need)
        cap *= 2;
    if (cap > limit)
        cap = limit;
    uint8_t *t = krealloc(*tab, (size_t)cap * size);
    if (!t)
        return -1;
    memset(t + (size_t)*n * size, 0, (size_t)(cap - *n) * size);
    *tab = t;
    *n = cap;
    return 0;
}

static int fd_grow(Process *p, int need)
{
    return table_grow((void **)&p->fds, &p->nfds, need, FD_LIMIT, sizeof(FdObj *));
}

static UThread *uth(Process *p, int tid)
{
    if (tid < 0)
        return 0;
    UThreadChunk *c = p->threads;
    for (; c && tid >= TH_CHUNK; tid -= TH_CHUNK)
        c = c->next;
    return c ? &c->t[tid] : 0;
}

static void uthread_reset(UThread *u)
{
    int tid = u->tid;
    memset(u, 0, sizeof(*u));
    u->tid = tid;
    u->joiner = -1;
}

/* Freien Thread-Platz suchen, sonst einen neuen Block anhaengen; 0 = zu viele oder kein Speicher */
static UThread *uthread_slot(Process *p)
{
    FOR_UTHREADS(p, u)
        if (!u->used)
            return u;
    if ((p->nchunks + 1) * TH_CHUNK > THREAD_LIMIT)
        return 0;
    UThreadChunk *c = kcalloc(1, sizeof(*c));
    if (!c)
        return 0;
    for (int i = 0; i < TH_CHUNK; i++) {
        c->t[i].tid = p->nchunks * TH_CHUNK + i;
        c->t[i].joiner = -1;
    }
    UThreadChunk **tail = &p->threads; /* erst fertig, dann einhaengen (wake_threads liest ohne BKL) */
    while (*tail)
        tail = &(*tail)->next;
    __atomic_store_n(tail, c, __ATOMIC_RELEASE);
    p->nchunks++;
    return &c->t[0];
}

static Process *alloc_process(void)
{
    Process *p = 0;
    FOR_PROCS(q)
        if (!q->used) {
            p = q;
            break;
        }
    if (!p) { /* alle belegt: neuen Eintrag anlegen und hinten anhaengen */
        if (proc_entries >= PROC_LIMIT || !(p = kcalloc(1, sizeof(*p))))
            return 0;
        uint64_t f = irq_save();
        if (proc_tail)
            __atomic_store_n(&proc_tail->next_proc, p, __ATOMIC_RELEASE);
        else
            __atomic_store_n(&proc_head, p, __ATOMIC_RELEASE);
        proc_tail = p;
        proc_entries++;
        irq_restore(f);
    }
    /* wiederverwenden: Zustand nullen, die gewachsenen Tabellen behalten (leer) */
    memset((uint8_t *)p + offsetof(Process, used), 0, sizeof(*p) - offsetof(Process, used));
    for (int i = 0; i < p->nfds; i++)
        p->fds[i] = 0;
    for (int i = 0; i < p->nvmas; i++)
        p->vmas[i].end = 0;
    for (int i = 0; i < p->nshm; i++)
        p->shm[i].obj = 0;
    FOR_UTHREADS(p, u)
        uthread_reset(u);
    p->pid = next_pid++;
    p->cwd[0] = '/';
    __atomic_store_n(&p->used, 1, __ATOMIC_RELEASE);
    return p;
}

static void set_name(Process *p, const char *path)
{
    const char *base = vfs_basename(path);
    memset(p->name, 0, sizeof(p->name));
    for (size_t i = 0; base[i] && i < sizeof(p->name) - 1; i++)
        p->name[i] = base[i];
}

static void close_all_fds(Process *p)
{
    for (int i = 0; i < p->nfds; i++) {
        if (p->fds[i]) {
            FdObj *o = p->fds[i];
            p->fds[i] = 0;
            fdobj_unref(o);
        }
    }
}

/* Konsole als stdin/stdout/stderr (ein gemeinsames Objekt) */
static int init_console_fds(Process *p)
{
    FdObj *c = fd_grow(p, 3) == 0 ? fdobj_new_console() : 0;
    if (!c)
        return -1;
    p->fds[0] = c;
    p->fds[1] = fdobj_ref(c);
    p->fds[2] = fdobj_ref(c);
    return 0;
}

static int inherit_from(Process *child, const Process *parent)
{
    int n = parent->nfds;
    while (n > 0 && !parent->fds[n - 1])
        n--;
    if (fd_grow(child, n) != 0)
        return -1;
    for (int i = 0; i < n; i++)
        if (parent->fds[i])
            child->fds[i] = fdobj_ref(parent->fds[i]);
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->pgid = parent->pgid;
    return 0;
}

/* Nach Ring 3: ab dort haelt die CPU den Big Kernel Lock nicht mehr (smp.h). Bis zum iretq bleiben Interrupts aus. */
static void __attribute__((noreturn)) to_user(uint64_t rip, uint64_t rsp, uint64_t arg1, uint64_t arg2)
{
    cpu_cli();
    bkl_release();
    enter_user(rip, rsp, arg1, arg2);
}

/* Platz fuer den ersten Thread (Nummer 0) bereitstellen, bevor er angelegt wird; 0 = ok */
static int first_slot(Process *p)
{
    return p->threads || uthread_slot(p) ? 0 : -1;
}

/* Thread gehoert zu p: process_current findet den Prozess, seine Ticks zaehlen als CPU-Zeit des Prozesses */
static void bind_thread(Thread *t, Process *p)
{
    thread_set_data(t, p);
    thread_set_tick_sink(t, &p->cpu_ticks);
}

/* Erster Thread eines neuen Prozesses (laeuft fruehestens, wenn der Erzeuger den BKL abgibt) */
static void first_thread(Process *p, Thread *t)
{
    bind_thread(t, p);
    UThread *u = uth(p, 0);
    u->t = t;
    u->used = 1;
    u->joiner = -1;
    p->nlive = 1;
}

static void process_main(void *arg)
{
    Process *p = arg;
    bind_thread(thread_current(), p);
    to_user(p->entry, p->user_rsp, (uint64_t)p->argc, p->argv);
}

int process_spawn(const char *path, const char *cmdline, uint32_t parent)
{
    /* Programm komplett einlesen (aus initrd oder von der Platte) */
    void *image;
    uint64_t image_size;
    if (fs_read_file(path, &image, &image_size) != 0)
        return -1;

    Process *p = alloc_process();
    if (!p) {
        kfree(image);
        return -1;
    }
    p->parent = parent;
    Process *pp = parent ? process_find(parent) : 0;
    int ok = first_slot(p) == 0;
    if (ok && pp) {
        ok = inherit_from(p, pp) == 0;
    } else if (ok) {
        ok = init_console_fds(p) == 0;
        p->pgid = p->pid;
    }
    set_name(p, path);

    p->as = ok ? as_create() : 0;
    int loaded = p->as && load_elf(p, image, image_size) == 0 && setup_args(p, cmdline, path) == 0;
    kfree(image); /* Segmente sind in die Frames des Prozesses kopiert */
    if (!loaded) {
        if (p->as)
            as_destroy(p->as);
        close_all_fds(p);
        p->used = 0;
        return -1;
    }

    Thread *t = thread_create_in(p->name, process_main, p, p->as);
    if (!t) {
        as_destroy(p->as);
        close_all_fds(p);
        p->used = 0;
        return -1;
    }
    first_thread(p, t);
    return (int)p->pid;
}

/* ---------- fork ---------- */

typedef struct {
    SyscallFrame regs;
    Process     *proc;
} ForkCtx;

static void fork_child_main(void *arg)
{
    ForkCtx *ctx = arg;
    Process *p = ctx->proc;
    SyscallFrame regs = ctx->regs;
    kfree(ctx);
    bind_thread(thread_current(), p);
    regs.rax = 0; /* fork liefert im Kind 0 */
    cpu_cli();
    bkl_release();
    enter_user_regs(&regs);
}

int process_fork(const SyscallFrame *f)
{
    Process *parent = process_current();
    if (!parent)
        return ERR_INVAL;
    Process *p = alloc_process();
    if (!p)
        return ERR_NOMEM;

    p->parent = parent->pid;
    int ok = first_slot(p) == 0 && inherit_from(p, parent) == 0 &&
             table_grow((void **)&p->vmas, &p->nvmas, parent->nvmas, VMA_LIMIT, sizeof(Vma)) == 0;
    memcpy(p->name, parent->name, sizeof(p->name));
    p->entry = parent->entry;
    p->user_rsp = parent->user_rsp;
    p->argv = parent->argv;
    p->argc = parent->argc;
    p->brk_start = parent->brk_start;
    p->brk_cur = parent->brk_cur;
    p->brk_mapped = parent->brk_mapped;
    p->mmap_next = parent->mmap_next;
    if (ok && parent->nvmas) /* noch nicht geladene Seiten laedt das Kind selbst */
        memcpy(p->vmas, parent->vmas, sizeof(Vma) * (size_t)parent->nvmas);

    /* Mit einem Thread Copy-on-Write: die Seiten werden erst beim Schreiben kopiert (meist ruft das Kind gleich exec
     * auf und braucht sie nie). Mit mehreren Threads eine echte Kopie - die anderen Threads koennten noch alte,
     * beschreibbare TLB-Eintraege haben, und Prozesse mit Threads haben nie PAGE_COW-Seiten (siehe thread_create). */
    p->as = !ok ? 0 : parent->nlive == 1 ? as_clone_cow(parent->as) : as_clone(parent->as);
    ForkCtx *ctx = p->as ? kmalloc(sizeof(*ctx)) : 0;
    if (ctx) {
        ctx->regs = *f;
        ctx->proc = p;
        Thread *t = thread_create_in(p->name, fork_child_main, ctx, p->as);
        if (!t) {
            kfree(ctx);
        } else {
            sched_fpu_copy_to(t); /* laeuft erst, wenn wir den BKL abgeben */
            first_thread(p, t);   /* das Kind hat nur den Thread, der fork aufgerufen hat (wie POSIX) */
        }
    }
    if (!p->nlive) {
        if (p->as)
            as_destroy(p->as);
        close_all_fds(p);
        p->used = 0;
        return ERR_NOMEM;
    }
    return (int)p->pid;
}

/* ---------- exec ---------- */

int process_exec(const char *path, const char *cmdline, void *release)
{
    Process *p = process_current();
    if (!p)
        return ERR_INVAL;
    if (p->nlive > 1)
        return ERR_AGAIN; /* erst die anderen Threads beenden: sie liefen sonst im alten Programm weiter */

    void *image;
    uint64_t image_size;
    int r = fs_read_file(path, &image, &image_size);
    if (r != 0)
        return r;

    /* Neues Programm in einen frischen Adressraum laden; der alte bleibt, bis alles geklappt hat */
    AddressSpace *old_as = p->as;
    uint64_t old_entry = p->entry, old_rsp = p->user_rsp, old_argv = p->argv;
    uint64_t old_brk_start = p->brk_start, old_brk_cur = p->brk_cur, old_brk_mapped = p->brk_mapped;
    uint64_t old_mmap = p->mmap_next;
    int old_argc = p->argc;

    AddressSpace *new_as = as_create();
    if (!new_as) {
        kfree(image);
        return ERR_NOMEM;
    }
    p->as = new_as;
    int ok = load_elf(p, image, image_size) == 0 && setup_args(p, cmdline, path) == 0;
    kfree(image);
    if (!ok) {
        p->as = old_as;
        p->entry = old_entry;
        p->user_rsp = old_rsp;
        p->argv = old_argv;
        p->argc = old_argc;
        p->brk_start = old_brk_start;
        p->brk_cur = old_brk_cur;
        p->brk_mapped = old_brk_mapped;
        p->mmap_next = old_mmap;
        as_destroy(new_as);
        return ERR_INVAL; /* kein gueltiges Programm */
    }

    set_name(p, path);
    int s = self_slot(p); /* das neue Programm beginnt als Thread 0, beendete alte Threads verfallen */
    Thread *self = s >= 0 ? uth(p, s)->t : thread_current();
    FOR_UTHREADS(p, u)
        uthread_reset(u);
    UThread *u0 = uth(p, 0);
    u0->t = self;
    u0->used = 1;
    thread_set_as(thread_current(), new_as); /* beim naechsten Threadwechsel gilt der neue Adressraum */
    this_cpu()->cur_as = new_as;
    as_switch(new_as);
    tlb_reclaim(p, 1); /* aufgehobene Seiten frueherer Threads */
    shm_release_all(p); /* geteilter Speicher gehoerte zum alten Programm */
    vma_clear_all(p);   /* eingeblendete Dateien ebenso */
    as_destroy(old_as);
    sched_fpu_reset();
    kfree(release); /* z.B. die Kommandozeile: setup_args hat sie kopiert, und von hier geht es nicht zurueck */
    to_user(p->entry, p->user_rsp, (uint64_t)p->argc, p->argv);
}

/* ---------- Warten, Beenden, Kill ---------- */

int process_wait_tick(void)
{
    Process *p = process_current();
    if (process_killed(p))
        return -1;
    thread_sleep_ms(1); /* ein Timer-Tick */
    return process_killed(p) ? -1 : 0;
}

int process_wait(int pid, uint32_t parent, int *exit_code, int *faulted, int timeout_ms)
{
    Process *p = process_find((uint32_t)pid);
    if (!p)
        return -1;
    if (parent && p->parent != parent)
        return -2;

    for (int waited = 0; !p->exited; waited += 10) {
        if (waited >= timeout_ms)
            return -1;
        if (process_wait_tick() != 0)
            return ERR_INTR;
    }
    if (exit_code)
        *exit_code = p->exit_code;
    if (faulted)
        *faulted = p->faulted ? 1 : (p->killed ? 2 : 0);
    p->used = 0; /* Eintrag frei; Adressraum und Stack raeumt der Scheduler nach dem Thread-Ende auf */
    return 0;
}

int process_poll(int pid, uint32_t parent)
{
    Process *p = process_find((uint32_t)pid);
    if (!p)
        return -1;
    if (parent && p->parent != parent)
        return -2;
    return p->exited ? 1 : 0;
}

Process *process_current(void)
{
    return thread_data(thread_current());
}

uint32_t process_pid(const Process *p)       { return p ? p->pid : 0; }
const char *process_name(const Process *p)   { return p ? p->name : "?"; }

/* Schliesst die Dateien (damit Pipes ihr Ende melden), meldet den Prozess als beendet und uebergibt Kinder */
static void finish_process(Process *p, int code, int faulted)
{
    tlb_reclaim(p, 1); /* kein anderer Thread laeuft mehr in diesem Adressraum */
    close_all_fds(p);
    shm_release_all(p); /* die Seiten bleiben bis as_destroy eingeblendet, werden aber nicht mehr benutzt */
    vma_clear_all(p);
    service_owner_exit(p->pid); /* angemeldete Dienste verschwinden */
    mouse_owner_exit(p->pid);
    igd_comp_release(p->pid);    /* Flaechen fuer das GPU-Zusammensetzen */
    console_gfx_release(p->pid); /* hatte das Programm den Bildschirm, bekommt ihn die Konsole zurueck */
    hda_close(p->pid);           /* spielte es Ton: sofort aus */
    uint64_t f = irq_save();
    p->exit_code = code;
    p->faulted = faulted;
    FOR_PROCS(q) {
        if (q->used && q->parent == p->pid) {
            if (q->exited)
                q->used = 0;              /* Zombie ohne Abnehmer: freigeben */
            else
                q->parent = PARENT_ORPHAN; /* laeuft weiter, wird beim Ende automatisch freigegeben */
        }
    }
    p->exited = 1;
    if (p->parent == PARENT_ORPHAN)
        p->used = 0;
    irq_restore(f);
}

/* ---------- Threads ---------- */

static int self_slot(Process *p)
{
    Thread *t = thread_current();
    FOR_UTHREADS(p, u)
        if (u->used && u->t == t)
            return u->tid;
    return -1;
}

/* Weckt alle wartenden Threads (Futex, join), damit sie ein Ende des Prozesses bemerken. Auch ohne BKL. */
static void wake_threads(Process *p)
{
    FOR_UTHREADS(p, u)
        if (u->used && u->t)
            event_signal(&u->ev);
}

/* Der laufende Thread verlaesst den Prozess (mit BKL). Der letzte raeumt ab: sein Kernel-Thread behaelt den
 * Adressraum, den der Scheduler nach dem Ende zerstoert. Die anderen geben ihn vorher ab - er gehoert dann den
 * verbliebenen Threads. */
static void __attribute__((noreturn)) thread_leave(Process *p, uint64_t ret)
{
    int s = self_slot(p);
    if (s >= 0) {
        UThread *u = uth(p, s);
        u->t = 0;
        u->ret = ret;
        u->done = 1;
        if (u->joiner >= 0)
            event_signal(&uth(p, u->joiner)->ev);
    }
    if (p->nlive == 1) {
        p->nlive = 0;
        if (p->exiting)
            finish_process(p, p->exit_req_code, p->exit_req_faulted);
        else
            finish_process(p, 0, 0); /* letzter Thread hat sich selbst beendet (SYS_THREAD_EXIT) */
        thread_exit();
    }
    /* Aufraeumen, solange noch mehrere zaehlen: mit einem Thread fasst der verbliebene die Liste ohne BKL an */
    tlb_reclaim(p, 0);
    p->nlive--;
    thread_set_as(thread_current(), 0);
    thread_exit();
}

/* Beendet den ganzen Prozess: der Code zaehlt vom ersten Grund (exit, Ausnahme, kill); die anderen Threads folgen
 * beim naechsten Eintritt in den Kernel (Syscall, Timer, Ende eines blockierenden Aufrufs). */
static void __attribute__((noreturn)) process_die(Process *p, int code, int faulted)
{
    if (!p->exiting) {
        p->exit_req_code = code;
        p->exit_req_faulted = faulted;
        p->exiting = 1;
        wake_threads(p);
    }
    thread_leave(p, 0);
}

void process_exit(int code)
{
    Process *p = process_current();
    if (p)
        process_die(p, code, 0);
    thread_exit();
}

void process_fault(void)
{
    Process *p = process_current();
    if (p)
        process_die(p, -1, 1);
    thread_exit();
}

void process_check_killed(void)
{
    Process *p = process_current();
    if (p && (p->killed || p->exiting))
        process_die(p, 130, 0); /* 128 + SIGINT, wie in Unix-Shells */
}

int process_kill_pending(void)
{
    const volatile Process *p = process_current();
    return p && (p->killed || p->exiting);
}

int process_killed(const Process *p)
{
    return p && (p->killed || p->exiting);
}

void process_kill_pgid(uint32_t pgid)
{
    uint64_t f = irq_save();
    FOR_PROCS(p)
        if (p->used && !p->exited && p->pgid == pgid) {
            p->killed = 1;
            wake_threads(p);
        }
    irq_restore(f);
}

int process_kill_pid(uint32_t pid)
{
    Process *p = process_find(pid);
    if (!p || p->exited)
        return ERR_NOENT;
    p->killed = 1;
    wake_threads(p);
    return 0;
}

int process_threads(const Process *p)
{
    return p ? p->nlive : 0;
}

typedef struct {
    Process *proc;
    uint64_t entry, rsp, arg;
} ThreadCtx;

static void user_thread_main(void *arg)
{
    ThreadCtx ctx = *(ThreadCtx *)arg;
    kfree(arg);
    to_user(ctx.entry, ctx.rsp, ctx.arg, 0);
}

int process_thread_create(Process *p, uint64_t entry, uint64_t stack_top, uint64_t arg)
{
    uint64_t rsp = (stack_top & ~15ULL) - 8; /* wie nach einem 'call': rsp + 8 ist 16-Byte-ausgerichtet */
    if (entry < USER_BASE || entry >= USER_END || !process_user_range_ok(p, rsp, 8, 1))
        return ERR_FAULT;
    if (p->exiting)
        return ERR_INTR;
    UThread *u = uthread_slot(p);
    if (!u)
        return ERR_AGAIN; /* THREAD_LIMIT erreicht (oder kein Speicher fuer einen neuen Block) */
    /* Vor dem zweiten Thread alle Copy-on-Write-Seiten aufloesen: loeste einer sie spaeter auf, muessten die anderen
     * CPUs erst ihren TLB leeren, bevor es weitergeht (warten koennte unter dem BKL haengen bleiben) */
    if (p->nlive == 1 && as_cow_break_all(p->as) != 0)
        return ERR_NOMEM;
    ThreadCtx *ctx = kmalloc(sizeof(*ctx));
    if (!ctx)
        return ERR_NOMEM;
    ctx->proc = p;
    ctx->entry = entry;
    ctx->rsp = rsp;
    ctx->arg = arg;
    Thread *t = thread_create_in(p->name, user_thread_main, ctx, p->as); /* laeuft erst, wenn wir den BKL abgeben */
    if (!t) {
        kfree(ctx);
        return ERR_NOMEM;
    }
    bind_thread(t, p);
    uthread_reset(u);
    u->t = t;
    u->used = 1;
    p->nlive++;
    return u->tid;
}

void process_thread_exit(Process *p, uint64_t ret)
{
    if (p->exiting)
        process_die(p, 0, 0);
    thread_leave(p, ret);
}

int process_thread_self(Process *p)
{
    return self_slot(p);
}

/* Wartet in kurzen Stuecken auf das eigene Event, bis cond wahr ist oder die Zeit um ist; ERR_INTR bei Ende/kill */
static int wait_own_event(Process *p, int s, volatile int *flag, uint64_t timeout_ms)
{
    uint64_t end = timeout_ms ? time_ms() + timeout_ms : ~0ULL;
    while (!*flag) {
        if (process_killed(p))
            return ERR_INTR;
        uint64_t now = time_ms();
        if (now >= end)
            return ERR_TIMEDOUT;
        uint64_t left = end - now;
        event_wait(&uth(p, s)->ev, left > 50 ? 50 : left);
    }
    return 0;
}

int process_thread_join(Process *p, int tid, uint64_t *ret)
{
    int s = self_slot(p);
    UThread *u = uth(p, tid);
    if (!u || !u->used || tid == s || s < 0)
        return ERR_INVAL;
    if (u->joiner >= 0 && u->joiner != s)
        return ERR_INVAL; /* wartet schon ein anderer */
    u->joiner = s;
    int r = wait_own_event(p, s, &u->done, 0);
    if (r != 0) {
        u->joiner = -1;
        return r;
    }
    if (ret)
        *ret = u->ret;
    u->used = 0; /* Platz frei */
    return 0;
}

/* Futex (nur innerhalb des Prozesses): schlafen, solange *addr == val, bis FUTEX_WAKE oder Zeitende. Pruefen und
 * Eintragen geschehen unter dem BKL, ebenso das Wecken: ein Wecken nach dem Aendern des Werts geht nicht verloren. */
int process_futex_wait(Process *p, uint64_t addr, uint32_t val, uint64_t timeout_ms)
{
    int s = self_slot(p);
    if (s < 0 || (addr & 3) || !process_user_range_ok(p, addr, 4, 0))
        return ERR_INVAL;
    if (*(volatile uint32_t *)addr != val)
        return ERR_AGAIN;
    UThread *u = uth(p, s);
    u->futex = addr;
    u->ev.pending = 0;
    uint64_t end = timeout_ms ? time_ms() + timeout_ms : ~0ULL;
    int r = 0;
    while (u->futex) { /* FUTEX_WAKE loescht den Eintrag */
        if (process_killed(p)) {
            r = ERR_INTR;
            break;
        }
        uint64_t now = time_ms();
        if (now >= end) {
            r = ERR_TIMEDOUT;
            break;
        }
        uint64_t left = end - now;
        event_wait(&u->ev, left > 50 ? 50 : left);
    }
    u->futex = 0;
    return r;
}

int process_futex_wake(Process *p, uint64_t addr, uint32_t count)
{
    int n = 0;
    FOR_UTHREADS(p, u) {
        if ((uint32_t)n >= count)
            return n;
        if (u->used && u->t && u->futex == addr && addr) {
            u->futex = 0;
            event_signal(&u->ev);
            n++;
        }
    }
    return n;
}

int process_setpgid(Process *self, uint32_t pid, uint32_t pgid)
{
    Process *t = pid == 0 ? self : process_find(pid);
    if (!t || (t != self && t->parent != self->pid))
        return ERR_INVAL;
    t->pgid = pgid ? pgid : t->pid;
    return 0;
}

int process_info(unsigned index, ProcInfo *out)
{
    uint64_t f = irq_save();
    unsigned seen = 0;
    int r = ERR_NOENT;
    FOR_PROCS(q) {
        if (!q->used || seen++ != index)
            continue;
        out->pid = q->pid;
        out->ppid = q->parent == PARENT_ORPHAN ? 0 : q->parent;
        out->pgid = q->pgid;
        out->state = q->exited ? 1 : 0;
        out->threads = (uint32_t)q->nlive;
        memcpy(out->name, q->name, sizeof(out->name));
        out->cpu_ticks = __atomic_load_n(&q->cpu_ticks, __ATOMIC_RELAXED);
        out->mem_bytes = out->shm_bytes = 0;
        if (!q->exited && q->as) { /* beendete: der Adressraum wird gerade abgebaut */
            uint64_t shared = 0;
            out->mem_bytes = as_user_pages(q->as, &shared) * PAGE;
            out->shm_bytes = shared * PAGE;
        }
        r = 0;
        break;
    }
    irq_restore(f);
    return r;
}

/* ---------- Pfade und Arbeitsverzeichnis ---------- */

/* Die Puffer (3 KiB) liegen auf dem Heap: der Kernel-Stack hat nur 16 KiB, und der Aufrufer haelt meist schon ein
 * oder zwei Pfade darauf */
int process_path(Process *p, uint64_t upath, char out[VFS_PATH_MAX])
{
    char *raw = kmalloc(3 * VFS_PATH_MAX), *joined = raw + VFS_PATH_MAX;
    if (!raw)
        return ERR_NOMEM;
    int r = process_copy_string(p, upath, raw, VFS_PATH_MAX);
    if (r < 0) {
        kfree(raw);
        return r == -2 ? ERR_NAMETOOLONG : ERR_FAULT;
    }
    size_t n = 0;
    if (raw[0] != '/') { /* relativ: Arbeitsverzeichnis voranstellen */
        size_t c = strlen(p->cwd);
        memcpy(joined, p->cwd, c);
        n = c;
        joined[n++] = '/';
    }
    memcpy(joined + n, raw, (size_t)r + 1);
    r = vfs_normalize(joined, out, VFS_PATH_MAX) == 0 ? 0 : ERR_NAMETOOLONG;
    kfree(raw);
    return r;
}

int process_chdir(Process *p, const char *abs_path)
{
    FsStat st;
    int r = fs_stat(abs_path, &st);
    if (r != 0)
        return r;
    if (!st.is_dir)
        return ERR_NOTDIR;
    size_t n = strlen(abs_path);
    memcpy(p->cwd, abs_path, n + 1);
    return 0;
}

const char *process_cwd(const Process *p)
{
    return p->cwd;
}

/* ---------- Zugriff auf User-Speicher ---------- */

int process_user_range_ok(const Process *p, uint64_t ptr, uint64_t len, int write)
{
    if (!p)
        return 0;
    if (len == 0)
        return 1;
    uint64_t end = ptr + len;
    if (end < ptr || ptr < USER_BASE || end > USER_END)
        return 0;
    for (uint64_t a = ptr & ~(PAGE - 1); a < end; a += PAGE) {
        uint64_t flags;
        if (!as_translate(p->as, a, 0, &flags) && /* eingeblendete Datei oder Stack: die Seite jetzt bereitstellen */
            !(process_page_fault((Process *)p, a, write) && as_translate(p->as, a, 0, &flags)))
            return 0;
        if (!(flags & PAGE_USER))
            return 0;
        if (write && !(flags & PAGE_WRITE) && !((flags & PAGE_COW) && as_cow_resolve(p->as, a) == 1))
            return 0; /* Copy-on-Write: der Kernel schreibt gleich hinein, also jetzt schon die eigene Kopie */
    }
    return 1;
}

int process_cow_fault(Process *p, uint64_t addr)
{
    return p && as_cow_resolve(p->as, addr) == 1;
}

int process_copy_string(const Process *p, uint64_t uptr, char *dst, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if ((i == 0 || ((uptr + i) & (PAGE - 1)) == 0) && !process_user_range_ok(p, uptr + i, 1, 0))
            return -1;
        dst[i] = ((const char *)uptr)[i];
        if (!dst[i])
            return (int)i;
    }
    return -2; /* zu lang */
}

/* ---------- Ausblenden bei mehreren Threads ----------
 * Ein Thread blendet Seiten aus, ein anderer laeuft gleichzeitig auf einer anderen CPU: deren TLB kennt die Seite
 * vielleicht noch. Die Frames bleiben deshalb reserviert, bis jede dieser CPUs ihren TLB geleert hat (tlb_flushes
 * zaehlt weiter: per IPI VECTOR_TLB sofort, sonst beim naechsten Laden von CR3). Ohne Warten, auch unter dem BKL. */

/* Platz fuer bis zu n Frames (vor dem Ausblenden holen: danach darf nichts mehr scheitern) */
static Retired *retired_new(uint64_t n)
{
    Retired *r = kmalloc(sizeof(Retired) + n * sizeof(uint64_t));
    if (r) {
        r->next = 0;
        r->mask = 0;
        r->shm = 0;
        r->n = 0;
    }
    return r;
}

static void shm_unref(struct Shm *s);

static void retired_free(Retired *r)
{
    for (uint64_t i = 0; i < r->n; i++)
        pmm_free_frame(r->frames[i]);
    if (r->shm)
        shm_unref(r->shm);
    kfree(r);
}

/* Die Seiten in r sind ausgeblendet: sofort frei, wenn kein anderer Thread des Prozesses gerade laeuft, sonst
 * spaeter (tlb_reclaim) */
static void tlb_retire(Process *p, Retired *r)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* geloeschte Eintraege vor dem Blick auf die anderen CPUs */
    Cpu *self = this_cpu();
    for (unsigned i = 0; i < smp_cpu_count() && i < SMP_MAX_CPUS; i++) {
        Cpu *c = smp_cpu(i);
        if (c && c != self && c->online && c->cur_as == (void *)p->as) {
            r->mask |= 1u << i;
            r->snap[i] = __atomic_load_n(&c->tlb_flushes, __ATOMIC_ACQUIRE);
        }
    }
    if (!r->mask) {
        retired_free(r);
        return;
    }
    for (unsigned i = 0; i < SMP_MAX_CPUS; i++)
        if (r->mask & (1u << i))
            apic_send_ipi(smp_cpu(i)->apic_id, VECTOR_TLB);
    uint64_t f = irq_save();
    r->next = p->retired;
    p->retired = r;
    irq_restore(f);
}

/* Gibt frei, was alle betroffenen CPUs nicht mehr im TLB haben koennen (all: alles, kein Thread laeuft mehr) */
static void tlb_reclaim(Process *p, int all)
{
    uint64_t f = irq_save();
    Retired *list = p->retired, *keep = 0;
    p->retired = 0;
    irq_restore(f);
    while (list) {
        Retired *r = list;
        list = r->next;
        int done = 1;
        for (unsigned i = 0; i < SMP_MAX_CPUS && !all && done; i++)
            if ((r->mask & (1u << i)) && __atomic_load_n(&smp_cpu(i)->tlb_flushes, __ATOMIC_ACQUIRE) == r->snap[i])
                done = 0;
        if (done) {
            retired_free(r);
        } else {
            r->next = keep;
            keep = r;
        }
    }
    if (keep) {
        f = irq_save();
        Retired **tail = &keep;
        while (*tail)
            tail = &(*tail)->next;
        *tail = p->retired;
        p->retired = keep;
        irq_restore(f);
    }
}

/* Blendet [a, end) aus. Mit einem Thread werden die Frames sofort frei, sonst ueber tlb_retire (r: Platz fuer alle) */
static void unmap_range(Process *p, uint64_t a, uint64_t end, Retired *r)
{
    for (; a < end; a += PAGE) {
        if (!r) {
            unmap_user_page(p->as, a);
            continue;
        }
        uint64_t phys, flags;
        if (as_translate(p->as, a, &phys, &flags)) {
            as_unmap(p->as, a);
            if (!(flags & PAGE_SHARED))
                r->frames[r->n++] = phys & ~(PAGE - 1);
        }
    }
    if (r)
        tlb_retire(p, r);
}

/* ---------- brk / mmap ----------
 * Mit einem Thread laufen sie ohne BKL (syscall_unlocked), mit mehreren mit BKL: dann kommen sich die Threads
 * nicht in die Quere. */

int64_t process_brk(Process *p, uint64_t addr)
{
    if (addr == 0)
        return (int64_t)p->brk_cur;
    if (addr < p->brk_start || addr > USER_BRK_LIMIT)
        return (int64_t)p->brk_cur; /* wie Linux: bei Fehler das alte Ende zurueckgeben */
    if (p->retired)
        tlb_reclaim(p, 0);

    uint64_t want = (addr + PAGE - 1) & ~(PAGE - 1);
    if (want > p->brk_mapped) {
        for (uint64_t a = p->brk_mapped; a < want; a += PAGE) {
            if (map_user_page(p->as, a, PAGE_WRITE | PAGE_NX) != 0) {
                for (uint64_t b = p->brk_mapped; b < a; b += PAGE) /* Rollback */
                    unmap_user_page(p->as, b);
                return (int64_t)p->brk_cur;
            }
        }
    } else if (want < p->brk_mapped) {
        Retired *r = 0;
        if (p->nlive > 1 && !(r = retired_new((p->brk_mapped - want) / PAGE)))
            return (int64_t)p->brk_cur;
        unmap_range(p, want, p->brk_mapped, r);
    }
    p->brk_mapped = want;
    p->brk_cur = addr;
    return (int64_t)addr;
}

int64_t process_mmap(Process *p, uint64_t len)
{
    if (len == 0)
        return ERR_INVAL;
    uint64_t size = (len + PAGE - 1) & ~(PAGE - 1);
    uint64_t base = p->mmap_next;
    if (size > USER_MMAP_LIMIT - base)
        return ERR_NOMEM;
    if (p->retired)
        tlb_reclaim(p, 0);

    for (uint64_t a = base; a < base + size; a += PAGE) {
        if (map_user_page(p->as, a, PAGE_WRITE | PAGE_NX) != 0) {
            for (uint64_t b = base; b < a; b += PAGE)
                unmap_user_page(p->as, b);
            return ERR_NOMEM;
        }
    }
    p->mmap_next = base + size + PAGE; /* eine ungemappte Luecke als Schutz zwischen Bereichen */
    return (int64_t)base;
}

/* ---------- Eingeblendete Dateien ---------- */

static Vma *vma_find(Process *p, uint64_t va)
{
    for (int i = 0; i < p->nvmas; i++)
        if (p->vmas[i].end && va >= p->vmas[i].start && va < p->vmas[i].end)
            return &p->vmas[i];
    return 0;
}

/* Freier Platz (die Tabelle waechst bei Bedarf; alte Vma-Zeiger gelten danach nicht mehr); -1 = VMA_LIMIT */
static int vma_free_slot(Process *p)
{
    for (int i = 0; i < p->nvmas; i++)
        if (!p->vmas[i].end)
            return i;
    int i = p->nvmas;
    return table_grow((void **)&p->vmas, &p->nvmas, i + 1, VMA_LIMIT, sizeof(Vma)) == 0 ? i : -1;
}

static void vma_clear_all(Process *p)
{
    for (int i = 0; i < p->nvmas; i++)
        if (p->vmas[i].end) {
            fs_close(&p->vmas[i].file);
            p->vmas[i].end = 0;
        }
}

/* Nimmt [a, end) aus den Einblendungen heraus (die Seiten selbst blendet munmap aus). -1: Teilen ginge nur mit
 * einem freien Platz, und es gibt keinen (dann bleibt alles, wie es ist). */
static int vma_trim(Process *p, uint64_t a, uint64_t end)
{
    int need = 0, free_slot = -1;
    for (int i = 0; i < p->nvmas; i++)
        if (p->vmas[i].end && a > p->vmas[i].start && end < p->vmas[i].end)
            need = 1; /* liegt mitten in einer Einblendung: die wird geteilt */
    if (need && (free_slot = vma_free_slot(p)) < 0)
        return -1;
    for (int i = 0; i < p->nvmas; i++) {
        Vma *v = &p->vmas[i];
        if (!v->end || end <= v->start || a >= v->end)
            continue;
        if (a <= v->start && end >= v->end) { /* ganz */
            fs_close(&v->file);
            v->end = 0;
        } else if (a <= v->start) { /* vorne */
            v->offset += end - v->start;
            v->start = end;
        } else if (end >= v->end) { /* hinten */
            v->end = a;
        } else { /* Mitte: hinterer Teil in einen neuen Platz */
            Vma *w = &p->vmas[free_slot];
            *w = *v;
            w->offset += end - v->start;
            w->start = end;
            v->end = a;
        }
    }
    return 0;
}

int64_t process_mmap_file(Process *p, int fd, uint64_t len, uint64_t offset, int writable)
{
    if (len == 0 || (offset & (PAGE - 1)))
        return ERR_INVAL;
    FdObj *o = process_fd_get(p, fd);
    if (!o || o->kind != FD_FILE)
        return ERR_BADF;
    uint64_t size = (len + PAGE - 1) & ~(PAGE - 1), base = p->mmap_next;
    int slot = size <= USER_MMAP_LIMIT - base ? vma_free_slot(p) : -1;
    if (slot < 0)
        return ERR_NOMEM;
    Vma *v = &p->vmas[slot];
    v->start = base;
    v->end = base + size;
    v->offset = offset;
    v->writable = writable;
    v->file = o->file;
    p->mmap_next = base + size + PAGE; /* eine ungemappte Luecke als Schutz, wie bei mmap */
    return (int64_t)base;
}

#define READAHEAD 16 /* Seiten je Seitenfehler (64 KiB am Stueck): weniger Fehler, und das FAT liest vorwaerts */

/* Laedt die Seite bei va aus ihrer Datei, dazu die folgenden noch fehlenden Seiten der Einblendung (bis READAHEAD).
 * 1 = eingeblendet, 0 = keine Datei-Seite (oder Schreiben auf eine nur lesbare Einblendung), -1 = kein Speicher.
 * Hinter dem Dateiende stehen Nullen. Mit BKL; das Lesen kann schlafen - danach wird nachgesehen, ob Seiten
 * inzwischen ein anderer Thread geladen oder ausgeblendet hat. */
static int vma_fault(Process *p, uint64_t va, int write)
{
    va &= ~(PAGE - 1);
    Vma *v = vma_find(p, va);
    if (!v || (write && !v->writable))
        return 0;
    uint64_t frames[READAHEAD];
    int n = 0;
    for (; n < READAHEAD && va + (uint64_t)n * PAGE < v->end; n++) {
        if (n && as_translate(p->as, va + (uint64_t)n * PAGE, 0, 0))
            break; /* schon da: hier endet das Stueck */
        if (!(frames[n] = pmm_alloc_frame()))
            break;
        memset((void *)frames[n], 0, PAGE);
    }
    if (!n)
        return -1;
    FsFile f = v->file; /* Kopie: ein anderer Thread koennte waehrenddessen dieselbe Datei lesen */
    uint64_t off = v->offset + (va - v->start);
    if (fs_seek(&f, (int64_t)off, 0) == (int64_t)off) /* hinter dem Ende geht seek nicht: dann Nullen */
        for (int k = 0; k < n; k++) {
            uint64_t got = 0;
            while (got < PAGE) {
                int64_t r = fs_read(&f, (uint8_t *)frames[k] + got, PAGE - got);
                if (r <= 0)
                    break;
                got += (uint64_t)r;
            }
            if (got < PAGE)
                break; /* Dateiende: der Rest bleibt Nullen */
        }
    v = vma_find(p, va); /* waehrend des Lesens ausgeblendet? */
    if (v)
        v->file = f; /* behaelt den Cluster-Cache: der naechste Zugriff dahinter muss die Kette nicht von vorn ablaufen */
    int ok = 0;
    for (int k = 0; k < n; k++) {
        uint64_t a = va + (uint64_t)k * PAGE;
        if (!v || a >= v->end || as_translate(p->as, a, 0, 0) ||
            as_map(p->as, a, frames[k], PAGE_USER | PAGE_NX | (v->writable ? PAGE_WRITE : 0)) != 0)
            pmm_free_frame(frames[k]);
        if (k == 0)
            ok = v && as_translate(p->as, va, 0, 0);
    }
    return ok ? 1 : (v ? -1 : 0);
}

int process_page_fault(Process *p, uint64_t addr, int write)
{
    if (!p)
        return 0;
    if (addr >= USER_END - USER_STACK_MAX && addr < USER_END) /* der Stack waechst: neue, genullte Seite */
        return map_user_page(p->as, addr & ~(PAGE - 1), PAGE_WRITE | PAGE_NX) == 0;
    return addr >= USER_MMAP_BASE && addr < USER_MMAP_LIMIT && vma_fault(p, addr, write) == 1;
}

int process_munmap(Process *p, uint64_t addr, uint64_t len)
{
    uint64_t size = (len + PAGE - 1) & ~(PAGE - 1);
    if ((addr & (PAGE - 1)) || len == 0 || addr < USER_MMAP_BASE || addr + size > p->mmap_next || addr + size < addr)
        return ERR_INVAL;
    if (p->retired)
        tlb_reclaim(p, 0);
    if (vma_trim(p, addr, addr + size) != 0)
        return ERR_NOMEM; /* eine Datei-Einblendung muesste geteilt werden, es ist aber kein Platz frei */
    Retired *r = 0;
    if (p->nlive > 1 && !(r = retired_new(size / PAGE)))
        return ERR_NOMEM;
    unmap_range(p, addr, addr + size, r);
    return 0;
}

/* ---------- Geteilter Speicher (SYS_SHM) ----------
 * Ein Objekt besteht aus einzelnen Frames, die in mehrere Adressraeume eingeblendet werden koennen (PTE-Bit
 * PAGE_SHARED: as_destroy und munmap geben sie nicht frei, fork vererbt sie nicht). Gezaehlt werden die Einblendungen;
 * mit der letzten verschwindet das Objekt. Wer die Nummer kennt, kann es einblenden (der Desktop bekommt sie von
 * seinen Fenster-Programmen). Die Objekte liegen einzeln auf dem Heap in einer Liste (unter shm_lock). */

#define SHM_MAX_BYTES (64ULL << 20)

typedef struct Shm {
    struct Shm *next;
    uint32_t    id;
    int         refs;
    uint64_t    npages;
    uint64_t   *frames;
} Shm;

static Shm     *shm_list;
static int      shm_count;
static uint32_t shm_next_id = 1;
static Spinlock shm_lock = SPINLOCK_INIT("shm");

static void shm_free_frames(uint64_t *frames, uint64_t npages)
{
    for (uint64_t i = 0; i < npages; i++)
        pmm_free_frame(frames[i]);
    kfree(frames);
}

static void shm_unref(Shm *s)
{
    uint64_t fl = spin_lock(&shm_lock);
    int last = --s->refs == 0;
    if (last) { /* aus der Liste nehmen: niemand findet es mehr */
        for (Shm **pp = &shm_list; *pp; pp = &(*pp)->next)
            if (*pp == s) {
                *pp = s->next;
                break;
            }
        shm_count--;
    }
    spin_unlock(&shm_lock, fl);
    if (last) {
        shm_free_frames(s->frames, s->npages);
        kfree(s);
    }
}

/* Blendet das Objekt (mit schon gezaehlter Referenz) ein; bei Fehler wird die Referenz abgegeben */
static int64_t shm_map(Process *p, Shm *s)
{
    int slot = -1;
    for (int i = 0; i < p->nshm && slot < 0; i++)
        if (!p->shm[i].obj)
            slot = i;
    if (slot < 0) { /* Tabelle der Einblendungen waechst */
        slot = p->nshm;
        if (table_grow((void **)&p->shm, &p->nshm, slot + 1, SHMMAP_LIMIT, sizeof(ShmMap)) != 0)
            slot = -1;
    }
    uint64_t size = s->npages * PAGE, base = p->mmap_next;
    if (slot < 0 || size > USER_MMAP_LIMIT - base) {
        shm_unref(s);
        return ERR_NOMEM;
    }
    for (uint64_t i = 0; i < s->npages; i++) {
        if (as_map(p->as, base + i * PAGE, s->frames[i], PAGE_WRITE | PAGE_NX | PAGE_USER | PAGE_SHARED) != 0) {
            for (uint64_t k = 0; k < i; k++)
                as_unmap(p->as, base + k * PAGE);
            shm_unref(s);
            return ERR_NOMEM;
        }
    }
    p->mmap_next = base + size + PAGE;
    p->shm[slot].addr = base;
    p->shm[slot].obj = s;
    return (int64_t)base;
}

static Shm *shm_find_ref(uint32_t id)
{
    Shm *r = 0;
    uint64_t fl = spin_lock(&shm_lock);
    for (Shm *s = shm_list; s && id && !r; s = s->next)
        if (s->id == id) {
            r = s;
            r->refs++;
        }
    spin_unlock(&shm_lock, fl);
    return r;
}

static void shm_release_all(Process *p)
{
    for (int i = 0; i < p->nshm; i++)
        if (p->shm[i].obj) {
            Shm *s = p->shm[i].obj;
            p->shm[i].obj = 0;
            shm_unref(s);
        }
}

/* Fuer Treiber (GPU-Zusammensetzen, igd_comp.c): Objekt mit eigener Referenz; die Frames bleiben bis shm_put gueltig */
void *shm_get(uint32_t id, uint64_t *npages, const uint64_t **frames)
{
    Shm *s = shm_find_ref(id);
    if (s) {
        *npages = s->npages;
        *frames = s->frames;
    }
    return s;
}

void shm_put(void *obj)
{
    if (obj)
        shm_unref((Shm *)obj);
}

int64_t process_shm(Process *p, uint64_t op, uint64_t a, uint64_t b)
{
    if (op == 0) { /* anlegen (Bytes, u32 *nummer) -> Adresse */
        if (a == 0 || a > SHM_MAX_BYTES || !process_user_range_ok(p, b, sizeof(uint32_t), 1))
            return ERR_INVAL;
        uint64_t n = (a + PAGE - 1) / PAGE;
        uint64_t *frames = kmalloc(n * sizeof(uint64_t));
        if (!frames)
            return ERR_NOMEM;
        for (uint64_t i = 0; i < n; i++) {
            frames[i] = pmm_alloc_frame();
            if (!frames[i]) {
                shm_free_frames(frames, i);
                return ERR_NOMEM;
            }
            memset((void *)frames[i], 0, PAGE);
        }
        Shm *s = kcalloc(1, sizeof(*s)), *drop = 0;
        uint64_t fl = spin_lock(&shm_lock);
        if (s && shm_count < SHM_LIMIT) {
            s->id = shm_next_id++;
            if (!shm_next_id)
                shm_next_id = 1;
            s->refs = 1;
            s->npages = n;
            s->frames = frames;
            s->next = shm_list;
            shm_list = s;
            shm_count++;
        } else {
            drop = s; /* zu viele Objekte im System */
            s = 0;
        }
        spin_unlock(&shm_lock, fl);
        kfree(drop);
        if (!s) {
            shm_free_frames(frames, n);
            return ERR_NOMEM;
        }
        uint32_t id = s->id;
        int64_t addr = shm_map(p, s);
        if (addr >= 0)
            *(uint32_t *)b = id;
        return addr;
    }
    if (op == 1) { /* einblenden (nummer) -> Adresse */
        Shm *s = shm_find_ref((uint32_t)a);
        return s ? shm_map(p, s) : ERR_NOENT;
    }
    if (op == 2) { /* ausblenden (adresse) */
        for (int i = 0; i < p->nshm; i++) {
            Shm *s = p->shm[i].obj;
            if (s && p->shm[i].addr == a) {
                Retired *r = 0;
                if (p->nlive > 1 && !(r = retired_new(0)))
                    return ERR_NOMEM;
                for (uint64_t k = 0; k < s->npages; k++)
                    as_unmap(p->as, a + k * PAGE);
                p->shm[i].obj = 0;
                if (r) {
                    r->shm = s; /* die Referenz geht mit: frei erst nach dem TLB der anderen CPUs */
                    tlb_retire(p, r);
                } else {
                    shm_unref(s);
                }
                return 0;
            }
        }
        return ERR_INVAL;
    }
    if (op == 3) { /* Groesse (nummer) -> Bytes */
        Shm *s = shm_find_ref((uint32_t)a);
        if (!s)
            return ERR_NOENT;
        int64_t bytes = (int64_t)(s->npages * PAGE);
        shm_unref(s);
        return bytes;
    }
    return ERR_INVAL;
}

/* ---------- Datei-Deskriptoren ---------- */

FdObj *process_fd_get(Process *p, int fd)
{
    return fd >= 0 && fd < p->nfds ? p->fds[fd] : 0;
}

FdObj *process_fd_hold(Process *p, int fd)
{
    FdObj *o = process_fd_get(p, fd);
    return o ? fdobj_ref(o) : 0;
}

/* Legt das Objekt im kleinsten freien Deskriptor ab (uebernimmt die Referenz); die Tabelle waechst bis FD_LIMIT */
static int fd_alloc(Process *p, FdObj *obj)
{
    for (int fd = 0; fd < p->nfds; fd++) {
        if (!p->fds[fd]) {
            p->fds[fd] = obj;
            return fd;
        }
    }
    int fd = p->nfds;
    if (fd_grow(p, fd + 1) != 0)
        return ERR_NOMEM;
    p->fds[fd] = obj;
    return fd;
}

int process_fd_install(Process *p, FdObj *obj)
{
    int fd = fd_alloc(p, obj);
    if (fd < 0)
        fdobj_unref(obj);
    return fd;
}

int process_fd_open(Process *p, const char *abs_path, int flags)
{
    FsFile f;
    int r = fs_open(abs_path, flags, &f);
    if (r < 0)
        return r;
    FdObj *o = fdobj_new_file(&f);
    if (!o) {
        fs_close(&f);
        return ERR_NOMEM;
    }
    int fd = fd_alloc(p, o);
    if (fd < 0)
        fdobj_unref(o); /* schliesst die Datei */
    return fd;
}

int process_fd_close(Process *p, int fd)
{
    FdObj *o = process_fd_get(p, fd);
    if (!o)
        return ERR_BADF;
    p->fds[fd] = 0;
    fdobj_unref(o);
    return 0;
}

int process_fd_closefrom(Process *p, int first)
{
    for (int fd = first < 0 ? 0 : first; fd < p->nfds; fd++)
        if (p->fds[fd]) {
            FdObj *o = p->fds[fd];
            p->fds[fd] = 0;
            fdobj_unref(o);
        }
    return 0;
}

int process_fd_dup(Process *p, int old_fd)
{
    FdObj *o = process_fd_get(p, old_fd);
    if (!o)
        return ERR_BADF;
    int fd = fd_alloc(p, fdobj_ref(o));
    if (fd < 0)
        fdobj_unref(o);
    return fd;
}

int process_fd_dup2(Process *p, int old_fd, int new_fd)
{
    FdObj *o = process_fd_get(p, old_fd);
    if (!o || new_fd < 0 || new_fd >= FD_LIMIT)
        return ERR_BADF;
    if (fd_grow(p, new_fd + 1) != 0)
        return ERR_NOMEM;
    if (old_fd == new_fd)
        return new_fd;
    if (p->fds[new_fd]) {
        FdObj *x = p->fds[new_fd];
        p->fds[new_fd] = 0;
        fdobj_unref(x);
    }
    p->fds[new_fd] = fdobj_ref(o);
    return new_fd;
}

int process_pipe(Process *p, int fds[2])
{
    FdObj *r, *w;
    if (fdobj_new_pipe(&r, &w) != 0)
        return ERR_NOMEM;
    int a = fd_alloc(p, r);
    if (a < 0) {
        fdobj_unref(r);
        fdobj_unref(w);
        return a;
    }
    int b = fd_alloc(p, w);
    if (b < 0) {
        p->fds[a] = 0;
        fdobj_unref(r);
        fdobj_unref(w);
        return b;
    }
    fds[0] = a;
    fds[1] = b;
    return 0;
}

/* Lesen und Schreiben koennen blockieren (und den BKL abgeben): solange haelt der Aufruf eine eigene Referenz,
 * damit ein anderer Thread den Deskriptor schliessen kann, ohne das Objekt unter ihm freizugeben. */
int64_t process_fd_read(Process *p, int fd, void *buf, uint64_t len)
{
    FdObj *o = process_fd_hold(p, fd);
    if (!o)
        return ERR_BADF;
    int64_t r = fdobj_read(o, buf, len);
    fdobj_unref(o);
    return r;
}

int64_t process_fd_write(Process *p, int fd, const void *buf, uint64_t len)
{
    FdObj *o = process_fd_hold(p, fd);
    if (!o)
        return ERR_BADF;
    int64_t r = fdobj_write(o, buf, len);
    fdobj_unref(o);
    return r;
}

int64_t process_fd_seek(Process *p, int fd, int64_t off, int whence)
{
    FdObj *o = process_fd_hold(p, fd);
    if (!o)
        return ERR_BADF;
    int64_t r = fdobj_seek(o, off, whence);
    fdobj_unref(o);
    return r;
}
