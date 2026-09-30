#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* Echtzeituhr (CMOS-RTC) und Wandzeit. Die RTC enthaelt die "Uhrzeit auf der Wand": Windows und die meisten BIOS speichern
 * die Ortszeit, Linux oft UTC. Der Kernel rechnet nicht um, er zeigt und speichert die Zeit so, wie sie in der RTC steht
 * (mit "date -s" stellbar). Nach dem Start laeuft die Zeit ueber den Zeitgeber weiter. */

typedef struct {
    int year, month, day, hour, min, sec;
    int wday; /* 0 = Sonntag */
} DateTime;

/* Liest die RTC (nach dem Start des Zeitgebers aufrufen). */
void rtc_init(void);

int      rtc_valid(void);              /* 1, wenn die RTC eine plausible Zeit lieferte */
uint64_t rtc_now(void);                /* Sekunden seit 1970-01-01 00:00 (Zeit wie in der RTC); 0 = unbekannt */
int      rtc_set(uint64_t unix_secs);  /* stellt RTC und Wandzeit; 0 = ok */
uint64_t rtc_now_ms(void);             /* wie rtc_now, in Millisekunden */
int      rtc_set_ms(uint64_t unix_ms);

/* Zeitzone (fuer NTP, das UTC liefert, waehrend die RTC Ortszeit enthaelt). cmdline.txt: tz=eu (Standard: MEZ/MESZ),
 * tz=uk (GMT/BST), tz=utc, tz=+2, tz=-5, tz=+5:30. Sommerzeit wie in der EU. */
int  tz_offset(uint64_t utc);          /* Ortszeit - UTC in Sekunden */
void tz_name(uint64_t utc, char out[12]); /* "MEZ", "MESZ", "UTC+2" ... */

void     unix_to_datetime(uint64_t t, DateTime *dt);
uint64_t datetime_to_unix(const DateTime *dt);

/* FAT/exFAT-Zeitstempel: Datum (Jahr-1980 << 9 | Monat << 5 | Tag) und Zeit (Stunde << 11 | Minute << 5 | Sekunde/2) */
void     dos_now(uint16_t *date, uint16_t *time); /* ohne gueltige Uhr: 2026-01-01 00:00 */
uint64_t dos_to_unix(uint16_t date, uint16_t time); /* 0 bei date == 0 */

#endif
