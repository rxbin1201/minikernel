#include "libc.h"
#include "thread.h"

/* Prueft eingeblendete Dateien (sys_mmap_file): Inhalt, Offset, Nullen hinter dem Ende, private Schreibzugriffe,
 * Schutz nur lesbarer Einblendungen, fork, munmap von Teilen, Kernel liest aus der Einblendung, Deskriptor schon
 * geschlossen, mehrere Threads, Fehlerfaelle - und dass wirklich erst beim Zugriff gelesen wird (Zeit). Exit-Code 0 =
 * alles in Ordnung, sonst die Nummer der ersten fehlgeschlagenen Pruefung. */

static int failed, checks;

static void check(int number, int ok)
{
    checks++;
    if (!ok && !failed) {
        failed = number;
        fprintf(2, "[mmaptest] Pruefung %d fehlgeschlagen\n", number);
    }
}

#define PATH "/disk/mmaptest.bin"
#define SIZE (2 * 1024 * 1024 + 1234) /* das Ende liegt mitten in einer Seite */

static unsigned char pattern(u64 i)
{
    return (unsigned char)(i * 7 + i / 4096);
}

static int matches(const unsigned char *p, u64 from, u64 n)
{
    for (u64 i = 0; i < n; i++)
        if (p[i] != pattern(from + i))
            return 0;
    return 1;
}

static int make_file(void)
{
    s64 fd = sys_open(PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return 0;
    static unsigned char buf[65536];
    for (u64 at = 0; at < SIZE;) {
        u64 n = SIZE - at < sizeof(buf) ? SIZE - at : sizeof(buf);
        for (u64 i = 0; i < n; i++)
            buf[i] = pattern(at + i);
        if (sys_write((int)fd, buf, n) != (s64)n)
            return 0;
        at += n;
    }
    sys_close((int)fd);
    return 1;
}

/* Kind ausfuehren: liefert state (0 normal, 1 Absturz), *code = Exit-Code */
static int in_child(int (*fn)(void *), void *arg, int *code)
{
    s64 pid = sys_fork();
    if (pid == 0)
        sys_exit(fn(arg));
    return (int)sys_wait((int)pid, code);
}

static int write_byte(void *p)
{
    *(volatile unsigned char *)p = 1;
    return 0;
}

static int read_byte(void *p)
{
    return *(volatile unsigned char *)p == 0x55 ? 1 : 0; /* stuerzt ab, wenn die Seite nicht mehr eingeblendet ist */
}

static int compare_tail(void *p)
{
    return matches((const unsigned char *)p + 1024 * 1024, 1024 * 1024, 64 * 1024) ? 0 : 1;
}

static const unsigned char *shared_map;

static void *verify_quarter(void *arg)
{
    u64 q = (u64)arg, part = (SIZE / 4) & ~4095ULL;
    return (void *)(u64)(matches(shared_map + q * part, q * part, part) ? 0 : 1);
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    check(1, make_file());
    s64 fd = sys_open(PATH, O_RDONLY);
    check(2, fd >= 0);
    if (failed)
        sys_exit(failed);

    /* 1. ganze Datei: Stichproben, dann alles, und Nullen hinter dem Ende */
    s64 a = sys_mmap_file((int)fd, SIZE, 0, 0);
    check(3, a > 0);
    const unsigned char *m = (const unsigned char *)a;
    check(4, m[0] == pattern(0) && m[4095] == pattern(4095) && m[4096] == pattern(4096) &&
                 m[SIZE / 2] == pattern(SIZE / 2) && m[SIZE - 1] == pattern(SIZE - 1));
    check(5, matches(m, 0, SIZE));
    int zeros = 1;
    for (u64 i = SIZE; i < ((SIZE + 4095) & ~4095ULL); i++)
        zeros = zeros && m[i] == 0;
    check(6, zeros);

    /* 2. ab einem Offset */
    s64 b = sys_mmap_file((int)fd, 8192, 3 * 4096, 0);
    check(7, b > 0 && matches((const unsigned char *)b, 3 * 4096, 8192));

    /* 3. privat beschreibbar: die Einblendung aendert sich, die Datei nicht */
    s64 w = sys_mmap_file((int)fd, 4096, 0, 1);
    check(8, w > 0);
    ((unsigned char *)w)[10] = 0xEE;
    unsigned char c = 0;
    sys_lseek((int)fd, 10, 0);
    sys_read((int)fd, &c, 1);
    check(9, ((unsigned char *)w)[10] == 0xEE && c == pattern(10));

    /* 4. nur lesbar: Schreiben beendet das Programm */
    int code = -1;
    check(10, in_child(write_byte, (void *)(m + 3 * 4096 + 5), &code) == 1);

    /* 5. fork: das Kind laedt noch nicht angefasste Seiten selbst */
    s64 lazy = sys_mmap_file((int)fd, SIZE, 0, 0);
    check(11, lazy > 0 && in_child(compare_tail, (void *)lazy, &code) == 0 && code == 0);

    /* 6. munmap aus der Mitte: davor und dahinter bleibt alles, in der Luecke stuerzt man ab */
    s64 cut = sys_mmap_file((int)fd, 40 * 4096, 0, 0);
    check(12, cut > 0 && sys_munmap((void *)(cut + 10 * 4096), 10 * 4096) == 0);
    const unsigned char *cm = (const unsigned char *)cut;
    check(13, matches(cm, 0, 10 * 4096) && matches(cm + 20 * 4096, 20 * 4096, 20 * 4096));
    check(14, in_child(read_byte, (void *)(cut + 15 * 4096), &code) == 1);

    /* 7. der Kernel liest aus noch nicht geladenen Seiten (write aus der Einblendung in eine Pipe) */
    s64 k = sys_mmap_file((int)fd, 64 * 1024, 512 * 1024, 0);
    int pf[2];
    sys_pipe(pf);
    check(15, k > 0 && sys_write(pf[1], (const void *)(k + 100), 3000) == 3000);
    static unsigned char back[3000];
    u64 got = 0;
    while (got < sizeof(back)) {
        s64 n = sys_read(pf[0], back + got, sizeof(back) - got);
        if (n <= 0)
            break;
        got += (u64)n;
    }
    check(16, got == sizeof(back) && matches(back, 512 * 1024 + 100, sizeof(back)));
    sys_close(pf[0]);
    sys_close(pf[1]);

    /* 8. Deskriptor schon geschlossen */
    s64 fd2 = sys_open(PATH, O_RDONLY);
    s64 late = sys_mmap_file((int)fd2, SIZE, 0, 0);
    sys_close((int)fd2);
    check(17, late > 0 && matches((const unsigned char *)late + 700 * 1024, 700 * 1024, 8192));

    /* 9. vier Threads laden gleichzeitig verschiedene Teile */
    s64 sh = sys_mmap_file((int)fd, SIZE, 0, 0);
    shared_map = (const unsigned char *)sh;
    int t[4], ok = sh > 0;
    for (int i = 0; i < 4; i++)
        t[i] = thread_create(verify_quarter, (void *)(u64)i);
    for (int i = 0; i < 4; i++) {
        void *r = (void *)1;
        ok = ok && t[i] > 0 && thread_join(t[i], &r) == 0 && r == 0;
    }
    check(18, ok);

    /* 10. Fehler */
    check(19, sys_mmap_file((int)fd, 4096, 100, 0) == ERR_INVAL);
    check(20, sys_mmap_file((int)fd, 0, 0, 0) == ERR_INVAL);
    int pp[2];
    sys_pipe(pp);
    check(21, sys_mmap_file(pp[0], 4096, 0, 0) == ERR_BADF);
    sys_close(pp[0]);
    sys_close(pp[1]);

    /* 11. erst beim Zugriff gelesen: der erste Zugriff (laedt 64 KiB) geht viel schneller als die Datei ganz zu lesen */
    s64 t0 = sys_time_us();
    s64 one = sys_mmap_file((int)fd, SIZE, 0, 0);
    volatile unsigned char x = ((const unsigned char *)one)[100]; /* erste Seite: ohne die Cluster-Kette abzulaufen */
    (void)x;
    s64 map_us = sys_time_us() - t0;
    static unsigned char all[SIZE];
    t0 = sys_time_us();
    sys_lseek((int)fd, 0, 0);
    for (got = 0; got < SIZE;) {
        s64 n = sys_read((int)fd, all + got, SIZE - got);
        if (n <= 0)
            break;
        got += (u64)n;
    }
    s64 read_us = sys_time_us() - t0;
    printf("[mmaptest] einblenden + erster Zugriff: %d us, ganze Datei lesen (2 MiB): %d us\n", (int)map_us, (int)read_us);
    check(22, one > 0 && got == SIZE && map_us * 3 < read_us);

    /* 12. Datei aus der initrd (sich selbst): ELF-Kopf */
    s64 self = sys_open("/bin/mmaptest", O_RDONLY);
    s64 e = sys_mmap_file((int)self, 4096, 0, 0);
    check(23, e > 0 && memcmp((const void *)e, "\x7f" "ELF", 4) == 0);
    sys_close((int)self);

    sys_close((int)fd);
    sys_unlink(PATH);
    if (!failed)
        printf("[mmaptest] alle %d Pruefungen OK\n", checks);
    sys_exit(failed);
}
