#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

/* Kernel-Log: alles, was kprintf ausgibt (ohne Farbfolgen), in einem Ringpuffer von KLOG_SIZE Bytes. Auf echter
 * Hardware ohne serielle Schnittstelle der einzige Weg an fruehe Meldungen: "dmesg > /disk/log.txt". */

#define KLOG_SIZE (256 * 1024)

void     klog_putc(char c);
uint64_t klog_total(void); /* bisher geschriebene Bytes; die aeltesten KLOG_SIZE davon sind noch da */

/* Kopiert ab Position pos (0 = Anfang des Logs) hoechstens max Bytes; Ergebnis: Anzahl. Ist pos schon aus dem
 * Puffer gefallen, geht es beim aeltesten noch vorhandenen Byte weiter (*pos wird angepasst). */
uint64_t klog_read(uint64_t *pos, char *buf, uint64_t max);

#endif
