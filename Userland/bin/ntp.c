#include "libc.h"

/* ntp: Uhrzeit von einem Zeitserver holen und die Uhr (RTC) stellen.
 *   ntp               Server vom Router (DHCP) oder pool.ntp.org
 *   ntp de.pool.ntp.org
 *   ntp -n [server]   nur anzeigen, die Uhr nicht stellen
 * Zeitserver liefern UTC; die RTC enthaelt Ortszeit. Die Zeitzone stellt man in cmdline.txt ein:
 * tz=eu (Standard, MEZ/MESZ), tz=uk, tz=utc, tz=+2, tz=-5 */

static const char *const wdays[7] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

void _start(int argc, char **argv)
{
    int set = 1;
    const char *server = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0)
            set = 0;
        else if (argv[i][0] != '-' && !server)
            server = argv[i];
        else {
            fprintf(2, "Aufruf: ntp [-n] [server]\n");
            sys_exit(2);
        }
    }
    int tty = sys_isatty(1) != 0;
    NtpResult r;
    s64 e = sys_ntp(server, &r, set);
    if (e < 0) {
        printf("%sntp: %s: %s%s\n", tty ? C_RED : "", r.server[0] ? r.server : (server ? server : "Zeitserver"),
               net_strerror(e), tty ? C_RESET : "");
        sys_exit(1);
    }
    DateTime dt;
    time_to_date(r.local, &dt);
    int tzh = r.tz_offset_s / 3600, tzm = (r.tz_offset_s < 0 ? -r.tz_offset_s : r.tz_offset_s) % 3600 / 60;
    printf("Server:     %s (%u.%u.%u.%u), Ebene %u, Laufzeit %u ms\n", r.server, r.ip[0], r.ip[1], r.ip[2], r.ip[3],
           r.stratum, r.rtt_ms);
    printf("Zeit:       %s%s %02d.%02d.%04d %02d:%02d:%02d %s%s (UTC%s%d", tty ? C_WHITE : "", wdays[dt.wday], dt.day,
           dt.month, dt.year, dt.hour, dt.min, dt.sec, r.tz, tty ? C_RESET : "", r.tz_offset_s < 0 ? "-" : "+",
           tzh < 0 ? -tzh : tzh);
    if (tzm)
        printf(":%02d", tzm);
    printf(")\n");
    s64 off = r.offset_ms < 0 ? -r.offset_ms : r.offset_ms;
    char ob[32];
    if (off < 1000)
        snprintf(ob, sizeof(ob), "%lld ms", off);
    else
        snprintf(ob, sizeof(ob), "%lld,%03lld s", off / 1000, off % 1000);
    int big = tty && off >= 2000;
    printf("Abweichung: %s%s%s%s%s\n", big ? C_YELLOW : "", ob, big ? C_RESET : "",
           !off ? "" : r.offset_ms > 0 ? " (Uhr ging vor)" : " (Uhr ging nach)",
           set ? " -> Uhr gestellt" : " -> nicht gestellt (-n)");
    sys_exit(0);
}
