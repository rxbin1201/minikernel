/* Selbsttests: Netzwerk */

#include "lib/kprintf.h"
#include "lib/string.h"
#include "arch/x86_64/apic.h"
#include "core/sched.h"
#include "core/process.h"
#include "fs/fs.h"
#include "net/net.h"
#include "drivers/rtc.h"
#include "tests/selftest.h"

/* Netzwerk in QEMU ("-netdev user"): DHCP-Server und Gateway 10.0.2.2, DNS 10.0.2.3, Gast 10.0.2.15 */
static int net_wait_dhcp(NetInfo *ni, uint64_t timeout_ms)
{
    uint64_t t0 = time_ms();
    while (time_ms() - t0 < timeout_ms) {
        if (net_info(0, ni) == 0 && ni->dhcp == 2)
            return 1;
        thread_sleep_ms(50);
    }
    return 0;
}

void test_net(void)
{
    title("Netzwerk");
    NetInfo ni;
    if (net_info(0, &ni) != 0) {
        kprintf("  (keine Netzwerkkarte - uebersprungen; QEMU mit NET=e1000 oder NET=e1000e starten)\n");
        return;
    }
    check("Netzwerkkarte gefunden, MAC-Adresse gelesen", (ni.mac[0] | ni.mac[1] | ni.mac[2] | ni.mac[3] | ni.mac[4] | ni.mac[5]) != 0);
    int bound = net_wait_dhcp(&ni, 10000);
    check("Verbindung steht", ni.link);
    static const uint8_t want_ip[4] = {10, 0, 2, 15}, want_mask[4] = {255, 255, 255, 0};
    static const uint8_t gw[4] = {10, 0, 2, 2}, dns[4] = {10, 0, 2, 3};
    check("DHCP: 10.0.2.15/24, Gateway 10.0.2.2, DNS 10.0.2.3", bound && memcmp(ni.ip, want_ip, 4) == 0 &&
          memcmp(ni.mask, want_mask, 4) == 0 && memcmp(ni.gateway, gw, 4) == 0 && memcmp(ni.dns, dns, 4) == 0);

    int64_t r = net_ping(gw, 1, 56, 2000);
    check("ping 10.0.2.2 antwortet", r >= 0);
    if (r >= 0)
        kprintf("  (Antwortzeit %lu us)\n", (unsigned long)(r & 0xFFFFFFFFFFLL));
    ArpInfo ai;
    int arp_gw = 0;
    for (unsigned i = 0; net_arp_info(i, &ai) == 0; i++)
        if (memcmp(ai.ip, gw, 4) == 0)
            arp_gw = 1;
    check("ARP-Tabelle kennt das Gateway", arp_gw);
    static const uint8_t nobody[4] = {10, 0, 2, 99};
    check("Nicht vorhandener Rechner: 'nicht erreichbar' (ARP ohne Antwort)", net_ping(nobody, 1, 56, 600) == ERR_HOSTUNREACH);
    static const uint8_t far[4] = {192, 0, 2, 1};
    uint8_t cfg[16] = {10, 0, 2, 20, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    net_set_static(0, cfg); /* ohne Gateway: fremde Netze unerreichbar */
    check("Feste Adresse ohne Gateway: fremdes Netz unerreichbar, lokales Netz geht",
          net_ping(far, 1, 56, 500) == ERR_NETUNREACH && net_ping(gw, 2, 56, 2000) >= 0);
    net_start_dhcp(0);
    check("DHCP erneut: wieder 10.0.2.15", net_wait_dhcp(&ni, 10000) && memcmp(ni.ip, want_ip, 4) == 0);

    if (fs_disk_volume() < 0)
        return;
    write_text("/disk/NET.SH", "lspci > /disk/N1.TXT\nifconfig -a > /disk/N2.TXT\nping -c 2 -s 1000 10.0.2.2 > /disk/N3.TXT\n");
    int rc = run_sh("sh /disk/NET.SH");
    check("lspci zeigt die Netzwerkkarte mit Treiber", rc == 0 && file_has("/disk/N1.TXT", "Netzwerk (Ethernet)") &&
                                                       file_has("/disk/N1.TXT", "[e1000]"));
    check("ifconfig zeigt Verbindung, Adresse, DHCP und ARP-Tabelle",
          file_has("/disk/N2.TXT", "1000 Mbit/s") && file_has("/disk/N2.TXT", "10.0.2.15/24  Gateway 10.0.2.2  DNS 10.0.2.3") &&
          file_has("/disk/N2.TXT", "DHCP:       von 10.0.2.2") && file_has("/disk/N2.TXT", "  10.0.2.2 "));
    check("ping-Programm (1000 Byte): 2 Antworten", file_has("/disk/N3.TXT", "2 gesendet, 2 empfangen, 0 % Verlust"));
    fs_unlink("/disk/NET.SH");
    fs_unlink("/disk/N1.TXT");
    fs_unlink("/disk/N2.TXT");
    fs_unlink("/disk/N3.TXT");

    /* UDP-Sockets (ueber die eigene Adresse, ohne Netz) */
    int err = 0;
    UdpSock *a = udp_open(7000, &err), *b = udp_open(0, &err);
    UdpSock *dup = udp_open(7000, &err);
    check("UDP: Socket auf Port 7000, zweiter auf demselben Port abgelehnt", a && b && !dup && err == ERR_EXIST);
    static const uint8_t lo[4] = {127, 0, 0, 1};
    uint8_t from[4], rb[64];
    uint16_t fport = 0;
    int sent = a && b ? udp_sendto(b, lo, 7000, "hallo", 5) : -1;
    int got = a ? udp_recvfrom(a, rb, sizeof(rb), from, &fport, 100) : -1;
    check("UDP: Datagramm an 127.0.0.1 kommt mit Absender-Port an", sent == 5 && got == 5 && memcmp(rb, "hallo", 5) == 0 &&
                                                                    b && fport == udp_local_port(b));
    check("UDP: leere Warteschlange -> ERR_AGAIN bzw. ERR_TIMEDOUT",
          a && udp_recvfrom(a, rb, sizeof(rb), 0, 0, 0) == ERR_AGAIN && udp_recvfrom(a, rb, sizeof(rb), 0, 0, 50) == ERR_TIMEDOUT);
    udp_close(b);

    /* Programm "udp send" gegen den Kernel-Socket als Echo-Server */
    write_text("/disk/NET.SH", "udp send 127.0.0.1 7000 hallo welt > /disk/N4.TXT\nnslookup localhost > /disk/N5.TXT\n");
    int pid = process_spawn("/bin/sh", "sh /disk/NET.SH", 0);
    got = a ? udp_recvfrom(a, rb, sizeof(rb) - 1, from, &fport, 5000) : -1;
    if (got > 0) {
        uint8_t echo[80];
        memcpy(echo, "echo: ", 6);
        memcpy(echo + 6, rb, (size_t)got);
        udp_sendto(a, from, fport, echo, (uint32_t)got + 6);
    }
    int code = -1, faulted = 0;
    if (pid > 0)
        process_wait(pid, 0, &code, &faulted, 20000);
    check("udp-Programm: senden, Antwort empfangen", got == 11 && code == 0 && file_has("/disk/N4.TXT", "11 Bytes an 127.0.0.1:7000") &&
                                                    file_has("/disk/N4.TXT", "echo: hallo welt"));
    check("nslookup localhost", file_has("/disk/N5.TXT", "Adresse: 127.0.0.1"));
    udp_close(a);
    fs_unlink("/disk/NET.SH");
    fs_unlink("/disk/N4.TXT");
    fs_unlink("/disk/N5.TXT");

    /* DNS ohne Netz */
    uint8_t ips[8][4];
    check("DNS: IP-Adresse und localhost ohne Anfrage", net_resolve("10.1.2.3", ips, 8) == 1 && ips[0][0] == 10 && ips[0][3] == 3 &&
                                                      net_resolve("LocalHost", ips, 8) == 1 && ips[0][0] == 127);
    check("DNS: ungueltige Namen abgelehnt", net_resolve("a..b", ips, 8) == ERR_INVAL && net_resolve("", ips, 8) == ERR_INVAL);

    /* Echtes DNS und NTP ueber QEMU (braucht Internet auf dem Host) */
    uint64_t t0 = time_ms();
    int n = net_resolve("google.de", ips, 8);
    if (n == ERR_TIMEDOUT || n == ERR_NETUNREACH || n == ERR_HOSTUNREACH) {
        kprintf("  (kein Internet: DNS/NTP ueber das Internet uebersprungen)\n");
        return;
    }
    uint64_t t1 = time_ms();
    int n2 = net_resolve("google.de", ips, 8);
    uint64_t t2 = time_ms();
    kprintf("  (google.de -> %u.%u.%u.%u, %lu ms; aus dem Zwischenspeicher %lu ms)\n", ips[0][0], ips[0][1], ips[0][2],
            ips[0][3], (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
    check("DNS: google.de aufgeloest, zweites Mal aus dem Zwischenspeicher", n > 0 && n2 == n && t2 - t1 < 5);
    check("DNS: unbekannter Name -> ERR_NOENT", net_resolve("gibt-es-nicht.invalid", ips, 8) == ERR_NOENT);
    NtpResult nr;
    int e = net_ntp(0, 0, &nr);
    if (e == 0)
        kprintf("  (NTP %s, Ebene %u, Laufzeit %u ms, Uhr weicht %ld ms ab)\n", nr.server, nr.stratum, nr.rtt_ms,
                (long)nr.offset_ms);
    check("NTP: Zeit von pool.ntp.org plausibel (nach 2026, Ebene 1-15)", e == 0 && nr.utc > 1767225600ULL &&
                                                                         nr.stratum >= 1 && nr.stratum <= 15);
}

/* Nur mit TESTS=netpeer und dem Python-Gegenrechner (scratchpad/peer.py) als DNS-, NTP- und UDP-Echo-Server auf 10.0.5.1 */
void test_netpeer(void)
{
    title("Netzwerk gegen Test-Gegenstelle");
    NetInfo ni;
    uint64_t t0 = time_ms();
    while (time_ms() - t0 < 5000 && (net_info(0, &ni) != 0 || !ni.link))
        thread_sleep_ms(20);
    uint8_t ips[8][4];
    int n = net_resolve("alias.minikernel", ips, 8);
    check("DNS: CNAME-Kette, zwei A-Records", n == 2 && ips[0][0] == 10 && ips[0][1] == 1 && ips[0][2] == 2 && ips[0][3] == 3 &&
                                              ips[1][3] == 4);
    check("DNS: NXDOMAIN -> ERR_NOENT", net_resolve("nx.minikernel", ips, 8) == ERR_NOENT);
    check("DNS: Antwort mit Fehlercode -> ERR_IO", net_resolve("servfail.minikernel", ips, 8) == ERR_IO);
    check("DNS: Server antwortet nicht -> ERR_TIMEDOUT", net_resolve("stumm.minikernel", ips, 8) == ERR_TIMEDOUT);
    NtpResult nr;
    int e = net_ntp("10.0.5.1", 0, &nr);
    DateTime dt;
    unix_to_datetime(nr.local, &dt);
    /* Gegenstelle liefert 2030-06-15 12:00:00 UTC: Sommerzeit, also 14:00 MESZ */
    check("NTP: Zeit, Sommerzeit (MESZ = UTC+2), Ebene", e == 0 && nr.utc == 1907755200ULL && nr.tz_offset_s == 7200 &&
                                                        dt.hour == 14 && strcmp(nr.tz, "MESZ") == 0 && nr.stratum == 2);
    e = net_ntp("winter.minikernel", 0, &nr); /* 2030-01-15 12:00:00 UTC */
    unix_to_datetime(nr.local, &dt);
    check("NTP: Winterzeit (MEZ = UTC+1), Server per Name", e == 0 && nr.utc == 1894708800ULL && dt.hour == 13 &&
                                                           strcmp(nr.tz, "MEZ") == 0);
    check("NTP: ungueltige Antwort (Ebene 0) -> ERR_IO", net_ntp("kod.minikernel", 0, &nr) == ERR_IO);
    int err;
    UdpSock *s = udp_open(0, &err);
    static const uint8_t peer[4] = {10, 0, 5, 1};
    uint8_t buf[1500], big[1400], from[4];
    uint16_t fport;
    for (int i = 0; i < 1400; i++)
        big[i] = (uint8_t)(i * 13);
    int ok = s && udp_sendto(s, peer, 7, big, 1400) == 1400 && udp_recvfrom(s, buf, sizeof(buf), from, &fport, 2000) == 1400 &&
             memcmp(buf, big, 1400) == 0 && fport == 7;
    check("UDP: 1400 Byte an den Echo-Server und zurueck", ok);
    udp_close(s);
}
