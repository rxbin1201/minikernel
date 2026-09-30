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

#define MAX_PROC          64
#define MAX_FD            32
#define USER_STACK_PAGES  16
#define MAX_ARGS          16
#define ARGS_BYTES        512
#define PAGE              4096ULL

extern void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg1, uint64_t arg2) __attribute__((noreturn));
extern void enter_user_regs(const SyscallFrame *r) __attribute__((noreturn));

struct Process {
    int          used;
    uint32_t     pid, parent, pgid;
    char         name[32];
    char         cwd[VFS_PATH_MAX];
    AddressSpace *as;
    Thread      *thread;
    uint64_t     entry, user_rsp, argv;
    int          argc;
    uint64_t     brk_start, brk_cur, brk_mapped; /* brk_mapped: page-aligned Ende der gemappten Seiten */
    uint64_t     mmap_next;
    FdObj       *fds[MAX_FD];
    volatile int exited;
    volatile int killed;
    int          exit_code;
    int          faulted;
};

static Process procs[MAX_PROC];
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
    uint64_t phys;
    if (as_translate(as, va, &phys, 0)) {
        as_unmap(as, va);
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

    p->thread = thread_create_in(p->name, process_main, p, p->as);
    if (!p->thread) {
        as_destroy(p->as);
        close_all_fds(p);
        p->used = 0;
        return -1;
    }
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

    p->as = as_clone(parent->as);
    ForkCtx *ctx = p->as ? kmalloc(sizeof(*ctx)) : 0;
    if (ctx) {
        ctx->regs = *f;
        ctx->proc = p;
        p->thread = thread_create_in(p->name, fork_child_main, ctx, p->as);
        if (!p->thread)
            kfree(ctx);
        else
            sched_fpu_copy_to(p->thread); /* laeuft erst, wenn wir den BKL abgeben */
    }
    if (!p->thread) {
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
    thread_set_as(thread_current(), new_as); /* beim naechsten Threadwechsel gilt der neue Adressraum */
    as_switch(new_as);
    as_destroy(old_as);
    sched_fpu_reset();
    to_user(p->entry, p->user_rsp, (uint64_t)p->argc, p->argv);
}

/* ---------- Warten, Beenden, Kill ---------- */

int process_wait_tick(void)
{
    Process *p = process_current();
    if (p && p->killed)
        return -1;
    thread_sleep_ms(1); /* ein Timer-Tick */
    return p && p->killed ? -1 : 0;
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
    close_all_fds(p);
    mouse_owner_exit(p->pid);
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

void process_exit(int code)
{
    Process *p = process_current();
    if (p)
        finish_process(p, code, 0);
    thread_exit();
}

void process_fault(void)
{
    Process *p = process_current();
    if (p)
        finish_process(p, -1, 1);
    thread_exit();
}

void process_check_killed(void)
{
    Process *p = process_current();
    if (p && p->killed) {
        finish_process(p, 130, 0); /* 128 + SIGINT, wie in Unix-Shells */
        thread_exit();
    }
}

int process_kill_pending(void)
{
    const volatile Process *p = process_current();
    return p && p->killed;
}

int process_killed(const Process *p)
{
    return p && p->killed;
}

void process_kill_pgid(uint32_t pgid)
{
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_PROC; i++)
        if (procs[i].used && !procs[i].exited && procs[i].pgid == pgid)
            procs[i].killed = 1;
    irq_restore(f);
}

int process_kill_pid(uint32_t pid)
{
    Process *p = process_find(pid);
    if (!p || p->exited)
        return ERR_NOENT;
    p->killed = 1;
    return 0;
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
        if (!as_translate(p->as, a, 0, &flags) || !(flags & PAGE_USER) || (write && !(flags & PAGE_WRITE)))
            return 0;
    }
    return 1;
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

/* ---------- brk / mmap ---------- */

int64_t process_brk(Process *p, uint64_t addr)
{
    if (addr == 0)
        return (int64_t)p->brk_cur;
    if (addr < p->brk_start || addr > USER_BRK_LIMIT)
        return (int64_t)p->brk_cur; /* wie Linux: bei Fehler das alte Ende zurueckgeben */

    uint64_t want = (addr + PAGE - 1) & ~(PAGE - 1);
    if (want > p->brk_mapped) {
        for (uint64_t a = p->brk_mapped; a < want; a += PAGE) {
            if (map_user_page(p->as, a, PAGE_WRITE | PAGE_NX) != 0) {
                for (uint64_t b = p->brk_mapped; b < a; b += PAGE) /* Rollback */
                    unmap_user_page(p->as, b);
                return (int64_t)p->brk_cur;
            }
        }
    } else {
        for (uint64_t a = want; a < p->brk_mapped; a += PAGE)
            unmap_user_page(p->as, a);
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
    for (uint64_t a = addr; a < addr + size; a += PAGE)
        unmap_user_page(p->as, a);
    return 0;
}

/* ---------- Datei-Deskriptoren ---------- */

FdObj *process_fd_get(Process *p, int fd)
{
    return fd >= 0 && fd < MAX_FD ? p->fds[fd] : 0;
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

int64_t process_fd_read(Process *p, int fd, void *buf, uint64_t len)
{
    FdObj *o = process_fd_get(p, fd);
    return o ? fdobj_read(o, buf, len) : ERR_BADF;
}

int64_t process_fd_write(Process *p, int fd, const void *buf, uint64_t len)
{
    FdObj *o = process_fd_get(p, fd);
    return o ? fdobj_write(o, buf, len) : ERR_BADF;
}

int64_t process_fd_seek(Process *p, int fd, int64_t off, int whence)
{
    FdObj *o = process_fd_get(p, fd);
    return o ? fdobj_seek(o, off, whence) : ERR_BADF;
}
