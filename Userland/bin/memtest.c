#include "user.h"
#include "malloc.h"

static int fail(const char *what)
{
    u_puts("[memtest] FEHLER: ");
    u_puts(what);
    u_puts("\n");
    return 1;
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* brk: 1 MiB Heap anfordern, beschreiben, pruefen, wieder verkleinern */
    char *base = sys_brk(0);
    char *end  = sys_brk(base + (1 << 20));
    if (end != base + (1 << 20))
        sys_exit(fail("brk wachsen"));
    for (int i = 0; i < (1 << 20); i += 4096)
        base[i] = (char)(i >> 12);
    for (int i = 0; i < (1 << 20); i += 4096)
        if (base[i] != (char)(i >> 12))
            sys_exit(fail("brk Inhalt"));
    if (sys_brk(base) != base)
        sys_exit(fail("brk schrumpfen"));
    u_puts("[memtest] brk ok\n");

    /* mmap: 256 KiB, muss genullt und beschreibbar sein */
    volatile unsigned char *m = (volatile unsigned char *)sys_mmap(256 * 1024);
    if ((s64)m < 0)
        sys_exit(fail("mmap"));
    for (int i = 0; i < 256 * 1024; i += 4096)
        if (m[i] != 0)
            sys_exit(fail("mmap nicht genullt"));
    for (int i = 0; i < 256 * 1024; i++)
        m[i] = (unsigned char)i;
    for (int i = 0; i < 256 * 1024; i++)
        if (m[i] != (unsigned char)i)
            sys_exit(fail("mmap Inhalt"));
    if (sys_munmap((void *)m, 256 * 1024) != 0)
        sys_exit(fail("munmap"));
    u_puts("[memtest] mmap ok\n");

    /* malloc/free auf brk-Basis */
    char *a = u_malloc(100), *b = u_malloc(2000), *c = u_malloc(50);
    if (!a || !b || !c)
        sys_exit(fail("malloc"));
    for (int i = 0; i < 100; i++)
        a[i] = 'a';
    for (int i = 0; i < 2000; i++)
        b[i] = 'b';
    for (int i = 0; i < 50; i++)
        c[i] = 'c';
    u_free(b);
    u_free(a);                       /* a und b verschmelzen */
    char *d = u_malloc(2100);        /* passt nur in den verschmolzenen Block */
    if (d != a)
        sys_exit(fail("malloc Wiederverwendung"));
    if (c[0] != 'c' || c[49] != 'c')
        sys_exit(fail("malloc ueberschrieben"));
    u_free(d);
    u_free(c);
    u_puts("[memtest] malloc ok\n");

    sys_exit(0);
}
