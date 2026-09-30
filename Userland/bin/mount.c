#include "libc.h"

/* mount: sucht nach neu angesteckten Datentraegern und listet alle Volumes mit ihrem Verzeichnis */
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int tty = sys_isatty(1) != 0;
    MountInfo m;
    int n = 0;
    printf("%s%-14s %-10s %-12s %-10s %6s  %s%s\n", tty ? C_BOLD : "", "Verzeichnis", "Geraet", "Name", "Typ", "MiB",
           "Zugriff", tty ? C_RESET : "");
    for (u64 i = 0; sys_mountinfo(i, &m) == 0; i++, n++) {
        const char *access = (m.flags & 2) ? "nicht unterstuetzt" : (m.flags & 4) ? "nur lesen (nicht sauber getrennt)" :
                             (m.flags & 1) ? "nur lesen" : "lesen+schreiben";
        const char *color = !tty ? "" : (m.flags & 2) ? C_RED : (m.flags & 1) ? C_YELLOW : C_GREEN;
        printf("%s%-14s%s %-10s %-12s %-10s %6lu  %s%s%s\n", tty ? C_BLUE : "", m.point, tty ? C_RESET : "", m.device,
               m.label[0] ? m.label : "-", m.fstype, (unsigned long)m.mib, color, access, tty ? C_RESET : "");
    }
    if (!n)
        printf("keine Datentraeger\n");
    else
        printf("%sDateien ansehen: ls /mnt/<geraet>, cat /mnt/<geraet>/<datei>%s\n", tty ? C_DIM : "", tty ? C_RESET : "");
    sys_exit(0);
}
