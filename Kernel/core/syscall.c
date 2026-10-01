#include "core/syscall.h"
#include "arch/x86_64/apic.h"
#include "console/console.h"
#include "arch/x86_64/cpu.h"
#include "core/fdobj.h"
#include "fs/fs.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/smp.h"
#include "lib/klog.h"
#include "drivers/gpu/igd.h"
#include "drivers/sound/hda.h"
#include "drivers/keyboard.h"
#include "lib/kprintf.h"
#include "arch/x86_64/power.h"
#include "drivers/rtc.h"
#include "core/process.h"
#include "core/sched.h"
#include "lib/string.h"
#include "core/tty.h"
#include "drivers/usb/usb.h"
#include "drivers/mouse.h"
#include "drivers/keymap.h"
#include "net/net.h"
#include "drivers/pci.h"
#include "console/font.h"
#include "drivers/video.h"
#include "fs/vfs.h"
#include "core/service.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE   (1ULL << 0)

extern void syscall_entry(void);

void syscall_init(void)
{
    /* STAR[47:32] = Kernel-CS (SS = +8), STAR[63:48] = Basis fuer sysret: SS = +8, CS = +16.
     * Passt zur GDT: 0x08 kcode, 0x10 kdata, 0x18 udata, 0x20 ucode. */
    wrmsr(MSR_STAR, ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x700); /* IF, DF und TF beim Einsprung loeschen */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

/* ---------- Hilfen ---------- */

#define IO_MAX (1u << 20) /* mehr als 1 MiB pro Aufruf wird abgeschnitten */

/* Holt einen Pfad aus dem User-Speicher und macht ihn absolut */
static int get_path(uint64_t upath, char out[VFS_PATH_MAX])
{
    return process_path(process_current(), upath, out);
}

/* Fuer Syscalls mit einem Pfad und einer Operation ohne weitere Argumente */
static int64_t path_op(uint64_t upath, int (*op)(const char *))
{
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    return r < 0 ? r : op(path);
}

/* ---------- Dateien ---------- */

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len)
{
    Process *p = process_current();
    if (len > IO_MAX)
        len = IO_MAX;
    if (!process_user_range_ok(p, buf, len, 0))
        return ERR_FAULT;
    return process_fd_write(p, (int)fd, (const void *)buf, len);
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{
    Process *p = process_current();
    if (len > IO_MAX)
        len = IO_MAX;
    if (!process_user_range_ok(p, buf, len, 1))
        return ERR_FAULT;
    return process_fd_read(p, (int)fd, (void *)buf, len);
}

static int64_t sys_open(uint64_t upath, uint64_t flags)
{
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    return r < 0 ? r : process_fd_open(process_current(), path, (int)flags);
}

static int64_t sys_readdir(uint64_t upath, uint64_t index, uint64_t uent)
{
    Process *p = process_current();
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0)
        return r;
    if (!process_user_range_ok(p, uent, sizeof(DirEnt), 1))
        return ERR_FAULT;

    FsDirEnt ent; /* gleiches Layout wie DirEnt */
    r = fs_readdir(path, (unsigned)index, &ent);
    if (r != 0)
        return r;
    memcpy((void *)uent, &ent, sizeof(ent));
    return 0;
}

static int64_t sys_stat(uint64_t upath, uint64_t ustat)
{
    Process *p = process_current();
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0)
        return r;
    if (!process_user_range_ok(p, ustat, sizeof(Stat), 1))
        return ERR_FAULT;
    FsStat st;
    r = fs_stat(path, &st);
    if (r != 0)
        return r;
    Stat out = {st.size, st.is_dir, st.mtime};
    memcpy((void *)ustat, &out, sizeof(out));
    return 0;
}

static int64_t sys_rename(uint64_t ufrom, uint64_t uto)
{
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    int r = get_path(ufrom, from);
    if (r == 0)
        r = get_path(uto, to);
    return r < 0 ? r : fs_rename(from, to);
}

static int64_t sys_chdir(uint64_t upath)
{
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    return r < 0 ? r : process_chdir(process_current(), path);
}

static int64_t sys_getcwd(uint64_t ubuf, uint64_t size)
{
    Process *p = process_current();
    const char *cwd = process_cwd(p);
    size_t n = strlen(cwd);
    if (n + 1 > size)
        return ERR_INVAL;
    if (!process_user_range_ok(p, ubuf, n + 1, 1))
        return ERR_FAULT;
    memcpy((void *)ubuf, cwd, n + 1);
    return (int64_t)n;
}

static int64_t sys_service(uint64_t op, uint64_t uname, uint64_t ufds)
{
    Process *p = process_current();
    char name[SERVICE_NAME_MAX];
    if (process_copy_string(p, uname, name, sizeof(name)) < 0)
        return ERR_FAULT;
    int n = op == 2 ? 2 : op == 3 ? 3 : 0, fds[3];
    if (n && !process_user_range_ok(p, ufds, (uint64_t)n * sizeof(int), 1))
        return ERR_FAULT;
    int64_t r;
    switch (op) {
    case 0: return service_register(name);
    case 1: return service_unregister(name);
    case 2: r = service_connect(name, fds); break;
    case 3: r = service_accept(name, fds); break;
    default: return ERR_INVAL;
    }
    if (r == 0)
        memcpy((void *)ufds, fds, (uint64_t)n * sizeof(int));
    return r;
}

static int64_t sys_pipe(uint64_t ufds)
{
    Process *p = process_current();
    if (!process_user_range_ok(p, ufds, 2 * sizeof(int), 1))
        return ERR_FAULT;
    int fds[2];
    int r = process_pipe(p, fds);
    if (r != 0)
        return r;
    memcpy((void *)ufds, fds, sizeof(fds));
    return 0;
}

/* ---------- Prozesse ---------- */

static int64_t sys_spawn(uint64_t upath, uint64_t ucmd)
{
    Process *p = process_current();
    char path[VFS_PATH_MAX], cmd[512];
    int r = get_path(upath, path);
    if (r < 0)
        return r;
    if (process_copy_string(p, ucmd, cmd, sizeof(cmd)) < 0)
        return ERR_FAULT;
    int pid = process_spawn(path, cmd, process_pid(p));
    return pid < 0 ? ERR_NOENT : pid;
}

static int64_t sys_exec(uint64_t upath, uint64_t ucmd)
{
    Process *p = process_current();
    char path[VFS_PATH_MAX], cmd[512];
    int r = get_path(upath, path);
    if (r < 0)
        return r;
    if (process_copy_string(p, ucmd, cmd, sizeof(cmd)) < 0)
        return ERR_FAULT;
    return process_exec(path, cmd); /* kehrt nur bei einem Fehler zurueck */
}

static int64_t sys_wait(uint64_t pid, uint64_t ucode, uint64_t flags)
{
    Process *p = process_current();
    if (ucode && !process_user_range_ok(p, ucode, sizeof(int), 1))
        return ERR_FAULT;
    if (flags & 1) { /* WNOHANG */
        int st = process_poll((int)pid, process_pid(p));
        if (st == -2)
            return ERR_CHILD;
        if (st < 0)
            return ERR_INVAL;
        if (st == 0)
            return ERR_AGAIN;
    }
    int code = 0, state = 0, r;
    /* ohne Zeitgrenze warten (frueher: eine Stunde - danach las die Shell wieder Tasten, obwohl ihr Kind, z.B. der
     * Desktop, noch lief) */
    while ((r = process_wait((int)pid, process_pid(p), &code, &state, 3600 * 1000)) == -1 &&
           process_poll((int)pid, process_pid(p)) == 0)
        ;
    if (r == -2)
        return ERR_CHILD;
    if (r == ERR_INTR)
        return ERR_INTR;
    if (r < 0)
        return ERR_INVAL;
    if (ucode)
        *(int *)ucode = code;
    return state; /* 0 normal, 1 Fehler, 2 gekillt */
}

static int64_t sys_procinfo(uint64_t index, uint64_t uinfo)
{
    if (!process_user_range_ok(process_current(), uinfo, sizeof(ProcInfo), 1))
        return ERR_FAULT;
    ProcInfo info;
    int r = process_info((unsigned)index, &info);
    if (r != 0)
        return r;
    memcpy((void *)uinfo, &info, sizeof(info));
    return 0;
}

static int64_t sys_usbinfo(uint64_t index, uint64_t uinfo)
{
    if (!process_user_range_ok(process_current(), uinfo, sizeof(UsbInfo), 1))
        return ERR_FAULT;
    UsbInfo info;
    if (usb_device_info((unsigned)index, &info) != 0)
        return ERR_NOENT;
    memcpy((void *)uinfo, &info, sizeof(info));
    return 0;
}

static int64_t sys_mountinfo(uint64_t index, uint64_t uinfo)
{
    if (!process_user_range_ok(process_current(), uinfo, sizeof(MountInfo), 1))
        return ERR_FAULT;
    if (index == 0)
        fs_rescan();
    MountInfo info;
    int r = fs_mount_info((unsigned)index, &info);
    if (r != 0)
        return r;
    memcpy((void *)uinfo, &info, sizeof(info));
    return 0;
}

static int64_t sys_isatty(uint64_t fd)
{
    FdObj *o = process_fd_get(process_current(), (int)fd);
    if (!o || o->kind != FD_CONSOLE)
        return 0;
    return ((int64_t)console_cols() << 16) | console_rows();
}

static void sys_sleep(uint64_t ms)
{
    Process *p = process_current();
    while (ms && !process_killed(p)) { /* in kurzen Stuecken, damit Ctrl-C/kill den Schlaf beendet */
        uint64_t slice = ms > 50 ? 50 : ms;
        thread_sleep_ms(slice);
        ms -= slice;
    }
}

static void syscall_do(SyscallFrame *f);

/* Syscalls, die nur Daten des eigenen Prozesses anfassen (ein Prozess hat genau einen Thread, sein Speicher und der
 * User-Teil seiner Seitentabellen gehoeren nur ihm) oder Teile mit eigenem Lock (PMM, Heap, Scheduler, Uhr): sie
 * laufen ohne Big Kernel Lock und damit auf allen CPUs gleichzeitig. 1 = erledigt, 0 = normaler Weg mit BKL.
 * Ein Kill wird hier nicht geprueft; das holt der naechste Timer-Tick oder Syscall mit BKL nach. */
static int syscall_unlocked(SyscallFrame *f)
{
    Process *p = process_current();
    int64_t ret;
    switch (f->rax) {
    case SYS_TICKS:  ret = f->rdi == 1 ? (int64_t)time_us() : (int64_t)apic_ticks(); break;
    case SYS_GETPID: ret = process_pid(p); break;
    case SYS_TIME:   ret = (int64_t)rtc_now(); break;
    case SYS_YIELD:
        if (sched_has_waiting())
            return 0; /* ein anderer Thread wartet: wechseln geht nur mit BKL */
        ret = 0;
        break;
    case SYS_BRK:    ret = process_brk(p, f->rdi); break;
    case SYS_MMAP:   ret = process_mmap(p, f->rdi); break;
    case SYS_MUNMAP: ret = process_munmap(p, f->rdi, f->rsi); break;
    case SYS_CPUINFO: {
        Cpu *c = smp_cpu((unsigned)f->rdi);
        if (!process_user_range_ok(p, f->rsi, sizeof(CpuInfo), 1))
            ret = ERR_FAULT;
        else if (!c)
            ret = ERR_NOENT;
        else {
            CpuInfo ci = {c->index, c->apic_id, c->ticks_user, c->ticks_kernel, c->ticks_idle};
            memcpy((void *)f->rsi, &ci, sizeof(ci));
            ret = 0;
        }
        break;
    }
    case SYS_KLOG: { /* das Log hat seinen eigenen Lock */
        uint64_t max = f->rdx > 65536 ? 65536 : f->rdx;
        if (!process_user_range_ok(p, f->rdi, 8, 1) || !process_user_range_ok(p, f->rsi, max, 1))
            ret = ERR_FAULT;
        else
            ret = (int64_t)klog_read((uint64_t *)f->rdi, (char *)f->rsi, max);
        break;
    }
    default:
        return 0;
    }
    f->rax = (uint64_t)ret;
    return 1;
}

/* Wird von syscall_entry auf dem Kernel-Stack des Threads aufgerufen (IF = 0). Aus dem User-Mode kommend haelt die
 * CPU den Big Kernel Lock nie (smp.h). */
void syscall_dispatch(SyscallFrame *f)
{
    Cpu *c = this_cpu();
    if (syscall_unlocked(f)) {
        c->sys_unlocked++;
        return;
    }
    c->sys_bkl++;
    bkl_acquire();
    syscall_do(f);
    bkl_release();
}

static void syscall_do(SyscallFrame *f)
{
    process_check_killed(); /* schon gekillt: gar nicht erst ausfuehren */

    int64_t ret;
    switch (f->rax) {
    case SYS_WRITE:    ret = sys_write(f->rdi, f->rsi, f->rdx); break;
    case SYS_EXIT:     process_exit((int)f->rdi);
    case SYS_GETPID:   ret = process_pid(process_current()); break;
    case SYS_YIELD:    thread_yield(); ret = 0; break;
    case SYS_SLEEP_MS: sys_sleep(f->rdi); ret = 0; break;
    case SYS_TICKS:    ret = f->rdi == 1 ? (int64_t)time_us() : (int64_t)apic_ticks(); break;
    case SYS_GETCHAR: /* hat ein Grafikprogramm den Bildschirm, gehoeren die Tasten nur ihm */
        ret = console_gfx_active() && !console_gfx_owner(process_pid(process_current())) ? -1 : keyboard_getchar();
        break;
    case SYS_OPEN:     ret = sys_open(f->rdi, f->rsi); break;
    case SYS_READ:     ret = sys_read(f->rdi, f->rsi, f->rdx); break;
    case SYS_CLOSE:    ret = process_fd_close(process_current(), (int)f->rdi); break;
    case SYS_READDIR:  ret = sys_readdir(f->rdi, f->rsi, f->rdx); break;
    case SYS_SPAWN:    ret = sys_spawn(f->rdi, f->rsi); break;
    case SYS_WAIT:     ret = sys_wait(f->rdi, f->rsi, f->rdx); break;
    case SYS_BRK:      ret = process_brk(process_current(), f->rdi); break;
    case SYS_MMAP:     ret = process_mmap(process_current(), f->rdi); break;
    case SYS_MUNMAP:   ret = process_munmap(process_current(), f->rdi, f->rsi); break;
    case SYS_MKDIR:    ret = path_op(f->rdi, fs_mkdir); break;
    case SYS_UNLINK:   ret = path_op(f->rdi, fs_unlink); break;
    case SYS_POWER:
        if (f->rdi == 0)
            power_off();
        if (f->rdi == 1)
            power_reboot();
        ret = ERR_INVAL;
        break;
    case SYS_FORK:     ret = process_fork(f); break;
    case SYS_EXEC:     ret = sys_exec(f->rdi, f->rsi); break;
    case SYS_PIPE:     ret = sys_pipe(f->rdi); break;
    case SYS_SHM:      ret = process_shm(process_current(), f->rdi, f->rsi, f->rdx); break;
    case SYS_SERVICE:  ret = sys_service(f->rdi, f->rsi, f->rdx); break;
    case SYS_DUP:      ret = process_fd_dup(process_current(), (int)f->rdi); break;
    case SYS_DUP2:     ret = process_fd_dup2(process_current(), (int)f->rdi, (int)f->rsi); break;
    case SYS_LSEEK:    ret = process_fd_seek(process_current(), (int)f->rdi, (int64_t)f->rsi, (int)f->rdx); break;
    case SYS_RENAME:   ret = sys_rename(f->rdi, f->rsi); break;
    case SYS_STAT:     ret = sys_stat(f->rdi, f->rsi); break;
    case SYS_CHDIR:    ret = sys_chdir(f->rdi); break;
    case SYS_GETCWD:   ret = sys_getcwd(f->rdi, f->rsi); break;
    case SYS_SETPGID:  ret = process_setpgid(process_current(), (uint32_t)f->rdi, (uint32_t)f->rsi); break;
    case SYS_TTY_FG:   tty_set_fg((uint32_t)f->rdi); ret = 0; break;
    case SYS_TTY_MODE: tty_set_mode((int)f->rdi); ret = 0; break;
    case SYS_KILL:     ret = process_kill_pid((uint32_t)f->rdi); break;
    case SYS_PROCINFO: ret = sys_procinfo(f->rdi, f->rsi); break;
    case SYS_USBINFO:  ret = sys_usbinfo(f->rdi, f->rsi); break;
    case SYS_AUDIO: {
        Process *p = process_current();
        uint32_t pid = process_pid(p);
        int64_t r;
        switch (f->rdi) {
        case 0: r = hda_open(pid, (uint32_t)f->rsi, (uint32_t)f->rdx); break;
        case 1:
            r = process_user_range_ok(p, f->rsi, f->rdx, 0) ? hda_write(pid, (const void *)f->rsi, f->rdx) : ERR_FAULT;
            break;
        case 2: r = hda_drain(pid); break;
        case 3: hda_close(pid); r = 0; break;
        case 4: r = hda_volume((int)(int64_t)f->rsi); break;
        case 5: r = (int64_t)hda_played(pid); break;
        case 6: r = hda_voice_volume(pid, (int)(int64_t)f->rsi); break;
        default: r = ERR_INVAL; break;
        }
        ret = r == HDA_ERR_NODEV ? ERR_NOSYS : r == HDA_ERR_BUSY ? ERR_AGAIN : r == HDA_ERR_FORMAT ? ERR_INVAL
            : r == HDA_ERR_NOTOPEN ? ERR_BADF : r;
        break;
    }
    case SYS_SETMODE: {
        int r = igd_mode_set((uint32_t)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx);
        ret = r == 0 ? 0 : r == IGD_MODE_NODRIVER ? ERR_NOSYS : r == IGD_MODE_NOMODE ? ERR_NOENT
            : r == IGD_MODE_BUSY ? ERR_AGAIN : ERR_IO;
        break;
    }
    case SYS_GPU:
        ret = f->rdi == 1 ? igd_flip_test() : f->rdi == 2 ? igd_cursor_test() : f->rdi == 3 ? igd_blit_test()
            : f->rdi == 4 ? igd_info_report()
            : f->rdi == 5 ? igd_edid_test()
            : f->rdi == 6 ? igd_scale_test()
            : f->rdi == 7 ? igd_mode_test()
            : f->rdi == 8 ? igd_dp_test()
            : f->rdi == 9 ? igd_dpmode_test()
            : f->rdi == 10 ? igd_dptrain_test()
            : (f->rdi & 0xFF) == 11 ? igd_output_test((int)(f->rdi >> 8))
            : f->rdi == 12 ? igd_vblank_test() : ERR_INVAL;
        break;
    case SYS_PCIINFO: {
        PciInfo pi;
        if (!process_user_range_ok(process_current(), f->rsi, sizeof(pi), 1))
            ret = ERR_FAULT;
        else if (pci_info((unsigned)f->rdi, &pi) != 0)
            ret = ERR_NOENT;
        else {
            memcpy((void *)f->rsi, &pi, sizeof(pi));
            ret = 0;
        }
        break;
    }
    case SYS_NETINFO: {
        NetInfo ni;
        if (!process_user_range_ok(process_current(), f->rsi, sizeof(ni), 1))
            ret = ERR_FAULT;
        else if (net_info((unsigned)f->rdi, &ni) != 0)
            ret = ERR_NOENT;
        else {
            memcpy((void *)f->rsi, &ni, sizeof(ni));
            ret = 0;
        }
        break;
    }
    case SYS_NETCFG:
        if (f->rdi == 0) {
            uint8_t cfg[16];
            if (!process_user_range_ok(process_current(), f->rdx, sizeof(cfg), 0)) {
                ret = ERR_FAULT;
            } else {
                memcpy(cfg, (const void *)f->rdx, sizeof(cfg));
                ret = net_set_static((unsigned)f->rsi, cfg);
            }
        } else if (f->rdi == 1) {
            ret = net_start_dhcp((unsigned)f->rsi);
        } else if (f->rdi == 2) {
            ArpInfo ai;
            if (!process_user_range_ok(process_current(), f->rdx, sizeof(ai), 1))
                ret = ERR_FAULT;
            else if (net_arp_info((unsigned)f->rsi, &ai) != 0)
                ret = ERR_NOENT;
            else {
                memcpy((void *)f->rdx, &ai, sizeof(ai));
                ret = 0;
            }
        } else {
            ret = ERR_INVAL;
        }
        break;
    case SYS_SOCKET: {
        if (f->rdi == 2) { /* TCP-Verbindung */
            uint8_t ip[4] = {(uint8_t)f->rsi, (uint8_t)(f->rsi >> 8), (uint8_t)(f->rsi >> 16), (uint8_t)(f->rsi >> 24)};
            TcpConn *c = 0;
            ret = tcp_connect(ip, (uint16_t)f->rdx, (uint32_t)(f->rdx >> 16), &c);
            if (ret < 0)
                break;
            FdObj *o = fdobj_new_tcp(c);
            if (!o) {
                tcp_close(c);
                ret = ERR_NOMEM;
                break;
            }
            ret = process_fd_install(process_current(), o);
            break;
        }
        if (f->rdi == 3) {
            TcpInfo ti;
            if (!process_user_range_ok(process_current(), f->rdx, sizeof(ti), 1))
                ret = ERR_FAULT;
            else if (tcp_info((int)f->rsi, &ti) != 0)
                ret = ERR_NOENT;
            else {
                memcpy((void *)f->rdx, &ti, sizeof(ti));
                ret = 0;
            }
            break;
        }
        if (f->rdi != 1 || f->rsi > 65535) {
            ret = ERR_INVAL;
            break;
        }
        int err = 0;
        UdpSock *s = udp_open((uint16_t)f->rsi, &err);
        if (!s) {
            ret = err;
            break;
        }
        FdObj *o = fdobj_new_udp(s);
        if (!o) {
            udp_close(s);
            ret = ERR_NOMEM;
            break;
        }
        ret = process_fd_install(process_current(), o);
        break;
    }
    case SYS_SENDTO:
    case SYS_RECVFROM: {
        Process *p = process_current();
        FdObj *o = process_fd_get(p, (int)f->rdi);
        SockMsg m;
        if (!o || o->kind != FD_UDP) {
            ret = ERR_BADF;
        } else if (!process_user_range_ok(p, f->rsi, sizeof(m), 1)) {
            ret = ERR_FAULT;
        } else {
            memcpy(&m, (const void *)f->rsi, sizeof(m));
            if (!process_user_range_ok(p, m.buf, m.len, f->rax == SYS_RECVFROM)) {
                ret = ERR_FAULT;
            } else if (f->rax == SYS_SENDTO) {
                ret = udp_sendto(o->udp, m.ip, m.port, (const void *)m.buf, m.len);
            } else {
                ret = udp_recvfrom(o->udp, (void *)m.buf, m.len, m.ip, &m.port, (uint32_t)f->rdx);
                if (ret >= 0)
                    memcpy((void *)f->rsi, &m, sizeof(m));
            }
        }
        break;
    }
    case SYS_SOCKPORT: {
        FdObj *o = process_fd_get(process_current(), (int)f->rdi);
        ret = o && o->kind == FD_UDP ? udp_local_port(o->udp) : ERR_BADF;
        break;
    }
    case SYS_RESOLVE: {
        Process *p = process_current();
        char name[128];
        int max = (int)f->rdx;
        if (max < 1 || max > 16) {
            ret = ERR_INVAL;
        } else if (process_copy_string(p, f->rdi, name, sizeof(name)) < 0) {
            ret = ERR_FAULT;
        } else if (!process_user_range_ok(p, f->rsi, (uint64_t)max * 4, 1)) {
            ret = ERR_FAULT;
        } else {
            uint8_t ips[16][4];
            ret = net_resolve(name, ips, max);
            if (ret > 0)
                memcpy((void *)f->rsi, ips, (size_t)ret * 4);
        }
        break;
    }
    case SYS_NTP: {
        Process *p = process_current();
        char server[64];
        NtpResult r;
        if (f->rdi && process_copy_string(p, f->rdi, server, sizeof(server)) < 0) {
            ret = ERR_FAULT;
        } else if (!process_user_range_ok(p, f->rsi, sizeof(r), 1)) {
            ret = ERR_FAULT;
        } else {
            ret = net_ntp(f->rdi ? server : 0, (int)f->rdx, &r);
            memcpy((void *)f->rsi, &r, sizeof(r));
        }
        break;
    }
    case SYS_PING: {
        uint8_t ip[4] = {(uint8_t)f->rdi, (uint8_t)(f->rdi >> 8), (uint8_t)(f->rdi >> 16), (uint8_t)(f->rdi >> 24)};
        ret = net_ping(ip, (uint16_t)f->rsi, (uint32_t)(f->rsi >> 16) & 0xFFFF, (uint32_t)f->rdx);
        break;
    }
    case SYS_GFX: {
        Process *p = process_current();
        uint32_t pid = process_pid(p);
        if (f->rdi == 0) {
            int r = console_gfx_acquire(pid);
            if (r == 0) {
                mouse_set_owner(pid);
                ret = ((int64_t)console_width_px() << 32) | console_height_px();
            } else {
                ret = r == -2 ? ERR_AGAIN : ERR_NOSYS;
            }
        } else if (f->rdi == 1) {
            GfxBlit b;
            if (!console_gfx_owner(pid)) {
                ret = ERR_INVAL;
            } else if (!process_user_range_ok(p, f->rsi, sizeof(b), 0)) {
                ret = ERR_FAULT;
            } else {
                memcpy(&b, (const void *)f->rsi, sizeof(b));
                uint64_t need = b.h > 0 && b.w > 0 ? ((uint64_t)(b.h - 1) * b.pitch + (uint64_t)b.w) * 4 : 0;
                if (b.w <= 0 || b.h <= 0 || (uint32_t)b.w > b.pitch) {
                    ret = 0;
                } else if (!process_user_range_ok(p, b.buf, need, 0)) {
                    ret = ERR_FAULT;
                } else {
                    console_gfx_blit((const uint32_t *)b.buf, b.pitch, b.x, b.y, b.w, b.h);
                    ret = 0;
                }
            }
        } else if (f->rdi == 4) { /* auf den naechsten Bildwechsel warten -> Zaehler der Bildwechsel */
            if (!igd_vblank_ok())
                ret = ERR_NOSYS;
            else {
                igd_wait_vblank(50);
                ret = (int64_t)igd_vblank_count();
            }
        } else if (f->rdi == 3) { /* Hardware-Mauszeiger: rsi = x | y << 16 | sichtbar << 32 */
            if (!console_gfx_owner(pid))
                ret = ERR_INVAL;
            else if (!igd_cursor_available())
                ret = ERR_NOSYS;
            else {
                igd_cursor_move((int16_t)(f->rsi & 0xFFFF), (int16_t)((f->rsi >> 16) & 0xFFFF), (int)((f->rsi >> 32) & 1));
                ret = 0;
            }
        } else if (f->rdi == 2) {
            if (console_gfx_owner(pid)) {
                console_gfx_release(pid);
                mouse_set_owner(0);
            }
            ret = 0;
        } else {
            ret = ERR_INVAL;
        }
        break;
    }
    case SYS_FONT: {
        if (!process_user_range_ok(process_current(), f->rsi, 16, 1)) {
            ret = ERR_FAULT;
        } else {
            memcpy((void *)f->rsi, font_glyph((unsigned)f->rdi), 16);
            ret = 0;
        }
        break;
    }
    case SYS_FDAVAIL: {
        FdObj *o = process_fd_get(process_current(), (int)f->rdi);
        ret = o ? fdobj_available(o) : ERR_BADF;
        break;
    }
    case SYS_STATFS: {
        char path[VFS_PATH_MAX];
        uint64_t t = 0, fr = 0;
        ret = get_path(f->rdi, path);
        if (ret >= 0 && !process_user_range_ok(process_current(), f->rsi, 16, 1))
            ret = ERR_FAULT;
        if (ret >= 0)
            ret = fs_statfs(path, &t, &fr);
        if (ret == 0) {
            ((uint64_t *)f->rsi)[0] = t;
            ((uint64_t *)f->rsi)[1] = fr;
        }
        break;
    }
    case SYS_MOUSEMODE:
        mouse_set_owner(f->rdi ? process_pid(process_current()) : 0);
        ret = 0;
        break;
    case SYS_CLIPBOARD: {
        Process *p = process_current();
        uint64_t len = f->rdx;
        if (len > 16383)
            len = 16383;
        if (!process_user_range_ok(p, f->rsi, len, f->rdi == 0)) {
            ret = ERR_FAULT;
        } else if (f->rdi == 0) {
            uint32_t n;
            const char *c = console_clipboard(&n);
            memcpy((void *)f->rsi, c, n < len ? n : len);
            ret = n;
        } else {
            console_clipboard_set((const char *)f->rsi, (uint32_t)len);
            ret = 0;
        }
        break;
    }
    case SYS_KEYMAP: {
        Process *p = process_current();
        char name[KEYMAP_NAME_MAX + 1];
        ret = 0;
        if (f->rdi && process_copy_string(p, f->rdi, name, sizeof(name)) < 0)
            ret = ERR_FAULT;
        else if (f->rdi && keymap_set(name) != 0)
            ret = ERR_INVAL;
        if (ret == 0 && f->rsi) {
            if (!process_user_range_ok(p, f->rsi, 16, 1)) {
                ret = ERR_FAULT;
            } else {
                char *out = (char *)f->rsi;
                const char *n = keymap_name();
                size_t l = strlen(n);
                memcpy(out, n, l + 1);
            }
        }
        break;
    }
    case SYS_MOUSE: {
        MouseInfo mi;
        if (!process_user_range_ok(process_current(), f->rdi, sizeof(mi), 1)) {
            ret = ERR_FAULT;
        } else {
            mouse_get(&mi);
            memcpy((void *)f->rdi, &mi, sizeof(mi));
            ret = 0;
        }
        break;
    }
    case SYS_VIDEOINFO: {
        VideoInfo vi;
        if (!process_user_range_ok(process_current(), f->rsi, sizeof(vi), 1))
            ret = ERR_FAULT;
        else if (video_mode_info((unsigned)f->rdi, &vi) != 0)
            ret = ERR_NOENT;
        else {
            memcpy((void *)f->rsi, &vi, sizeof(vi));
            ret = 0;
        }
        break;
    }
    case SYS_TIME:     ret = (int64_t)rtc_now(); break;
    case SYS_SETTIME:  ret = rtc_set(f->rdi) == 0 ? 0 : ERR_INVAL; break;
    case SYS_CURSOR: {
        uint32_t col, row;
        console_get_cursor(&col, &row);
        ret = ((int64_t)col << 16) | row;
        break;
    }
    case SYS_ISATTY:   ret = sys_isatty(f->rdi); break;
    case SYS_MOUNTINFO: ret = sys_mountinfo(f->rdi, f->rsi); break;
    default:           ret = ERR_NOSYS;
    }

    process_check_killed(); /* waehrend des Aufrufs gekillt (z.B. blockierendes read): jetzt beenden */
    f->rax = (uint64_t)ret;
}
