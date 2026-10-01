#include "libc.h"

/* Prueft geteilten Speicher (SYS_SHM). Exit-Code 0 = alles in Ordnung, sonst die Nummer der ersten fehlgeschlagenen
 * Pruefung. Die Schleifen am Ende legen zusammen mehr an, als der Rechner (512 MiB in QEMU) hat: gaebe ein Weg die
 * Seiten nicht frei, ginge der Speicher aus. */
static int failed;

static void check(int number, int ok)
{
    if (!ok && !failed) {
        failed = number;
        fprintf(2, "[shmtest] Pruefung %d fehlgeschlagen\n", number);
    }
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 1. anlegen: genullt, auf ganze Seiten aufgerundet */
    unsigned id = 0;
    s64 a = sys_shm_create(10000, &id);
    check(1, a > 0 && id != 0);
    unsigned *p = (unsigned *)a;
    check(2, p[0] == 0 && p[2499] == 0);
    for (int i = 0; i < 2500; i++)
        p[i] = 0xC0DE0000u + (unsigned)i;
    check(3, sys_shm_size(id) == 12288);

    /* 2. ein anderer Prozess blendet ihn ein: sieht die Daten und schreibt zurueck */
    s64 pid = sys_fork();
    if (pid == 0) {
        s64 b = sys_shm_map(id);
        unsigned *q = (unsigned *)b;
        int ok = b > 0 && q[0] == 0xC0DE0000u && q[2499] == 0xC0DE0000u + 2499;
        if (ok)
            q[1] = 0x12345678;
        sys_exit(ok ? 0 : 1); /* ohne Ausblenden: das Programmende raeumt auf */
    }
    int code = -1;
    check(4, pid > 0 && sys_wait((int)pid, &code) == 0 && code == 0);
    check(5, p[1] == 0x12345678);

    /* 3. zweites Einblenden im selben Prozess: andere Adresse, dieselben Seiten */
    s64 c = sys_shm_map(id);
    check(6, c > 0 && c != a);
    if (c > 0) {
        ((unsigned *)c)[5] = 77;
        check(7, p[5] == 77);
        check(8, sys_shm_unmap((void *)c) == 0);
    }
    check(9, sys_shm_unmap((void *)a) == 0);
    check(10, sys_shm_size(id) < 0);   /* letzte Einblendung weg: Objekt verschwunden */
    check(11, sys_shm_map(id) < 0);
    check(12, sys_shm_unmap((void *)a) < 0);
    check(13, sys_shm_create(0, &id) < 0);

    /* 4. munmap auf geteiltem Speicher darf die Seiten nicht freigeben (sonst wuerden sie doppelt frei) */
    a = sys_shm_create(8192, &id);
    check(14, a > 0 && sys_munmap((void *)a, 8192) == 0);
    check(15, sys_shm_size(id) == 8192);

    /* 5. Schreibende einer Pipe: SYS_FDAVAIL meldet den freien Platz (der Desktop schreibt nie in eine volle Pipe) */
    int fds[2];
    check(19, sys_pipe(fds) == 0);
    s64 room = sys_fdavail(fds[1]);
    check(20, room >= 64 && write_all(fds[1], "0123456789", 10) == 0 && sys_fdavail(fds[1]) == room - 10);
    sys_close(fds[0]);
    check(21, sys_fdavail(fds[1]) == -1); /* kein Leser mehr */
    sys_close(fds[1]);

    /* 6. Freigabe:100 x 8 MiB anlegen und ausblenden, 60 x 8 MiB in Kindern, die einfach enden */
    for (int i = 0; i < 100 && !failed; i++) {
        unsigned k;
        s64 m = sys_shm_create(8u << 20, &k);
        check(16, m > 0);
        if (m > 0) {
            ((char *)m)[(8u << 20) - 1] = 1;
            check(17, sys_shm_unmap((void *)m) == 0);
        }
    }
    for (int i = 0; i < 60 && !failed; i++) {
        s64 kid = sys_fork();
        if (kid == 0) {
            unsigned k;
            s64 m = sys_shm_create(8u << 20, &k);
            if (m > 0)
                ((char *)m)[123] = 1;
            sys_exit(m > 0 ? 0 : 1);
        }
        check(18, kid > 0 && sys_wait((int)kid, &code) == 0 && code == 0);
    }
    sys_exit(failed);
}
