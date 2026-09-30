#ifndef NET_H
#define NET_H

#include <stdint.h>

/* Netzwerk: Treiber melden Ethernet-Geraete an (NetDev), darueber liegt ein kleiner IPv4-Stack:
 * ARP, IPv4 (ohne Fragmentierung), ICMP (Echo), UDP (auch als Sockets fuer Programme), DHCP-, DNS- und NTP-Client.
 * Nach dem ersten DHCP-Ergebnis stellt der Kernel die Uhr per NTP (abschaltbar mit "nontp" in cmdline.txt). Empfangen wird per Polling im Thread "net";
 * blockierende Aufrufe (ping) pollen waehrend des Wartens selbst, damit die Antwortzeit genau gemessen wird.
 *
 * IPv4-Adressen liegen als 4 Bytes in Netzwerk-Reihenfolge vor (a.b.c.d = {a, b, c, d}). */

#define NET_MAX_DEVICES 4
#define NET_MTU         1500
#define NET_FRAME_MAX   1518

typedef struct NetDev NetDev;
struct NetDev {
    char    name[8];   /* "eth0", wird von net_register vergeben */
    char    model[40];
    uint8_t mac[6];
    int (*send)(NetDev *d, const void *frame, uint32_t len);            /* 0 = ok; der Rahmen wird kopiert */
    int (*recv)(NetDev *d, void *buf, uint32_t max);                   /* Laenge eines Rahmens, 0 = keiner da */
    int (*link)(NetDev *d, uint32_t *mbps, int *full_duplex);          /* 1 = Verbindung steht */
    void *priv;
};

/* Sucht Netzwerkkarten und startet den Thread "net" (nach pci_scan, mit laufendem Scheduler) */
void net_init(void);

/* --- fuer Treiber --- */
int  net_register(const NetDev *d); /* Kopie wird abgelegt; -1, wenn kein Platz */
void e1000_probe(void);

/* --- Syscalls --- */

/* Eintrag fuer SYS_NETINFO (gleiches Layout in Userland/user.h). dhcp: 0 aus/statisch, 1 laeuft, 2 gebunden, 3 fehlgeschlagen */
typedef struct {
    char     name[8];
    char     model[40];
    uint8_t  mac[6];
    uint8_t  link, full_duplex;
    uint32_t mbps;
    uint8_t  ip[4], mask[4], gateway[4], dns[4], dhcp_server[4];
    uint32_t dhcp, lease_s;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
} NetInfo;

/* ARP-Eintrag fuer SYS_NETCFG op 2 */
typedef struct {
    uint8_t  ip[4];
    uint8_t  mac[6];
    uint16_t pad;
    uint32_t age_s;
    char     dev[8];
} ArpInfo;

/* --- UDP (fuer Kernel und, als Datei-Deskriptor, fuer Programme) --- */

#define NET_WAIT_FOREVER 0xFFFFFFFFu
#define UDP_MAX_PAYLOAD  (NET_MTU - 28)

typedef struct UdpSock UdpSock;

UdpSock *udp_open(uint16_t port, int *err);   /* port 0: freien Port waehlen; err: ERR_EXIST (belegt), ERR_NOMEM */
void     udp_close(UdpSock *s);
uint16_t udp_local_port(const UdpSock *s);
/* Senden (ermittelt vorher per ARP die MAC der naechsten Station, wartet dafuer hoechstens 3 s). Bytes oder Fehler. */
int      udp_sendto(UdpSock *s, const uint8_t ip[4], uint16_t port, const void *data, uint32_t len);
/* Naechstes Datagramm; timeout_ms 0 = nicht warten, NET_WAIT_FOREVER = unbegrenzt. Laenge (gekuerzt auf max) oder
 * ERR_AGAIN (nichts da, bei timeout 0), ERR_TIMEDOUT, ERR_INTR. */
int      udp_recvfrom(UdpSock *s, void *buf, uint32_t max, uint8_t ip[4], uint16_t *port, uint32_t timeout_ms);
int      udp_pending(UdpSock *s); /* Anzahl wartender Datagramme */

/* --- DNS und NTP --- */

/* Name -> IPv4-Adressen (A-Records; ein Name in Punktschreibweise wird nur umgewandelt). Anzahl (>= 1) oder
 * ERR_NOENT (Name unbekannt), ERR_TIMEDOUT (kein DNS-Server antwortet), ERR_NETUNREACH, ERR_INVAL, ERR_INTR.
 * Ergebnisse werden entsprechend ihrer Gueltigkeit (TTL) zwischengespeichert. */
int net_resolve(const char *name, uint8_t out[][4], int max);

/* Ergebnis von net_ntp (gleiches Layout in Userland/user.h) */
typedef struct {
    int64_t  offset_ms;   /* Uhr des Rechners minus richtige Zeit (positiv: Rechner ging vor) */
    uint64_t utc;         /* richtige Zeit, Sekunden seit 1970 (UTC) */
    uint64_t local;       /* dasselbe als Ortszeit (so steht es danach in der RTC) */
    uint32_t rtt_ms;      /* Laufzeit hin und zurueck */
    uint32_t stratum;     /* Ebene des Servers (1 = direkt an einer Atomuhr/GPS) */
    uint8_t  ip[4];
    int32_t  tz_offset_s; /* Ortszeit - UTC */
    char     server[64];
    char     tz[12];
    uint32_t pad;
} NtpResult;

/* Fragt einen NTP-Server (NULL: den per DHCP genannten, sonst pool.ntp.org) und stellt auf Wunsch die Uhr.
 * 0 oder Fehler (wie net_resolve, zusaetzlich ERR_IO bei einer ungueltigen Antwort). */
int net_ntp(const char *server, int set_clock, NtpResult *out);

int net_info(unsigned index, NetInfo *out);                     /* 0 oder -1 am Ende */
int net_set_static(unsigned index, const uint8_t cfg[16]);      /* ip, mask, gateway, dns; ip 0.0.0.0 = Adresse entfernen */
int net_start_dhcp(unsigned index);
int net_arp_info(unsigned index, ArpInfo *out);                 /* 0 oder -1 am Ende */

/* Sendet ein ICMP-Echo und wartet auf die Antwort. Ergebnis: Laufzeit in Mikrosekunden (Bits 0-39) und TTL der Antwort
 * (Bits 40-47), oder ein negativer Fehler (ERR_TIMEDOUT, ERR_NETUNREACH, ERR_HOSTUNREACH, ERR_INTR). */
int64_t net_ping(const uint8_t ip[4], uint16_t seq, uint32_t size, uint32_t timeout_ms);

#endif
