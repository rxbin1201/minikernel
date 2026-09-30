#include "libc.h"

/* Prueft lseek, rename, stat, chdir und getcwd auf /disk. Exit-Code 0 = alles ok, sonst Nummer der ersten Fehlpruefung. */
static int failed;

static void check(int number, int ok)
{
    if (!ok && !failed) {
        failed = number;
        fprintf(2, "[fstest] Pruefung %d fehlgeschlagen\n", number);
    }
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char buf[32];
    Stat st;

    check(1, sys_mkdir("/disk/FST") == 0);
    check(2, sys_chdir("/disk/FST") == 0);
    check(3, sys_getcwd(buf, sizeof(buf)) == 9 && strcmp(buf, "/disk/FST") == 0);

    /* Relative Pfade gelten ab dem Arbeitsverzeichnis */
    s64 fd = sys_open("a.txt", O_RDWR | O_CREAT);
    check(4, fd >= 0);
    check(5, sys_write((int)fd, "0123456789", 10) == 10);
    check(6, sys_lseek((int)fd, 3, SEEK_SET) == 3);
    check(7, sys_read((int)fd, buf, 4) == 4 && memcmp(buf, "3456", 4) == 0);
    check(8, sys_lseek((int)fd, 0, SEEK_END) == 10);
    check(9, sys_lseek((int)fd, -2, SEEK_CUR) == 8);
    check(10, sys_read((int)fd, buf, 8) == 2 && memcmp(buf, "89", 2) == 0);
    check(11, sys_lseek((int)fd, 11, SEEK_SET) < 0);   /* hinter das Ende: nicht erlaubt */
    check(12, sys_lseek((int)fd, 0, SEEK_SET) == 0);
    check(13, sys_write((int)fd, "AB", 2) == 2);       /* ueberschreibt am Anfang */
    sys_close((int)fd);

    check(14, sys_stat("a.txt", &st) == 0 && st.size == 10 && !st.is_dir);
    check(15, sys_rename("a.txt", "b.txt") == 0);
    check(16, sys_stat("a.txt", &st) < 0);
    check(17, sys_stat("b.txt", &st) == 0 && st.size == 10);
    fd = sys_open("b.txt", O_RDONLY);
    check(18, fd >= 0 && sys_read((int)fd, buf, 4) == 4 && memcmp(buf, "AB23", 4) == 0);
    sys_close((int)fd);

    /* Verzeichnis umbenennen, ".." und absolute Pfade */
    check(19, sys_mkdir("sub") == 0 && sys_rename("sub", "sub2") == 0);
    check(20, sys_stat("/disk/FST/sub2", &st) == 0 && st.is_dir);
    check(21, sys_stat("../FST/./sub2", &st) == 0);
    check(22, sys_rename("b.txt", "sub2/b.txt") == 0);           /* Datei in anderes Verzeichnis verschieben */
    check(23, sys_stat("sub2/b.txt", &st) == 0 && sys_stat("b.txt", &st) < 0);

    check(24, sys_unlink("sub2/b.txt") == 0);
    check(25, sys_unlink("sub2") == 0);
    check(26, sys_chdir("..") == 0 && sys_getcwd(buf, sizeof(buf)) == 5 && strcmp(buf, "/disk") == 0);
    check(27, sys_unlink("FST") == 0);
    check(28, sys_stat("FST", &st) < 0);

    /* Fehlerfaelle */
    check(29, sys_chdir("/etc/motd") < 0);                        /* Datei ist kein Verzeichnis */
    check(30, sys_chdir("/gibt/es/nicht") < 0);
    check(31, sys_chdir("/") == 0 && sys_stat("disk", &st) == 0 && st.is_dir);
    int fds[2];
    check(32, sys_pipe(fds) == 0 && sys_lseek(fds[0], 0, SEEK_SET) < 0);   /* Pipes kann man nicht spulen */
    check(33, sys_rename("/etc/motd", "/etc/x") < 0);             /* initrd ist nur lesbar */

    if (!failed)
        printf("[fstest] alles ok\n");
    sys_exit(failed);
}
