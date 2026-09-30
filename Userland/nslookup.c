#include "libc.h"

/* nslookup: fragt den DNS-Server nach den IPv4-Adressen eines Namens.
 *   nslookup google.de */
void _start(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(2, "Aufruf: nslookup name   (z.B. nslookup google.de)\n");
        sys_exit(2);
    }
    int tty = sys_isatty(1) != 0;
    NetInfo ni;
    if (sys_netinfo(0, &ni) == 0) {
        const unsigned char *d = ni.dns[0] ? ni.dns : ni.gateway;
        if (d[0])
            printf("Server:  %u.%u.%u.%u\n", d[0], d[1], d[2], d[3]);
    }
    unsigned char ips[16][4];
    s64 n = sys_resolve(argv[1], ips, 16);
    if (n < 0) {
        printf("%s%s: %s%s\n", tty ? C_RED : "", argv[1], net_strerror(n), tty ? C_RESET : "");
        sys_exit(1);
    }
    printf("Name:    %s\n", argv[1]);
    for (s64 i = 0; i < n; i++)
        printf("%s %s%u.%u.%u.%u%s\n", i ? "        " : (n > 1 ? "Adressen:" : "Adresse:"), tty ? C_WHITE : "", ips[i][0],
               ips[i][1], ips[i][2], ips[i][3], tty ? C_RESET : "");
    sys_exit(0);
}
