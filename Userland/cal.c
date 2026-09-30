#include "util.h"

/* cal [monat [jahr]] | cal jahr: Kalender (Wochen beginnen am Montag, KW = Kalenderwoche nach ISO 8601).
 * Der heutige Tag ist markiert. */
static const char *const months[] = {"Januar", "Februar", "Maerz", "April", "Mai", "Juni", "Juli", "August",
                                     "September", "Oktober", "November", "Dezember"};

static int days_in(int m, int y)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

/* Wochentag (0 = Montag) */
static int weekday(int y, int m, int d)
{
    DateTime dt = {y, m, d, 12, 0, 0, 0};
    DateTime back;
    time_to_date(date_to_time(&dt), &back);
    return (back.wday + 6) % 7;
}

/* ISO-Kalenderwoche */
static int iso_week(int y, int m, int d)
{
    DateTime dt = {y, m, d, 12, 0, 0, 0};
    u64 t = date_to_time(&dt);
    int wd = weekday(y, m, d);
    DateTime thu; /* der Donnerstag derselben Woche bestimmt, zu welchem Jahr die Woche gehoert */
    time_to_date((u64)((s64)t + (s64)(3 - wd) * 86400), &thu);
    DateTime jan1 = {thu.year, 1, 1, 12, 0, 0, 0};
    s64 diff = ((s64)date_to_time(&thu) - (s64)date_to_time(&jan1)) / 86400;
    return (int)(diff / 7) + 1;
}

static int today_y, today_m, today_d, tty;

static void month(int m, int y)
{
    char title[40];
    snprintf(title, sizeof(title), "%s %d", months[m - 1], y);
    int pad = (27 - (int)strlen(title)) / 2;
    out_printf("%*s%s%s%s\n", pad > 0 ? pad : 0, "", tty ? C_BOLD : "", title, tty ? C_RESET : "");
    out_printf("%sKW  Mo Di Mi Do Fr Sa So%s\n", tty ? C_DIM : "", tty ? C_RESET : "");
    int first = weekday(y, m, 1), n = days_in(m, y), day = 1;
    while (day <= n) {
        int kw_day = day;
        out_printf("%s%2d%s  ", tty ? C_DIM : "", iso_week(y, m, kw_day), tty ? C_RESET : "");
        for (int wd = 0; wd < 7; wd++) {
            if ((day == 1 && wd < first) || day > n) {
                out_str("   ");
                continue;
            }
            int is_today = y == today_y && m == today_m && day == today_d;
            if (is_today && tty)
                out_printf("\x1b[7m%2d\x1b[0m ", day);
            else if (wd >= 5 && tty)
                out_printf("%s%2d%s ", C_CYAN, day, C_RESET);
            else
                out_printf("%2d ", day);
            day++;
        }
        out_write("\n", 1);
    }
}

void _start(int argc, char **argv)
{
    tty = sys_isatty(1) != 0;
    s64 now = sys_time();
    DateTime dt = {2026, 1, 1, 0, 0, 0, 0};
    if (now > 0)
        time_to_date((u64)now, &dt);
    today_y = dt.year;
    today_m = dt.month;
    today_d = dt.day;
    int m = dt.month, y = dt.year;
    if (argc == 2) {
        int v = atoi(argv[1]);
        if (v > 12) { /* ganzes Jahr */
            for (int k = 1; k <= 12; k++) {
                month(k, v);
                out_write("\n", 1);
            }
            out_flush();
            sys_exit(0);
        }
        m = v;
    } else if (argc >= 3) {
        m = atoi(argv[1]);
        y = atoi(argv[2]);
    }
    if (m < 1 || m > 12 || y < 1970 || y > 2100) {
        fprintf(2, "Aufruf: cal [monat [jahr]] oder cal jahr (1970..2100)\n");
        sys_exit(2);
    }
    month(m, y);
    out_flush();
    sys_exit(0);
}
