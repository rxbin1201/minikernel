#include "libc.h"

/* date                 zeigt Datum und Uhrzeit (die Zeit der Echtzeituhr, keine Zeitzonen-Umrechnung)
 * date -s "ZEIT"       stellt die Uhr. Formate: "2026-09-29 15:04[:30]", "29.09.2026 15:04[:30]", "2026-09-29" (Uhrzeit
 *                      bleibt), "15:04[:30]" (Datum bleibt)
 * date +s              gibt Sekunden seit 1970 aus */
static const char *const weekdays[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

/* Liest bis zu drei Zahlen getrennt durch `sep`. Liefert die Anzahl. */
static int numbers(const char **sp, char sep, int *out)
{
    int n = 0;
    const char *s = *sp;
    while (n < 3 && *s >= '0' && *s <= '9') {
        int v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        out[n++] = v;
        if (*s != sep)
            break;
        s++;
    }
    *sp = s;
    return n;
}

static int parse(const char *s, DateTime *dt)
{
    while (*s == ' ')
        s++;
    int v[3];
    const char *p = s;
    /* Datum? (enthaelt '-' oder '.' vor dem ersten Leerzeichen) */
    const char *q = s;
    while (*q && *q != ' ' && *q != '-' && *q != '.' && *q != ':')
        q++;
    if (*q == '-' || *q == '.') {
        char sep = *q;
        if (numbers(&p, sep, v) != 3)
            return -1;
        if (sep == '-') {
            dt->year = v[0]; dt->month = v[1]; dt->day = v[2];
        } else {
            dt->day = v[0]; dt->month = v[1]; dt->year = v[2];
        }
        while (*p == ' ' || *p == 'T')
            p++;
    }
    if (*p) {
        int t[3] = {0, 0, 0};
        int n = numbers(&p, ':', t);
        if (n < 2 || *p)
            return -1;
        dt->hour = t[0]; dt->min = t[1]; dt->sec = n > 2 ? t[2] : 0;
    }
    if (dt->year < 2000 || dt->year > 2099 || dt->month < 1 || dt->month > 12 || dt->day < 1 || dt->day > 31 ||
        dt->hour > 23 || dt->min > 59 || dt->sec > 59)
        return -1;
    return 0;
}

void _start(int argc, char **argv)
{
    s64 now = sys_time();
    if (argc >= 2 && strcmp(argv[1], "-s") == 0) {
        if (argc < 3) {
            fprintf(2, "Aufruf: date -s \"2026-09-29 15:04:00\"\n");
            sys_exit(2);
        }
        char text[64];
        int n = 0;
        for (int i = 2; i < argc; i++) { /* ohne Anfuehrungszeichen getrennte Teile zusammensetzen */
            if (i > 2)
                text[n++] = ' ';
            for (const char *c = argv[i]; *c && n < (int)sizeof(text) - 2; c++)
                text[n++] = *c;
        }
        text[n] = 0;

        DateTime dt;
        if (now) {
            time_to_date((u64)now, &dt);
        } else {
            dt.year = 2026; dt.month = 1; dt.day = 1; dt.hour = dt.min = dt.sec = 0;
        }
        if (parse(text, &dt) != 0) {
            fprintf(2, "date: '%s' nicht verstanden (Beispiel: date -s \"2026-09-29 15:04\")\n", text);
            sys_exit(2);
        }
        if (sys_settime(date_to_time(&dt)) != 0) {
            fprintf(2, "date: Uhr konnte nicht gestellt werden\n");
            sys_exit(1);
        }
        now = sys_time();
    }
    if (!now) {
        fprintf(2, "date: die Uhr ist nicht verfuegbar (keine gueltige RTC)\n");
        sys_exit(1);
    }
    if (argc >= 2 && strcmp(argv[1], "+s") == 0) {
        printf("%lld\n", (long long)now);
        sys_exit(0);
    }
    DateTime dt;
    time_to_date((u64)now, &dt);
    int tty = sys_isatty(1) != 0;
    printf("%s%s%s %04d-%02d-%02d %s%02d:%02d:%02d%s\n", tty ? C_DIM : "", weekdays[dt.wday], tty ? C_RESET : "", dt.year, dt.month,
           dt.day, tty ? C_CYAN : "", dt.hour, dt.min, dt.sec, tty ? C_RESET : "");
    sys_exit(0);
}
