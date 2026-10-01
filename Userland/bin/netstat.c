#include "libc.h"

/* netstat: zeigt die TCP-Verbindungen (lokale und entfernte Adresse, Zustand, Puffer, Laufzeit). */

static const char *const STATES[] = {"?", "SYN_SENT", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSING",
                                     "TIME_WAIT", "CLOSE_WAIT", "LAST_ACK", "CLOSED"};

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int tty = sys_isatty(1) != 0;
    TcpInfo ti;
    unsigned i = 0;
    for (; sys_tcpinfo(i, &ti) == 0; i++) {
        if (!i)
            printf("%sProto  Lokal                  Gegenstelle            Zustand      Empf.  Send.  RTT%s\n",
                   tty ? C_WHITE : "", tty ? C_RESET : "");
        char l[24], r[24];
        snprintf(l, sizeof(l), "%u.%u.%u.%u:%u", ti.local_ip[0], ti.local_ip[1], ti.local_ip[2], ti.local_ip[3], ti.lport);
        snprintf(r, sizeof(r), "%u.%u.%u.%u:%u", ti.ip[0], ti.ip[1], ti.ip[2], ti.ip[3], ti.rport);
        printf("tcp    %-22s %-22s %-12s %5u  %5u  %u ms\n", l, r, ti.state < 10 ? STATES[ti.state] : "?", ti.rx_queued,
               ti.tx_queued, ti.srtt_ms);
    }
    if (!i)
        printf("Keine TCP-Verbindungen.\n");
    sys_exit(0);
}
