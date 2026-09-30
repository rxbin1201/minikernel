#include "libc.h"

/* ping: schickt ICMP-Echo-Anfragen und misst die Antwortzeit.
 *   ping 192.168.1.1            4 Anfragen im Abstand von 1 s (Namen wie google.de gehen auch, per DNS)
 *   ping -c 10 192.168.1.1      10 Anfragen (-c 0: bis Strg+C)
 *   ping -s 1000 ...            Groesse der Nutzdaten (Standard 56 Byte)
 *   ping -W 2000 ...            Wartezeit pro Antwort in ms (Standard 1000) */

static void fmt_ms(u64 us, char *out, int n)
{
    if (us < 10000)
        snprintf(out, (size_t)n, "%llu,%02llu ms", us / 1000, us % 1000 / 10);
    else if (us < 100000)
        snprintf(out, (size_t)n, "%llu,%llu ms", us / 1000, us % 1000 / 100);
    else
        snprintf(out, (size_t)n, "%llu ms", us / 1000);
}

static int parse_ip(const char *s, unsigned char out[4])
{
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return -1;
        int v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        if (v > 255)
            return -1;
        out[i] = (unsigned char)v;
        if (i < 3 && *s++ != '.')
            return -1;
    }
    return *s ? -1 : 0;
}

void _start(int argc, char **argv)
{
    int count = 4, size = 56, wait = 1000;
    const char *host = 0;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-W") == 0) && i + 1 < argc) {
            int v = atoi(argv[i + 1]);
            if (argv[i][1] == 'c') count = v;
            else if (argv[i][1] == 's') size = v;
            else wait = v;
            i++;
        } else if (argv[i][0] != '-' && !host) {
            host = argv[i];
        } else {
            host = 0;
            break;
        }
    }
    if (!host || size < 0 || size > 1472 || wait < 10) {
        fprintf(2, "Aufruf: ping [-c anzahl] [-s groesse] [-W wartezeit_ms] name-oder-ip\n");
        sys_exit(2);
    }
    unsigned char ip[4];
    int tty = sys_isatty(1) != 0;
    if (parse_ip(host, ip) == 0) {
        printf("PING %u.%u.%u.%u: %d Bytes Daten\n", ip[0], ip[1], ip[2], ip[3], size);
    } else {
        unsigned char r[1][4];
        s64 n = sys_resolve(host, r, 1);
        if (n < 0) {
            fprintf(2, "ping: %s: %s\n", host, net_strerror(n));
            sys_exit(2);
        }
        memcpy(ip, r[0], 4);
        printf("PING %s (%u.%u.%u.%u): %d Bytes Daten\n", host, ip[0], ip[1], ip[2], ip[3], size);
    }

    int sent = 0, got = 0;
    u64 tmin = ~0ULL, tmax = 0, tsum = 0;
    for (int seq = 1; count == 0 || seq <= count; seq++) {
        s64 t0 = sys_ticks();
        s64 r = sys_ping(ip, (unsigned)seq, (unsigned)size, (unsigned)wait);
        sent++;
        if (r >= 0) {
            u64 us = (u64)r & 0xFFFFFFFFFFULL;
            unsigned ttl = (unsigned)((u64)r >> 40) & 0xFF;
            char t[24];
            fmt_ms(us, t, sizeof(t));
            printf("Antwort von %u.%u.%u.%u: seq=%d ttl=%u Zeit=%s%s%s\n", ip[0], ip[1], ip[2], ip[3], seq, ttl,
                   tty ? C_GREEN : "", t, tty ? C_RESET : "");
            got++;
            tsum += us;
            if (us < tmin) tmin = us;
            if (us > tmax) tmax = us;
        } else if (r == ERR_INTR) {
            break;
        } else {
            const char *why = r == ERR_TIMEDOUT ? "keine Antwort (Zeitueberschreitung)" :
                              r == ERR_HOSTUNREACH ? "Ziel nicht erreichbar (keine Antwort auf ARP)" :
                              r == ERR_NETUNREACH ? "kein Weg ins Netz (keine IP-Adresse oder kein Gateway, siehe ifconfig)" :
                                                    "Fehler";
            printf("seq=%d: %s%s%s\n", seq, tty ? C_RED : "", why, tty ? C_RESET : "");
            if (r == ERR_NETUNREACH)
                break;
        }
        if (count == 0 || seq < count) { /* Rest der Sekunde warten */
            s64 left = 100 - (sys_ticks() - t0);
            if (left > 0)
                sys_sleep_ms((u64)left * 10);
        }
    }
    printf("--- %u.%u.%u.%u: %d gesendet, %d empfangen, %d %% Verlust", ip[0], ip[1], ip[2], ip[3], sent, got,
           sent ? (sent - got) * 100 / sent : 0);
    if (got) {
        char a[24], b[24], c[24];
        fmt_ms(tmin, a, sizeof(a));
        fmt_ms(tsum / (u64)got, b, sizeof(b));
        fmt_ms(tmax, c, sizeof(c));
        a[strlen(a) - 3] = 0; /* " ms" nur einmal am Ende */
        b[strlen(b) - 3] = 0;
        printf(", min/mittel/max %s/%s/%s", a, b, c);
    }
    printf("\n");
    sys_exit(got ? 0 : 1);
}
