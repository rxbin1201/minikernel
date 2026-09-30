#include "drivers/mouse.h"
#include "arch/x86_64/apic.h"
#include "console/console.h"
#include "drivers/keyboard.h"
#include "lib/kprintf.h"

static volatile int32_t  mx, my;
static volatile uint32_t buttons;
static volatile int32_t  wheel_acc;      /* seit dem letzten mouse_get aufgelaufene Radschritte */
static volatile uint32_t events;
static volatile uint32_t left_presses, right_presses;
static volatile int32_t  press_x, press_y;
static volatile int      dirty;          /* Position/Sichtbarkeit hat sich geaendert */
static int               attached, placed;
static uint32_t          last_buttons;
static uint64_t          last_click_ms;
static int32_t           click_x, click_y;
static int               clicks;            /* 1, 2, 3: Einfach-, Doppel-, Dreifachklick */
static volatile uint32_t app_owner;         /* != 0: dieses Programm (PID) wertet die Maus selbst aus */

#define MULTI_CLICK_MS 400
#define MULTI_CLICK_PX 6

/* Leichte Beschleunigung: langsame Bewegungen bleiben genau, schnelle gehen weiter */
static int accel(int d)
{
    int a = d < 0 ? -d : d;
    int m = a <= 2 ? a : a <= 6 ? a * 2 : a * 3;
    return d < 0 ? -m : m;
}

/* Tasten und Rad auswerten (nach dem Setzen der neuen Position) */
static void handle(uint8_t b, int moved, int wheel)
{
    uint32_t old = buttons;
    buttons = b & 7;
    if ((buttons & 1) && !(old & 1)) {
        press_x = mx;
        press_y = my;
        left_presses++;
    }
    if ((buttons & 2) && !(old & 2))
        right_presses++;
    if (app_owner) { /* ein Programm (z.B. der Editor) wertet Tasten und Rad selbst aus */
        wheel_acc += wheel;
        events++;
        dirty = 1;
        return;
    }
    if ((buttons & 1) && !(old & 1)) {        /* linke Taste gedrueckt */
        uint64_t now = time_ms();
        int dx = mx - click_x, dy = my - click_y;
        int near = dx > -MULTI_CLICK_PX && dx < MULTI_CLICK_PX && dy > -MULTI_CLICK_PX && dy < MULTI_CLICK_PX;
        clicks = (near && now - last_click_ms <= MULTI_CLICK_MS && clicks < 3) ? clicks + 1 : 1;
        last_click_ms = now;
        click_x = mx;
        click_y = my;
        if (clicks == 2)
            console_select_word(mx, my);      /* Doppelklick: Wort */
        else if (clicks == 3)
            console_select_line(mx, my);      /* Dreifachklick: Zeile */
        else
            console_select_press(mx, my);     /* Markieren beginnt */
    } else if ((buttons & 1) && moved)
        console_select_move(mx, my);          /* gezogen: Markierung erweitern */
    else if (!(buttons & 1) && (old & 1))
        console_select_release();
    if ((buttons & 2) && !(old & 2))
        keyboard_paste();                     /* rechte Taste: einfuegen */
    wheel_acc += wheel;
    if (wheel)
        console_scroll_request(3 * wheel); /* Mausrad blaettert im Verlauf */
    events++;
    dirty = 1;
}

void mouse_report(uint8_t b, int dx, int dy, int wheel)
{
    int scale = (int)console_scale();
    int w = (int)console_width_px(), h = (int)console_height_px();
    if (!w || !h)
        return;
    if (!placed) { /* erste Meldung: Cursor in die Mitte */
        mx = w / 2;
        my = h / 2;
        placed = 1;
    }
    int nx = mx + accel(dx) * scale, ny = my + accel(dy) * scale;
    mx = nx < 0 ? 0 : nx >= w ? w - 1 : nx;
    my = ny < 0 ? 0 : ny >= h ? h - 1 : ny;
    handle(b, dx || dy, wheel);
}

void mouse_report_abs(uint8_t b, uint32_t fx, uint32_t fy, int wheel)
{
    int w = (int)console_width_px(), h = (int)console_height_px();
    if (!w || !h)
        return;
    int nx = (int)(((uint64_t)fx * (uint32_t)w) >> 16), ny = (int)(((uint64_t)fy * (uint32_t)h) >> 16);
    nx = nx < 0 ? 0 : nx >= w ? w - 1 : nx;
    ny = ny < 0 ? 0 : ny >= h ? h - 1 : ny;
    int moved = nx != mx || ny != my;
    mx = nx;
    my = ny;
    placed = 1;
    handle(b, moved, wheel);
}

void mouse_set_owner(uint32_t pid)
{
    app_owner = pid;
    if (pid)
        console_select_clear();
}

void mouse_owner_exit(uint32_t pid)
{
    if (app_owner == pid)
        app_owner = 0;
}

void mouse_click_reset(void)
{
    clicks = 0;
    last_click_ms = 0;
}

int mouse_attached(void)
{
    return attached;
}

void mouse_get(MouseInfo *out)
{
    out->x = mx;
    out->y = my;
    out->buttons = buttons;
    out->wheel = __atomic_exchange_n(&wheel_acc, 0, __ATOMIC_RELAXED);
    out->events = events;
    out->attached = (uint32_t)attached;
    out->width = console_width_px();
    out->height = console_height_px();
    out->left_presses = left_presses;
    out->right_presses = right_presses;
    out->press_x = press_x;
    out->press_y = press_y;
}

void mouse_tick(void)
{
    int now = usb_mouse_count() > 0;
    if (now != attached) {
        attached = now;
        if (now && !placed) { /* Cursor sofort sichtbar machen */
            mx = (int32_t)console_width_px() / 2;
            my = (int32_t)console_height_px() / 2;
            placed = 1;
        }
        dirty = 1;
    }
    if (dirty) {
        dirty = 0;
        console_cursor_set(mx, my, attached);
    }
    last_buttons = buttons;
}

void mouse_init(void)
{
    console_set_tick(mouse_tick);
}
