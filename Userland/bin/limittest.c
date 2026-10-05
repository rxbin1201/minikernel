#include "libc.h"
#include "thread.h"

/* Prueft, dass die frueheren festen Grenzen weg sind und die neuen Obergrenzen (nur gegen Ausreisser) greifen:
 * offene Deskriptoren (frueher 32, jetzt 1024), Threads (16 -> 1024), gleichzeitige Prozesse (64 -> 4096), der Stack
 * (64 KiB -> waechst bis 8 MiB), Datei-Einblendungen (32 -> 1024), geteilter Speicher (80 -> 1024), Kommandozeilen
 * (16 Woerter / 512 Byte -> 256 / 4 KiB) und Pfade (128 -> 1023 Zeichen; laengere werden abgelehnt, nicht gekuerzt).
 * Exit-Code 0 = alles in Ordnung, sonst die Nummer der ersten fehlgeschlagenen Pruefung. */

static int failed, checks;

static void check(int number, int ok)
{
    checks++;
    if (!ok && !failed) {
        failed = number;
        fprintf(2, "[limittest] Pruefung %d fehlgeschlagen\n", number);
    }
}

#define FILE_PATH "/share/ton.mp3"

/* ---------- Stack ---------- */

static u64 deep(int n) /* je Ebene 4 KiB auf dem Stack */
{
    volatile unsigned char buf[4096];
    buf[0] = (unsigned char)n;
    buf[4095] = (unsigned char)(n >> 8);
    if (n == 0)
        return buf[0];
    return deep(n - 1) + buf[0] + buf[4095];
}

static u64 expect_deep(int n)
{
    u64 s = 0;
    for (int i = 1; i <= n; i++)
        s += (unsigned char)i + (unsigned char)(i >> 8);
    return s;
}

/* ---------- Threads ---------- */

static volatile u32 go;

static void *wait_go(void *arg)
{
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE))
        sys_futex_wait(&go, 0, 0);
    return arg;
}

static int tids[1100];

void _start(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "args") == 0) { /* Kind fuer die lange Kommandozeile */
        int ok = argc == 201;
        for (int i = 2; ok && i < argc; i++) {
            char want[32];
            snprintf(want, sizeof(want), "wort%03d-xxxxxxxx", i - 2);
            ok = strcmp(argv[i], want) == 0;
        }
        sys_exit(ok ? 0 : 1);
    }
    if (argc > 1 && strcmp(argv[1], "abyss") == 0) /* Kind: Rekursion ohne Ende - mehr als 8 MiB Stack */
        sys_exit((int)deep(1 << 30));

    /* 1. Deskriptoren: weit ueber 32, die Obergrenze 1024 greift genau */
    static int fds[1100];
    int n = 0;
    s64 fd;
    while (n < 1100 && (fd = sys_open(FILE_PATH, O_RDONLY)) >= 0)
        fds[n++] = (int)fd;
    printf("[limittest] %d Dateien gleichzeitig offen\n", n + 3);
    check(1, n + 3 == 1024); /* 0, 1, 2 sind schon belegt */
    unsigned char c = 0;
    check(2, n > 500 && sys_read(fds[500], &c, 1) == 1 && c == 'I'); /* ID3-Tag */
    check(3, n > 0 && fds[n - 1] == 1023); /* der hoechste erlaubte Deskriptor */
    for (int i = 0; i < n; i++)
        sys_close(fds[i]);
    check(4, sys_dup2(0, 900) == 900 && sys_close(900) == 0 && sys_dup2(0, 1024) == ERR_BADF);

    /* 2. Threads: 200 warten gleichzeitig; die Obergrenze 1024 greift genau */
    n = 0;
    while (n < 200 && (tids[n] = thread_create(wait_go, (void *)(u64)n)) > 0)
        n++;
    check(5, n == 200);
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    sys_futex_wake(&go, 1000);
    int ok = 1;
    for (int i = 0; i < n; i++) {
        void *r = 0;
        ok = ok && thread_join(tids[i], &r) == 0 && (u64)r == (u64)i;
    }
    check(6, ok);
    go = 0;
    n = 0;
    s64 last = 0;
    while (n < 1100 && (last = thread_create(wait_go, 0)) > 0)
        tids[n++] = (int)last;
    printf("[limittest] %d Threads gleichzeitig\n", n + 1);
    check(7, n + 1 == 1024 && last == ERR_AGAIN);
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    sys_futex_wake(&go, 2000);
    for (int i = 0; i < n; i++)
        thread_join(tids[i], 0);

    /* 3. 100 Prozesse gleichzeitig (frueher hoechstens 64 im ganzen System) */
    int pf[2];
    sys_pipe(pf);
    static int pids[100];
    int started = 0;
    for (int i = 0; i < 100; i++) {
        s64 pid = sys_fork();
        if (pid == 0) {
            sys_close(pf[1]);
            char x;
            sys_exit(sys_read(pf[0], &x, 1) == 0 ? 0 : 1); /* wartet, bis der Elternprozess das Schreibende schliesst */
        }
        if (pid > 0)
            pids[started++] = (int)pid;
    }
    check(8, started == 100);
    sys_close(pf[1]); /* alle Kinder laufen noch - jetzt duerfen sie enden */
    ok = 1;
    for (int i = 0; i < started; i++) {
        int code = -1;
        ok = ok && sys_wait(pids[i], &code) == 0 && code == 0;
    }
    check(9, ok);
    sys_close(pf[0]);

    /* 4. Stack: 4 MiB tief (frueher 64 KiB), und ohne Ende stuerzt das Programm ab statt den Kernel */
    check(10, deep(1024) == expect_deep(1024));
    int code = -1;
    s64 pid = sys_spawn("/bin/limittest", "limittest abyss");
    check(11, pid > 0 && sys_wait((int)pid, &code) == 1);

    /* 5. 100 Datei-Einblendungen (frueher 32) */
    s64 f = sys_open(FILE_PATH, O_RDONLY);
    static s64 maps[100];
    ok = f >= 0;
    for (int i = 0; ok && i < 100; i++)
        ok = (maps[i] = sys_mmap_file((int)f, 4096, 0, 0)) > 0 && ((const char *)maps[i])[0] == 'I';
    check(12, ok);
    for (int i = 0; i < 100; i++)
        if (maps[i] > 0)
            sys_munmap((void *)maps[i], 4096);
    sys_close((int)f);

    /* 6. 100 Stuecke geteilten Speicher eingeblendet (frueher 80) */
    ok = 1;
    for (int i = 0; ok && i < 100; i++) {
        unsigned id;
        ok = (maps[i] = sys_shm_create(4096, &id)) > 0;
        if (ok)
            ((int *)maps[i])[0] = i;
    }
    check(13, ok);
    for (int i = 0; i < 100; i++)
        if (maps[i] > 0)
            sys_shm_unmap((void *)maps[i]);

    /* 7. Kommandozeile mit 200 Woertern und gut 3 KiB (frueher 16 Woerter, 512 Byte) */
    static char cmd[4096];
    int len = snprintf(cmd, sizeof(cmd), "limittest args");
    for (int i = 0; i < 199; i++)
        len += snprintf(cmd + len, sizeof(cmd) - (u64)len, " wort%03d-xxxxxxxx", i);
    pid = sys_spawn("/bin/limittest", cmd);
    code = -1;
    check(14, len > 3000 && pid > 0 && sys_wait((int)pid, &code) == 0 && code == 0);

    /* 8. Pfade: vier Ordner mit je 240 Zeichen auf der FAT-Platte (Pfad gut 1000 Zeichen, frueher hoechstens 127) */
    static char d[4][PATH_MAX], file[PATH_MAX], file2[PATH_MAX], cwd[PATH_MAX], name[241];
    for (int i = 0; i < 240; i++)
        name[i] = (char)('a' + i % 26);
    name[240] = 0;
    snprintf(d[0], PATH_MAX, "/disk/%s", name);
    for (int i = 1; i < 4; i++) {
        name[0] = (char)('0' + i); /* jede Ebene ein anderer Name */
        snprintf(d[i], PATH_MAX, "%s/%s", d[i - 1], name);
    }
    ok = 1;
    for (int i = 0; i < 4; i++)
        ok = ok && sys_mkdir(d[i]) == 0;
    snprintf(file, PATH_MAX, "%s/datei.txt", d[3]);
    printf("[limittest] Pfad mit %d Zeichen\n", (int)strlen(file));
    s64 w = ok ? sys_open(file, O_WRONLY | O_CREAT | O_TRUNC) : -1;
    ok = ok && w >= 0 && sys_write((int)w, "tief", 4) == 4;
    if (w >= 0)
        sys_close((int)w);
    Stat st;
    check(15, ok && strlen(file) > 900 && sys_stat(file, &st) == 0 && st.size == 4);
    /* relativ aus dem tiefen Ordner, getcwd liefert den ganzen Pfad, umbenennen, auflisten */
    char got[8] = "";
    ok = sys_chdir(d[3]) == 0 && sys_getcwd(cwd, sizeof(cwd)) > 900 && strcmp(cwd, d[3]) == 0;
    s64 r = ok ? sys_open("datei.txt", O_RDONLY) : -1;
    ok = ok && r >= 0 && sys_read((int)r, got, 4) == 4 && memcmp(got, "tief", 4) == 0;
    if (r >= 0)
        sys_close((int)r);
    snprintf(file2, PATH_MAX, "%s/neu.txt", d[3]);
    DirEnt de;
    ok = ok && sys_rename(file, file2) == 0 && sys_readdir(d[3], 0, &de) == 0 && strcmp(de.name, "neu.txt") == 0;
    check(16, ok);
    /* genau an der Grenze: 1023 Zeichen gehen (die Datei gibt es nur nicht), 1024 werden abgelehnt statt gekuerzt */
    static char edge[PATH_MAX + 8];
    int l = snprintf(edge, sizeof(edge), "%s/", d[3]);
    while (l < PATH_MAX - 1)
        edge[l++] = 'x';
    edge[l] = 0;
    check(17, strlen(edge) == PATH_MAX - 1 && sys_stat(edge, &st) == ERR_NOENT);
    edge[l++] = 'x';
    edge[l] = 0;
    check(18, sys_stat(edge, &st) == ERR_NAMETOOLONG && sys_open(edge, O_WRONLY | O_CREAT) == ERR_NAMETOOLONG);
    /* relativ zu einem langen Arbeitsverzeichnis zu lang: ebenfalls abgelehnt */
    check(19, sys_open("../../../../../disk/../disk/" "0123456789012345678901234567890123456789", O_RDONLY) == ERR_NOENT &&
                  sys_stat(edge + strlen(d[3]) - 8, &st) == ERR_NAMETOOLONG);
    sys_chdir("/disk");
    sys_unlink(file2);
    for (int i = 3; i >= 0; i--)
        sys_unlink(d[i]);
    check(20, sys_stat(d[0], &st) == ERR_NOENT);

    if (!failed)
        printf("[limittest] alle %d Pruefungen OK\n", checks);
    sys_exit(failed);
}
