#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <stdint.h>
#include "mm/paging.h"
#include "core/syscall.h"
#include "fs/vfs.h"

/* User-Prozesse: je ein eigener Adressraum, ein Thread und ein statisch gelinktes ELF64 (ET_EXEC) mit
 * Segmenten im User-Bereich (USER_BASE .. USER_END). Das Programm kommt aus dem VFS (initrd oder /disk).
 *
 * Jeder Prozess hat eine Tabelle von Datei-Deskriptoren (Objekte aus fdobj.h, die bei fork/dup geteilt werden),
 * ein Arbeitsverzeichnis, eine Prozessgruppe (fuer Ctrl-C) und ein Kill-Flag. */

typedef struct Process Process;
struct FdObj;

/* User-Speicherlayout (relativ zu USER_BASE = 0x7F8000000000):
 *   ELF-Segmente, dahinter der brk-Heap (waechst bis +32 GiB),
 *   mmap-Bereich +64 GiB .. +256 GiB, Stack: die obersten 64 KiB vor USER_END. */
#define USER_BRK_LIMIT   (USER_BASE + (32ULL << 30))
#define USER_MMAP_BASE   (USER_BASE + (64ULL << 30))
#define USER_MMAP_LIMIT  (USER_BASE + (256ULL << 30))

#define PARENT_ORPHAN 0xFFFFFFFFu /* Elternprozess ist beendet */

/* Startet `path` (Datei im VFS). `cmdline` wird an Leerzeichen zerlegt und als argc/argv uebergeben
 * (leer: argv[0] = path). `parent` = PID des Erzeugers oder 0 (Kernel). Ein Kind erbt Datei-Deskriptoren,
 * Arbeitsverzeichnis und Prozessgruppe des Erzeugers; ohne Erzeuger bekommt es die Konsole als stdin/stdout/stderr.
 * Liefert die PID oder -1. */
int process_spawn(const char *path, const char *cmdline, uint32_t parent);

/* fork: Kopie des aktuellen Prozesses (eigener Adressraum mit kopierten Seiten, geteilte Dateiobjekte).
 * Das Kind laeuft mit den Registern aus `f` weiter und bekommt 0 als Rueckgabewert. Liefert im Elternprozess die PID. */
int process_fork(const SyscallFrame *f);

/* exec: ersetzt das Programm des aktuellen Prozesses. Kehrt nur bei einem Fehler zurueck (dann bleibt alles unveraendert). */
int process_exec(const char *path, const char *cmdline);

/* Wartet, bis der Prozess beendet ist. `parent` != 0: nur eigene Kinder (sonst -2). 0 = beendet, -1 = Timeout
 * oder unbekannte PID, -4 = unterbrochen. *faulted: 0 = normal, 1 = durch Fehler, 2 = per Ctrl-C/kill beendet.
 * Gibt den Prozesseintrag frei. */
int process_wait(int pid, uint32_t parent, int *exit_code, int *faulted, int timeout_ms);
/* Ohne zu warten: -1 unbekannt, -2 kein Kind, 0 laeuft noch, 1 beendet (mit process_wait abholbar) */
int process_poll(int pid, uint32_t parent);

Process *process_current(void);          /* Prozess des laufenden Threads oder NULL */
uint32_t process_pid(const Process *p);
const char *process_name(const Process *p);

void process_exit(int code) __attribute__((noreturn));
void process_fault(void) __attribute__((noreturn)); /* Prozess wegen Ausnahme beenden (exit code -1) */

/* Prozessgruppen, Ctrl-C und kill. Ein "gekillter" Prozess beendet sich, sobald er wieder in den Kernel kommt
 * (Syscall oder Timer-Interrupt); blockierende Aufrufe kehren mit -4 zurueck. */
int  process_setpgid(Process *p, uint32_t pid, uint32_t pgid);
void process_kill_pgid(uint32_t pgid);
int  process_kill_pid(uint32_t pid);
int  process_killed(const Process *p);
void process_check_killed(void);        /* beendet den aktuellen Prozess, falls gekillt */
int  process_kill_pending(void);        /* soll der aktuelle Prozess beendet werden? (ohne BKL lesbar) */
int  process_wait_tick(void);           /* schlaeft einen Timer-Tick; -1, wenn der Prozess gekillt wurde (fuer blockierende Aufrufe) */

/* Auskunft fuer "ps" */
int process_info(unsigned index, ProcInfo *out);

/* Zugriff auf User-Speicher des aktuellen Prozesses (laufen im Adressraum des Prozesses). */
int process_user_range_ok(const Process *p, uint64_t ptr, uint64_t len, int write);
int process_copy_string(const Process *p, uint64_t uptr, char *dst, size_t max); /* Laenge oder -1 */

/* Pfad aus dem User-Speicher holen und relativ zum Arbeitsverzeichnis zu einem absoluten, normalisierten Pfad machen */
int  process_path(Process *p, uint64_t upath, char out[VFS_PATH_MAX]);
int  process_chdir(Process *p, const char *abs_path);
const char *process_cwd(const Process *p);

/* Speicher */
int64_t process_brk(Process *p, uint64_t addr);            /* addr 0 = abfragen; liefert neues Programmende */
int64_t process_mmap(Process *p, uint64_t len);            /* anonym, RW, genullt; Adresse oder negativer Fehler */
int     process_munmap(Process *p, uint64_t addr, uint64_t len);
/* Geteilter Speicher (SYS_SHM): 0 anlegen (bytes, u32 *nummer) -> Adresse, 1 einblenden (nummer) -> Adresse,
 * 2 ausblenden (adresse), 3 Groesse (nummer) -> Bytes */
int64_t process_shm(Process *p, uint64_t op, uint64_t a, uint64_t b);
void   *shm_get(uint32_t id, uint64_t *npages, const uint64_t **frames); /* Objekt mit Referenz (Treiber), 0 = gibt es nicht */
void    shm_put(void *obj);

/* Datei-Deskriptoren */
struct FdObj *process_fd_get(Process *p, int fd);
int process_fd_install(Process *p, struct FdObj *obj); /* kleinster freier fd (uebernimmt die Referenz; bei Fehler freigegeben) */
int     process_fd_open(Process *p, const char *abs_path, int flags); /* fd oder negativer Fehler */
int     process_fd_close(Process *p, int fd);
int     process_fd_dup(Process *p, int old_fd);
int     process_fd_dup2(Process *p, int old_fd, int new_fd);
int     process_pipe(Process *p, int fds[2]);
int64_t process_fd_read(Process *p, int fd, void *buf, uint64_t len);
int64_t process_fd_write(Process *p, int fd, const void *buf, uint64_t len);
int64_t process_fd_seek(Process *p, int fd, int64_t off, int whence);

#endif
