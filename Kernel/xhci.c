#include "usb.h"
#include "apic.h"
#include "blk.h"
#include "cpu.h"
#include "heap.h"
#include "kprintf.h"
#include "paging.h"
#include "pci.h"
#include "sched.h"
#include "string.h"

/* xHCI-Hostcontroller (USB 3.x; behandelt an den Root-Ports auch Low/Full/High-Speed-Geraete). Reines Polling, keine
 * Interrupts: ein Hintergrund-Thread verarbeitet Ereignisse und erkennt Anstecken/Abziehen. */

#define RING_TRBS 256

/* TRB-Typen */
#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_LINK         6
#define TRB_ENABLE_SLOT  9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EVAL_CTX     13
#define TRB_RESET_EP     14
#define TRB_SET_TR_DEQ   16
#define TRB_NOOP_CMD     23
#define TRB_EV_TRANSFER  32
#define TRB_EV_COMMAND   33
#define TRB_EV_PORT      34

/* Completion-Codes */
#define CC_SUCCESS       1
#define CC_STALL         6
#define CC_SHORT_PACKET  13

/* Operational-Register (relativ zu op) */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTS    0x400
#define CMD_RS      (1u << 0)
#define CMD_HCRST   (1u << 1)
#define STS_HCH     (1u << 0)
#define STS_CNR     (1u << 11)

/* PORTSC-Bits */
#define PORT_CCS    (1u << 0)
#define PORT_PED    (1u << 1)
#define PORT_PR     (1u << 4)
#define PORT_PP     (1u << 9)
#define PORT_CSC    (1u << 17)
#define PORT_PRC    (1u << 21)
#define PORT_WPR    (1u << 31)
#define PORT_CHANGE_BITS 0x00FE0000u /* CSC, PEC, WRC, OCC, PRC, PLC, CEC: schreiben von 1 loescht */

#define MAX_XHCI    4
#define MAX_DEVICES 32

typedef struct { uint32_t d0, d1, d2, d3; } Trb;

typedef struct {
    Trb     *trbs;
    uint32_t enq;
    uint32_t cycle;
} Ring;

typedef struct {
    Ring     ring;
    int      active;
    uint16_t mps;
    volatile int done;
    int      cc;
    uint32_t residual;
    /* wiederkehrender Interrupt-IN-Transfer */
    UsbInterruptCallback cb;
    void    *cb_buf;
    uint32_t cb_len;
    int      cb_errors;
} Ep;

struct Xhci;

struct UsbDevice {
    struct Xhci *x;
    int      slot, port, speed;
    volatile int alive;
    uint8_t *dev_ctx;   /* Ausgabe-Kontext (vom Controller gepflegt) */
    uint8_t *in_ctx;    /* Eingabe-Kontext fuer Kommandos */
    Ep       ep[32];    /* Index = DCI (Device Context Index): EP0 = 1, EPn OUT = 2n, EPn IN = 2n+1 */
    uint16_t vid, pid;
    uint8_t  cls;
    uint32_t driver;
    char     name[32];
    /* Hubs: Position in der Baumstruktur (route string, Eltern-Hub) und die angeschlossenen Geraete */
    struct UsbDevice *parent;   /* Hub, an dem das Geraet haengt (0 = direkt am Root-Port) */
    int      hub_port;          /* Port dieses Hubs */
    int      tier;              /* 1 = Root-Port, 2 = hinter einem Hub, ... */
    uint32_t route;             /* xHCI route string: 4 Bit je Hub-Ebene */
    int      is_hub, nports;
    struct UsbDevice *children[16];
    uint8_t  child_tries[16];
    char     path[16];          /* "1" (Root-Port 1), "1.3" (Port 3 des Hubs an Root-Port 1) usw. */
};

typedef struct Xhci {
    volatile uint8_t *cap, *op, *rt, *db;
    int      ports, slots, ctx_size;
    uint64_t *dcbaa;
    Ring     cmd;
    Trb     *events;
    uint32_t ev_deq, ev_cycle;
    volatile int cmd_done;
    int      cmd_cc, cmd_slot;
    uint8_t  port_proto[256];   /* 2 oder 3: USB-Hauptversion des Ports */
    uint8_t  port_changed[256];
    uint8_t  port_tries[256];
    UsbDevice *slot_dev[256];
    UsbDevice *port_dev[256];
    uint8_t *ctrl_buf;          /* 4 KiB DMA-Puffer fuer Control-Transfers */
    Mutex    lock;
    uint64_t last_scan_ms;
    PciDevice pci;
} Xhci;

static Xhci       *controllers[MAX_XHCI];
static int         controller_count;
static UsbDevice  *all_devices[MAX_DEVICES];
static int         device_total;
static int         thread_started;

/* ---------- Register und Hilfen ---------- */

static inline uint32_t rd32(volatile uint8_t *p, uint32_t off) { return *(volatile uint32_t *)(p + off); }
static inline void wr32(volatile uint8_t *p, uint32_t off, uint32_t v) { *(volatile uint32_t *)(p + off) = v; }
static inline void wr64(volatile uint8_t *p, uint32_t off, uint64_t v) { wr32(p, off, (uint32_t)v); wr32(p, off + 4, (uint32_t)(v >> 32)); }
static inline void barrier(void) { __asm__ __volatile__("mfence" : : : "memory"); }
static inline uint32_t lo(uint64_t v) { return (uint32_t)v; }
static inline uint32_t hi(uint64_t v) { return (uint32_t)(v >> 32); }

static inline uint32_t portsc(Xhci *x, int port) { return rd32(x->op, OP_PORTS + (uint32_t)(port - 1) * 0x10); }

/* Beim Schreiben von PORTSC nur die "neutralen" Bits erhalten: PED und die Aenderungsbits sind write-1-to-clear und
 * duerfen nicht versehentlich mitgeschrieben werden. */
static void portsc_write(Xhci *x, int port, uint32_t current, uint32_t set)
{
    uint32_t neutral = current & (PORT_PP | (3u << 14) | (7u << 25));
    wr32(x->op, OP_PORTS + (uint32_t)(port - 1) * 0x10, neutral | set);
}

static void doorbell(Xhci *x, int slot, int target)
{
    barrier();
    wr32(x->db, (uint32_t)slot * 4, (uint32_t)target);
}

static uint32_t *ctx_entry(Xhci *x, uint8_t *base, int index)
{
    return (uint32_t *)(base + (uint32_t)index * (uint32_t)x->ctx_size);
}

/* ---------- Ringe ---------- */

static int ring_init(Ring *r)
{
    r->trbs = blk_dma_alloc(RING_TRBS * sizeof(Trb));
    r->enq = 0;
    r->cycle = 1;
    return r->trbs ? 0 : -1;
}

/* Haengt ein TRB an. Das Cycle-Bit wird zuletzt geschrieben: erst damit gehoert der Eintrag dem Controller. */
static void ring_push(Ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    Trb *t = &r->trbs[r->enq];
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    barrier();
    t->d3 = d3 | r->cycle;
    r->enq++;
    if (r->enq == RING_TRBS - 1) { /* letzter Eintrag ist ein Link-TRB zurueck an den Anfang (mit Toggle Cycle) */
        Trb *l = &r->trbs[r->enq];
        l->d0 = lo((uint64_t)r->trbs);
        l->d1 = hi((uint64_t)r->trbs);
        l->d2 = 0;
        barrier();
        l->d3 = (TRB_LINK << 10) | (1u << 1) | r->cycle;
        r->cycle ^= 1;
        r->enq = 0;
    }
}

/* ---------- Ereignisse ---------- */

static void requeue_interrupt(UsbDevice *d, int dci)
{
    Ep *e = &d->ep[dci];
    ring_push(&e->ring, lo((uint64_t)e->cb_buf), hi((uint64_t)e->cb_buf), e->cb_len,
              (TRB_NORMAL << 10) | (1u << 5) | (1u << 2)); /* IOC + ISP */
    doorbell(d->x, d->slot, dci);
}

static void process_events(Xhci *x)
{
    int any = 0;
    for (;;) {
        Trb *t = &x->events[x->ev_deq];
        if ((t->d3 & 1) != x->ev_cycle)
            break;
        barrier();
        uint32_t type = (t->d3 >> 10) & 0x3F;

        if (type == TRB_EV_TRANSFER) {
            int slot = (int)(t->d3 >> 24), dci = (int)((t->d3 >> 16) & 0x1F), cc = (int)(t->d2 >> 24);
            uint32_t residual = t->d2 & 0xFFFFFF;
            UsbDevice *d = x->slot_dev[slot];
            if (d && dci > 0 && dci < 32) {
                Ep *e = &d->ep[dci];
                if (e->cb) {
                    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
                        e->cb_errors = 0;
                        e->cb(d, e->cb_buf, e->cb_len - residual);
                        requeue_interrupt(d, dci);
                    } else if (d->alive && ++e->cb_errors < 20) { /* Fehler: erneut versuchen, aber nicht endlos */
                        requeue_interrupt(d, dci);
                    }
                } else {
                    e->cc = cc;
                    e->residual = residual;
                    e->done = 1;
                }
            }
        } else if (type == TRB_EV_COMMAND) {
            x->cmd_cc = (int)(t->d2 >> 24);
            x->cmd_slot = (int)(t->d3 >> 24);
            x->cmd_done = 1;
        } else if (type == TRB_EV_PORT) {
            x->port_changed[(t->d0 >> 24) & 0xFF] = 1;
        }

        x->ev_deq++;
        if (x->ev_deq == RING_TRBS) {
            x->ev_deq = 0;
            x->ev_cycle ^= 1;
        }
        any = 1;
    }
    if (any) /* ERDP: neuer Lesezeiger; Bit 3 (Event Handler Busy) loeschen */
        wr64(x->rt, 0x38, (uint64_t)&x->events[x->ev_deq] | (1u << 3));
}

/* ---------- Kommandos ---------- */

/* Fuehrt ein Kommando aus. 0 = Erfolg (x->cmd_slot enthaelt die Slot-ID), -1 = Timeout, sonst -(Completion-Code). */
static int xhci_cmd(Xhci *x, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    x->cmd_done = 0;
    ring_push(&x->cmd, d0, d1, d2, d3);
    doorbell(x, 0, 0);
    if (!WAIT_UNTIL((process_events(x), x->cmd_done), 3000)) {
        kprintf("xhci: Kommando (Typ %u) ohne Antwort\n", (d3 >> 10) & 0x3F);
        kprintf("xhci: USBCMD %#x USBSTS %#x CRCR %#x%08x, Ereignis[0].d3 %#x, Kommando[0].d3 %#x, ERDP %#x\n",
                rd32(x->op, OP_USBCMD), rd32(x->op, OP_USBSTS), rd32(x->op, OP_CRCR + 4), rd32(x->op, OP_CRCR),
                x->events[0].d3, x->cmd.trbs[0].d3, rd32(x->rt, 0x38));
        return -1;
    }
    return x->cmd_cc == CC_SUCCESS ? 0 : -x->cmd_cc;
}

/* ---------- Transfers ---------- */

int usb_alive(const UsbDevice *d) { return d->alive; }
void usb_lock(UsbDevice *d)       { mutex_lock(&d->x->lock); }
void usb_unlock(UsbDevice *d)     { mutex_unlock(&d->x->lock); }

int usb_control(UsbDevice *d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, void *buf, uint16_t len)
{
    Xhci *x = d->x;
    Ep *e = &d->ep[1];
    if (!d->alive)
        return -2;
    if (len > 4096)
        return -4;

    int in = (request_type & 0x80) != 0;
    if (!in && len)
        memcpy(x->ctrl_buf, buf, len);
    e->done = 0;

    uint32_t setup0 = request_type | ((uint32_t)request << 8) | ((uint32_t)value << 16);
    uint32_t setup1 = index | ((uint32_t)len << 16);
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    ring_push(&e->ring, setup0, setup1, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16)); /* IDT: die 8 Bytes stehen im TRB */
    if (len)
        ring_push(&e->ring, lo((uint64_t)x->ctrl_buf), hi((uint64_t)x->ctrl_buf), len, (TRB_DATA << 10) | ((uint32_t)in << 16));
    int status_in = !(len && in); /* Status-Phase in Gegenrichtung der Daten; ohne Daten: IN */
    ring_push(&e->ring, 0, 0, 0, (TRB_STATUS << 10) | ((uint32_t)status_in << 16) | (1u << 5)); /* IOC */
    doorbell(x, d->slot, 1);

    if (!WAIT_UNTIL((process_events(x), e->done || !d->alive), 3000))
        return -1;
    if (!d->alive)
        return -2;
    if (e->cc != CC_SUCCESS && e->cc != CC_SHORT_PACKET)
        return -e->cc;
    if (in && len) {
        memcpy(buf, x->ctrl_buf, len);
        return (int)len - (int)e->residual;
    }
    return len;
}

int usb_bulk(UsbDevice *d, uint8_t endpoint_address, void *buf, uint32_t len, uint32_t timeout_ms)
{
    Xhci *x = d->x;
    int dci = ((endpoint_address & 0x0F) << 1) | (endpoint_address >> 7);
    Ep *e = &d->ep[dci];
    if (!d->alive)
        return -2;
    if (!e->active)
        return -3;

    e->done = 0;
    uint64_t addr = (uint64_t)buf;
    uint32_t remaining = len;
    do { /* ein TD aus mehreren TRBs: kein TRB darf eine 64-KiB-Grenze ueberqueren */
        uint32_t chunk = 65536 - (uint32_t)(addr & 0xFFFF);
        if (chunk > remaining)
            chunk = remaining;
        remaining -= chunk;
        uint32_t packets_left = e->mps ? (remaining + e->mps - 1) / e->mps : 0;
        if (packets_left > 31)
            packets_left = 31;
        ring_push(&e->ring, lo(addr), hi(addr), chunk | (packets_left << 17),
                  (TRB_NORMAL << 10) | (remaining ? (1u << 4) : (1u << 5))); /* Chain oder IOC */
        addr += chunk;
    } while (remaining);
    doorbell(x, d->slot, dci);

    if (!WAIT_UNTIL((process_events(x), e->done || !d->alive), timeout_ms))
        return -1;
    if (!d->alive)
        return -2;
    if (e->cc != CC_SUCCESS && e->cc != CC_SHORT_PACKET)
        return -e->cc;
    return (int)(len - e->residual);
}

int usb_endpoint_recover(UsbDevice *d, uint8_t endpoint_address)
{
    Xhci *x = d->x;
    int dci = ((endpoint_address & 0x0F) << 1) | (endpoint_address >> 7);
    Ep *e = &d->ep[dci];
    if (!e->active)
        return -1;

    /* Controller: Endpunkt aus dem Halted-Zustand holen und den Ring hinter das fehlgeschlagene TD setzen */
    xhci_cmd(x, 0, 0, 0, (TRB_RESET_EP << 10) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24));
    uint64_t deq = (uint64_t)&e->ring.trbs[e->ring.enq] | e->ring.cycle; /* Bit 0 = Dequeue Cycle State */
    xhci_cmd(x, lo(deq), hi(deq), 0, (TRB_SET_TR_DEQ << 10) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24));
    /* Geraet: ENDPOINT_HALT loeschen (CLEAR_FEATURE) */
    usb_control(d, 0x02, 1, 0, endpoint_address, 0, 0);
    return 0;
}

int usb_interrupt_start(UsbDevice *d, uint8_t endpoint_address, void *buf, uint32_t len, UsbInterruptCallback callback)
{
    int dci = ((endpoint_address & 0x0F) << 1) | (endpoint_address >> 7);
    Ep *e = &d->ep[dci];
    if (!e->active)
        return -1;
    e->cb = callback;
    e->cb_buf = buf;
    e->cb_len = len;
    e->cb_errors = 0;
    requeue_interrupt(d, dci);
    return 0;
}

/* ---------- Endpunkte konfigurieren ---------- */

static int interval_exponent(int speed, int type, int b_interval)
{
    if (type != 3) /* nur Interrupt-Endpunkte brauchen ein Intervall */
        return 0;
    if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) { /* bInterval in ms -> 2^n * 125 us */
        int ms = b_interval ? b_interval : 1, n = 3;
        while ((1 << n) < ms * 8 && n < 10)
            n++;
        return n;
    }
    int n = b_interval - 1; /* Hoch-/SuperSpeed: bInterval ist schon 2^(n) * 125 us + 1 */
    return n < 0 ? 0 : (n > 15 ? 15 : n);
}

int usb_add_endpoints(UsbDevice *d, const UsbEpDesc *eps, int count)
{
    Xhci *x = d->x;
    memset(d->in_ctx, 0, (size_t)x->ctx_size * 33);

    /* Slot-Kontext aus dem aktuellen Geraetekontext uebernehmen und die Zahl der Eintraege anheben */
    memcpy(ctx_entry(x, d->in_ctx, 1), ctx_entry(x, d->dev_ctx, 0), (size_t)x->ctx_size);
    uint32_t *slot = ctx_entry(x, d->in_ctx, 1);
    int max_dci = (int)(slot[0] >> 27);
    uint32_t add = 1; /* Bit 0: Slot-Kontext */

    Ring *rings[8];
    for (int i = 0; i < count; i++) {
        int addr = eps[i].address, type = eps[i].attrs & 3;
        int dci = ((addr & 0x0F) << 1) | (addr >> 7);
        int in = (addr & 0x80) != 0;
        if (dci < 2 || dci > 31 || type == 1) /* Isochron wird nicht unterstuetzt */
            return -1;
        Ep *e = &d->ep[dci];
        if (ring_init(&e->ring) != 0)
            return -1;
        rings[i] = &e->ring;
        e->mps = eps[i].max_packet & 0x7FF;
        e->done = 0;

        uint32_t ep_type = type == 2 ? (in ? 6 : 2) : (in ? 7 : 3); /* Bulk / Interrupt */
        uint32_t *c = ctx_entry(x, d->in_ctx, 1 + dci);
        c[0] = (uint32_t)interval_exponent(d->speed, type, eps[i].interval) << 16;
        c[1] = ((uint32_t)e->mps << 16) | (ep_type << 3) | (3u << 1); /* CErr = 3 */
        c[2] = lo((uint64_t)e->ring.trbs) | 1;                          /* Dequeue Cycle State = 1 */
        c[3] = hi((uint64_t)e->ring.trbs);
        c[4] = (uint32_t)e->mps | (type == 3 ? (uint32_t)e->mps << 16 : 0); /* mittlere TRB-Laenge, Max ESIT Payload */
        add |= 1u << dci;
        if (dci > max_dci)
            max_dci = dci;
    }
    slot[0] = (slot[0] & 0x07FFFFFF) | ((uint32_t)max_dci << 27);
    uint32_t *icc = ctx_entry(x, d->in_ctx, 0);
    icc[0] = 0;
    icc[1] = add;

    int r = xhci_cmd(x, lo((uint64_t)d->in_ctx), hi((uint64_t)d->in_ctx), 0,
                     (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24));
    if (r != 0) {
        kprintf("xhci: Configure Endpoint fehlgeschlagen (%d)\n", r);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        int addr = eps[i].address;
        d->ep[((addr & 0x0F) << 1) | (addr >> 7)].active = 1;
    }
    (void)rings;
    return 0;
}

/* ---------- Ports und Enumeration ---------- */

/* Setzt den Port zurueck (USB2) bzw. wartet, bis er aktiv ist (USB3). Liefert die Geschwindigkeit oder -1. */
static int port_enable(Xhci *x, int port)
{
    uint32_t v = portsc(x, port);
    if (!(v & PORT_CCS))
        return -1;

    if (x->port_proto[port] == 3) { /* USB3: die Verbindung wird vom Controller selbst aufgebaut */
        if (!WAIT_UNTIL(portsc(x, port) & PORT_PED, 600)) {
            v = portsc(x, port);
            portsc_write(x, port, v, PORT_WPR); /* Warm Reset */
            WAIT_UNTIL(portsc(x, port) & PORT_PED, 1000);
        }
    } else { /* USB2: immer zuruecksetzen (die Firmware kann dem Geraet schon eine Adresse gegeben haben) */
        portsc_write(x, port, v, PORT_PR);
        WAIT_UNTIL(portsc(x, port) & PORT_PRC, 800);
        v = portsc(x, port);
        portsc_write(x, port, v, PORT_PRC | PORT_CSC); /* Aenderungsbits loeschen */
        (void)WAIT_UNTIL(0, 20);                        /* Erholzeit nach dem Reset (USB: 10 ms) */
    }

    v = portsc(x, port);
    portsc_write(x, port, v, v & PORT_CHANGE_BITS);
    v = portsc(x, port);
    if (!(v & PORT_PED) || !(v & PORT_CCS))
        return -1;
    return (int)((v >> 10) & 0xF);
}

static int get_descriptor(UsbDevice *d, uint8_t type, uint8_t index, uint16_t lang, void *buf, uint16_t len)
{
    return usb_control(d, 0x80, 6, (uint16_t)(((uint16_t)type << 8) | index), lang, buf, len);
}

static int add_to_list(UsbDevice *d)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!all_devices[i]) {
            all_devices[i] = d;
            device_total++;
            return 0;
        }
    }
    return -1;
}

static void remove_device(Xhci *x, UsbDevice *d)
{
    if (d->is_hub) { /* erst alles, was am Hub haengt */
        for (int i = 0; i < 16; i++)
            if (d->children[i])
                remove_device(x, d->children[i]);
    }
    d->alive = 0;
    if (d->parent)
        d->parent->children[d->hub_port] = 0;
    else
        x->port_dev[d->port] = 0;
    x->slot_dev[d->slot] = 0;
    xhci_cmd(x, 0, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)d->slot << 24));
    x->dcbaa[d->slot] = 0;
    /* Die Struktur bleibt (mit alive = 0) bestehen: Klassentreiber (z.B. ein registriertes Blockgeraet) halten Zeiger */
    kprintf("usb: %s (Port %s) entfernt\n", d->name, d->path);
}

static int hub_init(Xhci *x, UsbDevice *d);

/* Legt ein Geraet an: parent = Hub (0 = Root-Port), hub_port = Port des Hubs, root_port = Root-Port des Controllers */
static UsbDevice *enumerate_dev(Xhci *x, UsbDevice *parent, int hub_port, int root_port, int speed)
{
    int port = root_port;
    if (xhci_cmd(x, 0, 0, 0, TRB_ENABLE_SLOT << 10) != 0)
        return 0;
    int slot = x->cmd_slot;

    UsbDevice *d = kcalloc(1, sizeof(*d));
    if (!d) {
        xhci_cmd(x, 0, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)slot << 24));
        return 0;
    }
    d->x = x;
    d->slot = slot;
    d->port = port;
    d->speed = speed;
    d->alive = 1;
    d->parent = parent;
    d->hub_port = hub_port;
    d->tier = parent ? parent->tier + 1 : 1;
    d->route = parent ? parent->route | ((uint32_t)(hub_port > 15 ? 15 : hub_port) << (4 * (parent->tier - 1))) : 0;
    if (parent)
        ksnprintf(d->path, sizeof(d->path), "%s.%d", parent->path, hub_port);
    else
        ksnprintf(d->path, sizeof(d->path), "%d", port);
    d->dev_ctx = blk_dma_alloc(4096);
    d->in_ctx = blk_dma_alloc(4096);
    if (!d->dev_ctx || !d->in_ctx || ring_init(&d->ep[1].ring) != 0)
        goto fail;
    x->slot_dev[slot] = d;
    x->dcbaa[slot] = (uint64_t)d->dev_ctx;

    /* Adresse vergeben: Slot- und EP0-Kontext vorbereiten. Max. Paketgroesse von EP0 zunaechst raten (LS/FS: 8). */
    uint16_t mps0 = (speed >= USB_SPEED_SUPER) ? 512 : (speed == USB_SPEED_HIGH ? 64 : 8);
    d->ep[1].mps = mps0;
    d->ep[1].active = 1;
    uint32_t *icc = ctx_entry(x, d->in_ctx, 0);
    icc[1] = 3; /* Slot + EP0 hinzufuegen */
    uint32_t *sc = ctx_entry(x, d->in_ctx, 1);
    sc[0] = (1u << 27) | ((uint32_t)speed << 20) | (d->route & 0xFFFFF);
    sc[1] = (uint32_t)port << 16;
    if (parent && speed < USB_SPEED_HIGH) { /* Low/Full-Speed hinter einem High-Speed-Hub: dessen Transaction Translator benutzen */
        UsbDevice *tt = parent;
        int tt_port = hub_port;
        while (tt && tt->speed != USB_SPEED_HIGH) { /* ein FS-Hub reicht die Aufgabe an den naechsten HS-Hub darueber weiter */
            tt_port = tt->hub_port;
            tt = tt->parent;
        }
        if (tt)
            sc[2] = (uint32_t)tt->slot | ((uint32_t)tt_port << 8);
    }
    uint32_t *e0 = ctx_entry(x, d->in_ctx, 2);
    e0[1] = ((uint32_t)mps0 << 16) | (4u << 3) | (3u << 1); /* Control, CErr = 3 */
    e0[2] = lo((uint64_t)d->ep[1].ring.trbs) | 1;
    e0[3] = hi((uint64_t)d->ep[1].ring.trbs);
    e0[4] = 8;
    if (xhci_cmd(x, lo((uint64_t)d->in_ctx), hi((uint64_t)d->in_ctx), 0,
                 (TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24)) != 0) {
        kprintf("xhci: Address Device fehlgeschlagen (Port %d)\n", port);
        goto fail;
    }
    (void)WAIT_UNTIL(0, 10); /* Erholzeit nach SET_ADDRESS */

    /* Geraetebeschreibung: erst 8 Bytes (bMaxPacketSize0), bei Full-Speed EP0 anpassen, dann komplett */
    uint8_t desc[18];
    if (get_descriptor(d, 1, 0, 0, desc, 8) < 8) {
        kprintf("xhci: Geraetedeskriptor nicht lesbar (Port %d)\n", port);
        goto fail;
    }
    uint16_t real_mps = speed >= USB_SPEED_SUPER ? (uint16_t)(1u << desc[7]) : desc[7];
    if (real_mps && real_mps != d->ep[1].mps && speed == USB_SPEED_FULL) {
        d->ep[1].mps = real_mps;
        memset(d->in_ctx, 0, (size_t)x->ctx_size * 33);
        memcpy(ctx_entry(x, d->in_ctx, 1), ctx_entry(x, d->dev_ctx, 0), (size_t)x->ctx_size);
        memcpy(ctx_entry(x, d->in_ctx, 2), ctx_entry(x, d->dev_ctx, 1), (size_t)x->ctx_size);
        ctx_entry(x, d->in_ctx, 0)[1] = 3;
        uint32_t *ep0 = ctx_entry(x, d->in_ctx, 2);
        ep0[1] = (ep0[1] & 0xFFFF) | ((uint32_t)real_mps << 16);
        xhci_cmd(x, lo((uint64_t)d->in_ctx), hi((uint64_t)d->in_ctx), 0, (TRB_EVAL_CTX << 10) | ((uint32_t)slot << 24));
    }
    if (get_descriptor(d, 1, 0, 0, desc, 18) < 18) {
        kprintf("xhci: Geraetedeskriptor unvollstaendig (Port %d)\n", port);
        goto fail;
    }
    d->vid = (uint16_t)(desc[8] | (desc[9] << 8));
    d->pid = (uint16_t)(desc[10] | (desc[11] << 8));
    d->cls = desc[4];
    ksnprintf(d->name, sizeof(d->name), "USB %04x:%04x", d->vid, d->pid);

    /* Konfiguration (erste) lesen und Schnittstellen zerlegen */
    uint8_t cfg[512];
    if (get_descriptor(d, 2, 0, 0, cfg, 9) < 9)
        goto fail;
    uint32_t total = (uint32_t)(cfg[2] | (cfg[3] << 8));
    if (total > sizeof(cfg))
        total = sizeof(cfg);
    if (get_descriptor(d, 2, 0, 0, cfg, (uint16_t)total) < (int)total && total > 9)
        goto fail;

    UsbIface ifaces[4];
    int n_if = 0;
    for (uint32_t p = 0; p + 2 <= total && cfg[p] >= 2;) {
        uint8_t len = cfg[p], type = cfg[p + 1];
        if (type == 4 && len >= 9 && cfg[p + 3] == 0 && n_if < 4) { /* Interface, nur Alternativ-Einstellung 0 */
            UsbIface *i = &ifaces[n_if++];
            memset(i, 0, sizeof(*i));
            i->number = cfg[p + 2];
            i->cls = cfg[p + 5];
            i->sub = cfg[p + 6];
            i->proto = cfg[p + 7];
        } else if (type == 5 && len >= 7 && n_if > 0) { /* Endpunkt der aktuellen Schnittstelle */
            UsbIface *i = &ifaces[n_if - 1];
            if (i->ep_count < 8) {
                UsbEpDesc *e = &i->eps[i->ep_count++];
                e->address = cfg[p + 2];
                e->attrs = cfg[p + 3];
                e->max_packet = (uint16_t)(cfg[p + 4] | (cfg[p + 5] << 8));
                e->interval = cfg[p + 6];
            }
        }
        p += len;
    }

    if (usb_control(d, 0x00, 9, cfg[5], 0, 0, 0) < 0) { /* SET_CONFIGURATION */
        kprintf("xhci: SET_CONFIGURATION fehlgeschlagen (Port %d)\n", port);
        goto fail;
    }

    static const char *speeds[] = {"?", "Full", "Low", "High", "Super", "Super+"};
    kprintf("usb: Port %s: %s, %s-Speed, Klasse %02x, %d Schnittstelle(n)\n", d->path, d->name,
            speed < 6 ? speeds[speed] : "?", d->cls, n_if);

    add_to_list(d);
    if (parent)
        parent->children[hub_port] = d;
    else
        x->port_dev[port] = d;
    for (int i = 0; i < n_if; i++) {
        if (ifaces[i].cls == 3 && ifaces[i].sub == 1 && ifaces[i].proto == 2) { /* Maus (Boot-Protokoll) */
            if (usb_mouse_probe(d, &ifaces[i]) == 0 && !d->driver)
                d->driver = 4;
        } else if (ifaces[i].cls == 3 && ifaces[i].sub == 1 && ifaces[i].proto == 1) {
            if (usb_hid_probe(d, &ifaces[i]) == 0 && !d->driver)
                d->driver = 1;
        } else if (ifaces[i].cls == 3) { /* sonstiges HID: Maus/Tablet ohne Boot-Kennung? (Report-Deskriptor mit X/Y) */
            if (usb_mouse_probe(d, &ifaces[i]) == 0 && !d->driver)
                d->driver = 4;
        } else if (ifaces[i].cls == 8 && ifaces[i].sub == 6 && ifaces[i].proto == 0x50) {
            if (!d->driver && usb_msc_probe(d, &ifaces[i], d->name) == 0)
                d->driver = 2;
        } else if (ifaces[i].cls == 9) {
            if (!d->driver && hub_init(x, d) == 0)
                d->driver = 3;
        }
    }
    if (!d->driver)
        kprintf("usb: %s: keine passende Klasse (Tastatur/Maus/Massenspeicher/Hub), Geraet bleibt ungenutzt\n", d->name);
    return d;

fail:
    x->slot_dev[slot] = 0;
    x->dcbaa[slot] = 0;
    xhci_cmd(x, 0, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)slot << 24));
    kfree(d);
    return 0;
}

static UsbDevice *enumerate(Xhci *x, int port)
{
    int speed = port_enable(x, port);
    if (speed <= 0)
        return 0;
    return enumerate_dev(x, 0, 0, port, speed);
}

/* ---------- Hubs ---------- */

/* Hub-Anfragen (Klassen-Requests). Port-Features: 4 RESET, 8 POWER; Aenderungsbits loeschen: 16 CONNECTION, 17 ENABLE,
 * 18 SUSPEND, 19 OVER_CURRENT, 20 RESET. */
static int hub_set_feature(UsbDevice *h, int port, int feature)
{
    return usb_control(h, 0x23, 3, (uint16_t)feature, (uint16_t)port, 0, 0);
}

static int hub_clear_feature(UsbDevice *h, int port, int feature)
{
    return usb_control(h, 0x23, 1, (uint16_t)feature, (uint16_t)port, 0, 0);
}

static int hub_port_status(UsbDevice *h, int port, uint16_t *status, uint16_t *change)
{
    uint8_t b[4];
    if (usb_control(h, 0xA3, 0, 0, (uint16_t)port, b, 4) < 4)
        return -1;
    *status = (uint16_t)(b[0] | (b[1] << 8));
    *change = (uint16_t)(b[2] | (b[3] << 8));
    return 0;
}

static int hub_reset_done(UsbDevice *h, int port)
{
    uint16_t st, ch;
    return hub_port_status(h, port, &st, &ch) != 0 || (ch & (1u << 4));
}

/* Setzt einen Hub-Port zurueck und liefert die Geschwindigkeit des Geraets dahinter (oder -1) */
static int hub_port_enable(UsbDevice *h, int port, uint16_t status)
{
    int ss = h->speed >= USB_SPEED_SUPER;
    if (!(ss && (status & 2))) { /* USB3-Ports bauen die Verbindung selbst auf; sonst zuruecksetzen */
        if (hub_set_feature(h, port, 4) < 0)
            return -1;
        WAIT_UNTIL(hub_reset_done(h, port), 1000);
        hub_clear_feature(h, port, 20);
        (void)WAIT_UNTIL(0, 20); /* Erholzeit nach dem Reset */
    }
    uint16_t st, ch;
    if (hub_port_status(h, port, &st, &ch) != 0 || !(st & 1) || !(st & 2))
        return -1;
    if (ss)
        return USB_SPEED_SUPER;
    return (st & (1u << 10)) ? USB_SPEED_HIGH : (st & (1u << 9)) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* Erkennt neue und entfernte Geraete an den Ports eines Hubs */
static void scan_hub(Xhci *x, UsbDevice *h, int initial)
{
    for (int p = 1; p <= h->nports && p <= 15 && h->alive; p++) {
        uint16_t st, ch;
        if (hub_port_status(h, p, &st, &ch) != 0)
            continue;
        for (int bit = 0; bit < 5; bit++) /* Aenderungsbits quittieren */
            if (ch & (1u << bit))
                hub_clear_feature(h, p, 16 + bit);
        int connected = (st & 1) != 0;

        if (!connected) {
            h->child_tries[p] = 0;
            if (h->children[p])
                remove_device(x, h->children[p]);
            continue;
        }
        if (h->children[p] || h->child_tries[p] >= 3 || h->tier >= 5)
            continue;
        h->child_tries[p]++;
        int speed = hub_port_enable(h, p, st);
        UsbDevice *d = speed > 0 ? enumerate_dev(x, h, p, h->port, speed) : 0;
        if (d)
            h->child_tries[p] = 0;
        else if (!initial)
            kprintf("usb: Port %s.%d: Geraet laesst sich nicht ansprechen (Versuch %d)\n", h->path, p, h->child_tries[p]);
    }
}

/* Richtet einen Hub ein: Hub-Deskriptor lesen, Slot-Kontext (Hub, Portzahl, Think Time) setzen, Ports einschalten */
static int hub_init(Xhci *x, UsbDevice *d)
{
    int ss = d->speed >= USB_SPEED_SUPER;
    uint8_t hd[12];
    memset(hd, 0, sizeof(hd));
    int n = usb_control(d, 0xA0, 6, (uint16_t)((ss ? 0x2A : 0x29) << 8), 0, hd, ss ? 12 : 8);
    if (n < 7 || hd[2] == 0) {
        kprintf("usb: %s: Hub-Deskriptor nicht lesbar\n", d->name);
        return -1;
    }
    int nports = hd[2], ttt = ss ? 0 : (hd[3] >> 5) & 3, pwr_ms = hd[5] * 2;
    if (nports > 15) { /* der route string kennt nur 4 Bit je Ebene */
        kprintf("usb: %s: Hub mit %d Ports, nur die ersten 15 werden benutzt\n", d->name, nports);
    }

    /* Slot-Kontext: Hub-Flag, Portzahl, Think Time */
    memset(d->in_ctx, 0, (size_t)x->ctx_size * 33);
    memcpy(ctx_entry(x, d->in_ctx, 1), ctx_entry(x, d->dev_ctx, 0), (size_t)x->ctx_size);
    uint32_t *slot = ctx_entry(x, d->in_ctx, 1);
    slot[0] |= 1u << 26;
    slot[1] = (slot[1] & 0x00FFFFFF) | ((uint32_t)nports << 24);
    slot[2] = (slot[2] & ~(3u << 16)) | ((uint32_t)ttt << 16);
    ctx_entry(x, d->in_ctx, 0)[1] = 1; /* nur den Slot-Kontext auswerten */
    int r = xhci_cmd(x, lo((uint64_t)d->in_ctx), hi((uint64_t)d->in_ctx), 0, (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24));
    if (r != 0) {
        kprintf("usb: %s: Hub laesst sich nicht konfigurieren (%d)\n", d->name, r);
        return -1;
    }
    if (ss) /* SuperSpeed-Hubs muessen ihre Tiefe im Baum kennen */
        usb_control(d, 0x20, 12, (uint16_t)(d->tier - 1), 0, 0, 0);

    d->is_hub = 1;
    d->nports = nports;
    for (int p = 1; p <= nports && p <= 15; p++)
        hub_set_feature(d, p, 8); /* PORT_POWER */
    kprintf("usb: %s ist ein %sHub mit %d Ports\n", d->name, ss ? "SuperSpeed-" : "", nports);
    (void)WAIT_UNTIL(0, pwr_ms + 150); /* Strom stabil, Geraete melden sich */
    scan_hub(x, d, 1);
    return 0;
}

/* Erkennt neue und entfernte Geraete an den Root-Ports (Aufruf mit gehaltener Sperre) */
static void scan_ports(Xhci *x, int initial)
{
    for (int p = 1; p <= x->ports; p++) {
        uint32_t v = portsc(x, p);
        if (v & PORT_CHANGE_BITS)
            portsc_write(x, p, v, v & PORT_CHANGE_BITS); /* Aenderungsbits quittieren */
        int connected = (v & PORT_CCS) != 0;

        if (connected && !x->port_dev[p]) {
            if (x->port_tries[p] < 3) { /* nicht endlos versuchen, wenn ein Geraet sich nicht melden will */
                x->port_tries[p]++;
                UsbDevice *d = enumerate(x, p);
                if (d)
                    x->port_tries[p] = 0;
                else if (!initial)
                    kprintf("usb: Port %d: Geraet laesst sich nicht ansprechen (Versuch %d)\n", p, x->port_tries[p]);
            }
        } else if (!connected) {
            x->port_tries[p] = 0;
            if (x->port_dev[p])
                remove_device(x, x->port_dev[p]);
        }
    }
    for (int i = 0; i < MAX_DEVICES; i++) { /* Hubs: ihre Ports abfragen (auch Hubs, die dabei neu dazukommen) */
        UsbDevice *h = all_devices[i];
        if (h && h->x == x && h->alive && h->is_hub)
            scan_hub(x, h, initial);
    }
}

/* ---------- Controller ---------- */

static int wait_clear(volatile uint8_t *base, uint32_t off, uint32_t mask, int ms)
{
    return WAIT_UNTIL(!(rd32(base, off) & mask), ms);
}

static int init_controller(Xhci *x, const PciDevice *pci)
{
    x->pci = *pci;
    uint64_t bar = pci_bar_mem(pci, 0);
    if (!bar) {
        kprintf("xhci: %02x:%02x.%u ohne BAR0\n", pci->bus, pci->dev, pci->fn);
        return -1;
    }
    pci_enable(pci, 0, 1, 1);
    pci_set_driver(pci, "xhci");

    if (paging_map_mmio(bar, 0x1000) != 0)
        return -1;
    x->cap = (volatile uint8_t *)bar;
    uint32_t hcs1 = rd32(x->cap, 0x04), hcs2 = rd32(x->cap, 0x08), hcc1 = rd32(x->cap, 0x10);
    uint32_t caplen = x->cap[0];
    uint32_t dboff = rd32(x->cap, 0x14) & ~3u, rtsoff = rd32(x->cap, 0x18) & ~0x1Fu;
    x->ports = (int)(hcs1 >> 24);
    x->slots = (int)(hcs1 & 0xFF);
    x->ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;

    uint64_t need = caplen + OP_PORTS + (uint64_t)x->ports * 0x10;
    if (dboff + 4ULL * (x->slots + 1) > need)
        need = dboff + 4ULL * (x->slots + 1);
    if (rtsoff + 0x40 > need)
        need = rtsoff + 0x40;
    if (paging_map_mmio(bar, need + 0x100) != 0)
        return -1;
    x->op = x->cap + caplen;
    x->rt = x->cap + rtsoff;
    x->db = x->cap + dboff;
    if (x->slots > 255)
        x->slots = 255;

    /* Erweiterte Faehigkeiten: BIOS-Uebergabe (USB Legacy Support) und Protokolle der Ports */
    uint64_t mapped = need + 0x100;
    int guard = 0;
    for (uint32_t off = ((hcc1 >> 16) & 0xFFFF) * 4; off && guard++ < 64;) {
        if (off + 0x20ULL > mapped) { /* die Liste liegt oft weit hinter den Registern (z.B. bei 0x8000) */
            mapped = off + 0x1000ULL;
            if (paging_map_mmio(bar, mapped) != 0)
                return -1;
        }
        uint32_t c = rd32(x->cap, off);
        uint32_t id = c & 0xFF, next = (c >> 8) & 0xFF;
        if (id == 1) { /* USB Legacy Support: dem BIOS den Controller abnehmen */
            if (c & (1u << 16)) {
                wr32(x->cap, off, c | (1u << 24)); /* OS Owned Semaphore */
                if (!WAIT_UNTIL(!(rd32(x->cap, off) & (1u << 16)), 1000))
                    kprintf("xhci: BIOS gibt den Controller nicht frei, mache trotzdem weiter\n");
            }
            wr32(x->cap, off + 4, 0xE0000000u); /* alle SMI-Quellen aus, Statusbits loeschen */
        } else if (id == 2) { /* Supported Protocol */
            uint32_t major = c >> 24, ports = rd32(x->cap, off + 8);
            uint32_t first = ports & 0xFF, count = (ports >> 8) & 0xFF;
            for (uint32_t p = first; p < first + count && p < 256; p++)
                x->port_proto[p] = (uint8_t)major;
        }
        if (!next)
            break;
        off += next * 4;
    }
    for (int p = 1; p <= x->ports; p++)
        if (!x->port_proto[p])
            x->port_proto[p] = 2; /* unbekannt: wie USB2 behandeln (Reset) */

    /* Aeltere Intel-Chipsaetze: Ports vom EHCI auf den xHCI umschalten */
    if (pci->vendor == 0x8086 && (pci->device == 0x1E31 || pci->device == 0x8C31 || pci->device == 0x9C31 || pci->device == 0x9CB1 || pci->device == 0x8CB1)) {
        pci_write32(pci, 0xD8, pci_read32(pci, 0xDC));
        pci_write32(pci, 0xD0, pci_read32(pci, 0xD4));
    }

    /* Anhalten und zuruecksetzen */
    if (!(rd32(x->op, OP_USBSTS) & STS_HCH)) {
        wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) & ~CMD_RS);
        if (!WAIT_UNTIL(rd32(x->op, OP_USBSTS) & STS_HCH, 500)) {
            kprintf("xhci: Controller haelt nicht an\n");
            return -1;
        }
    }
    wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) | CMD_HCRST);
    if (!wait_clear(x->op, OP_USBCMD, CMD_HCRST, 1500) || !wait_clear(x->op, OP_USBSTS, STS_CNR, 1500)) {
        kprintf("xhci: Reset haengt\n");
        return -1;
    }

    /* Speicher: Geraete-Kontext-Array, Scratchpad-Puffer, Kommando- und Ereignisring */
    x->dcbaa = blk_dma_alloc(4096);
    x->ctrl_buf = blk_dma_alloc(4096);
    if (!x->dcbaa || !x->ctrl_buf || ring_init(&x->cmd) != 0)
        return -1;
    x->events = blk_dma_alloc(RING_TRBS * sizeof(Trb));
    uint64_t *erst = blk_dma_alloc(4096);
    if (!x->events || !erst)
        return -1;

    uint32_t scratch = ((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F);
    if (scratch) {
        uint64_t *array = blk_dma_alloc(scratch * 8);
        if (!array)
            return -1;
        for (uint32_t i = 0; i < scratch; i++) {
            void *page = blk_dma_alloc(4096);
            if (!page)
                return -1;
            array[i] = (uint64_t)page;
        }
        x->dcbaa[0] = (uint64_t)array;
    }

    wr32(x->op, OP_CONFIG, (uint32_t)x->slots);
    wr64(x->op, OP_DCBAAP, (uint64_t)x->dcbaa);
    wr64(x->op, OP_CRCR, (uint64_t)x->cmd.trbs | 1); /* Ring Cycle State = 1 */

    erst[0] = (uint64_t)x->events;
    erst[1] = RING_TRBS; /* Segmentgroesse (Rest reserviert) */
    x->ev_deq = 0;
    x->ev_cycle = 1;
    wr32(x->rt, 0x28, 1);                          /* ERSTSZ */
    wr64(x->rt, 0x38, (uint64_t)x->events);        /* ERDP */
    wr64(x->rt, 0x30, (uint64_t)erst);             /* ERSTBA (zuletzt) */
    wr32(x->rt, 0x20, rd32(x->rt, 0x20) | 1);      /* IMAN: Interrupt Pending loeschen; IE bleibt aus (wir pollen) */

    wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) | CMD_RS);
    if (!WAIT_UNTIL(!(rd32(x->op, OP_USBSTS) & STS_HCH), 500)) {
        kprintf("xhci: Controller startet nicht\n");
        return -1;
    }

    x->lock = (Mutex)MUTEX_INIT;
    /* Testkommando: irgendeine Antwort beweist, dass Kommando- und Ereignisring funktionieren (QEMU kennt NoOp nicht und
     * antwortet mit "TRB Error"; echte Controller mit Erfolg). Nur ein Timeout ist ein Fehler. */
    if (xhci_cmd(x, 0, 0, 0, TRB_NOOP_CMD << 10) == -1) {
        kprintf("xhci: Kommandoring antwortet nicht (USBSTS %#x)\n", rd32(x->op, OP_USBSTS));
        return -1;
    }
    kprintf("xhci: %02x:%02x.%u @ %#lx, Version %x.%02x, %d Ports, %d Slots, %d-Byte-Kontexte\n", pci->bus, pci->dev, pci->fn,
            bar, rd32(x->cap, 0) >> 24, (rd32(x->cap, 0) >> 16) & 0xFF, x->ports, x->slots, x->ctx_size);
    return 0;
}

/* ---------- Hintergrund-Thread ---------- */

static void usb_thread(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < controller_count; i++) {
            Xhci *x = controllers[i];
            mutex_lock(&x->lock);
            process_events(x);
            uint64_t now = time_ms();
            if (now - x->last_scan_ms >= 300) { /* Anstecken/Abziehen erkennen */
                x->last_scan_ms = now;
                scan_ports(x, 0);
            }
            mutex_unlock(&x->lock);
        }
        usb_hid_tick(); /* gehaltene Tasten wiederholen */
        thread_sleep_ms(10);
    }
}

int usb_init(void)
{
    PciDevice pci;
    for (unsigned i = 0; controller_count < MAX_XHCI && pci_find_class(0x0C, 0x03, 0x30, i, &pci) == 0; i++) {
        Xhci *x = kcalloc(1, sizeof(*x));
        if (!x)
            break;
        if (init_controller(x, &pci) != 0) {
            kfree(x);
            continue;
        }
        controllers[controller_count++] = x;
        mutex_lock(&x->lock);
        scan_ports(x, 1);
        mutex_unlock(&x->lock);
    }
    if (controller_count && !thread_started) {
        thread_started = 1;
        thread_create("usb", usb_thread, 0);
    }
    if (!controller_count)
        kprintf("usb: kein xHCI-Controller gefunden\n");
    return controller_count;
}

int usb_device_info(unsigned index, UsbInfo *out)
{
    unsigned seen = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        UsbDevice *d = all_devices[i];
        if (!d || !d->alive)
            continue;
        if (seen++ != index)
            continue;
        out->vid = d->vid;
        out->pid = d->pid;
        out->cls = d->cls;
        out->speed = (uint32_t)d->speed;
        out->port = (uint32_t)d->port;
        out->slot = (uint32_t)d->slot;
        out->driver = d->driver;
        memcpy(out->name, d->name, sizeof(out->name));
        memcpy(out->path, d->path, sizeof(out->path));
        return 0;
    }
    return -2;
}
