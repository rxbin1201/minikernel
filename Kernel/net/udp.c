/* IPv4-Stack: UDP-Sockets */

#include "arch/x86_64/apic.h"
#include "mm/heap.h"
#include "core/syscall.h"
#include "net/net_internal.h"

static UdpSock  udp_socks[UDP_SOCKS];

static uint16_t next_port = 49152;

/* ---------- UDP: Empfang ---------- */

/* Datagramm in die Warteschlange des Sockets mit diesem Port legen; 0 = kein Socket oder Warteschlange voll */
static int udp_deliver(const uint8_t src[4], uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len)
{
    for (int i = 0; i < UDP_SOCKS; i++) {
        UdpSock *s = &udp_socks[i];
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

void udp_in(Iface *f, const uint8_t *iph, const uint8_t *u, uint32_t len)
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
        if (net_csum_fold(net_csum_add(net_csum_add(0, pseudo, 12), u, ulen)) != 0) {
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

/* ---------- UDP-Sockets ---------- */

UdpSock *udp_open(uint16_t port, int *err)
{
    Dgram *q = kcalloc(UDP_QUEUE, sizeof(Dgram));
    if (!q) {
        *err = ERR_NOMEM;
        return 0;
    }
    mutex_lock(&net_lock);
    UdpSock *s = 0;
    for (int i = 0; i < UDP_SOCKS && !s; i++)
        if (!udp_socks[i].used)
            s = &udp_socks[i];
    int in_use = port == 68;
    for (int i = 0; i < UDP_SOCKS; i++)
        if (port && udp_socks[i].used && udp_socks[i].port == port)
            in_use = 1;
    if (!s || in_use) {
        mutex_unlock(&net_lock);
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
                if (udp_socks[i].used && udp_socks[i].port == p)
                    used = 1;
            if (!used)
                port = p;
        }
    }
    s->used = 1;
    s->port = port;
    s->q = q;
    s->head = s->count = 0;
    mutex_unlock(&net_lock);
    return s;
}

void udp_close(UdpSock *s)
{
    if (!s)
        return;
    mutex_lock(&net_lock);
    Dgram *q = s->q;
    s->used = 0;
    s->q = 0;
    s->count = 0;
    mutex_unlock(&net_lock);
    kfree(q);
}

uint16_t udp_local_port(const UdpSock *s)
{
    return s->port;
}

int udp_pending(UdpSock *s)
{
    mutex_lock(&net_lock);
    net_poll_locked();
    int n = s->count;
    mutex_unlock(&net_lock);
    return n;
}

int udp_sendto(UdpSock *s, const uint8_t ip[4], uint16_t port, const void *data, uint32_t len)
{
    if (len > UDP_MAX_PAYLOAD || !port)
        return ERR_INVAL;
    mutex_lock(&net_lock);
    int r;
    if (net_is_local(ip)) { /* an sich selbst: direkt zustellen */
        static const uint8_t lo[4] = {127, 0, 0, 1};
        r = udp_deliver(lo, s->port, port, data, len) ? (int)len : ERR_AGAIN;
        mutex_unlock(&net_lock);
        return r;
    }
    Iface *f = 0;
    uint8_t next[4], mac[6];
    if (ip_eq(ip, BCAST_IP)) {
        for (int i = 0; i < net_nif && !f; i++)
            if (net_ifs[i].link)
                f = &net_ifs[i];
        memcpy(mac, BCAST_MAC, 6);
        r = f ? 0 : ERR_NETUNREACH;
    } else {
        f = net_route(ip, next);
        r = f ? arp_resolve(f, next, mac, 3000) : ERR_NETUNREACH;
    }
    if (r == 0) {
        uint8_t dst[4];
        memcpy(dst, ip, 4);
        memcpy(net_txbuf + ETH_HDR + IP_HDR + UDP_HDR, data, len);
        r = udp_send(f, mac, f->ip, dst, s->port, port, len) == 0 ? (int)len : ERR_IO;
    }
    mutex_unlock(&net_lock);
    return r;
}

int udp_recvfrom(UdpSock *s, void *buf, uint32_t max, uint8_t ip[4], uint16_t *port, uint32_t timeout_ms)
{
    mutex_lock(&net_lock);
    uint64_t start = time_ms();
    int r;
    for (;;) {
        if (!s->count)
            net_poll_locked();
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
        if (net_interrupted()) {
            r = ERR_INTR;
            break;
        }
        net_wait_step(start);
    }
    mutex_unlock(&net_lock);
    return r;
}
