#include "libc.h"

/* udp: Datagramme senden und empfangen (zum Ausprobieren der UDP-Sockets).
 *   udp send ziel port text...    sendet den Text und zeigt 2 s lang Antworten
 *   udp listen port [-e]          zeigt eingehende Datagramme (-e: schickt jedes als Echo zurueck), Ende mit Strg+C
 * Gegenstelle unter Windows z.B. mit PowerShell oder "ncat -u". */

static void show(const unsigned char ip[4], unsigned port, const char *buf, s64 n, int tty)
{
    printf("%s%u.%u.%u.%u:%u%s (%lld Bytes): ", tty ? C_CYAN : "", ip[0], ip[1], ip[2], ip[3], port, tty ? C_RESET : "", n);
    for (s64 i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n' && i == n - 1)
            break;
        if (c >= 32 || c == '\n' || c == '\t')
            printf("%c", c);
        else
            printf("\\x%02x", c);
    }
    printf("\n");
}

void _start(int argc, char **argv)
{
    int tty = sys_isatty(1) != 0;
    static char buf[1500];
    if (argc >= 4 && strcmp(argv[1], "send") == 0) {
        unsigned char ip[1][4];
        s64 r = sys_resolve(argv[2], ip, 1);
        if (r < 0) {
            fprintf(2, "udp: %s: %s\n", argv[2], net_strerror(r));
            sys_exit(1);
        }
        int port = atoi(argv[3]);
        int len = 0;
        for (int i = 4; i < argc; i++) {
            int l = (int)strlen(argv[i]);
            if (len + l + 2 >= (int)sizeof(buf))
                break;
            if (i > 4)
                buf[len++] = ' ';
            memcpy(buf + len, argv[i], (size_t)l);
            len += l;
        }
        buf[len++] = '\n';
        s64 fd = sys_udp_socket(0);
        if (fd < 0) {
            fprintf(2, "udp: %s\n", net_strerror(fd));
            sys_exit(1);
        }
        r = sys_sendto((int)fd, ip[0], (unsigned)port, buf, (unsigned)len);
        if (r < 0) {
            fprintf(2, "udp: senden: %s\n", net_strerror(r));
            sys_exit(1);
        }
        printf("%d Bytes an %u.%u.%u.%u:%d gesendet (von Port %lld)\n", len, ip[0][0], ip[0][1], ip[0][2], ip[0][3], port,
               sys_sockport((int)fd));
        s64 t0 = sys_ticks();
        int answers = 0;
        while (sys_ticks() - t0 < 200) {
            unsigned char from[4];
            unsigned fport;
            s64 n = sys_recvfrom((int)fd, buf, sizeof(buf), from, &fport, (unsigned)((200 - (sys_ticks() - t0)) * 10));
            if (n < 0)
                break;
            show(from, fport, buf, n, tty);
            answers++;
        }
        if (!answers)
            printf("(keine Antwort)\n");
        sys_close((int)fd);
        sys_exit(0);
    }
    if (argc >= 3 && strcmp(argv[1], "listen") == 0) {
        int echo = argc >= 4 && strcmp(argv[3], "-e") == 0;
        s64 fd = sys_udp_socket((unsigned)atoi(argv[2]));
        if (fd < 0) {
            fprintf(2, "udp: Port %s: %s\n", argv[2], net_strerror(fd));
            sys_exit(1);
        }
        NetInfo ni;
        if (sys_netinfo(0, &ni) == 0)
            printf("Warte auf UDP-Port %s von %u.%u.%u.%u%s (Ende mit Strg+C)\n", argv[2], ni.ip[0], ni.ip[1], ni.ip[2],
                   ni.ip[3], echo ? ", sende Echos" : "");
        for (;;) {
            unsigned char from[4];
            unsigned fport;
            s64 n = sys_recvfrom((int)fd, buf, sizeof(buf), from, &fport, 0xFFFFFFFFu);
            if (n < 0)
                break;
            show(from, fport, buf, n, tty);
            if (echo)
                sys_sendto((int)fd, from, fport, buf, (unsigned)n);
        }
        sys_exit(0);
    }
    fprintf(2, "Aufruf: udp send ziel port text...\n"
               "        udp listen port [-e]\n");
    sys_exit(2);
}
