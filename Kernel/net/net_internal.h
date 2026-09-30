#ifndef NET_NET_INTERNAL_H
#define NET_NET_INTERNAL_H

/* Interne Schnittstelle des kleinen IPv4-Stacks (oeffentlich: net/net.h).
 *
 * Alles laeuft unter einer Sperre (net_lock): der Thread "net" pollt die Karten und erledigt Zeitgesteuertes
 * (Link-Pruefung, DHCP); Syscalls wie ping nehmen dieselbe Sperre und pollen beim Warten selbst. Gesendet wird aus
 * net_txbuf, empfangen nach net_rxbuf (beide nur mit gehaltener Sperre benutzt).
 *
 *   core.c  Schnittstellen, Senden/Empfangen, Routing, Netz-Thread, oeffentliche Funktionen
 *   arp.c   ARP          icmp.c  ICMP (Echo)      dhcp.c  DHCP-Client
 *   udp.c   UDP-Sockets  dns.c   DNS mit Cache    ntp.c   NTP */

#include "net/net.h"
#include "core/sched.h"
#include "lib/string.h"

#define ETH_HDR 14
#define IP_HDR  20
#define UDP_HDR 8

#define ARP_SIZE 32
#define PINGS    4

/* UDP-Sockets: pro Socket eine kleine Warteschlange empfangener Datagramme */
#define UDP_SOCKS 32
#define UDP_QUEUE 16

/* DNS-Zwischenspeicher */
#define DNS_CACHE 16

enum { DS_IDLE, DS_SELECTING, DS_REQUESTING, DS_BOUND, DS_RENEWING, DS_FAILED };

typedef struct {
    NetDev   dev;
    int      link, full_duplex;
    uint32_t mbps;
    uint8_t  ip[4], mask[4], gw[4], dns[4], ntp[4];
    /* DHCP */
    int      want_dhcp;     /* automatisch (wieder) starten, sobald die Verbindung steht */
    int      dstate;
    uint32_t xid, lease_s;
    uint64_t next_ms, bound_ms, start_ms;
    int      tries;
    uint8_t  offer_ip[4], server[4];
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
} Iface;

typedef struct {
    int      valid;
    uint8_t  ip[4], mac[6];
    int      iface;
    uint64_t ms;
} ArpEnt;

typedef struct {
    int      used, done;
    uint16_t id, seq;
    uint64_t sent_us;
    int64_t  result;
} PingSlot;

typedef struct {
    uint8_t  ip[4];
    uint16_t port, len;
    uint8_t  data[UDP_MAX_PAYLOAD];
} Dgram;

struct UdpSock {
    int      used;
    uint16_t port;
    Dgram   *q;
    int      head, count;
};

typedef struct {
    char     name[64];
    uint8_t  ip[4][4];
    int      n;
    uint64_t expires_ms;
} DnsEntry;

static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t BCAST_IP[4] = {255, 255, 255, 255};

/* ---------- Zustand (core.c) ---------- */

extern Iface    net_ifs[NET_MAX_DEVICES];
extern int      net_nif;
extern ArpEnt   arp_table[ARP_SIZE];
extern PingSlot net_pings[PINGS];
extern Mutex    net_lock;
extern uint8_t  net_txbuf[NET_FRAME_MAX + 64];

/* ---------- Kleine Helfer (Netzwerk-Bytefolge) ---------- */

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline int ip_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 4) == 0; }
static inline int ip_zero(const uint8_t *a) { return !a[0] && !a[1] && !a[2] && !a[3]; }

/* ---------- core.c ---------- */

uint32_t    net_rnd(void);
uint32_t    net_csum_add(uint32_t sum, const uint8_t *p, uint32_t len);
uint16_t    net_csum_fold(uint32_t sum);
int         net_prefix_len(const uint8_t *m);
int         net_interrupted(void);
int         net_dev_send(Iface *f, uint8_t *frame, uint32_t len);
int         net_ip_send(Iface *f, const uint8_t dst_mac[6], const uint8_t src[4], const uint8_t dst[4], uint8_t proto,
                        uint32_t len);
int         udp_send(Iface *f, const uint8_t dst_mac[6], const uint8_t src[4], const uint8_t dst[4], uint16_t sport,
                     uint16_t dport, uint32_t len);
void        net_wait_step(uint64_t started_ms);
int         arp_resolve(Iface *f, const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms);
int         net_is_local(const uint8_t ip[4]);
Iface      *net_route(const uint8_t dst[4], uint8_t next[4]);
int         net_poll_locked(void);
const char *net_parse_ip(const char *s, uint8_t out[4]);

/* ---------- arp.c, icmp.c, dhcp.c, udp.c, ntp.c ---------- */

int  arp_lookup(const uint8_t ip[4], uint8_t mac[6]);
void arp_update(const uint8_t ip[4], const uint8_t mac[6], int iface, int create);
void arp_send(Iface *f, uint16_t op, const uint8_t dst_mac[6], const uint8_t tha[6], const uint8_t tpa[4]);
void arp_in(Iface *f, const uint8_t *a, uint32_t len);
void icmp_in(Iface *f, const uint8_t *iph, const uint8_t *p, uint32_t len, const uint8_t *src_mac);
void dhcp_start(Iface *f);
void dhcp_in(Iface *f, const uint8_t *b, uint32_t len);
void dhcp_timer(Iface *f, uint64_t now);
void udp_in(Iface *f, const uint8_t *iph, const uint8_t *u, uint32_t len);
void ntp_start(void);

#endif
