#include "drivers/rtc.h"
#include "arch/x86_64/apic.h"
#include "core/cmdline.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SEC    0x00
#define REG_MIN    0x02
#define REG_HOUR   0x04
#define REG_DAY    0x07
#define REG_MONTH  0x08
#define REG_YEAR   0x09
#define REG_A      0x0A
#define REG_B      0x0B
#define REG_CENT   0x32

static uint64_t base_unix, base_ms;
static int      valid;

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t v)
{
    outb(CMOS_INDEX, reg);
    outb(CMOS_DATA, v);
}

/* ---------- Kalender ---------- */

/* Tage seit 1970-01-01 (proleptischer gregorianischer Kalender, nach H. Hinnant) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

uint64_t datetime_to_unix(const DateTime *dt)
{
    int64_t days = days_from_civil(dt->year, (unsigned)dt->month, (unsigned)dt->day);
    int64_t t = days * 86400 + dt->hour * 3600 + dt->min * 60 + dt->sec;
    return t < 0 ? 0 : (uint64_t)t;
}

void unix_to_datetime(uint64_t t, DateTime *dt)
{
    int64_t days = (int64_t)(t / 86400);
    uint32_t rem = (uint32_t)(t % 86400);
    civil_from_days(days, &dt->year, &dt->month, &dt->day);
    dt->hour = (int)(rem / 3600);
    dt->min = (int)(rem % 3600 / 60);
    dt->sec = (int)(rem % 60);
    dt->wday = (int)((days + 4) % 7); /* 1970-01-01 war ein Donnerstag */
    if (dt->wday < 0)
        dt->wday += 7;
}

/* ---------- RTC lesen und schreiben ---------- */

typedef struct {
    uint8_t sec, min, hour, day, month, year, cent;
} RawTime;

static int update_in_progress(void)
{
    return cmos_read(REG_A) & 0x80;
}

static void read_raw(RawTime *r)
{
    /* Nicht mitten in einer Aktualisierung lesen und zweimal hintereinander dasselbe lesen */
    RawTime prev;
    int first = 1;
    for (int tries = 0; tries < 10; tries++) {
        WAIT_UNTIL(!update_in_progress(), 20);
        r->sec = cmos_read(REG_SEC);
        r->min = cmos_read(REG_MIN);
        r->hour = cmos_read(REG_HOUR);
        r->day = cmos_read(REG_DAY);
        r->month = cmos_read(REG_MONTH);
        r->year = cmos_read(REG_YEAR);
        r->cent = cmos_read(REG_CENT);
        if (!first && r->sec == prev.sec && r->min == prev.min && r->hour == prev.hour && r->day == prev.day &&
            r->month == prev.month && r->year == prev.year)
            return;
        prev = *r;
        first = 0;
    }
}

static int bcd(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }

static int read_rtc(DateTime *dt)
{
    RawTime r;
    read_raw(&r);
    uint8_t b = cmos_read(REG_B);
    int binary = (b & 4) != 0, h24 = (b & 2) != 0;

    int pm = !h24 && (r.hour & 0x80);
    int hour = r.hour & 0x7F;
    int sec = binary ? r.sec : bcd(r.sec), min = binary ? r.min : bcd(r.min);
    int day = binary ? r.day : bcd(r.day), month = binary ? r.month : bcd(r.month);
    int year = binary ? r.year : bcd(r.year);
    hour = binary ? hour : bcd((uint8_t)hour);
    if (!h24) {
        hour %= 12;
        if (pm)
            hour += 12;
    }
    int cent = binary ? r.cent : bcd(r.cent);
    if (cent < 19 || cent > 21)
        cent = 20; /* Jahrhundert-Register fehlt oder ist Unsinn */
    year += cent * 100;

    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59 || year < 2000 || year > 2099)
        return -1;
    dt->year = year;
    dt->month = month;
    dt->day = day;
    dt->hour = hour;
    dt->min = min;
    dt->sec = sec;
    dt->wday = 0;
    return 0;
}

void rtc_init(void)
{
    DateTime dt;
    if (read_rtc(&dt) != 0) {
        valid = 0;
        kprintf("rtc: keine gueltige Uhrzeit in der RTC\n");
        return;
    }
    base_unix = datetime_to_unix(&dt);
    base_ms = time_ms();
    valid = 1;
    unix_to_datetime(base_unix, &dt);
    kprintf("rtc: %04d-%02d-%02d %02d:%02d:%02d\n", dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
}

int rtc_valid(void) { return valid; }

uint64_t rtc_now(void)
{
    if (!valid)
        return 0;
    return base_unix + (time_ms() - base_ms) / 1000;
}

uint64_t rtc_now_ms(void)
{
    if (!valid)
        return 0;
    return base_unix * 1000 + (time_ms() - base_ms);
}

int rtc_set_ms(uint64_t ms)
{
    if (rtc_set(ms / 1000) != 0)
        return -1;
    base_ms -= ms % 1000; /* die laufende Sekunde ist schon zum Teil vorbei */
    return 0;
}

/* ---------- Zeitzone ---------- */

enum { TZ_EU, TZ_UK, TZ_FIXED };
static int tz_kind = -1, tz_fixed_s;

static void tz_load(void)
{
    if (tz_kind >= 0)
        return;
    tz_kind = TZ_EU;
    const char *v = cmdline_get("tz");
    if (!v)
        return;
    if ((v[0] == 'u' || v[0] == 'U') && (v[1] == 'k' || v[1] == 'K')) {
        tz_kind = TZ_UK;
        return;
    }
    if ((v[0] == 'u' || v[0] == 'U') && (v[1] == 't' || v[1] == 'T')) { /* utc */
        tz_kind = TZ_FIXED;
        tz_fixed_s = 0;
        return;
    }
    if (v[0] == '+' || v[0] == '-') {
        int h = 0, m = 0;
        const char *p = v + 1;
        while (*p >= '0' && *p <= '9')
            h = h * 10 + (*p++ - '0');
        if (*p == ':')
            for (p++; *p >= '0' && *p <= '9'; p++)
                m = m * 10 + (*p - '0');
        if (h <= 14 && m < 60) {
            tz_kind = TZ_FIXED;
            tz_fixed_s = (h * 3600 + m * 60) * (v[0] == '-' ? -1 : 1);
            return;
        }
    }
    if (!(v[0] == 'e' || v[0] == 'E'))
        kprintf("rtc: Zeitzone 'tz=%s' unbekannt, benutze tz=eu (bekannt: eu, uk, utc, +2, -5, +5:30)\n", v);
}

/* Sommerzeit in EU und UK: vom letzten Sonntag im Maerz bis zum letzten Sonntag im Oktober, jeweils 01:00 UTC */
static int eu_summer(uint64_t utc)
{
    DateTime dt, tmp;
    unix_to_datetime(utc, &dt);
    DateTime mar = {dt.year, 3, 31, 1, 0, 0, 0}, oct = {dt.year, 10, 31, 1, 0, 0, 0};
    uint64_t start = datetime_to_unix(&mar), end = datetime_to_unix(&oct);
    unix_to_datetime(start, &tmp);
    start -= (uint64_t)tmp.wday * 86400;
    unix_to_datetime(end, &tmp);
    end -= (uint64_t)tmp.wday * 86400;
    return utc >= start && utc < end;
}

int tz_offset(uint64_t utc)
{
    tz_load();
    if (tz_kind == TZ_FIXED)
        return tz_fixed_s;
    int summer = eu_summer(utc);
    return (tz_kind == TZ_EU ? 3600 : 0) + (summer ? 3600 : 0);
}

void tz_name(uint64_t utc, char out[12])
{
    tz_load();
    const char *n;
    if (tz_kind == TZ_EU) {
        n = eu_summer(utc) ? "MESZ" : "MEZ";
    } else if (tz_kind == TZ_UK) {
        n = eu_summer(utc) ? "BST" : "GMT";
    } else if (!tz_fixed_s) {
        n = "UTC";
    } else {
        int a = tz_fixed_s < 0 ? -tz_fixed_s : tz_fixed_s;
        if (a % 3600)
            ksnprintf(out, 12, "UTC%c%d:%02d", tz_fixed_s < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
        else
            ksnprintf(out, 12, "UTC%c%d", tz_fixed_s < 0 ? '-' : '+', a / 3600);
        return;
    }
    int i = 0;
    for (; n[i]; i++)
        out[i] = n[i];
    out[i] = 0;
}

int rtc_set(uint64_t t)
{
    DateTime dt;
    unix_to_datetime(t, &dt);
    if (dt.year < 2000 || dt.year > 2099)
        return -1;

    uint8_t b = cmos_read(REG_B);
    int binary = (b & 4) != 0;
#define ENC(v) ((uint8_t)(binary ? (v) : (((v) / 10) << 4) | ((v) % 10)))
    cmos_write(REG_B, (uint8_t)(b | 0x80)); /* SET: Aktualisierung anhalten */
    cmos_write(REG_SEC, ENC(dt.sec));
    cmos_write(REG_MIN, ENC(dt.min));
    cmos_write(REG_HOUR, ENC(dt.hour));
    cmos_write(REG_DAY, ENC(dt.day));
    cmos_write(REG_MONTH, ENC(dt.month));
    cmos_write(REG_YEAR, ENC(dt.year % 100));
    cmos_write(REG_CENT, ENC(dt.year / 100));
#undef ENC
    cmos_write(REG_B, (uint8_t)((b | 0x02) & ~0x80)); /* 24-Stunden-Modus, Aktualisierung wieder an */

    base_unix = t;
    base_ms = time_ms();
    valid = 1;
    return 0;
}

/* ---------- FAT-Zeitstempel ---------- */

void dos_now(uint16_t *date, uint16_t *time)
{
    uint64_t t = rtc_now();
    DateTime dt;
    if (!t) {
        *date = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
        *time = 0;
        return;
    }
    unix_to_datetime(t, &dt);
    if (dt.year < 1980 || dt.year > 2107) {
        *date = (uint16_t)(1 << 5 | 1);
        *time = 0;
        return;
    }
    *date = (uint16_t)(((dt.year - 1980) << 9) | (dt.month << 5) | dt.day);
    *time = (uint16_t)((dt.hour << 11) | (dt.min << 5) | (dt.sec / 2));
}

uint64_t dos_to_unix(uint16_t date, uint16_t time)
{
    if (!date)
        return 0;
    DateTime dt;
    dt.year = 1980 + (date >> 9);
    dt.month = (date >> 5) & 15;
    dt.day = date & 31;
    dt.hour = time >> 11;
    dt.min = (time >> 5) & 63;
    dt.sec = (time & 31) * 2;
    if (dt.month < 1 || dt.month > 12 || dt.day < 1)
        return 0;
    if (dt.hour > 23 || dt.min > 59 || dt.sec > 59)
        dt.hour = dt.min = dt.sec = 0;
    return datetime_to_unix(&dt);
}
