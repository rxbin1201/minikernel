#include "user.h"

/* Verursacht absichtlich Fehler, um den Schutz des Kernels zu testen: crash null|kread|priv|badptr|unmapped */
void _start(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "null";

    if (u_streq(mode, "null")) {
        *(volatile int *)0 = 1;                       /* Schreibzugriff auf Adresse 0 */
    } else if (u_streq(mode, "kread")) {
        (void)*(volatile int *)0x100000;              /* Kernel-Speicher lesen */
    } else if (u_streq(mode, "priv")) {
        __asm__ __volatile__("cli");                  /* privilegierte Instruktion */
    } else if (u_streq(mode, "badptr")) {
        /* Syscall mit Kernel-Zeiger: muss einen Fehler liefern statt abzustuerzen */
        sys_exit(sys_write(1, (const void *)0x100000, 16) < 0 ? 7 : 8);
    } else if (u_streq(mode, "unmapped")) {
        volatile char *p = (volatile char *)sys_mmap(4096);
        p[0] = 1;
        sys_munmap((void *)p, 4096);
        p[0] = 2;                                     /* Seite ist weg -> Page Fault */
    } else {
        u_puts("crash: unbekannter Modus (null|kread|priv|badptr|unmapped)\n");
        sys_exit(2);
    }
    sys_exit(99); /* nur erreicht, wenn der Fehler ausblieb */
}
