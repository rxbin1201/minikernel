#ifndef BT_INTERNAL_H
#define BT_INTERNAL_H

/* Gemeinsames von btusb.c (USB-Transport, Intel-Firmware, Suche) und btconn.c (Verbindung, Koppeln, L2CAP, AVDTP).
 *
 * Sperren: bt_lock haelt, wer eine Operation ausfuehrt (Firmware laden, suchen, verbinden). Jeder HCI-Befehl nimmt
 * zusaetzlich kurz eine eigene Sperre - so kann der Thread "bt" auf Anfragen des Geraets (Koppeln) antworten, waehrend
 * ein Verbinden wartet. Ereignisse und ACL-Daten kommen im Kontext des USB-Controllers an (nicht blockieren!). */

#include <stdint.h>
#include "drivers/bt/bt.h"
#include "core/sched.h"

struct UsbDevice;

extern Mutex bt_lock;

struct UsbDevice *bt_usb_dev(void);
BtInfo *bt_state(void);

/* HCI-Befehl mit Command Complete: Laenge der Rueckgabeparameter (ab Status) oder <0 */
int hci_cmd(uint16_t op, const void *param, uint8_t plen, uint8_t *resp, uint32_t max, uint32_t timeout_ms);
int hci_cmd_status(uint16_t op, const void *param, uint8_t plen); /* antwortet mit Command Status: 0 = angenommen */
int hci_ok(uint16_t op, const void *param, uint8_t plen, const char *what); /* Command Complete mit Status 0 */
int hci_init_locked(void);   /* mit bt_lock: Firmware laden (falls noetig) und HCI einrichten */
int wait_events(volatile int *cond, uint32_t timeout_ms); /* Ereignisse auswerten, bis *cond: 0, -1 Zeit um, -2 weg */

/* btconn.c */
int  bt_conn_event(uint8_t code, const uint8_t *p, uint32_t plen); /* 1 = Ereignis gehoert zur Verbindung */
void bt_conn_acl(const uint8_t *buf, uint32_t len);                /* Bulk-IN im Betrieb */
void bt_conn_start(void);                                          /* nach dem Einrichten: Thread "bt" starten */
int  bt_conn_ready(void);                                          /* verbunden, AVDTP-Signalkanal offen */
BtConn *bt_conn_state(void);
int  bt_av_setup(void);              /* mit bt_lock: SBC einstellen, Open, Medienkanal; 0 oder <0 */
int  bt_av_start(int start);         /* mit bt_lock: AVDTP Start (1) bzw. Suspend (0) */
int  bt_media_send(const uint8_t *d, uint32_t len); /* ein Medienpaket (RTP) */

/* a2dp.c */
void bt_a2dp_init(void);             /* Thread "a2dp" starten */

#endif
