/* IPv4-Stack: DHCP-Client */

#include "arch/x86_64/apic.h"
#include "lib/kprintf.h"
#include "net/net_internal.h"

static const uint8_t ZERO_IP[4] = {0, 0, 0, 0};

/* ---------- DHCP ---------- */

#define DHCP_DISCOVER 1

#define DHCP_OFFER    2

#define DHCP_REQUEST  3

#define DHCP_ACK      5

#define DHCP_NAK      6

static void dhcp_send(Iface *f, int type)
{
    uint8_t *b = net_txbuf + ETH_HDR + IP_HDR + UDP_HDR;
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

void dhcp_start(Iface *f)
{
    f->dstate = DS_SELECTING;
    f->xid = net_rnd();
    f->tries = 0;
    f->start_ms = time_ms();
    f->next_ms = f->start_ms + 2000;
    dhcp_send(f, DHCP_DISCOVER);
}

void dhcp_in(Iface *f, const uint8_t *b, uint32_t len)
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
                    f->dev.name, f->ip[0], f->ip[1], f->ip[2], f->ip[3], net_prefix_len(f->mask), f->gw[0], f->gw[1], f->gw[2],
                    f->gw[3], f->dns[0], f->dns[1], f->dns[2], f->dns[3], f->server[0], f->server[1], f->server[2],
                    f->server[3]);
        static const uint8_t zero_mac[6] = {0};
        arp_send(f, 1, BCAST_MAC, zero_mac, f->ip); /* "gratuitous ARP": die neue Adresse bekannt machen */
        ntp_start();
    } else if (type == DHCP_NAK && f->dstate != DS_SELECTING) {
        kprintf("net: %s: DHCP-Server lehnt die Adresse ab, fange neu an\n", f->dev.name);
        memset(f->ip, 0, 4);
        dhcp_start(f);
    }
}

void dhcp_timer(Iface *f, uint64_t now)
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
            f->xid = net_rnd();
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
