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

#define MAX_PROC          64
#define MAX_FD            32
#define USER_STACK_PAGES  16
#define MAX_ARGS          16
#define ARGS_BYTES        512
#define PAGE              4096ULL
#define MAX_SHM_MAPS      80   /* Desktop: Fenster der Programme, eigene Fensterbilder und Schatten */
#define MAX_THREADS       16   /* Threads je Prozess (mit beendeten, noch nicht abgeholten) */

extern void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg1, uint64_t arg2) __attribute__((noreturn));
extern void enter_user_regs(const SyscallFrame *r) __attribute__((noreturn));

/* Ein Thread des Prozesses. Platz 0 ist der erste (main), weitere kommen per SYS_THREAD_CREATE. Die Nummer des
 * Platzes ist die Thread-Nummer fuer das Programm. Ein beendeter Thread haelt seinen Platz, bis ihn jemand abholt
 * (SYS_THREAD_JOIN). Alles unter dem BKL. */
typedef struct {
    Thread      *t;      /* laufender Kernel-Thread, 0 = beendet */
    int          used;
    volatile int done;
    uint64_t     ret;    /* Rueckgabewert (SYS_THREAD_EXIT) */
    int          joiner; /* Platz des Threads, der auf diesen wartet, -1 = keiner */
    uint64_t     futex;  /* Adresse, auf die er wartet (SYS_FUTEX_WAIT), 0 = keine */
    Event        ev;     /* weckt ihn: Futex, join, Ende des Prozesses */
} UThread;

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

struct Process {
    int          used;
    uint32_t     pid, parent, pgid;
    char         name[32];
    char         cwd[VFS_PATH_MAX];
    AddressSpace *as;
    UThread      th[MAX_THREADS];
    int          nlive;      /* laufende Threads; der letzte raeumt den Prozess ab */
    volatile int exiting;    /* exit, Ausnahme oder kill: alle Threads beenden sich beim naechsten Kernel-Eintritt */
    int          exit_req_code, exit_req_faulted;
    Retired     *retired;
    uint64_t     entry, user_rsp, argv;
    int          argc;
    uint64_t     brk_start, brk_cur, brk_mapped; /* brk_mapped: page-aligned Ende der gemappten Seiten */
    uint64_t     mmap_next;
    FdObj       *fds[MAX_FD];
    volatile int exited;
    volatile int killed;
    int          exit_code;
    int          faulted;
    struct { uint64_t addr; struct Shm *obj; } shm[MAX_SHM_MAPS]; /* eingeblendeter geteilter Speicher */
};

static Process procs[MAX_PROC];
static void shm_release_all(Process *p);
static void tlb_reclaim(Process *p, int all);
static int  self_slot(Process *p);
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

    /* Stack am oberen Ende; darunter bleibt eine ungemappte Guard-Region */
    for (unsigned i = 1; i <= USER_STACK_PAGES; i++)
        if (map_user_page(p->as, USER_END - i * PAGE, PAGE_WRITE | PAGE_NX) != 0)
            return -1;
    return 0;
}

/* Zerlegt die Kommandozeile und legt argv (SysV-artig) am oberen Ende des Stacks ab. */
static int setup_args(Process *p, const char *cmdline, const char *path)
{
    char buf[ARGS_BYTES];
    char *argvp[MAX_ARGS];
    int argc = 0;

    size_t n = 0;
    for (; cmdline[n] && n < sizeof(buf) - 1; n++)
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

    uint64_t ptrs[MAX_ARGS + 1];
    uint64_t va = str_base;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argvp[i]) + 1;
        ptrs[i] = va;
        if (user_write(p->as, va, argvp[i], len) != 0)
            return -1;
        va += len;
    }
    ptrs[argc] = 0;
    if (user_write(p->as, argv_addr, ptrs, (uint64_t)(argc + 1) * 8) != 0)
        return -1;

    p->argc = argc;
    p->argv = argv_addr;
    p->user_rsp = argv_addr - 8; /* wie nach einem 'call': rsp + 8 ist 16-Byte-ausgerichtet */
    return 0;
}

/* ---------- Prozesse ---------- */

static Process *process_find(uint32_t pid)
{
    for (int i = 0; i < MAX_PROC; i++)
        if (procs[i].used && procs[i].pid == pid)
            return &procs[i];
    return 0;
}

static Process *alloc_process(void)
{
    uint64_t f = irq_save();
    Process *p = 0;
    for (int i = 0; i < MAX_PROC; i++) {
        if (!procs[i].used) {
            p = &procs[i];
            break;
        }
    }
    if (p) {
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->pid = next_pid++;
        p->cwd[0] = '/';
    }
    irq_restore(f);
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
    for (int i = 0; i < MAX_FD; i++) {
        if (p->fds[i]) {
            FdObj *o = p->fds[i];
            p->fds[i] = 0;
            fdobj_unref(o);
        }
    }
}

/* Konsole als stdin/stdout/stderr (ein gemeinsames Objekt) */
static void init_console_fds(Process *p)
{
    FdObj *c = fdobj_new_console();
    if (!c)
        return;
    p->fds[0] = c;
    p->fds[1] = fdobj_ref(c);
    p->fds[2] = fdobj_ref(c);
}

static void inherit_from(Process *child, const Process *parent)
{
    for (int i = 0; i < MAX_FD; i++)
        if (parent->fds[i])
            child->fds[i] = fdobj_ref(parent->fds[i]);
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->pgid = parent->pgid;
}

/* Nach Ring 3: ab dort haelt die CPU den Big Kernel Lock nicht mehr (smp.h). Bis zum iretq bleiben Interrupts aus. */
static void __attribute__((noreturn)) to_user(uint64_t rip, uint64_t rsp, uint64_t arg1, uint64_t arg2)
{
    cpu_cli();
    bkl_release();
    enter_user(rip, rsp, arg1, arg2);
}

/* Erster Thread eines neuen Prozesses (laeuft fruehestens, wenn der Erzeuger den BKL abgibt) */
static void first_thread(Process *p, Thread *t)
{
    thread_set_data(t, p);
    p->th[0].t = t;
    p->th[0].used = 1;
    p->th[0].joiner = -1;
    p->nlive = 1;
}

static void process_main(void *arg)
{
    Process *p = arg;
    thread_set_data(thread_current(), p);
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
    if (pp) {
        inherit_from(p, pp);
    } else {
        init_console_fds(p);
        p->pgid = p->pid;
    }
    set_name(p, path);

    p->as = as_create();
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
    thread_set_data(thread_current(), p);
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
    inherit_from(p, parent);
    memcpy(p->name, parent->name, sizeof(p->name));
    p->entry = parent->entry;
    p->user_rsp = parent->user_rsp;
    p->argv = parent->argv;
    p->argc = parent->argc;
    p->brk_start = parent->brk_start;
    p->brk_cur = parent->brk_cur;
    p->brk_mapped = parent->brk_mapped;
    p->mmap_next = parent->mmap_next;

    /* Mit einem Thread Copy-on-Write: die Seiten werden erst beim Schreiben kopiert (meist ruft das Kind gleich exec
     * auf und braucht sie nie). Mit mehreren Threads eine echte Kopie - die anderen Threads koennten noch alte,
     * beschreibbare TLB-Eintraege haben, und Prozesse mit Threads haben nie PAGE_COW-Seiten (siehe thread_create). */
    p->as = parent->nlive == 1 ? as_clone_cow(parent->as) : as_clone(parent->as);
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

int process_exec(const char *path, const char *cmdline)
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
    Thread *self = s >= 0 ? p->th[s].t : thread_current();
    memset(p->th, 0, sizeof(p->th));
    p->th[0].t = self;
    p->th[0].used = 1;
    p->th[0].joiner = -1;
    thread_set_as(thread_current(), new_as); /* beim naechsten Threadwechsel gilt der neue Adressraum */
    this_cpu()->cur_as = new_as;
    as_switch(new_as);
    tlb_reclaim(p, 1); /* aufgehobene Seiten frueherer Threads */
    shm_release_all(p); /* geteilter Speicher gehoerte zum alten Programm */
    as_destroy(old_as);
    sched_fpu_reset();
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
    service_owner_exit(p->pid); /* angemeldete Dienste verschwinden */
    mouse_owner_exit(p->pid);
    igd_comp_release(p->pid);    /* Flaechen fuer das GPU-Zusammensetzen */
    console_gfx_release(p->pid); /* hatte das Programm den Bildschirm, bekommt ihn die Konsole zurueck */
    hda_close(p->pid);           /* spielte es Ton: sofort aus */
    uint64_t f = irq_save();
    p->exit_code = code;
    p->faulted = faulted;
    for (int i = 0; i < MAX_PROC; i++) {
        Process *q = &procs[i];
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
    for (int i = 0; i < MAX_THREADS; i++)
        if (p->th[i].used && p->th[i].t == t)
            return i;
    return -1;
}

/* Weckt alle wartenden Threads (Futex, join), damit sie ein Ende des Prozesses bemerken. Auch ohne BKL. */
static void wake_threads(Process *p)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (p->th[i].used && p->th[i].t)
            event_signal(&p->th[i].ev);
}

/* Der laufende Thread verlaesst den Prozess (mit BKL). Der letzte raeumt ab: sein Kernel-Thread behaelt den
 * Adressraum, den der Scheduler nach dem Ende zerstoert. Die anderen geben ihn vorher ab - er gehoert dann den
 * verbliebenen Threads. */
static void __attribute__((noreturn)) thread_leave(Process *p, uint64_t ret)
{
    int s = self_slot(p);
    if (s >= 0) {
        UThread *u = &p->th[s];
        u->t = 0;
        u->ret = ret;
        u->done = 1;
        if (u->joiner >= 0)
            event_signal(&p->th[u->joiner].ev);
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
    for (int i = 0; i < MAX_PROC; i++)
        if (procs[i].used && !procs[i].exited && procs[i].pgid == pgid) {
            procs[i].killed = 1;
            wake_threads(&procs[i]);
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
    int s = -1;
    for (int i = 0; i < MAX_THREADS && s < 0; i++)
        if (!p->th[i].used)
            s = i;
    if (s < 0)
        return ERR_AGAIN;
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
    thread_set_data(t, p);
    UThread *u = &p->th[s];
    memset(u, 0, sizeof(*u));
    u->t = t;
    u->used = 1;
    u->joiner = -1;
    p->nlive++;
    return s;
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
        event_wait(&p->th[s].ev, left > 50 ? 50 : left);
    }
    return 0;
}

int process_thread_join(Process *p, int tid, uint64_t *ret)
{
    int s = self_slot(p);
    if (tid < 0 || tid >= MAX_THREADS || !p->th[tid].used || tid == s || s < 0)
        return ERR_INVAL;
    UThread *u = &p->th[tid];
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
    UThread *u = &p->th[s];
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
    for (int i = 0; i < MAX_THREADS && (uint32_t)n < count; i++) {
        UThread *u = &p->th[i];
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
    for (int i = 0; i < MAX_PROC; i++) {
        if (!procs[i].used || seen++ != index)
            continue;
        out->pid = procs[i].pid;
        out->ppid = procs[i].parent == PARENT_ORPHAN ? 0 : procs[i].parent;
        out->pgid = procs[i].pgid;
        out->state = procs[i].exited ? 1 : 0;
        out->threads = (uint32_t)procs[i].nlive;
        memcpy(out->name, procs[i].name, sizeof(out->name));
        r = 0;
        break;
    }
    irq_restore(f);
    return r;
}

/* ---------- Pfade und Arbeitsverzeichnis ---------- */

int process_path(Process *p, uint64_t upath, char out[VFS_PATH_MAX])
{
    char raw[VFS_PATH_MAX];
    if (process_copy_string(p, upath, raw, sizeof(raw)) < 0)
        return ERR_FAULT;

    char joined[2 * VFS_PATH_MAX];
    size_t n = 0;
    if (raw[0] != '/') { /* relativ: Arbeitsverzeichnis voranstellen */
        size_t c = strlen(p->cwd);
        memcpy(joined, p->cwd, c);
        n = c;
        joined[n++] = '/';
    }
    size_t r = strlen(raw);
    memcpy(joined + n, raw, r + 1);
    vfs_normalize(joined, out, VFS_PATH_MAX);
    return 0;
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
        if (!as_translate(p->as, a, 0, &flags) || !(flags & PAGE_USER))
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
    return -1; /* zu lang */
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

int process_munmap(Process *p, uint64_t addr, uint64_t len)
{
    uint64_t size = (len + PAGE - 1) & ~(PAGE - 1);
    if ((addr & (PAGE - 1)) || len == 0 || addr < USER_MMAP_BASE || addr + size > p->mmap_next || addr + size < addr)
        return ERR_INVAL;
    if (p->retired)
        tlb_reclaim(p, 0);
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
 * seinen Fenster-Programmen). */

#define MAX_SHM       192
#define SHM_MAX_BYTES (64ULL << 20)

typedef struct Shm {
    uint32_t  id; /* 0 = frei */
    int       refs;
    uint64_t  npages;
    uint64_t *frames;
} Shm;

static Shm      shms[MAX_SHM];
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
    uint64_t *frames = 0, n = 0;
    if (--s->refs == 0) {
        frames = s->frames;
        n = s->npages;
        s->frames = 0;
        s->id = 0;
    }
    spin_unlock(&shm_lock, fl);
    if (frames)
        shm_free_frames(frames, n);
}

/* Blendet das Objekt (mit schon gezaehlter Referenz) ein; bei Fehler wird die Referenz abgegeben */
static int64_t shm_map(Process *p, Shm *s)
{
    int slot = -1;
    for (int i = 0; i < MAX_SHM_MAPS && slot < 0; i++)
        if (!p->shm[i].obj)
            slot = i;
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
    for (int i = 0; i < MAX_SHM && id; i++)
        if (shms[i].id == id) {
            r = &shms[i];
            r->refs++;
            break;
        }
    spin_unlock(&shm_lock, fl);
    return r;
}

static void shm_release_all(Process *p)
{
    for (int i = 0; i < MAX_SHM_MAPS; i++)
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
        Shm *s = 0;
        uint64_t fl = spin_lock(&shm_lock);
        for (int i = 0; i < MAX_SHM && !s; i++)
            if (!shms[i].id && !shms[i].frames)
                s = &shms[i];
        if (s) {
            s->id = shm_next_id++;
            if (!shm_next_id)
                shm_next_id = 1;
            s->refs = 1;
            s->npages = n;
            s->frames = frames;
        }
        spin_unlock(&shm_lock, fl);
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
        for (int i = 0; i < MAX_SHM_MAPS; i++) {
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
    return fd >= 0 && fd < MAX_FD ? p->fds[fd] : 0;
}

FdObj *process_fd_hold(Process *p, int fd)
{
    FdObj *o = process_fd_get(p, fd);
    return o ? fdobj_ref(o) : 0;
}

/* Legt das Objekt im kleinsten freien Deskriptor ab (uebernimmt die Referenz) */
static int fd_alloc(Process *p, FdObj *obj)
{
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (!p->fds[fd]) {
            p->fds[fd] = obj;
            return fd;
        }
    }
    return ERR_NOMEM;
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
    if (!o || new_fd < 0 || new_fd >= MAX_FD)
        return ERR_BADF;
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
