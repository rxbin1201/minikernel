#include "drivers/usb/usb.h"
#include "arch/x86_64/apic.h"
#include "drivers/block/blk.h"
#include "lib/kprintf.h"
#include "drivers/keyboard.h"
#include "drivers/keymap.h"
#include "lib/string.h"
#include "core/tty.h"

/* USB-Tastatur (HID Boot-Protokoll, 8-Byte-Berichte): uebersetzt Tastendruecke in Zeichen bzw. Sondertastencodes und liefert
 * sie ueber keyboard_deliver an dieselbe Eingabe wie die PS/2-Tastatur. Layout: keymap.h. */

typedef struct {
    uint8_t report[8];  /* DMA-Puffer fuer den Interrupt-Transfer: muss vorne stehen */
    uint8_t prev[8];
    int     caps;
    UsbDevice *dev;
    uint8_t repeat_key;  /* zuletzt gedrueckte (noch gehaltene) Taste, 0 = keine */
    uint8_t mods;        /* aktuelle Umschalttasten */
    uint64_t next_ms;    /* wann sie das naechste Mal wiederholt wird */
} HidState;

#define REPEAT_DELAY_MS 500 /* wie bei PS/2-Tastaturen ueblich */
#define REPEAT_RATE_MS  33  /* etwa 30 Zeichen pro Sekunde */

static HidState *states[8];
static int state_count;

static void key_pressed(HidState *s, uint8_t mods, uint8_t usage)
{
    int shift = (mods & 0x22) != 0, ctrl = (mods & 0x11) != 0;
    int altgr = (mods & 0x40) ? 1 : (mods & 0x04) ? 2 : 0; /* rechte Alt = AltGr, linke Alt */

    if (usage == 0x39) { /* Caps Lock */
        s->caps = !s->caps;
        return;
    }
    if ((usage >= 0x04 && usage <= 0x38) || (usage >= 0x54 && usage <= 0x64)) {
        keymap_key(usage, shift, ctrl, altgr, s->caps); /* Zeichen nach dem eingestellten Layout */
        return;
    }
    switch (usage) {
    case 0x4F: keyboard_deliver(KEY_RIGHT); break;
    case 0x50: keyboard_deliver(KEY_LEFT);  break;
    case 0x51: keyboard_deliver(KEY_DOWN);  break;
    case 0x52: keyboard_deliver(KEY_UP);    break;
    case 0x4A: if (!keyboard_scroll_key(KEY_HOME, shift)) keyboard_deliver(KEY_HOME); break;
    case 0x4D: if (!keyboard_scroll_key(KEY_END, shift)) keyboard_deliver(KEY_END); break;
    case 0x4C: keyboard_deliver(KEY_DEL);   break;
    case 0x4B: if (!keyboard_scroll_key(KEY_PGUP, shift)) keyboard_deliver(KEY_PGUP); break;
    case 0x4E: if (!keyboard_scroll_key(KEY_PGDN, shift)) keyboard_deliver(KEY_PGDN); break;
    case 0x58: keyboard_deliver('\n');      break; /* Enter des Ziffernblocks */
    default: break;
    }
}

/* Aufruf aus dem Controller bei jedem fertigen Interrupt-Transfer */
static void on_report(UsbDevice *d, const uint8_t *buf, uint32_t len)
{
    (void)d;
    HidState *s = (HidState *)buf; /* der Bericht liegt am Anfang der Zustandsstruktur */
    if (len < 8)
        return;
    for (int i = 2; i < 8; i++) {
        uint8_t k = s->report[i];
        if (k < 4) /* 0 = nichts, 1..3 = Fehler/Ueberlauf */
            continue;
        int was_down = 0;
        for (int j = 2; j < 8; j++)
            if (s->prev[j] == k)
                was_down = 1;
        if (!was_down) { /* nur neu gedrueckte Tasten (die Hardware wiederholt nicht: das macht usb_hid_tick) */
            key_pressed(s, s->report[0], k);
            if (k != 0x39) { /* Feststelltaste nicht wiederholen */
                s->repeat_key = k;
                s->next_ms = time_ms() + REPEAT_DELAY_MS;
            }
        }
    }
    s->mods = s->report[0];
    if (s->repeat_key) { /* losgelassen? */
        int held = 0;
        for (int j = 2; j < 8; j++)
            held |= s->report[j] == s->repeat_key;
        if (!held)
            s->repeat_key = 0;
    }
    memcpy(s->prev, s->report, 8);
}

void usb_hid_tick(void)
{
    uint64_t now = time_ms();
    for (int i = 0; i < state_count; i++) {
        HidState *s = states[i];
        if (!s->repeat_key)
            continue;
        if (!usb_alive(s->dev)) {
            s->repeat_key = 0;
            continue;
        }
        if (now >= s->next_ms) {
            key_pressed(s, s->mods, s->repeat_key);
            s->next_ms = now + REPEAT_RATE_MS;
        }
    }
}

int usb_hid_probe(UsbDevice *d, const UsbIface *iface)
{
    const UsbEpDesc *in = 0;
    for (int i = 0; i < iface->ep_count; i++)
        if ((iface->eps[i].attrs & 3) == 3 && (iface->eps[i].address & 0x80))
            in = &iface->eps[i];
    if (!in || state_count >= 8)
        return -1;

    HidState *s = blk_dma_alloc(4096);
    if (!s)
        return -1;
    if (usb_add_endpoints(d, in, 1) != 0)
        return -1;

    usb_control(d, 0x21, 0x0B, 0, iface->number, 0, 0); /* SET_PROTOCOL: Boot-Protokoll (feste 8-Byte-Berichte) */
    usb_control(d, 0x21, 0x0A, 0, iface->number, 0, 0); /* SET_IDLE 0: nur bei Aenderungen berichten (darf mit Stall enden) */

    s->dev = d;
    states[state_count++] = s;
    if (usb_interrupt_start(d, in->address, s, 8, on_report) != 0)
        return -1;
    kprintf("usb: Tastatur aktiv (Endpunkt %#x)\n", in->address);
    return 0;
}
