/* IPv4-Stack: ICMP (Echo-Anfragen beantworten, Ping-Antworten zuordnen) */

#include "arch/x86_64/apic.h"
#include "core/syscall.h"
#include "net/net_internal.h"

/* ---------- ICMP ---------- */

void icmp_in(Iface *f, const uint8_t *iph, const uint8_t *p, uint32_t len, const uint8_t *src_mac)
{
    if (len < 8 || net_csum_fold(net_csum_add(0, p, len)) != 0)
        return;
    const uint8_t *src = iph + 12, *dst = iph + 16;
    if (p[0] == 8 && ip_eq(dst, f->ip)) { /* Echo-Anfrage: antworten */
        if (len > NET_MTU - IP_HDR)
            return;
        uint8_t *r = net_txbuf + ETH_HDR + IP_HDR;
        memcpy(r, p, len);
        r[0] = 0;
        put16(r + 2, 0);
        put16(r + 2, net_csum_fold(net_csum_add(0, r, len)));
        uint8_t to[4];
        memcpy(to, src, 4);
        net_ip_send(f, src_mac, f->ip, to, 1, len);
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
        PingSlot *s = &net_pings[i];
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
