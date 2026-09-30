#include "libc.h"

/* ifconfig: Netzwerkkarten anzeigen und einstellen.
 *   ifconfig                          alle Karten: Verbindung, IP-Adresse, DHCP, Paketzaehler
 *   ifconfig -a                       zusaetzlich die ARP-Tabelle (IP -> MAC der Nachbarn)
 *   ifconfig [eth0] dhcp              Adresse per DHCP holen (wartet bis zu 15 s auf das Ergebnis)
 *   ifconfig [eth0] 192.168.1.50/24 [gw 192.168.1.1] [dns 192.168.1.1]
 *                                     feste Adresse (ohne /Praefix: /24) */

static int tty;

static int parse_ip(const char *s, unsigned char out[4], int *prefix)
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
    if (*s == '/' && prefix) {
        int p = atoi(s + 1);
        if (p < 0 || p > 32)
            return -1;
        *prefix = p;
        return 0;
    }
    return *s ? -1 : 0;
}

static int prefix_of(const unsigned char m[4])
{
    int n = 0;
    for (int i = 0; i < 4; i++)
        for (int b = 7; b >= 0; b--)
            n += (m[i] >> b) & 1;
    return n;
}

static int is_zero(const unsigned char a[4]) { return !a[0] && !a[1] && !a[2] && !a[3]; }

static void bytes(u64 b, char *out, int n)
{
    if (b < 10 * 1024)
        snprintf(out, (size_t)n, "%llu B", b);
    else if (b < 10ULL * 1024 * 1024)
        snprintf(out, (size_t)n, "%llu KB", b / 1024);
    else
        snprintf(out, (size_t)n, "%llu MB", b >> 20);
}

static void show(const NetInfo *n)
{
    printf("%s%s%s  %s  MAC %02x:%02x:%02x:%02x:%02x:%02x\n", tty ? C_CYAN : "", n->name, tty ? C_RESET : "", n->model,
           n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    if (n->link)
        printf("      Verbindung: %s%u Mbit/s, %s%s\n", tty ? C_GREEN : "", n->mbps, n->full_duplex ? "Vollduplex" : "Halbduplex",
               tty ? C_RESET : "");
    else
        printf("      Verbindung: %skeine (Kabel eingesteckt?)%s\n", tty ? C_RED : "", tty ? C_RESET : "");
    if (!is_zero(n->ip)) {
        printf("      IPv4:       %s%u.%u.%u.%u/%d%s", tty ? C_WHITE : "", n->ip[0], n->ip[1], n->ip[2], n->ip[3],
               prefix_of(n->mask), tty ? C_RESET : "");
        if (!is_zero(n->gateway))
            printf("  Gateway %u.%u.%u.%u", n->gateway[0], n->gateway[1], n->gateway[2], n->gateway[3]);
        if (!is_zero(n->dns))
            printf("  DNS %u.%u.%u.%u", n->dns[0], n->dns[1], n->dns[2], n->dns[3]);
        printf("\n");
    } else {
        printf("      IPv4:       keine Adresse\n");
    }
    if (n->dhcp == 0)
        printf("      DHCP:       aus (feste Adresse)\n");
    else if (n->dhcp == 1)
        printf("      DHCP:       %s\n", n->link ? "fragt an ..." : "wartet auf eine Verbindung");
    else if (n->dhcp == 3)
        printf("      DHCP:       %skeine Antwort vom Router%s (neuer Versuch alle 30 s)\n", tty ? C_RED : "", tty ? C_RESET : "");
    else
        printf("      DHCP:       von %u.%u.%u.%u, noch %u h %02u min gueltig\n", n->dhcp_server[0], n->dhcp_server[1],
               n->dhcp_server[2], n->dhcp_server[3], n->lease_s / 3600, n->lease_s / 60 % 60);
    char rb[24], tb[24];
    bytes(n->rx_bytes, rb, sizeof(rb));
    bytes(n->tx_bytes, tb, sizeof(tb));
    printf("      Pakete:     %llu empfangen (%s), %llu gesendet (%s), %llu verworfen\n", n->rx_packets, rb, n->tx_packets,
           tb, n->rx_dropped);
}

static void show_arp(void)
{
    ArpInfo a;
    printf("\nARP-Tabelle (Nachbarn im lokalen Netz):\n");
    int k = 0;
    for (u64 i = 0; sys_arpinfo(i, &a) == 0; i++, k++) {
        char ip[20];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
        printf("  %-16s %02x:%02x:%02x:%02x:%02x:%02x  %s  vor %u s\n", ip, a.mac[0], a.mac[1], a.mac[2], a.mac[3],
               a.mac[4], a.mac[5], a.dev, a.age_s);
    }
    if (!k)
        printf("  (leer)\n");
}

static void usage(void)
{
    fprintf(2, "Aufruf: ifconfig [-a]\n"
               "        ifconfig [eth0] dhcp\n"
               "        ifconfig [eth0] 192.168.1.50/24 [gw 192.168.1.1] [dns 192.168.1.1]\n");
    sys_exit(1);
}

void _start(int argc, char **argv)
{
    tty = sys_isatty(1) != 0;
    NetInfo n;
    int count = 0;
    while (sys_netinfo((u64)count, &n) == 0)
        count++;
    if (!count) {
        printf("Keine Netzwerkkarte mit Treiber gefunden.\n"
               "'lspci' zeigt alle Geraete; Netzwerkkarten ohne [Treiber] werden (noch) nicht unterstuetzt.\n");
        sys_exit(1);
    }

    int a = 1, dev = 0;
    if (a < argc && strncmp(argv[a], "eth", 3) == 0) {
        dev = atoi(argv[a] + 3);
        if (dev < 0 || dev >= count) {
            fprintf(2, "ifconfig: %s gibt es nicht\n", argv[a]);
            sys_exit(1);
        }
        a++;
    }
    if (a >= argc || strcmp(argv[a], "-a") == 0) { /* anzeigen */
        for (int i = 0; i < count; i++) {
            if (sys_netinfo((u64)i, &n) != 0)
                continue;
            if (i)
                printf("\n");
            show(&n);
        }
        if (a < argc)
            show_arp();
        sys_exit(0);
    }

    if (strcmp(argv[a], "dhcp") == 0) {
        sys_net_dhcp((u64)dev);
        printf("DHCP: frage den Router nach einer Adresse ...\n");
        for (int t = 0; t < 150; t++) {
            sys_sleep_ms(100);
            if (sys_netinfo((u64)dev, &n) != 0)
                break;
            if (n.dhcp == 2 || n.dhcp == 3 || (!n.link && t >= 30))
                break;
        }
        show(&n);
        sys_exit(n.dhcp == 2 ? 0 : 1);
    }

    unsigned char cfg[16];
    memset(cfg, 0, sizeof(cfg));
    int prefix = 24;
    if (parse_ip(argv[a], cfg, &prefix) != 0)
        usage();
    for (int i = 0; i < prefix; i++)
        cfg[4 + i / 8] |= (unsigned char)(0x80 >> (i % 8));
    for (a++; a < argc; a += 2) {
        if (a + 1 >= argc)
            usage();
        if (strcmp(argv[a], "gw") == 0 || strcmp(argv[a], "gateway") == 0) {
            if (parse_ip(argv[a + 1], cfg + 8, 0) != 0)
                usage();
        } else if (strcmp(argv[a], "dns") == 0) {
            if (parse_ip(argv[a + 1], cfg + 12, 0) != 0)
                usage();
        } else {
            usage();
        }
    }
    if (sys_net_static((u64)dev, cfg) != 0) {
        fprintf(2, "ifconfig: Einstellen fehlgeschlagen\n");
        sys_exit(1);
    }
    if (sys_netinfo((u64)dev, &n) == 0)
        show(&n);
    sys_exit(0);
}
