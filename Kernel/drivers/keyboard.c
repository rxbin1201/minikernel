#include "drivers/keyboard.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/cpu.h"
#include "console/console.h"
#include "drivers/keymap.h"
#include "lib/kprintf.h"
#include "core/tty.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUTPUT_FULL (1 << 0)
#define STATUS_INPUT_FULL  (1 << 1)
#define STATUS_AUX_DATA    (1 << 5) /* Byte kommt von der PS/2-Maus bzw. dem Touchpad, nicht von der Tastatur */

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CAPS   0x3A
#define SC_CTRL   0x1D

/* Scancode-Set 1 im US-Layout, Index = Make-Code (0x00-0x39): nur noch fuer keyboard_scancode_for (Selbsttests tippen
 * damit). Die Zeichen beim Tippen kommen aus keymap.c. Getrennte Literale, damit \x1b nicht die '1' verschluckt. */
static const char map_normal[] =
    "\0\x1b" "1234567890-=\b\t"
    "qwertyuiop[]\n\0"
    "asdfghjkl;'`\0\\"
    "zxcvbnm,./\0*\0 ";
static const char map_shift[] =
    "\0\x1b" "!@#$%^&*()_+\b\t"
    "QWERTYUIOP{}\n\0"
    "ASDFGHJKL:\"~\0|"
    "ZXCVBNM<>?\0*\0 ";

#define BUF_SIZE 16384 /* gross genug fuer eingefuegten Text */
static volatile char     buf[BUF_SIZE];
static volatile unsigned head, tail; /* head: Schreiber (IRQ), tail: Leser */

static int shift, caps, ctrl, extended, lalt, ralt;

static void put(char c)
{
    uint64_t f = irq_save(); /* PS/2-Interrupt und USB-Thread schreiben beide */
    unsigned next = (head + 1) % BUF_SIZE;
    if (next != tail) { /* voll: Zeichen verwerfen */
        buf[head] = c;
        head = next;
    }
    irq_restore(f);
}

/* Gemeinsamer Eingang fuer alle Tastaturen (PS/2 und USB): ein fertiges Zeichen bzw. ein Sondertastencode (>= 0x80) */
void keyboard_paste(void)
{
    uint32_t len;
    const char *t = console_clipboard(&len);
    console_select_clear();
    for (uint32_t i = 0; i < len; i++)
        if (t[i] != '\r')
            put(t[i]);
}

void keyboard_deliver(unsigned char c)
{
    console_live_request(); /* jeder Tastendruck zeigt wieder das Ende */
    int gfx = console_gfx_active(); /* ein Grafikprogramm kopiert und fuegt selbst ein (Strg+C/V im Editor) */
    if (!gfx && c == 3 && console_has_selection()) { /* Ctrl-C mit Markierung: kopieren statt abbrechen */
        console_copy_selection();
        return;
    }
    if (!gfx && c == 0x16) { /* Ctrl-V: einfuegen */
        keyboard_paste();
        return;
    }
    if (c == 3 && tty_interrupt())
        return; /* Ctrl-C: Vordergrundgruppe wurde beendet, kein Zeichen weiterreichen */
    put((char)c);
}

void keyboard_deliver_mods(int mods, unsigned char k)
{
    if (mods && console_gfx_active()) {
        console_live_request();
        if (mods == 2) { /* nur Alt */
            put((char)KEY_ALT);
        } else {
            put((char)KEY_MODS);
            put((char)mods);
        }
        put((char)k);
        return;
    }
    if (mods & 2) /* Alt-Kombinationen gibt es in der Konsole nicht */
        return;
    if (!keyboard_scroll_key(k, mods & 1))
        keyboard_deliver(k);
}

int keyboard_scroll_key(unsigned char k, int shift)
{
    if (!shift)
        return 0;
    int half = (int)(console_rows() / 2);
    if (half < 1)
        half = 1;
    if (k == KEY_PGUP)
        console_scroll_request(half);
    else if (k == KEY_PGDN)
        console_scroll_request(-half);
    else if (k == KEY_HOME)
        console_scroll_request(0x3FFFFFFF);
    else if (k == KEY_END)
        console_scroll_request(-0x3FFFFFFF);
    else
        return 0;
    return 1;
}

/* Erweiterte Tasten (Praefix 0xE0): Pfeile usw. werden zu Codes >= 0x80 (siehe tty.h) */
static int extended_key(uint8_t code)
{
    switch (code) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x53: return KEY_DEL;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    case 0x1C: return '\n'; /* Enter des Ziffernblocks */
    default:   return 0;
    }
}

static void keyboard_handler(InterruptFrame *f)
{
    (void)f;
    uint8_t st;
    while ((st = inb(PS2_STATUS)) & STATUS_OUTPUT_FULL) {
        uint8_t code = inb(PS2_DATA);
        if (st & STATUS_AUX_DATA)
            continue; /* Mausdaten verwerfen: sie wuerden sonst als Tasten gelesen */

        if (code == 0xE0) {
            extended = 1;
            continue;
        }
        int release = code & 0x80;
        code &= 0x7F;

        if (extended) {
            extended = 0;
            if (code == SC_CTRL) { /* rechte Strg-Taste */
                ctrl = !release;
                continue;
            }
            if (code == 0x38) { /* rechte Alt-Taste = AltGr */
                ralt = !release;
                continue;
            }
            if (code == 0x35 && !release) { /* / des Ziffernblocks */
                keymap_key(0x54, shift, ctrl, 0, caps);
                continue;
            }
            if (!release) {
                int k = extended_key(code);
                if (k) /* Pfeile usw., mit Shift/Alt/Strg */
                    keyboard_deliver_mods((shift ? 1 : 0) | (lalt ? 2 : 0) | (ctrl ? 4 : 0), (unsigned char)k);
            }
            continue;
        }

        if (code == SC_LSHIFT || code == SC_RSHIFT) {
            shift = !release;
            continue;
        }
        if (code == SC_CTRL) {
            ctrl = !release;
            continue;
        }
        if (code == 0x38) { /* linke Alt-Taste */
            lalt = !release;
            continue;
        }
        if (release)
            continue;
        if (code == SC_CAPS) {
            caps = !caps;
            continue;
        }
        uint8_t usage = keymap_ps2_to_usage(code);
        if (usage) /* das Zeichen bestimmt das eingestellte Layout */
            keymap_key(usage, shift, ctrl, ralt ? 1 : lalt ? 2 : 0, caps);
    }
}

static void wait_input_ready(void)
{
    for (int i = 0; i < 100000 && (inb(PS2_STATUS) & STATUS_INPUT_FULL); i++)
        ;
}

int keyboard_init(void)
{
    /* Ausgabepuffer leeren */
    for (int i = 0; i < 16 && (inb(PS2_STATUS) & STATUS_OUTPUT_FULL); i++)
        inb(PS2_DATA);

    /* Konfigurationsbyte lesen, IRQ 1 erlauben, zurueckschreiben */
    wait_input_ready();
    outb(PS2_CMD, 0x20);
    for (int i = 0; i < 100000 && !(inb(PS2_STATUS) & STATUS_OUTPUT_FULL); i++)
        ;
    uint8_t config = inb(PS2_DATA);
    wait_input_ready();
    outb(PS2_CMD, 0x60);
    wait_input_ready();
    outb(PS2_DATA, config | 0x01);

    idt_set_handler(VECTOR_KEYBOARD, keyboard_handler);
    return ioapic_route_isa_irq(1, VECTOR_KEYBOARD);
}

int keyboard_getchar(void)
{
    if (tail == head)
        return -1;
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return (unsigned char)c;
}

int keyboard_scancode_for(char c, int *needs_shift)
{
    for (unsigned i = 1; i < sizeof(map_normal) - 1; i++) {
        if (map_normal[i] == c) {
            *needs_shift = 0;
            return (int)i;
        }
        if (map_shift[i] == c) {
            *needs_shift = 1;
            return (int)i;
        }
    }
    return -1;
}

void keyboard_inject_scancode(unsigned char code)
{
    wait_input_ready();
    outb(PS2_CMD, 0xD2); /* "naechstes Byte in den Ausgabepuffer, als kaeme es von der Tastatur" */
    wait_input_ready();
    outb(PS2_DATA, code);
}
