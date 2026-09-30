/* IPv4-Stack: Uhrzeit per NTP */

#include "arch/x86_64/apic.h"
#include "core/cmdline.h"
#include "lib/kprintf.h"
#include "drivers/rtc.h"
#include "core/syscall.h"
#include "net/net_internal.h"

static int      ntp_started;

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
        mutex_lock(&net_lock);
        for (int i = 0; i < net_nif; i++)
            if (!ip_zero(net_ifs[i].ntp)) {
                const uint8_t *a = net_ifs[i].ntp;
                ksnprintf(out->server, sizeof(out->server), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
                break;
            }
        mutex_unlock(&net_lock);
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

void ntp_start(void)
{
    if (ntp_started || cmdline_has("nontp") || cmdline_has("selftest"))
        return;
    ntp_started = 1;
    thread_create("ntp", ntp_thread, 0);
}
