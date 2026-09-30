/* IPv4-Stack: DNS-Aufloesung mit Zwischenspeicher */

#include "arch/x86_64/apic.h"
#include "core/syscall.h"
#include "net/net_internal.h"

static DnsEntry dns_cache[DNS_CACHE];

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
    uint16_t id = (uint16_t)net_rnd();
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
    const char *e = net_parse_ip(name, ip);
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

    mutex_lock(&net_lock);
    uint64_t now = time_ms();
    for (int i = 0; i < DNS_CACHE; i++) {
        DnsEntry *c = &dns_cache[i];
        if (c->n && c->expires_ms > now && strcmp(c->name, n) == 0) {
            int k = c->n < max ? c->n : max;
            memcpy(out, c->ip, (size_t)k * 4);
            mutex_unlock(&net_lock);
            return k;
        }
    }
    uint8_t servers[3][4];
    int ns = 0;
    for (int i = 0; i < net_nif && ns < 3; i++) {
        Iface *f = &net_ifs[i];
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
    mutex_unlock(&net_lock);
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

    mutex_lock(&net_lock);
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
    mutex_unlock(&net_lock);
    int k = r < max ? r : max;
    memcpy(out, got, (size_t)k * 4);
    return k;
}
