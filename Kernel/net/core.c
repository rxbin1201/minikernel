/* IPv4-Stack: Schnittstellen, Senden und Empfangen, Routing, Netz-Thread, oeffentliche Funktionen */

#include "arch/x86_64/apic.h"
#include "core/cmdline.h"
#include "lib/kprintf.h"
#include "core/process.h"
#include "net/net_internal.h"

Iface    net_ifs[NET_MAX_DEVICES];

int      net_nif;

ArpEnt   arp_table[ARP_SIZE];

PingSlot net_pings[PINGS];

Mutex    net_lock = MUTEX_INIT;

static uint8_t  net_rxbuf[NET_FRAME_MAX + 64];

uint8_t  net_txbuf[NET_FRAME_MAX + 64];

static uint16_t ip_ident;

static uint64_t rnd_state;

/* ---------- Hilfen ---------- */

uint32_t net_rnd(void)
{
    if (!rnd_state)
        rnd_state = time_us() | 1;
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return (uint32_t)rnd_state;
}

uint32_t net_csum_add(uint32_t sum, const uint8_t *p, uint32_t len)
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

uint16_t net_csum_fold(uint32_t sum)
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

int net_prefix_len(const uint8_t *m)
{
    int n = 0;
    for (int i = 0; i < 4; i++)
        for (int b = 7; b >= 0; b--)
            if (m[i] & (1 << b))
                n++;
    return n;
}

int net_interrupted(void)
{
    Process *p = process_current();
    return p && process_killed(p);
}

/* ---------- Senden ---------- */

int net_dev_send(Iface *f, uint8_t *frame, uint32_t len)
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

/* Sendet net_txbuf mit IPv4-Kopf; die Nutzdaten (len Bytes) stehen schon ab net_txbuf + ETH_HDR + IP_HDR */
int net_ip_send(Iface *f, const uint8_t dst_mac[6], const uint8_t src[4], const uint8_t dst[4], uint8_t proto,
                   uint32_t len)
{
    uint8_t *e = net_txbuf, *h = net_txbuf + ETH_HDR;
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
    put16(h + 10, net_csum_fold(net_csum_add(0, h, IP_HDR)));
    return net_dev_send(f, net_txbuf, ETH_HDR + IP_HDR + len);
}

/* UDP: Daten (len Bytes) stehen ab net_txbuf + ETH_HDR + IP_HDR + UDP_HDR */
int udp_send(Iface *f, const uint8_t dst_mac[6], const uint8_t src[4], const uint8_t dst[4], uint16_t sport,
                    uint16_t dport, uint32_t len)
{
    uint8_t *u = net_txbuf + ETH_HDR + IP_HDR;
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
    uint16_t c = net_csum_fold(net_csum_add(net_csum_add(0, pseudo, 12), u, UDP_HDR + len));
    put16(u + 6, c ? c : 0xFFFF);
    return net_ip_send(f, dst_mac, src, dst, 17, UDP_HDR + len);
}

/* ---------- Warten (mit gehaltener Sperre) ---------- */

/* Sperre kurz freigeben und warten: anfangs nur den Prozessor abgeben (genaue Zeiten), spaeter schlafen */
void net_wait_step(uint64_t started_ms)
{
    mutex_unlock(&net_lock);
    if (time_ms() - started_ms < 20)
        thread_yield();
    else
        thread_sleep_ms(10);
    mutex_lock(&net_lock);
}

int arp_resolve(Iface *f, const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms)
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
            net_poll_locked();
            if (arp_lookup(ip, mac))
                return 0;
            if (net_interrupted())
                return ERR_INTR;
            net_wait_step(start);
        }
    }
    return ERR_HOSTUNREACH;
}

/* Adresse dieses Rechners (eine eigene oder 127.x.x.x) */
int net_is_local(const uint8_t ip[4])
{
    if (ip[0] == 127)
        return 1;
    for (int i = 0; i < net_nif; i++)
        if (!ip_zero(net_ifs[i].ip) && ip_eq(net_ifs[i].ip, ip))
            return 1;
    return 0;
}

/* Interface und naechste Station (Ziel selbst oder Gateway) fuer eine Zieladresse */
Iface *net_route(const uint8_t dst[4], uint8_t next[4])
{
    for (int i = 0; i < net_nif; i++) {
        Iface *f = &net_ifs[i];
        if (f->link && !ip_zero(f->ip) && same_subnet(f, dst)) {
            memcpy(next, dst, 4);
            return f;
        }
    }
    for (int i = 0; i < net_nif; i++) {
        Iface *f = &net_ifs[i];
        if (f->link && !ip_zero(f->ip) && !ip_zero(f->gw)) {
            memcpy(next, f->gw, 4);
            return f;
        }
    }
    return 0;
}

/* ---------- Empfang ---------- */

static void ip_in(Iface *f, const uint8_t *h, uint32_t len, const uint8_t *src_mac)
{
    if (len < IP_HDR || (h[0] >> 4) != 4)
        return;
    uint32_t ihl = (h[0] & 15) * 4u, tot = be16(h + 2);
    if (ihl < IP_HDR || tot < ihl || tot > len || net_csum_fold(net_csum_add(0, h, ihl)) != 0) {
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
        arp_update(h + 12, src_mac, (int)(f - net_ifs), 0);
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
int net_poll_locked(void)
{
    int total = 0;
    for (int i = 0; i < net_nif; i++) {
        Iface *f = &net_ifs[i];
        for (int k = 0; k < 64; k++) {
            int len = f->dev.recv(&f->dev, net_rxbuf, NET_FRAME_MAX);
            if (len <= 0)
                break;
            frame_in(f, net_rxbuf, (uint32_t)len);
            total++;
        }
    }
    return total;
}

static void periodic_locked(uint64_t now)
{
    for (int i = 0; i < net_nif; i++) {
        Iface *f = &net_ifs[i];
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
                        ntp_start();
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
        mutex_lock(&net_lock);
        int n = net_poll_locked();
        uint64_t now = time_ms();
        if (now - last >= 100) {
            periodic_locked(now);
            last = now;
        }
        mutex_unlock(&net_lock);
        if (n)
            thread_yield();
        else
            thread_sleep_ms(10);
    }
}

/* ---------- Oeffentliche Funktionen ---------- */

/* "a.b.c.d" am Anfang von s; liefert das Ende oder NULL */
const char *net_parse_ip(const char *s, uint8_t out[4])
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
    const char *s = net_parse_ip(v, ip);
    if (s && *s == '/') {
        prefix = 0;
        for (s++; *s >= '0' && *s <= '9'; s++)
            prefix = prefix * 10 + (*s - '0');
    }
    if (s && *s == ',')
        s = net_parse_ip(s + 1, gw);
    if (s && *s == ',')
        s = net_parse_ip(s + 1, dns);
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
    if (net_nif >= NET_MAX_DEVICES)
        return -1;
    Iface *f = &net_ifs[net_nif];
    memset(f, 0, sizeof(*f));
    f->dev = *d;
    ksnprintf(f->dev.name, sizeof(f->dev.name), "eth%d", net_nif);
    f->want_dhcp = !cmdline_has("nodhcp") && !(net_nif == 0 && static_from_cmdline(f));
    net_nif++;
    return 0;
}

void net_init(void)
{
    if (cmdline_has("nonet")) {
        kprintf("net: abgeschaltet (nonet)\n");
        return;
    }
    e1000_probe();
    if (!net_nif) {
        kprintf("net: keine unterstuetzte Netzwerkkarte gefunden\n");
        return;
    }
    thread_create("net", net_thread, 0);
}

int net_info(unsigned index, NetInfo *out)
{
    if (index >= (unsigned)net_nif)
        return -1;
    mutex_lock(&net_lock);
    Iface *f = &net_ifs[index];
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
    mutex_unlock(&net_lock);
    return 0;
}

int net_set_static(unsigned index, const uint8_t cfg[16])
{
    if (index >= (unsigned)net_nif)
        return ERR_NOENT;
    mutex_lock(&net_lock);
    Iface *f = &net_ifs[index];
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
            ntp_start();
    }
    mutex_unlock(&net_lock);
    return 0;
}

int net_start_dhcp(unsigned index)
{
    if (index >= (unsigned)net_nif)
        return ERR_NOENT;
    mutex_lock(&net_lock);
    Iface *f = &net_ifs[index];
    f->want_dhcp = 1;
    memset(f->ip, 0, 4);
    memset(f->gw, 0, 4);
    memset(f->dns, 0, 4);
    if (f->link)
        dhcp_start(f);
    else
        f->dstate = DS_IDLE;
    mutex_unlock(&net_lock);
    return 0;
}

int net_arp_info(unsigned index, ArpInfo *out)
{
    mutex_lock(&net_lock);
    unsigned seen = 0;
    int r = -1;
    for (int i = 0; i < ARP_SIZE; i++) {
        if (!arp_table[i].valid || seen++ != index)
            continue;
        memset(out, 0, sizeof(*out));
        memcpy(out->ip, arp_table[i].ip, 4);
        memcpy(out->mac, arp_table[i].mac, 6);
        out->age_s = (uint32_t)((time_ms() - arp_table[i].ms) / 1000);
        memcpy(out->dev, net_ifs[arp_table[i].iface].dev.name, sizeof(out->dev));
        r = 0;
        break;
    }
    mutex_unlock(&net_lock);
    return r;
}

int64_t net_ping(const uint8_t ip[4], uint16_t seq, uint32_t size, uint32_t timeout_ms)
{
    if (!net_nif)
        return ERR_NETUNREACH;
    if (size > NET_MTU - IP_HDR - 8)
        size = NET_MTU - IP_HDR - 8;
    if (timeout_ms < 10)
        timeout_ms = 10;
    mutex_lock(&net_lock);
    if (net_is_local(ip)) { /* eigene Adresse oder 127.x.x.x */
        mutex_unlock(&net_lock);
        return 1 | (64LL << 40);
    }
    uint8_t next[4], mac[6];
    Iface *f = net_route(ip, next);
    if (!f) {
        mutex_unlock(&net_lock);
        return ERR_NETUNREACH;
    }
    int r = arp_resolve(f, next, mac, timeout_ms < 3000 ? timeout_ms : 3000);
    if (r != 0) {
        mutex_unlock(&net_lock);
        return r;
    }
    PingSlot *s = 0;
    int slot = 0;
    for (; slot < PINGS; slot++)
        if (!net_pings[slot].used) {
            s = &net_pings[slot];
            break;
        }
    if (!s) {
        mutex_unlock(&net_lock);
        return ERR_AGAIN;
    }
    s->used = 1;
    s->done = 0;
    s->id = (uint16_t)(0x4D00 + slot);
    s->seq = seq;

    uint8_t *m = net_txbuf + ETH_HDR + IP_HDR;
    m[0] = 8;
    m[1] = 0;
    put16(m + 2, 0);
    put16(m + 4, s->id);
    put16(m + 6, seq);
    for (uint32_t i = 0; i < size; i++)
        m[8 + i] = (uint8_t)(0x20 + i % 64);
    put16(m + 2, net_csum_fold(net_csum_add(0, m, 8 + size)));
    uint8_t dst[4];
    memcpy(dst, ip, 4);
    s->sent_us = time_us();
    int64_t result = ERR_TIMEDOUT;
    if (net_ip_send(f, mac, f->ip, dst, 1, 8 + size) != 0) {
        result = ERR_IO;
    } else {
        uint64_t start = time_ms();
        while (time_ms() - start < timeout_ms) {
            net_poll_locked();
            if (s->done) {
                result = s->result;
                break;
            }
            if (net_interrupted()) {
                result = ERR_INTR;
                break;
            }
            net_wait_step(start);
        }
    }
    s->used = 0;
    mutex_unlock(&net_lock);
    return result;
}
