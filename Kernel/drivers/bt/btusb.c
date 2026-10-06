/* Bluetooth ueber USB (HCI-Transport wie btusb in Linux), Stufe 1 - fuer den Bluetooth-Teil der Intel AX200
 * (8087:0029), passt aber auf jedes Geraet der Klasse E0/01/01.
 *
 * Transport: HCI-Befehle gehen als Class-Request (bmRequestType 0x20, bRequest 0) ueber Endpunkt 0 hinaus (Opcode,
 * Laenge, Parameter), Ereignisse kommen ueber den Interrupt-IN-Endpunkt - auch in mehreren Stuecken, wenn sie laenger
 * als ein Paket sind; der Treiber setzt sie wieder zusammen. ACL-Daten laufen spaeter ueber Bulk-IN/-OUT (Stufe 3).
 * Die Isochron-Schnittstelle (Sprache) bleibt ungenutzt.
 *
 * Intel: Nach dem Einschalten laeuft der Bootloader (Firmware-Variante 0x06); er kennt nur wenige Befehle, und die
 * eigentliche Firmware (ibt-<hw_variant>-<hw_revision>-<fw_revision>.sfi aus linux-firmware) muss erst geladen werden
 * (Stufe 2). Hat ein anderes System sie schon geladen (Neustart ohne Stromausfall), meldet sich die Betriebs-Firmware
 * (0x23). Read Version (0xFC05) ohne Parameter liefert in beiden Faellen das alte Format (Plattform 0x37). */

#include "drivers/bt/bt.h"
#include "drivers/bt/bt_internal.h"
#include "drivers/usb/usb.h"
#include "arch/x86_64/apic.h"
#include "core/sched.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"

#define EVT_CMD_COMPLETE 0x0E
#define EVT_CMD_STATUS   0x0F
#define EVT_VENDOR       0xFF
#define EV_MAX           260 /* Kopf (2) + bis 255 Byte Parameter */

_Static_assert(sizeof(BtInfo) == 360 && sizeof(BtDev) == 68, "BtInfo/BtDev wie in user.h");

/* Ereignisse kommen in Stuecken (je USB-Paket); je Quelle (Interrupt-IN, Bulk-IN) wird zusammengesetzt */
typedef struct {
    uint8_t  buf[EV_MAX + 1024];
    uint32_t len;
} Assembler;

static BtInfo     info;
static UsbDevice *dev;
static uint8_t   *ev_dma, *bulk_dma, *out_dma; /* DMA-Puffer: Interrupt-IN, Bulk-IN, Bulk-OUT */
static Assembler  as_intr, as_bulk;
static int        bulk_listening;
Mutex             bt_lock = MUTEX_INIT;         /* eine Operation (Laden, Suchen, Verbinden) nach der anderen */
static Mutex      cmd_lock = MUTEX_INIT;        /* ein HCI-Befehl nach dem anderen (auch aus dem Thread "bt") */
static volatile int dl_event, boot_event; /* Intel-Ereignis 0x06 (Download fertig) bzw. 0x02 (Firmware gestartet) */
static uint8_t    dl_payload[8], boot_payload[8];

/* Worauf gewartet wird: Command Complete bzw. Command Status zu want_op */
static struct {
    volatile int got;
    uint16_t     op;
    int          status_only; /* Command Status mit Fehler */
    int          accept_status; /* Befehl antwortet mit Command Status (z.B. Inquiry): auch Status 0 beendet das Warten */
    uint32_t     len;
    uint8_t      data[256];   /* Rueckgabeparameter von Command Complete (ab dem Status) */
} want;
static uint32_t logged;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* ---------- Stufe 3: gefundene Geraete (aus Inquiry- und LE-Ereignissen, im Controller-Kontext gefuellt) ---------- */

static BtDev           devs[BT_MAX_DEVS];
static volatile uint32_t n_devs;
static volatile int    inq_done;
static int             scanning;

static BtDev *dev_entry(const uint8_t *addr_le, uint8_t kind)
{
    uint8_t a[6];
    for (int i = 0; i < 6; i++)
        a[i] = addr_le[5 - i]; /* HCI: niedrigstes Byte zuerst; gespeichert wie geschrieben */
    for (uint32_t i = 0; i < n_devs; i++)
        if (memcmp(devs[i].addr, a, 6) == 0 && (devs[i].kind == 0) == (kind == 0))
            return &devs[i];
    if (n_devs >= BT_MAX_DEVS)
        return 0;
    BtDev *d = &devs[n_devs];
    memset(d, 0, sizeof(*d));
    memcpy(d->addr, a, 6);
    d->kind = kind;
    d->rssi = -127;
    __sync_synchronize();
    n_devs++;
    return d;
}

/* EIR bzw. Werbedaten: Folge aus Laenge, Typ, Daten - Name (8 gekuerzt, 9 vollstaendig), Erscheinungsbild (0x19) */
static void parse_ad(BtDev *d, const uint8_t *p, uint32_t len)
{
    for (uint32_t o = 0; o + 1 < len;) {
        uint8_t l = p[o];
        if (!l || o + 1 + l > len)
            break;
        uint8_t t = p[o + 1];
        const uint8_t *v = p + o + 2;
        uint32_t vl = l - 1u;
        if ((t == 0x09 || (t == 0x08 && !d->name_len)) && vl) {
            uint32_t k = vl < sizeof(d->name) - 1 ? vl : sizeof(d->name) - 1;
            memcpy(d->name, v, k);
            d->name[k] = 0;
            d->name_len = (uint8_t)k;
        } else if (t == 0x19 && vl >= 2) {
            d->appearance = le16(v);
        } else if (t == 0x01 && vl >= 1) {
            d->le_flags = v[0];
        }
        o += 1u + l;
    }
}

static void seen(BtDev *d, int rssi)
{
    if (rssi != 127 && rssi > d->rssi)
        d->rssi = (int8_t)rssi;
    d->seen++;
}

/* 1 = Ereignis gehoert zur Suche (und ist ausgewertet) */
static int scan_event(uint8_t code, const uint8_t *p, uint32_t plen)
{
    if (code == 0x01) { /* Inquiry Complete */
        inq_done = 1;
        return 1;
    }
    if ((code == 0x02 || code == 0x22) && plen >= 1) { /* Inquiry Result (mit RSSI): Parameter je Art hintereinander */
        /* je Antwort 14 Byte: Adresse (6), Page-Scan-Modus (1), reserviert (2 bzw. 1), Klasse (3), Uhr (2), [RSSI (1)] */
        uint32_t n = p[0];
        if (plen < 1 + n * 14)
            return 1;
        for (uint32_t i = 0; i < n; i++) {
            BtDev *d = dev_entry(p + 1 + i * 6, 0);
            if (!d)
                continue;
            const uint8_t *cod = code == 0x02 ? p + 1 + n * 9 + i * 3 : p + 1 + n * 8 + i * 3;
            d->cod = (uint32_t)cod[0] | (uint32_t)cod[1] << 8 | (uint32_t)cod[2] << 16;
            seen(d, code == 0x22 ? (int8_t)p[1 + n * 13 + i] : 127);
        }
        return 1;
    }
    if (code == 0x2F && plen >= 15) { /* Extended Inquiry Result: eine Antwort mit EIR (Name) */
        BtDev *d = dev_entry(p + 1, 0);
        if (d) {
            d->cod = (uint32_t)p[9] | (uint32_t)p[10] << 8 | (uint32_t)p[11] << 16;
            seen(d, (int8_t)p[14]);
            parse_ad(d, p + 15, plen - 15);
        }
        return 1;
    }
    if (code == 0x3E && plen >= 2 && p[0] == 0x02) { /* LE Meta: Advertising Report (je Bericht hintereinander) */
        uint32_t n = p[1], o = 2;
        for (uint32_t i = 0; i < n && o + 9 <= plen; i++) {
            uint8_t type = p[o], atype = p[o + 1], dl = p[o + 8];
            if (o + 9 + dl + 1 > plen)
                break;
            BtDev *d = dev_entry(p + o + 2, (uint8_t)(atype & 1 ? 2 : 1));
            if (d) {
                if (type != 4) /* Scan Response sagt nichts ueber die Verbindbarkeit */
                    d->le_connectable = type == 0 || type == 1;
                seen(d, (int8_t)p[o + 9 + dl]);
                parse_ad(d, p + o + 9, dl);
            }
            o += 9u + dl + 1u;
        }
        return 1;
    }
    return 0;
}

/* ein vollstaendiges Ereignis (Code, Laenge, Parameter) */
static void on_event(const uint8_t *e, uint32_t len)
{
    info.events++;
    uint8_t code = e[0];
    const uint8_t *p = e + 2;
    uint32_t plen = len - 2;
    if (code == EVT_CMD_COMPLETE && plen >= 3) { /* Zahl erlaubter Befehle, Opcode, Rueckgabeparameter */
        uint16_t op = le16(p + 1);
        if (!want.got && op == want.op) {
            want.len = plen - 3 < sizeof(want.data) ? plen - 3 : sizeof(want.data);
            memcpy(want.data, p + 3, want.len);
            want.status_only = 0;
            __sync_synchronize();
            want.got = 1;
        }
        return;
    }
    if (code == EVT_CMD_STATUS && plen >= 4) { /* Status, Zahl, Opcode */
        uint16_t op = le16(p + 2);
        if (!want.got && op == want.op && (p[0] != 0 || want.accept_status)) {
            want.len = 1;
            want.data[0] = p[0];
            want.status_only = p[0] != 0;
            __sync_synchronize();
            want.got = 1;
        }
        return;
    }
    if (scan_event(code, p, plen) || bt_conn_event(code, p, plen))
        return;
    if (code == EVT_VENDOR && plen >= 1) {
        info.vendor_events++;
        if (p[0] == 0x06) { /* Ergebnis des Downloads: Ergebnis, Opcode, Status */
            memcpy(dl_payload, p + 1, plen - 1 < sizeof(dl_payload) ? plen - 1 : sizeof(dl_payload));
            __sync_synchronize();
            dl_event = 1;
        } else if (p[0] == 0x02) { /* Firmware gestartet: 0, Zahl der Befehle, Quelle, Art und Grund des Resets, DDC */
            memcpy(boot_payload, p + 1, plen - 1 < sizeof(boot_payload) ? plen - 1 : sizeof(boot_payload));
            __sync_synchronize();
            boot_event = 1;
        }
    }
    if (logged++ < 30)
        kprintf("bt:   Ereignis %#x, %u Byte: %02x %02x %02x %02x %02x %02x\n", code, plen, plen > 0 ? p[0] : 0,
                plen > 1 ? p[1] : 0, plen > 2 ? p[2] : 0, plen > 3 ? p[3] : 0, plen > 4 ? p[4] : 0,
                plen > 5 ? p[5] : 0);
}

static void assemble(Assembler *a, const uint8_t *buf, uint32_t len)
{
    if (a->len + len > sizeof(a->buf)) /* aus dem Tritt: neu anfangen */
        a->len = 0;
    memcpy(a->buf + a->len, buf, len);
    a->len += len;
    while (a->len >= 2 && a->len >= 2u + a->buf[1]) {
        uint32_t n = 2u + a->buf[1];
        on_event(a->buf, n);
        memmove(a->buf, a->buf + n, a->len - n);
        a->len -= n;
    }
}

/* Interrupt-IN (Controller-Kontext) */
void bt_wake_all(void);

static void on_intr(UsbDevice *d, const uint8_t *buf, uint32_t len)
{
    (void)d;
    assemble(&as_intr, buf, len);
    bt_wake_all();
}

/* Bulk-IN: im Bootloader kommen hier ebenfalls Ereignisse (Antworten auf Secure Send); im Betrieb waeren es
 * ACL-Daten - die wertet erst Stufe 3 aus */
static void on_bulk(UsbDevice *d, const uint8_t *buf, uint32_t len)
{
    (void)d;
    if (info.mode == BT_MODE_BOOTLOADER)
        assemble(&as_bulk, buf, len);
    else
        bt_conn_acl(buf, len); /* im Betrieb: ACL-Daten */
    bt_wake_all();
}

UsbDevice *bt_usb_dev(void)
{
    return dev;
}

BtInfo *bt_state(void)
{
    return &info;
}

int bt_usb_probe(UsbDevice *d, const UsbIface *iface)
{
    if (dev && usb_alive(dev))
        return -1; /* nur ein Bluetooth-Geraet */
    UsbEpDesc eps[3];
    int n = 0;
    uint8_t intr = 0, bin = 0, bout = 0;
    memset(&info, 0, sizeof(info));
    for (int i = 0; i < iface->ep_count; i++) {
        const UsbEpDesc *e = &iface->eps[i];
        int type = e->attrs & 3, in = (e->address & 0x80) != 0;
        if (type == 3 && in && !intr) {
            intr = e->address;
            info.mps_intr = e->max_packet;
            eps[n++] = *e;
        } else if (type == 2 && in && !bin) {
            bin = e->address;
            info.mps_bulk = e->max_packet;
            eps[n++] = *e;
        } else if (type == 2 && !in && !bout) {
            bout = e->address;
            eps[n++] = *e;
        }
    }
    if (!intr || !bin || !bout) {
        kprintf("bt: Schnittstelle %u ohne Interrupt-IN/Bulk-IN/Bulk-OUT\n", iface->number);
        return -1;
    }
    if (!ev_dma && !(ev_dma = (uint8_t *)pmm_alloc_frames(1)))
        return -1;
    if (!bulk_dma && !(bulk_dma = (uint8_t *)pmm_alloc_frames(1)))
        return -1;
    if (!out_dma && !(out_dma = (uint8_t *)pmm_alloc_frames(1)))
        return -1;
    bulk_listening = 0;
    if (usb_add_endpoints(d, eps, n) != 0) {
        kprintf("bt: Endpunkte nicht eingerichtet\n");
        return -1;
    }
    usb_describe(d, &info.vid, &info.pid, info.path);
    info.present = 1;
    info.ep_intr = intr;
    info.ep_bulk_in = bin;
    info.ep_bulk_out = bout;
    as_intr.len = as_bulk.len = 0;
    if (usb_interrupt_start(d, intr, ev_dma, EV_MAX, on_intr) != 0) {
        kprintf("bt: Ereignis-Endpunkt nicht gestartet\n");
        return -1;
    }
    dev = d;
    ksnprintf(info.state, sizeof(info.state), "eingerichtet (bt zeigt Version und Firmware)");
    kprintf("bt: Bluetooth-Geraet %04x:%04x an Port %s eingerichtet: Ereignisse %#x (%u Byte), Bulk %#x/%#x (%u Byte)\n",
            info.vid, info.pid, info.path, intr, info.mps_intr, bin, bout, info.mps_bulk);
    return 0;
}

/* Bis cond erfuellt ist, Ereignisse auswerten: anfangs nur den Prozessor abgeben (Antworten kommen nach etwa 1 ms),
 * spaeter schlafen. 0, -1 Zeit um, -2 Geraet weg */
/* Wartende Threads (Befehl, Verbinden, Koppeln, freie ACL-Puffer): jedes Ereignis und jedes Datenpaket vom Controller
 * weckt alle. Frueher wurde mit thread_yield gewartet - der Thread blieb dabei im Kernel und hielt den BKL, das ganze
 * System stand bis zu 80 ms (Mauszeiger, Ton). Jetzt schlaeft er und gibt den BKL frei. */
#define MAX_WAITERS 8
static Event *waiters[MAX_WAITERS];

void bt_wake_all(void)
{
    for (int i = 0; i < MAX_WAITERS; i++)
        if (waiters[i])
            event_signal(waiters[i]);
}

int wait_events(volatile int *cond, uint32_t timeout_ms)
{
    Event ev = EVENT_INIT;
    int slot = -1;
    for (int i = 0; i < MAX_WAITERS && slot < 0; i++)
        if (!waiters[i]) {
            waiters[i] = &ev;
            slot = i;
        }
    uint64_t t0 = time_ms();
    int r = 0;
    while (!*cond) {
        usb_lock(dev);
        usb_poll(dev);
        int alive = usb_alive(dev);
        usb_unlock(dev);
        if (*cond)
            break;
        if (!alive) {
            r = -2;
            break;
        }
        if (time_ms() - t0 > timeout_ms) {
            r = -1;
            break;
        }
        if (slot >= 0)
            event_wait(&ev, 10); /* schlafen, bis der Controller etwas meldet (spaetestens 10 ms) */
        else
            thread_sleep_ms(1);
    }
    if (slot >= 0)
        waiters[slot] = 0;
    return r;
}

/* HCI-Befehl senden (Endpunkt 0, bzw. bulk = ueber Bulk-OUT wie Secure Send im Bootloader) und auf Command Complete
 * warten. Rueckgabeparameter (ab dem Status) nach resp.
 * Ergebnis: Laenge, -1 keine Antwort, -2 Geraet weg, -3 Senden fehlgeschlagen, -4 Command Status mit Fehler */
static int hci_cmd_locked(uint16_t op, const void *param, uint8_t plen, uint8_t *resp, uint32_t max,
                          uint32_t timeout_ms, int bulk, int quiet, int accept_status);

static int hci_cmd_ex(uint16_t op, const void *param, uint8_t plen, uint8_t *resp, uint32_t max, uint32_t timeout_ms,
                      int bulk, int quiet, int accept_status)
{
    mutex_lock(&cmd_lock);
    int r = hci_cmd_locked(op, param, plen, resp, max, timeout_ms, bulk, quiet, accept_status);
    want.accept_status = 0;
    mutex_unlock(&cmd_lock);
    return r;
}

static int hci_cmd_locked(uint16_t op, const void *param, uint8_t plen, uint8_t *resp, uint32_t max,
                          uint32_t timeout_ms, int bulk, int quiet, int accept_status)
{
    uint8_t *cmd = out_dma;
    cmd[0] = (uint8_t)op;
    cmd[1] = (uint8_t)(op >> 8);
    cmd[2] = plen;
    memcpy(cmd + 3, param, plen);
    usb_lock(dev);
    if (!usb_alive(dev)) {
        usb_unlock(dev);
        return -2;
    }
    want.got = 0;
    want.op = op;
    want.accept_status = accept_status;
    int r = bulk ? usb_bulk(dev, info.ep_bulk_out, cmd, 3u + plen, 1000)
                 : usb_control(dev, 0x20, 0, 0, 0, cmd, (uint16_t)(3 + plen));
    usb_unlock(dev);
    info.cmds++;
    info.last_opcode = op;
    if (r < 0) {
        kprintf("bt:   Befehl %#06x: Senden fehlgeschlagen (%d)\n", op, r);
        info.last_error = -3;
        return -3;
    }
    uint64_t t0 = time_ms();
    int w = wait_events(&want.got, timeout_ms);
    if (w == -2)
        return -2;
    if (w < 0) {
        kprintf("bt:   Befehl %#06x: keine Antwort nach %u ms\n", op, timeout_ms);
        info.last_error = -1;
        return -1;
    }
    info.last_error = want.data[0];
    if (want.status_only) {
        kprintf("bt:   Befehl %#06x: abgelehnt (Status %#x)\n", op, want.data[0]);
        return -4;
    }
    if (!quiet)
        kprintf("bt:   Befehl %#06x: Antwort nach %u ms, Status %#x, %u Byte\n", op, (uint32_t)(time_ms() - t0),
                want.data[0], want.len);
    uint32_t n = want.len < max ? want.len : max;
    memcpy(resp, want.data, n);
    return (int)want.len;
}

int hci_cmd(uint16_t op, const void *param, uint8_t plen, uint8_t *resp, uint32_t max, uint32_t timeout_ms)
{
    return hci_cmd_ex(op, param, plen, resp, max, timeout_ms, 0, 0, 0);
}

static void find_firmware(void)
{
    ksnprintf(info.fw_name, sizeof(info.fw_name), "ibt-%u-%u-%u.sfi", info.hw_variant, info.hw_revision,
              info.fw_revision);
    char path[64];
    ksnprintf(path, sizeof(path), "/firmware/%s", info.fw_name);
    const VfsNode *n = vfs_lookup(path);
    info.fw_found = n && !n->is_dir;
    info.fw_size = info.fw_found ? (uint32_t)n->size : 0;
}

static int query_locked(void)
{
    uint8_t r[64];
    int n = hci_cmd(0xFC05, 0, 0, r, sizeof(r), 2000); /* Intel Read Version, altes Format */
    int ret = 0;
    if (n >= 10 && r[0] == 0) {
        info.ver_ok = 1;
        info.hw_platform = r[1];
        info.hw_variant = r[2];
        info.hw_revision = r[3];
        info.fw_variant = r[4];
        info.fw_revision = r[5];
        info.fw_build_num = r[6];
        info.fw_build_ww = r[7];
        info.fw_build_yy = r[8];
        info.fw_patch_num = r[9];
        info.mode = r[4] == 0x06 ? BT_MODE_BOOTLOADER : r[4] == 0x23 ? BT_MODE_OPERATIONAL : BT_MODE_UNKNOWN;
        kprintf("bt: Intel: Plattform %#x, Variante %#x, Revision %u, Firmware-Variante %#x (%s), Revision %u, "
                "Build %u Woche %u Jahr 20%02u, Patch %u\n", r[1], r[2], r[3], r[4],
                info.mode == BT_MODE_BOOTLOADER ? "Bootloader" : info.mode == BT_MODE_OPERATIONAL ? "Betrieb" : "?",
                r[5], r[6], r[7], r[8], r[9]);
        find_firmware();
    } else {
        ret = -2;
    }
    if (info.mode == BT_MODE_BOOTLOADER) {
        n = hci_cmd(0xFC0D, 0, 0, r, sizeof(r), 2000); /* Intel Read Boot Params */
        if (n >= 23 && r[0] == 0) {
            info.boot_ok = 1;
            info.otp_format = r[1];
            info.otp_content = r[2];
            info.otp_patch = r[3];
            info.dev_revid = le16(r + 4);
            info.secure_boot = r[6];
            info.key_from_hdr = r[7];
            info.key_type = r[8];
            info.otp_lock = r[9];
            info.api_lock = r[10];
            info.debug_lock = r[11];
            for (int i = 0; i < 6; i++)
                info.otp_bdaddr[i] = r[17 - i]; /* wie BD_ADDR: niedrigstes Byte zuerst gesendet */
            info.min_fw_build_nn = r[18];
            info.min_fw_build_cw = r[19];
            info.min_fw_build_yy = r[20];
            info.limited_cce = r[21];
            info.unlocked_state = r[22];
            kprintf("bt: Boot-Parameter: Geraete-Revision %u, Secure Boot %u, Schluessel %u, OTP-Sperre %u, "
                    "mindestens Build %u.%u.%u\n", info.dev_revid, info.secure_boot, info.key_type, info.otp_lock,
                    info.min_fw_build_nn, info.min_fw_build_cw, info.min_fw_build_yy);
        }
    } else if (info.mode == BT_MODE_OPERATIONAL) {
        n = hci_cmd(0x1009, 0, 0, r, sizeof(r), 2000); /* HCI_Read_BD_ADDR */
        if (n >= 7 && r[0] == 0)
            for (int i = 0; i < 6; i++)
                info.bdaddr[i] = r[6 - i]; /* kommt mit dem niedrigsten Byte zuerst */
        n = hci_cmd(0x1001, 0, 0, r, sizeof(r), 2000); /* HCI_Read_Local_Version_Information */
        if (n >= 9 && r[0] == 0) {
            info.local_ok = 1;
            info.hci_version = r[1];
            info.hci_revision = le16(r + 2);
            info.lmp_version = r[4];
            info.manufacturer = le16(r + 5);
            info.lmp_subversion = le16(r + 7);
        }
    }
    ksnprintf(info.state, sizeof(info.state), "%s", !info.ver_ok ? "keine Antwort auf Read Version" :
              info.mode == BT_MODE_BOOTLOADER ? "Bootloader: Firmware muss geladen werden (Stufe 2)" :
              info.mode == BT_MODE_OPERATIONAL ? "Firmware laeuft" : "unbekannte Firmware-Variante");
    return ret;
}

int bt_query(void)
{
    if (!dev || !usb_alive(dev))
        return -1;
    mutex_lock(&bt_lock);
    logged = 0;
    int r = query_locked();
    mutex_unlock(&bt_lock);
    return r;
}

/* ---------- Stufe 2: Firmware laden (wie btintel_bootloader_setup in Linux) ----------
 * Datei (.sfi): CSS-Kopf (128 Byte), oeffentlicher Schluessel (256), Signatur (256), danach HCI-Befehle (Opcode,
 * Laenge, Parameter), die der Bootloader nach der Pruefung der Signatur ausfuehrt. Alles geht per Secure Send (0xFC09,
 * erstes Byte = Art: 0 Kopf, 3 Schluessel, 2 Signatur, 1 Daten; hoechstens 252 Byte je Befehl) ueber Bulk-OUT. Die
 * Daten werden an Befehlsgrenzen geschnitten, so dass jedes Stueck eine durch 4 teilbare Laenge hat (die Datei enthaelt
 * dafuer passende Intel-NOPs). Ist alles da, meldet der Bootloader das Ergebnis (Intel-Ereignis 0x06). Intel Reset
 * (0xFC01) mit der Boot-Adresse aus dem Befehl 0xFC0E der Datei startet die Firmware; sie meldet sich mit dem
 * Intel-Ereignis 0x02. */
#define RSA_HEADER_LEN        644
#define CMD_WRITE_BOOT_PARAMS 0xFC0E

/* Bulk-IN dauerhaft abhoeren: im Bootloader Ereignisse, im Betrieb ACL-Daten */
static int start_bulk(void)
{
    if (bulk_listening)
        return 0;
    usb_lock(dev);
    int r = usb_interrupt_start(dev, info.ep_bulk_in, bulk_dma, 1024, on_bulk);
    usb_unlock(dev);
    if (r == 0)
        bulk_listening = 1;
    return r;
}

static int dl_fail(int err, const char *msg)
{
    ksnprintf(info.dl_msg, sizeof(info.dl_msg), "%s", msg);
    kprintf("bt: Firmware laden: %s\n", msg);
    return err;
}

static int secure_send(uint8_t type, const uint8_t *data, uint32_t len)
{
    static uint8_t param[253];
    uint8_t r[8];
    while (len) {
        uint32_t k = len > 252 ? 252 : len;
        param[0] = type;
        memcpy(param + 1, data, k);
        int n = hci_cmd_ex(0xFC09, param, (uint8_t)(k + 1), r, sizeof(r), 3000, 1, 1, 0);
        if (n < 0) {
            kprintf("bt: Secure Send (Art %u, Stueck %u): Fehler %d\n", type, info.dl_frags, n);
            return n;
        }
        if (r[0] != 0) {
            kprintf("bt: Secure Send (Art %u, Stueck %u): Status %#x\n", type, info.dl_frags, r[0]);
            return -5;
        }
        info.dl_frags++;
        data += k;
        len -= k;
    }
    return 0;
}

static int load_locked(void)
{
    info.dl_done = 1;
    info.dl_ok = info.booted = 0;
    info.dl_result = 0xFF;
    info.dl_frags = info.dl_ms = info.boot_ms = info.boot_addr = 0;
    info.dl_msg[0] = 0;
    if (query_locked() != 0 || !info.ver_ok)
        return dl_fail(-2, "keine Antwort auf Read Version");
    if (info.mode == BT_MODE_OPERATIONAL) {
        info.dl_ok = info.booted = 1;
        return dl_fail(0, "Firmware laeuft schon - nichts zu tun");
    }
    if (info.mode != BT_MODE_BOOTLOADER)
        return dl_fail(-6, "weder Bootloader noch Betriebs-Firmware");
    if (info.limited_cce)
        return dl_fail(-6, "eingeschraenkter Bootloader (limited CCE) wird nicht unterstuetzt");
    char path[64];
    ksnprintf(path, sizeof(path), "/firmware/%s", info.fw_name);
    const VfsNode *node = vfs_lookup(path);
    if (!node || node->is_dir)
        return dl_fail(-4, "Firmware-Datei fehlt in /firmware");
    const uint8_t *fw = node->data;
    uint32_t size = (uint32_t)node->size;
    if (size < RSA_HEADER_LEN + 3)
        return dl_fail(-4, "Firmware-Datei zu kurz");

    /* Boot-Adresse und Version aus dem Befehl 0xFC0E der Datei */
    for (uint32_t off = RSA_HEADER_LEN; off + 3 <= size;) {
        uint16_t op = le16(fw + off);
        uint8_t plen = fw[off + 2];
        if (off + 3 + plen > size)
            return dl_fail(-4, "Befehl in der Datei reicht ueber das Ende");
        if (op == CMD_WRITE_BOOT_PARAMS && plen >= 7) {
            info.boot_addr = (uint32_t)fw[off + 3] | (uint32_t)fw[off + 4] << 8 | (uint32_t)fw[off + 5] << 16 |
                             (uint32_t)fw[off + 6] << 24;
            info.file_build_num = fw[off + 7];
            info.file_build_ww = fw[off + 8];
            info.file_build_yy = fw[off + 9];
        }
        off += 3u + plen;
    }
    if (!info.boot_addr)
        return dl_fail(-4, "keine Boot-Adresse (0xFC0E) in der Datei");
    kprintf("bt: %s: %u Byte, Boot-Adresse %#x, Firmware-Build %u Woche %u 20%02u\n", info.fw_name, size,
            info.boot_addr, info.file_build_num, info.file_build_ww, info.file_build_yy);

    /* Bulk-IN abhoeren: dort kommen im Bootloader die Antworten auf Secure Send */
    if (start_bulk() != 0)
        return dl_fail(-3, "Bulk-IN nicht gestartet");
    as_bulk.len = 0;
    dl_event = 0;
    uint64_t t0 = time_ms();
    if (secure_send(0x00, fw, 128) != 0)
        return dl_fail(-5, "CSS-Kopf abgelehnt");
    if (secure_send(0x03, fw + 128, 256) != 0)
        return dl_fail(-5, "oeffentlicher Schluessel abgelehnt");
    if (secure_send(0x02, fw + 388, 256) != 0)
        return dl_fail(-5, "Signatur abgelehnt");
    kprintf("bt: Kopf, Schluessel und Signatur angenommen nach %u ms\n", (uint32_t)(time_ms() - t0));
    const uint8_t *p = fw + RSA_HEADER_LEN;
    uint32_t frag = 0, next_log = 100 * 1024;
    while ((uint32_t)(p - fw) + frag < size) {
        uint32_t at = (uint32_t)(p - fw) + frag;
        if (at + 3 > size)
            return dl_fail(-4, "Datei endet mitten in einem Befehl");
        frag += 3u + fw[at + 2];
        if (frag % 4 == 0) {
            if (secure_send(0x01, p, frag) != 0)
                return dl_fail(-5, "Daten abgelehnt");
            p += frag;
            frag = 0;
            if ((uint32_t)(p - fw) >= next_log) {
                kprintf("bt:   %u von %u KiB nach %u ms\n", (uint32_t)(p - fw) / 1024, size / 1024,
                        (uint32_t)(time_ms() - t0));
                next_log += 100 * 1024;
            }
        }
    }
    if (frag)
        return dl_fail(-4, "Datei endet nicht auf einer 4-Byte-Grenze");
    if (wait_events(&dl_event, 5000) != 0)
        return dl_fail(-1, "kein Ergebnis vom Bootloader (Ereignis 0x06)");
    info.dl_result = dl_payload[0];
    info.dl_ms = (uint32_t)(time_ms() - t0);
    kprintf("bt: Firmware uebertragen in %u ms (%u Stuecke), Ergebnis %#x (Opcode %#x, Status %#x)\n", info.dl_ms,
            info.dl_frags, dl_payload[0], le16(dl_payload + 1), dl_payload[3]);
    if (info.dl_result != 0)
        return dl_fail(-5, "Bootloader meldet einen Fehler (Ergebnis != 0)");
    info.dl_ok = 1;

    /* Firmware starten: Reset-Art 0, Patch an, DDC nicht neu laden, Boot-Option 1 (Adresse aus boot_param) */
    uint8_t rp[8] = {0x00, 0x01, 0x00, 0x01, (uint8_t)info.boot_addr, (uint8_t)(info.boot_addr >> 8),
                     (uint8_t)(info.boot_addr >> 16), (uint8_t)(info.boot_addr >> 24)};
    uint8_t r[8];
    boot_event = 0;
    uint64_t tb = time_ms();
    int n = hci_cmd(0xFC01, rp, sizeof(rp), r, sizeof(r), 2000);
    if (n < 0)
        kprintf("bt: Intel Reset ohne Antwort (%d) - warte trotzdem auf den Start\n", n);
    if (wait_events(&boot_event, 3000) != 0)
        return dl_fail(-1, "Firmware meldet sich nicht (Ereignis 0x02)");
    info.boot_ms = (uint32_t)(time_ms() - tb);
    info.booted = 1;
    kprintf("bt: Firmware gestartet nach %u ms (Quelle %u, Reset-Art %u, Grund %u, DDC %u)\n", info.boot_ms,
            boot_payload[2], boot_payload[3], boot_payload[4], boot_payload[5]);
    thread_sleep_ms(10);
    query_locked();
    if (info.mode != BT_MODE_OPERATIONAL)
        return dl_fail(-6, "nach dem Start nicht im Betrieb (Read Version)");
    return dl_fail(0, "Firmware geladen und gestartet");
}

int bt_load_fw(void)
{
    if (!dev || !usb_alive(dev))
        return -1;
    mutex_lock(&bt_lock);
    logged = 0;
    int r = load_locked();
    mutex_unlock(&bt_lock);
    return r;
}

/* ---------- Stufe 3: HCI einrichten und Geraete suchen ---------- */

/* Befehl, der nur mit Command Status antwortet (Inquiry, Verbinden): 0 = angenommen */
int hci_cmd_status(uint16_t op, const void *param, uint8_t plen)
{
    uint8_t r[4];
    int n = hci_cmd_ex(op, param, plen, r, sizeof(r), 2000, 0, 0, 1);
    return n < 0 ? n : r[0] ? -4 : 0;
}

/* Befehl mit Command Complete, Status 0 erwartet */
int hci_ok(uint16_t op, const void *param, uint8_t plen, const char *what)
{
    uint8_t r[8];
    int n = hci_cmd(op, param, plen, r, sizeof(r), 2000);
    if (n < 1 || r[0] != 0) {
        kprintf("bt: %s (%#06x) fehlgeschlagen: %d, Status %#x\n", what, op, n, n >= 1 ? r[0] : 0);
        return -1;
    }
    return 0;
}

/* DDC (Geraete-Einstellungen von Intel, ibt-...ddc): Eintraege aus Laenge, Kennung (2), Wert - je einer per 0xFC8B */
static void load_ddc(void)
{
    char path[64];
    ksnprintf(path, sizeof(path), "/firmware/%s", info.fw_name);
    char *dot = path + strlen(path) - 4;
    if (strcmp(dot, ".sfi") != 0)
        return;
    memcpy(dot, ".ddc", 5);
    const VfsNode *node = vfs_lookup(path);
    info.ddc_records = 0;
    if (!node || node->is_dir) {
        kprintf("bt: %s fehlt - ohne DDC-Einstellungen weiter (Linux macht es genauso)\n", path + 10);
        return;
    }
    const uint8_t *d = node->data;
    uint32_t size = (uint32_t)node->size;
    for (uint32_t o = 0; o < size;) {
        uint32_t l = d[o] + 1u;
        if (o + l > size || l > 255)
            break;
        if (hci_ok(0xFC8B, d + o, (uint8_t)l, "DDC-Eintrag") != 0)
            break;
        info.ddc_records++;
        o += l;
    }
    kprintf("bt: %u DDC-Eintraege aus %s geladen\n", info.ddc_records, path + 10);
}

int hci_init_locked(void)
{
    if (info.mode != BT_MODE_OPERATIONAL) {
        int r = load_locked();
        if (r < 0)
            return r;
    }
    if (info.hci_ready)
        return 0;
    if (start_bulk() != 0)
        return -3;
    if (hci_ok(0x0C03, 0, 0, "HCI_Reset") != 0)
        return -6;
    load_ddc();
    uint8_t r[16];
    if (hci_cmd(0x1003, 0, 0, r, sizeof(r), 2000) >= 9 && r[0] == 0) /* Read Local Supported Features */
        memcpy(info.features, r + 1, 8);
    if (hci_cmd(0x1005, 0, 0, r, sizeof(r), 2000) >= 8 && r[0] == 0) { /* Read Buffer Size */
        info.acl_mtu = le16(r + 1);
        info.acl_pkts = le16(r + 4);
    }
    if (info.features[4] & 0x40 && hci_cmd(0x2002, 0, 0, r, sizeof(r), 2000) >= 4 && r[0] == 0) { /* LE Read Buffer Size */
        info.le_mtu = le16(r + 1);
        info.le_pkts = r[3];
    }
    /* Ereignisse wie Linux fuer BR/EDR (u.a. Inquiry Result mit RSSI, Extended Inquiry Result, LE Meta) */
    static const uint8_t mask[8] = {0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D};
    if (hci_ok(0x0C01, mask, 8, "Set Event Mask") != 0)
        return -6;
    uint8_t mode = 2; /* Inquiry-Ergebnisse mit RSSI bzw. EIR */
    hci_ok(0x0C45, &mode, 1, "Write Inquiry Mode");
    uint8_t ssp = 1; /* Secure Simple Pairing */
    hci_ok(0x0C56, &ssp, 1, "Write Simple Pairing Mode");
    static const uint8_t cod[3] = {0x0C, 0x01, 0x00}; /* Computer, Laptop */
    hci_ok(0x0C24, cod, 3, "Write Class of Device");
    static uint8_t name[248];
    memcpy(name, "MiniKernel", 11);
    hci_ok(0x0C13, name, sizeof(name), "Write Local Name");
    if (info.features[4] & 0x40) {
        static const uint8_t le_mask[8] = {0x1F, 0, 0, 0, 0, 0, 0, 0}; /* u.a. Advertising Report */
        hci_ok(0x2001, le_mask, 8, "LE Set Event Mask");
    }
    info.hci_ready = 1;
    bt_conn_start();
    kprintf("bt: HCI eingerichtet: ACL %u x %u Byte, LE %u x %u Byte, %s\n", info.acl_pkts, info.acl_mtu, info.le_pkts,
            info.le_mtu, info.features[4] & 0x40 ? "mit LE" : "ohne LE");
    return 0;
}

static int scan_locked(uint32_t seconds)
{
    int r = hci_init_locked();
    if (r < 0)
        return r;
    if (seconds < 2)
        seconds = 2;
    if (seconds > 30)
        seconds = 30;
    n_devs = 0;
    inq_done = 0;
    scanning = 1;
    uint64_t t0 = time_ms();
    int le = (info.features[4] & 0x40) != 0;
    if (le) { /* aktiv (fragt nach Namen in der Scan Response), 10 ms Fenster alle 10 ms, alle Geraete */
        static const uint8_t sp[7] = {0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00};
        static const uint8_t en[2] = {0x01, 0x00}; /* einschalten, Doppelte melden (fuer die Signalstaerke) */
        if (hci_ok(0x200B, sp, 7, "LE Set Scan Parameters") != 0 || hci_ok(0x200C, en, 2, "LE Set Scan Enable") != 0)
            le = 0;
    }
    /* Inquiry: allgemeiner Zugangscode 0x9E8B33, Dauer in 1,28 s, beliebig viele Antworten */
    uint8_t inq[5] = {0x33, 0x8B, 0x9E, (uint8_t)((seconds * 100 + 127) / 128), 0x00};
    int inq_ok = hci_cmd_status(0x0401, inq, 5) == 0;
    if (!inq_ok)
        kprintf("bt: Inquiry nicht gestartet - nur LE\n");
    uint64_t until = seconds * 1000ull + 1000;
    while (time_ms() - t0 < until && !(inq_ok && inq_done && time_ms() - t0 >= seconds * 1000ull)) {
        usb_lock(dev);
        usb_poll(dev);
        usb_unlock(dev);
        thread_sleep_ms(20);
    }
    if (inq_ok && !inq_done)
        hci_ok(0x0402, 0, 0, "Inquiry Cancel");
    if (le) {
        static const uint8_t dis[2] = {0x00, 0x00};
        hci_ok(0x200C, dis, 2, "LE Set Scan Enable (aus)");
    }
    scanning = 0;
    info.scan_ms = (uint32_t)(time_ms() - t0);
    info.scan_devs = n_devs;
    kprintf("bt: Suche fertig nach %u ms: %u Geraete\n", info.scan_ms, n_devs);
    return (int)n_devs;
}

int bt_scan(uint32_t seconds)
{
    if (!dev || !usb_alive(dev))
        return -1;
    mutex_lock(&bt_lock);
    logged = 0;
    int r = scan_locked(seconds);
    mutex_unlock(&bt_lock);
    return r;
}

int bt_device(unsigned i, BtDev *out)
{
    if (i >= n_devs)
        return -1;
    *out = devs[i];
    return 0;
}

int bt_info(BtInfo *out)
{
    *out = info;
    return 0;
}
