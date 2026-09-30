#include "drivers/usb/usb.h"
#include "drivers/block/blk.h"
#include "lib/kprintf.h"
#include "drivers/mouse.h"
#include "lib/string.h"

/* USB-Maus (HID). Bevorzugt das Report-Protokoll: der Report-Deskriptor sagt, wo Tasten, X, Y und das Rad im Bericht
 * stehen (im Boot-Protokoll schicken die meisten Mause kein Rad mit). Absolute Zeigegeraete (Tablets, die Maus von
 * VMware/QEMU "usb-tablet") werden ebenfalls unterstuetzt. Klappt das Lesen des Deskriptors nicht, gilt das
 * Boot-Protokoll: Tasten, dx, dy, (Rad) als Bytes. */

#define MAX_MICE 4
#define MAX_REPORT 64

typedef struct {
    int bit, size;      /* Lage im Bericht (ohne Report-ID-Byte); size 0 = nicht vorhanden */
    int is_signed, rel; /* logischer Bereich mit negativen Werten, relative Achse */
    int32_t lmin, lmax;
} Field;

typedef struct {
    uint8_t report[MAX_REPORT]; /* DMA-Puffer fuer den Interrupt-Transfer: muss vorne stehen */
    UsbDevice *dev;
    int     report_protocol;
    int     report_id;         /* 0 = Berichte ohne ID-Byte */
    Field   x, y, wheel;
    int     btn_bit[3], btn_size;
    int     btn_count;
} MouseState;

static MouseState *mice[MAX_MICE];
static int         mouse_total;

static int32_t get_field(const uint8_t *data, uint32_t len, const Field *f)
{
    if (!f->size || (uint32_t)(f->bit + f->size) > len * 8)
        return 0;
    uint32_t v = 0;
    for (int i = 0; i < f->size && i < 32; i++) {
        int b = f->bit + i;
        if (data[b / 8] & (1u << (b % 8)))
            v |= 1u << i;
    }
    if (f->is_signed && f->size < 32 && (v & (1u << (f->size - 1))))
        v |= ~0u << f->size;
    return (int32_t)v;
}

static void on_report(UsbDevice *d, const uint8_t *buf, uint32_t len)
{
    (void)d;
    MouseState *s = (MouseState *)buf;
    if (!s->report_protocol) {
        if (len < 3)
            return;
        mouse_report(buf[0], (int8_t)buf[1], (int8_t)buf[2], len >= 4 ? (int8_t)buf[3] : 0);
        return;
    }
    const uint8_t *data = buf;
    if (s->report_id) {
        if (len < 1 || buf[0] != s->report_id)
            return; /* Bericht einer anderen Funktion (z.B. Zusatztasten) */
        data++;
        len--;
    }
    uint8_t buttons = 0;
    for (int i = 0; i < s->btn_count && i < 3; i++) {
        Field bf = {s->btn_bit[i], s->btn_size, 0, 0, 0, 1};
        if (get_field(data, len, &bf))
            buttons |= (uint8_t)(1u << i);
    }
    int wheel = get_field(data, len, &s->wheel);
    if (s->x.rel || !s->x.size) {
        mouse_report(buttons, get_field(data, len, &s->x), get_field(data, len, &s->y), wheel);
    } else { /* absolute Koordinaten: logischen Bereich auf den Bildschirm abbilden */
        int32_t vx = get_field(data, len, &s->x), vy = get_field(data, len, &s->y);
        int64_t rx = (int64_t)s->x.lmax - s->x.lmin, ry = (int64_t)s->y.lmax - s->y.lmin;
        uint32_t fx = rx > 0 ? (uint32_t)(((int64_t)(vx - s->x.lmin) << 16) / rx) : 0;
        uint32_t fy = ry > 0 ? (uint32_t)(((int64_t)(vy - s->y.lmin) << 16) / ry) : 0;
        mouse_report_abs(buttons, fx, fy, wheel);
    }
}

int usb_mouse_count(void)
{
    int n = 0;
    for (int i = 0; i < mouse_total; i++)
        if (usb_alive(mice[i]->dev))
            n++;
    return n;
}

/* ---------- Report-Deskriptor ---------- */

/* Sucht im Report-Deskriptor die Felder fuer Tasten, X, Y und Rad. 0 = X und Y gefunden. */
static int parse_report_descriptor(MouseState *s, const uint8_t *p, int len)
{
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[32], nusages = 0, umin = 0, umax = 0;
    int have_range = 0;
    int offset[256];
    for (int i = 0; i < 256; i++)
        offset[i] = 0;
    int chosen_id = -1;

    for (int i = 0; i < len;) {
        uint8_t prefix = p[i];
        if (prefix == 0xFE) { /* langes Element: ueberspringen */
            if (i + 2 >= len)
                break;
            i += 3 + p[i + 1];
            continue;
        }
        int sz = prefix & 3;
        if (sz == 3)
            sz = 4;
        int type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (i + 1 + sz > len)
            break;
        uint32_t u = 0;
        for (int k = 0; k < sz; k++)
            u |= (uint32_t)p[i + 1 + k] << (8 * k);
        int32_t sv = sz == 1 ? (int8_t)u : sz == 2 ? (int16_t)u : (int32_t)u;
        i += 1 + sz;

        if (type == 1) { /* global */
            if (tag == 0) page = u;
            else if (tag == 1) lmin = sv;
            else if (tag == 2) lmax = sz == 4 ? (int32_t)u : (lmin >= 0 && sv < 0 ? (int32_t)u : sv);
            else if (tag == 7) rsize = u;
            else if (tag == 8) rid = u & 0xFF;
            else if (tag == 9) rcount = u;
        } else if (type == 2) { /* lokal */
            if (tag == 0 && nusages < 32)
                usages[nusages++] = u;
            else if (tag == 1) { umin = u; have_range = 1; }
            else if (tag == 2) { umax = u; have_range = 1; }
        } else if (type == 0) { /* main */
            if (tag == 8) { /* Input */
                int constant = u & 1, relative = (u >> 2) & 1;
                for (uint32_t f = 0; f < rcount; f++) {
                    uint32_t usage = 0;
                    if (have_range && umin + f <= umax)
                        usage = umin + f;
                    else if (f < nusages)
                        usage = usages[f];
                    else if (nusages)
                        usage = usages[nusages - 1];
                    uint32_t upage = usage >> 16 ? usage >> 16 : page;
                    usage &= 0xFFFF;
                    int bit = offset[rid] + (int)(f * rsize);
                    if (constant || (chosen_id >= 0 && (int)rid != chosen_id))
                        continue;
                    Field fl = {bit, (int)rsize, lmin < 0, relative, lmin, lmax};
                    if (upage == 1 && usage == 0x30 && !s->x.size) {
                        s->x = fl;
                        chosen_id = (int)rid;
                    } else if (upage == 1 && usage == 0x31 && !s->y.size) {
                        s->y = fl;
                    } else if (upage == 1 && usage == 0x38 && !s->wheel.size) {
                        s->wheel = fl;
                    } else if (upage == 9 && usage >= 1 && usage <= 3) {
                        s->btn_bit[usage - 1] = bit;
                        s->btn_size = (int)rsize;
                        if ((int)usage > s->btn_count)
                            s->btn_count = (int)usage;
                        if (chosen_id < 0)
                            chosen_id = (int)rid;
                    }
                }
                offset[rid] += (int)(rsize * rcount);
            }
            nusages = 0;
            have_range = 0;
            umin = umax = 0;
        }
    }
    if (!s->x.size || !s->y.size)
        return -1;
    s->report_id = chosen_id > 0 ? chosen_id : 0;
    return 0;
}

int usb_mouse_probe(UsbDevice *d, const UsbIface *iface)
{
    const UsbEpDesc *in = 0;
    for (int i = 0; i < iface->ep_count; i++)
        if ((iface->eps[i].attrs & 3) == 3 && (iface->eps[i].address & 0x80))
            in = &iface->eps[i];
    if (!in || mouse_total >= MAX_MICE)
        return -1;

    MouseState *s = blk_dma_alloc(4096);
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    s->dev = d;

    /* Report-Deskriptor lesen (GET_DESCRIPTOR, Typ 0x22, an die Schnittstelle) */
    static uint8_t desc[1024];
    int boot = iface->sub == 1 && iface->proto == 2;
    int n = usb_control(d, 0x81, 6, 0x2200, iface->number, desc, sizeof(desc));
    if (n > 0 && parse_report_descriptor(s, desc, n) == 0) {
        s->report_protocol = 1;
    } else {
        if (!boot)
            return -1; /* keine Maus (z.B. Zusatztasten einer Tastatur) */
        memset(&s->x, 0, sizeof(Field) * 3);
        s->btn_count = 0;
    }
    if (usb_add_endpoints(d, in, 1) != 0)
        return -1;
    if (boot) /* nur Boot-faehige Schnittstellen kennen SET_PROTOCOL */
        usb_control(d, 0x21, 0x0B, s->report_protocol ? 1 : 0, iface->number, 0, 0);
    usb_control(d, 0x21, 0x0A, 0, iface->number, 0, 0); /* SET_IDLE 0 (darf mit Stall enden) */

    uint32_t len = in->max_packet & 0x7FF;
    if (len < 3)
        len = 4;
    if (len > MAX_REPORT)
        len = MAX_REPORT;
    mice[mouse_total++] = s;
    if (usb_interrupt_start(d, in->address, s, len, on_report) != 0) {
        mouse_total--;
        return -1;
    }
    if (s->report_protocol)
        kprintf("usb: Maus aktiv (Report-Protokoll, %s, %d Tasten%s%s)\n", s->x.rel ? "relativ" : "absolut", s->btn_count,
                s->wheel.size ? ", Rad" : ", ohne Rad", s->report_id ? ", mit Report-ID" : "");
    else
        kprintf("usb: Maus aktiv (Boot-Protokoll, %u-Byte-Berichte)\n", len);
    return 0;
}
