#include "net.h"
#include "apic.h"
#include "cmdline.h"
#include "heap.h"
#include "kprintf.h"
#include "rtc.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "syscall.h"

/* Kleiner IPv4-Stack, siehe net.h. Alles laeuft unter einer Sperre (lock): der Thread "net" pollt die Karten und
 * erledigt Zeitgesteuertes (Link-Pruefung, DHCP); Syscalls wie ping nehmen dieselbe Sperre und pollen beim Warten
 * selbst. Gesendet wird aus txbuf, empfangen nach rxbuf (beide nur mit gehaltener Sperre benutzt). */

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

#define ARP_SIZE 32
#define PINGS    4

/* UDP-Sockets: pro Socket eine kleine Warteschlange empfangener Datagramme */
#define UDP_SOCKS 32
#define UDP_QUEUE 16

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

static UdpSock  socks[UDP_SOCKS];
static uint16_t next_port = 49152;

/* DNS-Zwischenspeicher */
#define DNS_CACHE 16
typedef struct {
    char     name[64];
    uint8_t  ip[4][4];
    int      n;
    uint64_t expires_ms;
} DnsEntry;
static DnsEntry dns_cache[DNS_CACHE];
static int      ntp_started;
static void     start_ntp(void);

static Iface    ifs[NET_MAX_DEVICES];
static int      nif;
static ArpEnt   arp[ARP_SIZE];
static PingSlot pings[PINGS];
static Mutex    lock = MUTEX_INIT;
static uint8_t  rxbuf[NET_FRAME_MAX + 64];
static uint8_t  txbuf[NET_FRAME_MAX + 64];
static uint16_t ip_ident;
static uint64_t rnd_state;

static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t BCAST_IP[4] = {255, 255, 255, 255};
static const uint8_t ZERO_IP[4] = {0, 0, 0, 0};

#define ETH_HDR 14
#define IP_HDR  20
#define UDP_HDR 8

/* ---------- Hilfen ---------- */

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int ip_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 4) == 0; }
static int ip_zero(const uint8_t *a) { return !a[0] && !a[1] && !a[2] && !a[3]; }

static uint32_t rnd(void)
{
    if (!rnd_state)
        rnd_state = time_us() | 1;
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return (uint32_t)rnd_state;
}

static uint32_t csum_add(uint32_t sum, const uint8_t *p, uint32_t len)
{
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int same_subnet(const Iface *f, const uint8_t *a)
{
    for (int i = 0; i < 4; i++)
        if ((a[i] & f->mask[i]) != (f->ip[i] & f->mask[i]))
            return 0;
    return 1;
}

static int prefix_len(const uint8_t *m)
{
    int n = 0;
    for (int i = 0; i < 4; i++)
        for (int b = 7; b >= 0; b--)
            if (m[i] & (1 << b))
                n++;
    return n;
}

static int interrupted(void)
{
    Process *p = process_current();
    return p && process_killed(p);
}

/* ---------- Senden ---------- */

static int dev_send(Iface *f, uint8_t *frame, uint32_t len)
{
    if (len < 60) { /* Mindestlaenge eines Ethernet-Rahmens (ohne Pruefsumme) */
        memset(frame + len, 0, 60 - len);
        len = 60;
    }
    if (f->dev.send(&f->dev, frame, len) != 0)
        return -1;
    f->tx_packets++;
    f->tx_bytes += len;
    return 0;
}

/* Sendet txbuf mit IPv4-Kopf; die Nutzdaten (len Bytes) stehen schon ab txbuf + ETH_HDR + IP_HDR */
static int ip_send(Iface *f, const uint8_t dst_mac[6], const uint8_t src[4], const uint8_t dst[4], uint8_t proto,
                   uint32_t len)
{
    uint8_t *e = txbuf, *h = txbuf + ETH_HDR;
    memcpy(e, dst_mac, 6);
    memcpy(e + 6, f->dev.mac, 6);
    put16(e + 12, 0x0800);
    h[0] = 0x45;
    h[1] = 0;
    put16(h + 2, (uint16_t)(IP_HDR + len));
    put16(h + 4, ip_ident++);
    put16(h + 6, 0x4000); /* nicht fragmentieren */
    h[8] = 64;
    h[9] = proto;
    put16(h + 10, 0);
    memcpy(h + 12, src, 4);
    memcpy(h + 16, dst, 4);
    put16(h + 10, csum_fold(csum_add(0, h, IP_HDR)));
    return dev_send(f, txbuf, ETH_HDR + IP_HDR + len);
}

/* UDP: Daten (len Bytes) stehen ab txbuf + ETH_HDR + IP_HDR + UDP_HDR */
static int udp_send(Iface *f, const uint8_t dst_mac[6], const uint8_t src[4], const uint8_t dst[4], uint16_t sport,
                    uint16_t dport, uint32_t len)
{
    uint8_t *u = txbuf + ETH_HDR + IP_HDR;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (uint16_t)(UDP_HDR + len));
    put16(u + 6, 0);
    uint8_t pseudo[12];
    memcpy(pseudo, src, 4);
    memcpy(pseudo + 4, dst, 4);
    pseudo[8] = 0;
    pseudo[9] = 17;
    put16(pseudo + 10, (uint16_t)(UDP_HDR + len));
    uint16_t c = csum_fold(csum_add(csum_add(0, pseudo, 12), u, UDP_HDR + len));
    put16(u + 6, c ? c : 0xFFFF);
    return ip_send(f, dst_mac, src, dst, 17, UDP_HDR + len);
}

/* ---------- ARP ---------- */

static int arp_lookup(const uint8_t ip[4], uint8_t mac[6])
{
    for (int i = 0; i < ARP_SIZE; i++)
        if (arp[i].valid && ip_eq(arp[i].ip, ip)) {
            memcpy(mac, arp[i].mac, 6);
            return 1;
        }
    return 0;
}

/* Eintrag setzen; create = 0: nur vorhandene Eintraege auffrischen */
static void arp_update(const uint8_t ip[4], const uint8_t mac[6], int iface, int create)
{
    if (ip_zero(ip))
        return;
    int slot = -1, free_slot = -1, oldest = 0;
    for (int i = 0; i < ARP_SIZE; i++) {
        if (arp[i].valid && ip_eq(arp[i].ip, ip))
            slot = i;
        else if (!arp[i].valid && free_slot < 0)
            free_slot = i;
        if (arp[i].valid && arp[i].ms < arp[oldest].ms)
            oldest = i;
    }
    if (slot < 0) {
        if (!create)
            return;
        slot = free_slot >= 0 ? free_slot : oldest; /* voll: aeltesten Eintrag ersetzen */
    }
    arp[slot].valid = 1;
    memcpy(arp[slot].ip, ip, 4);
    memcpy(arp[slot].mac, mac, 6);
    arp[slot].iface = iface;
    arp[slot].ms = time_ms();
}

static void arp_send(Iface *f, uint16_t op, const uint8_t dst_mac[6], const uint8_t tha[6], const uint8_t tpa[4])
{
    uint8_t *e = txbuf, *a = txbuf + ETH_HDR;
    memcpy(e, dst_mac, 6);
    memcpy(e + 6, f->dev.mac, 6);
    put16(e + 12, 0x0806);
    put16(a, 1);
    put16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, f->dev.mac, 6);
    memcpy(a + 14, f->ip, 4);
    memcpy(a + 18, tha, 6);
    memcpy(a + 24, tpa, 4);
    dev_send(f, txbuf, ETH_HDR + 28);
}

static void arp_in(Iface *f, const uint8_t *a, uint32_t len)
{
    if (len < 28 || be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4)
        return;
    uint16_t op = be16(a + 6);
    const uint8_t *sha = a + 8, *spa = a + 14, *tpa = a + 24;
    int for_us = !ip_zero(f->ip) && ip_eq(tpa, f->ip);
    arp_update(spa, sha, (int)(f - ifs), for_us);
    if (for_us && op == 1)
        arp_send(f, 2, sha, sha, spa);
}

/* ---------- Warten (mit gehaltener Sperre) ---------- */

static int poll_locked(void);

/* Sperre kurz freigeben und warten: anfangs nur den Prozessor abgeben (genaue Zeiten), spaeter schlafen */
static void wait_step(uint64_t started_ms)
{
    mutex_unlock(&lock);
    if (time_ms() - started_ms < 20)
        thread_yield();
    else
        thread_sleep_ms(10);
    mutex_lock(&lock);
}

static int arp_resolve(Iface *f, const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms)
{
    if (ip_eq(ip, BCAST_IP)) {
        memcpy(mac, BCAST_MAC, 6);
        return 0;
    }
    uint64_t start = time_ms();
    for (int attempt = 0; attempt < 3; attempt++) {
        if (arp_lookup(ip, mac))
            return 0;
        static const uint8_t zero_mac[6] = {0};
        arp_send(f, 1, BCAST_MAC, zero_mac, ip);
        uint64_t until = time_ms() + timeout_ms / 3;
        while (time_ms() < until) {
            poll_locked();
            if (arp_lookup(ip, mac))
                return 0;
            if (interrupted())
                return ERR_INTR;
            wait_step(start);
        }
    }
    return ERR_HOSTUNREACH;
}

/* Adresse dieses Rechners (eine eigene oder 127.x.x.x) */
static int is_local(const uint8_t ip[4])
{
    if (ip[0] == 127)
        return 1;
    for (int i = 0; i < nif; i++)
        if (!ip_zero(ifs[i].ip) && ip_eq(ifs[i].ip, ip))
            return 1;
    return 0;
}

/* Interface und naechste Station (Ziel selbst oder Gateway) fuer eine Zieladresse */
static Iface *route(const uint8_t dst[4], uint8_t next[4])
{
    for (int i = 0; i < nif; i++) {
        Iface *f = &ifs[i];
        if (f->link && !ip_zero(f->ip) && same_subnet(f, dst)) {
            memcpy(next, dst, 4);
            return f;
        }
    }
    for (int i = 0; i < nif; i++) {
        Iface *f = &ifs[i];
        if (f->link && !ip_zero(f->ip) && !ip_zero(f->gw)) {
            memcpy(next, f->gw, 4);
            return f;
        }
    }
    return 0;
}

/* ---------- ICMP ---------- */

static void icmp_in(Iface *f, const uint8_t *iph, const uint8_t *p, uint32_t len, const uint8_t *src_mac)
{
    if (len < 8 || csum_fold(csum_add(0, p, len)) != 0)
        return;
    const uint8_t *src = iph + 12, *dst = iph + 16;
    if (p[0] == 8 && ip_eq(dst, f->ip)) { /* Echo-Anfrage: antworten */
        if (len > NET_MTU - IP_HDR)
            return;
        uint8_t *r = txbuf + ETH_HDR + IP_HDR;
        memcpy(r, p, len);
        r[0] = 0;
        put16(r + 2, 0);
        put16(r + 2, csum_fold(csum_add(0, r, len)));
        uint8_t to[4];
        memcpy(to, src, 4);
        ip_send(f, src_mac, f->ip, to, 1, len);
        return;
    }
    uint16_t id, seq;
    int64_t result;
    if (p[0] == 0) { /* Echo-Antwort */
        id = be16(p + 4);
        seq = be16(p + 6);
        result = 0;
    } else if ((p[0] == 3 || p[0] == 11) && len >= 8 + 20 + 8 && p[8 + 9] == 1) { /* nicht erreichbar / TTL abgelaufen */
        const uint8_t *orig = p + 8 + (p[8] & 15) * 4;
        if (orig + 8 > p + len || orig[0] != 8)
            return;
        id = be16(orig + 4);
        seq = be16(orig + 6);
        result = p[0] == 3 ? ERR_HOSTUNREACH : ERR_TIMEDOUT;
    } else {
        return;
    }
    for (int i = 0; i < PINGS; i++) {
        PingSlot *s = &pings[i];
        if (s->used && !s->done && s->id == id && s->seq == seq) {
            if (result == 0) {
                uint64_t us = time_us() - s->sent_us;
                result = (int64_t)((us & 0xFFFFFFFFFFULL) | ((uint64_t)iph[8] << 40));
            }
            s->result = result;
            s->done = 1;
        }
    }
}

/* ---------- DHCP ---------- */

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

static void dhcp_send(Iface *f, int type)
{
    uint8_t *b = txbuf + ETH_HDR + IP_HDR + UDP_HDR;
    memset(b, 0, 300);
    int renew = f->dstate == DS_RENEWING;
    b[0] = 1;
    b[1] = 1;
    b[2] = 6;
    put32(b + 4, f->xid);
    uint64_t secs = (time_ms() - f->start_ms) / 1000;
    put16(b + 8, (uint16_t)(secs > 0xFFFF ? 0xFFFF : secs));
    put16(b + 10, 0x8000); /* Antwort bitte als Broadcast */
    if (renew)
        memcpy(b + 12, f->ip, 4);
    memcpy(b + 28, f->dev.mac, 6);
    put32(b + 236, 0x63825363);
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    *o++ = 61; *o++ = 7; *o++ = 1;
    memcpy(o, f->dev.mac, 6);
    o += 6;
    *o++ = 12; *o++ = 10;
    memcpy(o, "minikernel", 10);
    o += 10;
    if (type == DHCP_REQUEST && !renew) {
        *o++ = 50; *o++ = 4; memcpy(o, f->offer_ip, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, f->server, 4); o += 4;
    }
    *o++ = 55; *o++ = 6; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 42; *o++ = 51; *o++ = 54;
    *o++ = 255;
    uint32_t len = (uint32_t)(o - b);
    if (len < 300)
        len = 300;
    udp_send(f, BCAST_MAC, renew ? f->ip : ZERO_IP, BCAST_IP, 68, 67, len);
}

static void dhcp_start(Iface *f)
{
    f->dstate = DS_SELECTING;
    f->xid = rnd();
    f->tries = 0;
    f->start_ms = time_ms();
    f->next_ms = f->start_ms + 2000;
    dhcp_send(f, DHCP_DISCOVER);
}

static void dhcp_in(Iface *f, const uint8_t *b, uint32_t len)
{
    if (len < 240 || b[0] != 2 || be32(b + 4) != f->xid || memcmp(b + 28, f->dev.mac, 6) != 0 ||
        be32(b + 236) != 0x63825363)
        return;
    int type = 0;
    uint8_t mask[4] = {255, 255, 255, 0}, router[4] = {0}, dns[4] = {0}, server[4] = {0}, ntp[4] = {0};
    uint32_t lease = 3600;
    for (uint32_t i = 240; i < len;) {
        uint8_t opt = b[i];
        if (opt == 255)
            break;
        if (opt == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + b[i + 1] > len)
            break;
        uint8_t l = b[i + 1];
        const uint8_t *v = b + i + 2;
        if (opt == 53 && l >= 1) type = v[0];
        else if (opt == 1 && l >= 4) memcpy(mask, v, 4);
        else if (opt == 3 && l >= 4) memcpy(router, v, 4);
        else if (opt == 6 && l >= 4) memcpy(dns, v, 4);
        else if (opt == 42 && l >= 4) memcpy(ntp, v, 4);
        else if (opt == 51 && l >= 4) lease = be32(v);
        else if (opt == 54 && l >= 4) memcpy(server, v, 4);
        i += 2u + l;
    }
    if (type == DHCP_OFFER && f->dstate == DS_SELECTING) {
        memcpy(f->offer_ip, b + 16, 4);
        memcpy(f->server, server, 4);
        f->dstate = DS_REQUESTING;
        f->tries = 0;
        f->next_ms = time_ms() + 2000;
        dhcp_send(f, DHCP_REQUEST);
    } else if (type == DHCP_ACK && (f->dstate == DS_REQUESTING || f->dstate == DS_RENEWING)) {
        int changed = !ip_eq(f->ip, b + 16) || !ip_eq(f->gw, router);
        memcpy(f->ip, b + 16, 4);
        memcpy(f->mask, mask, 4);
        memcpy(f->gw, router, 4);
        memcpy(f->dns, dns, 4);
        memcpy(f->ntp, ntp, 4);
        if (!ip_zero(server))
            memcpy(f->server, server, 4);
        f->lease_s = lease < 60 ? 60 : lease;
        f->bound_ms = time_ms();
        f->dstate = DS_BOUND;
        if (changed)
            kprintf("net: %s: %u.%u.%u.%u/%d, Gateway %u.%u.%u.%u, DNS %u.%u.%u.%u (DHCP von %u.%u.%u.%u)\n",
                    f->dev.name, f->ip[0], f->ip[1], f->ip[2], f->ip[3], prefix_len(f->mask), f->gw[0], f->gw[1], f->gw[2],
                    f->gw[3], f->dns[0], f->dns[1], f->dns[2], f->dns[3], f->server[0], f->server[1], f->server[2],
                    f->server[3]);
        static const uint8_t zero_mac[6] = {0};
        arp_send(f, 1, BCAST_MAC, zero_mac, f->ip); /* "gratuitous ARP": die neue Adresse bekannt machen */
        start_ntp();
    } else if (type == DHCP_NAK && f->dstate != DS_SELECTING) {
        kprintf("net: %s: DHCP-Server lehnt die Adresse ab, fange neu an\n", f->dev.name);
        memset(f->ip, 0, 4);
        dhcp_start(f);
    }
}

static void dhcp_timer(Iface *f, uint64_t now)
{
    switch (f->dstate) {
    case DS_SELECTING:
    case DS_REQUESTING:
        if (now < f->next_ms)
            break;
        if (++f->tries > 4) {
            kprintf("net: %s: keine Antwort von einem DHCP-Server (neuer Versuch in 30 s; 'ifconfig' zum Einstellen)\n",
                    f->dev.name);
            f->dstate = DS_FAILED;
            f->next_ms = now + 30000;
            break;
        }
        if (f->dstate == DS_REQUESTING && f->tries > 2) { /* Angebot verfallen? von vorn */
            dhcp_start(f);
            break;
        }
        dhcp_send(f, f->dstate == DS_SELECTING ? DHCP_DISCOVER : DHCP_REQUEST);
        f->next_ms = now + (2000u << (f->tries < 3 ? f->tries : 3));
        break;
    case DS_BOUND:
        if (now >= f->bound_ms + (uint64_t)f->lease_s * 500) { /* halbe Laufzeit: verlaengern */
            f->dstate = DS_RENEWING;
            f->xid = rnd();
            f->tries = 0;
            f->next_ms = now + 5000;
            dhcp_send(f, DHCP_REQUEST);
        }
        break;
    case DS_RENEWING:
        if (now >= f->bound_ms + (uint64_t)f->lease_s * 1000) { /* abgelaufen */
            kprintf("net: %s: DHCP-Adresse abgelaufen\n", f->dev.name);
            memset(f->ip, 0, 4);
            dhcp_start(f);
        } else if (now >= f->next_ms) {
            f->next_ms = now + 5000;
            dhcp_send(f, DHCP_REQUEST);
        }
        break;
    case DS_FAILED:
        if (now >= f->next_ms)
            dhcp_start(f);
        break;
    }
}

/* ---------- Empfang ---------- */

/* Datagramm in die Warteschlange des Sockets mit diesem Port legen; 0 = kein Socket oder Warteschlange voll */
static int udp_deliver(const uint8_t src[4], uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len)
{
    for (int i = 0; i < UDP_SOCKS; i++) {
        UdpSock *s = &socks[i];
        if (!s->used || s->port != dport)
            continue;
        if (s->count == UDP_QUEUE || len > UDP_MAX_PAYLOAD)
            return 0;
        Dgram *d = &s->q[(s->head + s->count) % UDP_QUEUE];
        memcpy(d->ip, src, 4);
        d->port = sport;
        d->len = (uint16_t)len;
        memcpy(d->data, data, len);
        s->count++;
        return 1;
    }
    return 0;
}

static void udp_in(Iface *f, const uint8_t *iph, const uint8_t *u, uint32_t len)
{
    if (len < UDP_HDR)
        return;
    uint16_t ulen = be16(u + 4);
    if (ulen < UDP_HDR || ulen > len)
        return;
    if (be16(u + 6)) { /* Pruefsumme (0 = keine) */
        uint8_t pseudo[12];
        memcpy(pseudo, iph + 12, 8);
        pseudo[8] = 0;
        pseudo[9] = 17;
        put16(pseudo + 10, ulen);
        if (csum_fold(csum_add(csum_add(0, pseudo, 12), u, ulen)) != 0) {
            f->rx_dropped++;
            return;
        }
    }
    uint16_t dport = be16(u + 2);
    if (dport == 68 && be16(u) == 67) {
        dhcp_in(f, u + UDP_HDR, ulen - UDP_HDR);
        return;
    }
    if (!udp_deliver(iph + 12, be16(u), dport, u + UDP_HDR, ulen - UDP_HDR))
        f->rx_dropped++;
}

static void ip_in(Iface *f, const uint8_t *h, uint32_t len, const uint8_t *src_mac)
{
    if (len < IP_HDR || (h[0] >> 4) != 4)
        return;
    uint32_t ihl = (h[0] & 15) * 4u, tot = be16(h + 2);
    if (ihl < IP_HDR || tot < ihl || tot > len || csum_fold(csum_add(0, h, ihl)) != 0) {
        f->rx_dropped++;
        return;
    }
    if (be16(h + 6) & 0x3FFF) { /* Fragment: wird nicht zusammengesetzt */
        f->rx_dropped++;
        return;
    }
    const uint8_t *dst = h + 16;
    int subnet_bcast = 1;
    for (int i = 0; i < 4; i++)
        if ((dst[i] | f->mask[i]) != 255 || (dst[i] & f->mask[i]) != (f->ip[i] & f->mask[i]))
            subnet_bcast = 0;
    if (!ip_zero(f->ip) && !ip_eq(dst, f->ip) && !ip_eq(dst, BCAST_IP) && !subnet_bcast)
        return; /* nicht fuer uns (ohne Adresse, waehrend DHCP, wird alles angenommen) */
    if (!ip_zero(f->ip) && same_subnet(f, h + 12))
        arp_update(h + 12, src_mac, (int)(f - ifs), 0);
    if (h[9] == 1)
        icmp_in(f, h, h + ihl, tot - ihl, src_mac);
    else if (h[9] == 17)
        udp_in(f, h, h + ihl, tot - ihl);
}

static void frame_in(Iface *f, const uint8_t *p, uint32_t len)
{
    f->rx_packets++;
    f->rx_bytes += len;
    if (memcmp(p, f->dev.mac, 6) != 0 && memcmp(p, BCAST_MAC, 6) != 0)
        return;
    uint16_t type = be16(p + 12);
    if (type == 0x0806)
        arp_in(f, p + ETH_HDR, len - ETH_HDR);
    else if (type == 0x0800)
        ip_in(f, p + ETH_HDR, len - ETH_HDR, p + 6);
}

/* Holt alle wartenden Rahmen ab; Anzahl */
static int poll_locked(void)
{
    int total = 0;
    for (int i = 0; i < nif; i++) {
        Iface *f = &ifs[i];
        for (int k = 0; k < 64; k++) {
            int len = f->dev.recv(&f->dev, rxbuf, NET_FRAME_MAX);
            if (len <= 0)
                break;
            frame_in(f, rxbuf, (uint32_t)len);
            total++;
        }
    }
    return total;
}

static void periodic_locked(uint64_t now)
{
    for (int i = 0; i < nif; i++) {
        Iface *f = &ifs[i];
        uint32_t mbps = 0;
        int fd = 0;
        int up = f->dev.link(&f->dev, &mbps, &fd);
        if (up != f->link) {
            f->link = up;
            f->mbps = mbps;
            f->full_duplex = fd;
            if (up) {
                kprintf("net: %s: Verbindung hergestellt, %u Mbit/s, %s\n", f->dev.name, mbps, fd ? "Vollduplex" : "Halbduplex");
                if (f->want_dhcp) {
                    dhcp_start(f);
                } else if (!ip_zero(f->ip)) {
                    static const uint8_t zero_mac[6] = {0};
                    arp_send(f, 1, BCAST_MAC, zero_mac, f->ip); /* feste Adresse bekannt machen */
                    if (!ip_zero(f->gw))
                        start_ntp();
                }
            } else {
                kprintf("net: %s: Verbindung getrennt\n", f->dev.name);
                if (f->want_dhcp)
                    f->dstate = DS_IDLE; /* nach dem Wiederverbinden neu anfragen */
            }
        }
        if (f->link && f->want_dhcp)
            dhcp_timer(f, now);
    }
}

static void net_thread(void *arg)
{
    (void)arg;
    uint64_t last = 0;
    for (;;) {
        mutex_lock(&lock);
        int n = poll_locked();
        uint64_t now = time_ms();
        if (now - last >= 100) {
            periodic_locked(now);
            last = now;
        }
        mutex_unlock(&lock);
        if (n)
            thread_yield();
        else
            thread_sleep_ms(10);
    }
}

/* ---------- Oeffentliche Funktionen ---------- */

/* "a.b.c.d" am Anfang von s; liefert das Ende oder NULL */
static const char *parse_ip(const char *s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return 0;
        int v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        if (v > 255 || (i < 3 && *s++ != '.'))
            return 0;
        out[i] = (uint8_t)v;
    }
    return s;
}

/* cmdline.txt: ip=192.168.1.50/24,192.168.1.1[,dns] setzt eine feste Adresse fuer eth0 (statt DHCP) */
static int static_from_cmdline(Iface *f)
{
    const char *v = cmdline_get("ip");
    if (!v)
        return 0;
    uint8_t ip[4], gw[4] = {0}, dns[4] = {0};
    int prefix = 24;
    const char *s = parse_ip(v, ip);
    if (s && *s == '/') {
        prefix = 0;
        for (s++; *s >= '0' && *s <= '9'; s++)
            prefix = prefix * 10 + (*s - '0');
    }
    if (s && *s == ',')
        s = parse_ip(s + 1, gw);
    if (s && *s == ',')
        s = parse_ip(s + 1, dns);
    if (!s || (*s && *s != ' ') || prefix > 32) {
        kprintf("net: 'ip=%s' nicht verstanden (Beispiel: ip=192.168.1.50/24,192.168.1.1)\n", v);
        return 0;
    }
    memcpy(f->ip, ip, 4);
    memset(f->mask, 0, 4);
    for (int i = 0; i < prefix; i++)
        f->mask[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    memcpy(f->gw, gw, 4);
    memcpy(f->dns, dns, 4);
    kprintf("net: %s: feste Adresse %u.%u.%u.%u/%d (aus cmdline.txt)\n", f->dev.name, ip[0], ip[1], ip[2], ip[3], prefix);
    return 1;
}

int net_register(const NetDev *d)
{
    if (nif >= NET_MAX_DEVICES)
        return -1;
    Iface *f = &ifs[nif];
    memset(f, 0, sizeof(*f));
    f->dev = *d;
    ksnprintf(f->dev.name, sizeof(f->dev.name), "eth%d", nif);
    f->want_dhcp = !cmdline_has("nodhcp") && !(nif == 0 && static_from_cmdline(f));
    nif++;
    return 0;
}

void net_init(void)
{
    if (cmdline_has("nonet")) {
        kprintf("net: abgeschaltet (nonet)\n");
        return;
    }
    e1000_probe();
    if (!nif) {
        kprintf("net: keine unterstuetzte Netzwerkkarte gefunden\n");
        return;
    }
    thread_create("net", net_thread, 0);
}

int net_info(unsigned index, NetInfo *out)
{
    if (index >= (unsigned)nif)
        return -1;
    mutex_lock(&lock);
    Iface *f = &ifs[index];
    memset(out, 0, sizeof(*out));
    memcpy(out->name, f->dev.name, sizeof(out->name));
    memcpy(out->model, f->dev.model, sizeof(out->model));
    memcpy(out->mac, f->dev.mac, 6);
    out->link = (uint8_t)f->link; /* Stand des Netzwerk-Threads (danach richtet sich auch das Routing) */
    out->full_duplex = (uint8_t)f->full_duplex;
    out->mbps = f->mbps;
    memcpy(out->ip, f->ip, 4);
    memcpy(out->mask, f->mask, 4);
    memcpy(out->gateway, f->gw, 4);
    memcpy(out->dns, f->dns, 4);
    memcpy(out->dhcp_server, f->server, 4);
    out->dhcp = !f->want_dhcp ? 0 : f->dstate == DS_BOUND || f->dstate == DS_RENEWING ? 2 : f->dstate == DS_FAILED ? 3 : 1;
    if (f->dstate == DS_BOUND || f->dstate == DS_RENEWING) {
        uint64_t end = f->bound_ms + (uint64_t)f->lease_s * 1000, now = time_ms();
        out->lease_s = end > now ? (uint32_t)((end - now) / 1000) : 0;
    }
    out->rx_packets = f->rx_packets;
    out->tx_packets = f->tx_packets;
    out->rx_bytes = f->rx_bytes;
    out->tx_bytes = f->tx_bytes;
    out->rx_dropped = f->rx_dropped;
    mutex_unlock(&lock);
    return 0;
}

int net_set_static(unsigned index, const uint8_t cfg[16])
{
    if (index >= (unsigned)nif)
        return ERR_NOENT;
    mutex_lock(&lock);
    Iface *f = &ifs[index];
    f->want_dhcp = 0;
    f->dstate = DS_IDLE;
    memcpy(f->ip, cfg, 4);
    memcpy(f->mask, cfg + 4, 4);
    memcpy(f->gw, cfg + 8, 4);
    memcpy(f->dns, cfg + 12, 4);
    memset(f->server, 0, 4);
    memset(f->ntp, 0, 4);
    if (!ip_zero(f->ip) && f->link) {
        static const uint8_t zero_mac[6] = {0};
        arp_send(f, 1, BCAST_MAC, zero_mac, f->ip);
        if (!ip_zero(f->gw))
            start_ntp();
    }
    mutex_unlock(&lock);
    return 0;
}

int net_start_dhcp(unsigned index)
{
    if (index >= (unsigned)nif)
        return ERR_NOENT;
    mutex_lock(&lock);
    Iface *f = &ifs[index];
    f->want_dhcp = 1;
    memset(f->ip, 0, 4);
    memset(f->gw, 0, 4);
    memset(f->dns, 0, 4);
    if (f->link)
        dhcp_start(f);
    else
        f->dstate = DS_IDLE;
    mutex_unlock(&lock);
    return 0;
}

int net_arp_info(unsigned index, ArpInfo *out)
{
    mutex_lock(&lock);
    unsigned seen = 0;
    int r = -1;
    for (int i = 0; i < ARP_SIZE; i++) {
        if (!arp[i].valid || seen++ != index)
            continue;
        memset(out, 0, sizeof(*out));
        memcpy(out->ip, arp[i].ip, 4);
        memcpy(out->mac, arp[i].mac, 6);
        out->age_s = (uint32_t)((time_ms() - arp[i].ms) / 1000);
        memcpy(out->dev, ifs[arp[i].iface].dev.name, sizeof(out->dev));
        r = 0;
        break;
    }
    mutex_unlock(&lock);
    return r;
}

int64_t net_ping(const uint8_t ip[4], uint16_t seq, uint32_t size, uint32_t timeout_ms)
{
    if (!nif)
        return ERR_NETUNREACH;
    if (size > NET_MTU - IP_HDR - 8)
        size = NET_MTU - IP_HDR - 8;
    if (timeout_ms < 10)
        timeout_ms = 10;
    mutex_lock(&lock);
    if (is_local(ip)) { /* eigene Adresse oder 127.x.x.x */
        mutex_unlock(&lock);
        return 1 | (64LL << 40);
    }
    uint8_t next[4], mac[6];
    Iface *f = route(ip, next);
    if (!f) {
        mutex_unlock(&lock);
        return ERR_NETUNREACH;
    }
    int r = arp_resolve(f, next, mac, timeout_ms < 3000 ? timeout_ms : 3000);
    if (r != 0) {
        mutex_unlock(&lock);
        return r;
    }
    PingSlot *s = 0;
    int slot = 0;
    for (; slot < PINGS; slot++)
        if (!pings[slot].used) {
            s = &pings[slot];
            break;
        }
    if (!s) {
        mutex_unlock(&lock);
        return ERR_AGAIN;
    }
    s->used = 1;
    s->done = 0;
    s->id = (uint16_t)(0x4D00 + slot);
    s->seq = seq;

    uint8_t *m = txbuf + ETH_HDR + IP_HDR;
    m[0] = 8;
    m[1] = 0;
    put16(m + 2, 0);
    put16(m + 4, s->id);
    put16(m + 6, seq);
    for (uint32_t i = 0; i < size; i++)
        m[8 + i] = (uint8_t)(0x20 + i % 64);
    put16(m + 2, csum_fold(csum_add(0, m, 8 + size)));
    uint8_t dst[4];
    memcpy(dst, ip, 4);
    s->sent_us = time_us();
    int64_t result = ERR_TIMEDOUT;
    if (ip_send(f, mac, f->ip, dst, 1, 8 + size) != 0) {
        result = ERR_IO;
    } else {
        uint64_t start = time_ms();
        while (time_ms() - start < timeout_ms) {
            poll_locked();
            if (s->done) {
                result = s->result;
                break;
            }
            if (interrupted()) {
                result = ERR_INTR;
                break;
            }
            wait_step(start);
        }
    }
    s->used = 0;
    mutex_unlock(&lock);
    return result;
}

/* ---------- UDP-Sockets ---------- */

UdpSock *udp_open(uint16_t port, int *err)
{
    Dgram *q = kcalloc(UDP_QUEUE, sizeof(Dgram));
    if (!q) {
        *err = ERR_NOMEM;
        return 0;
    }
    mutex_lock(&lock);
    UdpSock *s = 0;
    for (int i = 0; i < UDP_SOCKS && !s; i++)
        if (!socks[i].used)
            s = &socks[i];
    int in_use = port == 68;
    for (int i = 0; i < UDP_SOCKS; i++)
        if (port && socks[i].used && socks[i].port == port)
            in_use = 1;
    if (!s || in_use) {
        mutex_unlock(&lock);
        kfree(q);
        *err = !s ? ERR_NOMEM : ERR_EXIST;
        return 0;
    }
    if (!port) { /* freien Port aus dem Bereich 49152-65535 */
        for (int tries = 0; tries < 16384 && !port; tries++) {
            uint16_t p = next_port;
            next_port = next_port == 65535 ? 49152 : next_port + 1;
            int used = 0;
            for (int i = 0; i < UDP_SOCKS; i++)
                if (socks[i].used && socks[i].port == p)
                    used = 1;
            if (!used)
                port = p;
        }
    }
    s->used = 1;
    s->port = port;
    s->q = q;
    s->head = s->count = 0;
    mutex_unlock(&lock);
    return s;
}

void udp_close(UdpSock *s)
{
    if (!s)
        return;
    mutex_lock(&lock);
    Dgram *q = s->q;
    s->used = 0;
    s->q = 0;
    s->count = 0;
    mutex_unlock(&lock);
    kfree(q);
}

uint16_t udp_local_port(const UdpSock *s)
{
    return s->port;
}

int udp_pending(UdpSock *s)
{
    mutex_lock(&lock);
    poll_locked();
    int n = s->count;
    mutex_unlock(&lock);
    return n;
}

int udp_sendto(UdpSock *s, const uint8_t ip[4], uint16_t port, const void *data, uint32_t len)
{
    if (len > UDP_MAX_PAYLOAD || !port)
        return ERR_INVAL;
    mutex_lock(&lock);
    int r;
    if (is_local(ip)) { /* an sich selbst: direkt zustellen */
        static const uint8_t lo[4] = {127, 0, 0, 1};
        r = udp_deliver(lo, s->port, port, data, len) ? (int)len : ERR_AGAIN;
        mutex_unlock(&lock);
        return r;
    }
    Iface *f = 0;
    uint8_t next[4], mac[6];
    if (ip_eq(ip, BCAST_IP)) {
        for (int i = 0; i < nif && !f; i++)
            if (ifs[i].link)
                f = &ifs[i];
        memcpy(mac, BCAST_MAC, 6);
        r = f ? 0 : ERR_NETUNREACH;
    } else {
        f = route(ip, next);
        r = f ? arp_resolve(f, next, mac, 3000) : ERR_NETUNREACH;
    }
    if (r == 0) {
        uint8_t dst[4];
        memcpy(dst, ip, 4);
        memcpy(txbuf + ETH_HDR + IP_HDR + UDP_HDR, data, len);
        r = udp_send(f, mac, f->ip, dst, s->port, port, len) == 0 ? (int)len : ERR_IO;
    }
    mutex_unlock(&lock);
    return r;
}

int udp_recvfrom(UdpSock *s, void *buf, uint32_t max, uint8_t ip[4], uint16_t *port, uint32_t timeout_ms)
{
    mutex_lock(&lock);
    uint64_t start = time_ms();
    int r;
    for (;;) {
        if (!s->count)
            poll_locked();
        if (s->count) {
            Dgram *d = &s->q[s->head];
            uint32_t n = d->len < max ? d->len : max;
            memcpy(buf, d->data, n);
            if (ip)
                memcpy(ip, d->ip, 4);
            if (port)
                *port = d->port;
            s->head = (s->head + 1) % UDP_QUEUE;
            s->count--;
            r = (int)n;
            break;
        }
        if (timeout_ms == 0) {
            r = ERR_AGAIN;
            break;
        }
        if (timeout_ms != NET_WAIT_FOREVER && time_ms() - start >= timeout_ms) {
            r = ERR_TIMEDOUT;
            break;
        }
        if (interrupted()) {
            r = ERR_INTR;
            break;
        }
        wait_step(start);
    }
    mutex_unlock(&lock);
    return r;
}

/* ---------- DNS ---------- */

static int skip_name(const uint8_t *b, int n, int pos)
{
    while (pos < n) {
        uint8_t l = b[pos];
        if (l == 0)
            return pos + 1;
        if ((l & 0xC0) == 0xC0)
            return pos + 2 <= n ? pos + 2 : -1;
        if (l & 0xC0)
            return -1;
        pos += l + 1;
    }
    return -1;
}

/* Eine Anfrage (Typ A) an einen Server. Anzahl der Adressen oder Fehler; ttl = kleinste Gueltigkeit */
static int dns_query(const uint8_t server[4], const char *name, uint8_t out[][4], int max, uint32_t *ttl, uint32_t timeout_ms)
{
    uint8_t q[300];
    uint16_t id = (uint16_t)rnd();
    put16(q, id);
    put16(q + 2, 0x0100); /* rekursive Anfrage */
    put16(q + 4, 1);
    put16(q + 6, 0);
    put16(q + 8, 0);
    put16(q + 10, 0);
    int pos = 12;
    for (const char *p = name; *p;) {
        const char *e = p;
        while (*e && *e != '.')
            e++;
        int l = (int)(e - p);
        if (l < 1 || l > 63 || pos + l + 6 > (int)sizeof(q))
            return ERR_INVAL;
        q[pos++] = (uint8_t)l;
        memcpy(q + pos, p, (size_t)l);
        pos += l;
        p = *e ? e + 1 : e;
    }
    q[pos++] = 0;
    put16(q + pos, 1); /* Typ A */
    put16(q + pos + 2, 1); /* Klasse IN */
    pos += 4;

    int err;
    UdpSock *s = udp_open(0, &err);
    if (!s)
        return err;
    int r = udp_sendto(s, server, 53, q, (uint32_t)pos);
    if (r < 0) {
        udp_close(s);
        return r;
    }
    static uint8_t resp[UDP_MAX_PAYLOAD]; /* nur mit dns_lock benutzt */
    uint64_t start = time_ms();
    r = ERR_TIMEDOUT;
    while (time_ms() - start < timeout_ms) {
        uint8_t from[4];
        uint16_t fport;
        int n = udp_recvfrom(s, resp, sizeof(resp), from, &fport, (uint32_t)(timeout_ms - (time_ms() - start)));
        if (n == ERR_INTR || n == ERR_TIMEDOUT) {
            r = n;
            break;
        }
        if (n < 12 || fport != 53 || !ip_eq(from, server) || be16(resp) != id || !(resp[2] & 0x80))
            continue; /* nicht die Antwort auf diese Anfrage */
        int rcode = resp[3] & 15;
        if (rcode == 3) {
            r = ERR_NOENT;
            break;
        }
        if (rcode != 0) {
            r = ERR_IO;
            break;
        }
        int qd = be16(resp + 4), an = be16(resp + 6), p = 12, count = 0;
        *ttl = 3600;
        for (int i = 0; i < qd && p >= 0; i++) {
            p = skip_name(resp, n, p);
            if (p >= 0)
                p += 4;
        }
        for (int i = 0; i < an && p >= 0 && p <= n; i++) {
            p = skip_name(resp, n, p);
            if (p < 0 || p + 10 > n)
                break;
            uint16_t type = be16(resp + p), cls = be16(resp + p + 2), rdlen = be16(resp + p + 8);
            uint32_t t = be32(resp + p + 4);
            p += 10;
            if (p + rdlen > n)
                break;
            if (type == 1 && cls == 1 && rdlen == 4 && count < max) {
                memcpy(out[count++], resp + p, 4);
                if (t < *ttl)
                    *ttl = t;
            }
            p += rdlen;
        }
        r = count ? count : ERR_NOENT;
        break;
    }
    udp_close(s);
    return r;
}

static Mutex dns_lock = MUTEX_INIT;

int net_resolve(const char *name, uint8_t out[][4], int max)
{
    if (max < 1)
        return ERR_INVAL;
    uint8_t ip[4];
    const char *e = parse_ip(name, ip);
    if (e && !*e) {
        memcpy(out[0], ip, 4);
        return 1;
    }
    char n[64];
    int len = 0;
    for (; name[len]; len++) {
        if (len >= 63)
            return ERR_INVAL;
        char c = name[len];
        n[len] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (len && n[len - 1] == '.')
        len--;
    n[len] = 0;
    if (!len)
        return ERR_INVAL;
    if (strcmp(n, "localhost") == 0) {
        static const uint8_t lo[4] = {127, 0, 0, 1};
        memcpy(out[0], lo, 4);
        return 1;
    }

    mutex_lock(&lock);
    uint64_t now = time_ms();
    for (int i = 0; i < DNS_CACHE; i++) {
        DnsEntry *c = &dns_cache[i];
        if (c->n && c->expires_ms > now && strcmp(c->name, n) == 0) {
            int k = c->n < max ? c->n : max;
            memcpy(out, c->ip, (size_t)k * 4);
            mutex_unlock(&lock);
            return k;
        }
    }
    uint8_t servers[3][4];
    int ns = 0;
    for (int i = 0; i < nif && ns < 3; i++) {
        Iface *f = &ifs[i];
        if (!f->link || ip_zero(f->ip))
            continue;
        const uint8_t *cand = !ip_zero(f->dns) ? f->dns : f->gw;
        if (ip_zero(cand))
            continue;
        int dup = 0;
        for (int k = 0; k < ns; k++)
            dup |= ip_eq(servers[k], cand);
        if (!dup)
            memcpy(servers[ns++], cand, 4);
    }
    mutex_unlock(&lock);
    if (!ns)
        return ERR_NETUNREACH;

    mutex_lock(&dns_lock);
    uint8_t got[8][4];
    uint32_t ttl = 0;
    int r = ERR_TIMEDOUT;
    for (int attempt = 0; attempt < 2 && r == ERR_TIMEDOUT; attempt++)
        for (int k = 0; k < ns; k++) {
            r = dns_query(servers[k], n, got, 8, &ttl, 1500);
            if (r != ERR_TIMEDOUT && r != ERR_HOSTUNREACH)
                break;
            if (r == ERR_HOSTUNREACH)
                r = ERR_TIMEDOUT;
        }
    mutex_unlock(&dns_lock);
    if (r <= 0)
        return r;

    mutex_lock(&lock);
    int slot = 0;
    for (int i = 1; i < DNS_CACHE; i++) /* abgelaufenen oder am fruehesten ablaufenden Eintrag ersetzen */
        if (dns_cache[i].expires_ms < dns_cache[slot].expires_ms)
            slot = i;
    DnsEntry *c = &dns_cache[slot];
    memcpy(c->name, n, (size_t)len + 1);
    c->n = r < 4 ? r : 4;
    memcpy(c->ip, got, (size_t)c->n * 4);
    if (ttl < 30) ttl = 30;
    if (ttl > 3600) ttl = 3600;
    c->expires_ms = time_ms() + (uint64_t)ttl * 1000;
    mutex_unlock(&lock);
    int k = r < max ? r : max;
    memcpy(out, got, (size_t)k * 4);
    return k;
}

/* ---------- NTP ---------- */

#define NTP_UNIX_DIFF 2208988800ULL /* Sekunden von 1900 bis 1970 */

int net_ntp(const char *server, int set_clock, NtpResult *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t ip[1][4];
    if (server && *server) {
        int i = 0;
        for (; server[i] && i < (int)sizeof(out->server) - 1; i++)
            out->server[i] = server[i];
        out->server[i] = 0;
    } else {
        mutex_lock(&lock);
        for (int i = 0; i < nif; i++)
            if (!ip_zero(ifs[i].ntp)) {
                const uint8_t *a = ifs[i].ntp;
                ksnprintf(out->server, sizeof(out->server), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
                break;
            }
        mutex_unlock(&lock);
        if (!out->server[0])
            ksnprintf(out->server, sizeof(out->server), "pool.ntp.org");
    }
    int r = net_resolve(out->server, ip, 1);
    if (r < 0)
        return r;
    memcpy(out->ip, ip[0], 4);

    int err;
    UdpSock *s = udp_open(0, &err);
    if (!s)
        return err;
    uint8_t p[64];
    uint64_t t1 = 0, t4 = 0;
    r = ERR_TIMEDOUT;
    for (int attempt = 0; attempt < 3 && r == ERR_TIMEDOUT; attempt++) {
        memset(p, 0, 48);
        p[0] = 0x23; /* keine Schaltsekunde, Version 4, Client */
        t1 = time_us();
        int w = udp_sendto(s, ip[0], 123, p, 48);
        if (w < 0) {
            r = w == ERR_HOSTUNREACH ? ERR_TIMEDOUT : w;
            continue;
        }
        uint64_t start = time_ms();
        while (time_ms() - start < 1500) {
            uint8_t from[4];
            uint16_t fport;
            int n = udp_recvfrom(s, p, sizeof(p), from, &fport, (uint32_t)(1500 - (time_ms() - start)));
            if (n == ERR_INTR) {
                r = n;
                break;
            }
            if (n < 48 || fport != 123 || !ip_eq(from, ip[0]))
                continue;
            t4 = time_us();
            r = 0;
            break;
        }
    }
    udp_close(s);
    if (r != 0)
        return r;
    uint64_t secs = be32(p + 40), frac = be32(p + 44);
    if ((p[0] & 7) != 4 || p[1] == 0 || p[1] > 15 || secs < NTP_UNIX_DIFF)
        return ERR_IO; /* keine Server-Antwort, "Kiss of Death" oder unsinnige Zeit */
    uint64_t rtt_us = t4 - t1;
    uint64_t utc_ms = (secs - NTP_UNIX_DIFF) * 1000 + ((frac * 1000) >> 32) + rtt_us / 2000;
    int tzo = tz_offset(utc_ms / 1000);
    uint64_t local_ms = utc_ms + (int64_t)tzo * 1000;
    uint64_t now = rtc_now_ms();
    out->offset_ms = now ? (int64_t)now - (int64_t)local_ms : 0;
    out->utc = utc_ms / 1000;
    out->local = local_ms / 1000;
    out->rtt_ms = (uint32_t)(rtt_us / 1000);
    out->stratum = p[1];
    out->tz_offset_s = tzo;
    tz_name(utc_ms / 1000, out->tz);
    if (set_clock && rtc_set_ms(local_ms) != 0)
        return ERR_IO;
    return 0;
}

/* Beim ersten Mal, wenn das Netz steht: Uhr im Hintergrund stellen */
static void ntp_thread(void *arg)
{
    (void)arg;
    for (int attempt = 0; attempt < 3; attempt++) {
        NtpResult r;
        int e = net_ntp(0, 1, &r);
        if (e == 0) {
            DateTime dt;
            unix_to_datetime(r.local, &dt);
            int64_t off = r.offset_ms < 0 ? -r.offset_ms : r.offset_ms;
            kprintf("ntp: Uhr gestellt: %02d.%02d.%04d %02d:%02d:%02d %s (Server %s, sie ging %ld,%03ld s %s)\n", dt.day,
                    dt.month, dt.year, dt.hour, dt.min, dt.sec, r.tz, r.server, (long)(off / 1000), (long)(off % 1000),
                    r.offset_ms > 0 ? "vor" : "nach");
            return;
        }
        thread_sleep_ms(10000);
    }
    kprintf("ntp: kein Zeitserver erreichbar, die Uhr bleibt unveraendert ('ntp' stellt sie von Hand)\n");
}

static void start_ntp(void)
{
    if (ntp_started || cmdline_has("nontp") || cmdline_has("selftest"))
        return;
    ntp_started = 1;
    thread_create("ntp", ntp_thread, 0);
}
