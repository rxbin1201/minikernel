/* Selbsttests: Maus, Zwischenablage, Tastaturlayouts, Mehrfachklicks */

#include "console/console.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "drivers/keyboard.h"
#include "core/sched.h"
#include "drivers/mouse.h"
#include "drivers/keymap.h"
#include "tests/selftest.h"

void test_mouse(void)
{
    title("Maus");
    kprintf("  %s\n", mouse_attached() ? "USB-Maus angeschlossen" : "keine USB-Maus angeschlossen (Ereignisse werden simuliert)");
    int scale = (int)console_scale();
    MouseInfo mi;
    if (mouse_attached()) { /* echte Maus: der Zeiger muss schon auf dem Bildschirm stehen (Spitze + (1,2) ist ein weisser Pixel) */
        mouse_get(&mi);
        thread_sleep_ms(50);
        check("Angeschlossene Maus: Zeiger steht auf dem Bildschirm",
              console_debug_fb_pixel((uint32_t)mi.x + 1, (uint32_t)mi.y + 2) == 0xFFFFFF);
    }

    /* Meldungen wie von einem Treiber: Bewegung mit Beschleunigung, Tasten, Rad */
    mouse_report(0, 0, 0, 0);
    mouse_get(&mi);
    int x0 = mi.x, y0 = mi.y;
    unsigned ev0 = mi.events;
    mouse_report(1, 4, -3, 2);
    mouse_get(&mi);
    int moved_ok = mi.x == x0 + 8 * scale && mi.y == y0 - 6 * scale && mi.buttons == 1 && mi.wheel == 2 && mi.events == ev0 + 1;
    MouseInfo again;
    mouse_get(&again);
    int wheel_reset = again.wheel == 0;
    thread_sleep_ms(60); /* das Rad hat ein Blaettern im Verlauf angefordert: zurueck zum Ende */
    console_view_live();

    for (int i = 0; i < 300; i++)
        mouse_report(0, -127, -127, 0);
    mouse_get(&mi);
    int corner_tl = mi.x == 0 && mi.y == 0;
    for (int i = 0; i < 300; i++)
        mouse_report(0, 127, 127, 0);
    mouse_get(&mi);
    int corner_br = mi.x == (int)console_width_px() - 1 && mi.y == (int)console_height_px() - 1;
    mouse_report(0, 0, 0, 0);

    /* Zeiger zeichnen: nur im Framebuffer, nicht im Abbild; ueber Text liegend bleibt er sichtbar; ausblendbar */
    console_clear();
    console_cursor_set(48, 48, 1);
    int drawn = console_debug_fb_pixel(49, 50) == 0xFFFFFF && console_read_pixel(49, 50) != 0xFFFFFF;
    for (const char *c = "\x1b[4;7H#"; *c; c++) /* Zelle (6,3) = Pixel 48..55 x 48..63 liegt unter dem Zeiger */
        console_putc(*c);
    int over_text = console_debug_fb_pixel(49, 50) == 0xFFFFFF;
    console_cursor_set(48, 48, 0);
    int hidden = console_debug_fb_pixel(49, 50) == console_read_pixel(49, 50);
    console_clear();
    console_cursor_set(mi.x, mi.y, mouse_attached());
    title("Maus");
    check("Bewegung mit Beschleunigung, Tasten, Rad", moved_ok);
    check("Radwert wird beim Abholen zurueckgesetzt", wheel_reset);
    check("Position bleibt im Bildschirm (Ecken)", corner_tl && corner_br);
    check("Zeiger wird in den Framebuffer gezeichnet (nicht ins Abbild)", drawn);
    check("Zeiger bleibt sichtbar, wenn darunter Text gezeichnet wird", over_text);
    check("Zeiger ausblenden stellt den Hintergrund wieder her", hidden);
}

void test_clipboard(void)
{
    title("Markieren, Kopieren, Einfuegen");
    while (keyboard_getchar() >= 0) /* Tastaturpuffer leeren */
        ;
    int cw = 8 * (int)console_scale(), ch = 16 * (int)console_scale();
    console_clear();
    for (const char *c = "Zeile eins\nHallo Maus 123\nGr\xC3\xBC\xC3\x9F" "e   \n"; *c; c++)
        console_putc(*c);

    /* Zeile 1, Spalten 0..9 ("Hallo Maus") markieren */
    console_select_press(1, ch + 1);
    console_select_move(9 * cw + 1, ch + 1);
    console_select_release();
    int active = console_has_selection();
    int hl_in = console_read_pixel(0, (uint32_t)ch) == 0xC0C0C0;             /* markiert: Hintergrund in Textfarbe */
    int hl_out = console_read_pixel((uint32_t)(11 * cw), (uint32_t)ch) == 0; /* dahinter nicht */
    keyboard_deliver(3);                                                      /* Ctrl-C kopiert */
    uint32_t len;
    const char *t = console_clipboard(&len);
    int copy1 = len == 10 && memcmp(t, "Hallo Maus", 10) == 0;
    int no_ctrl_c = keyboard_getchar() < 0 && !console_has_selection();
    int unhl = console_read_pixel(0, (uint32_t)ch) == 0;

    /* Ueber zwei Zeilen, rueckwaerts gezogen, mit Umlauten und Leerzeichen am Zeilenende */
    console_select_press(10 * cw + 1, 2 * ch + 1);
    console_select_move(6 * cw + 1, ch + 1);
    keyboard_deliver(3);
    t = console_clipboard(&len);
    int copy2 = len == 16 && memcmp(t, "Maus 123\nGr\xC3\xBC\xC3\x9F" "e", 16) == 0;

    /* Ein einfacher Klick markiert nichts; Ctrl-V fuegt die Zwischenablage als Eingabe ein */
    console_select_press(3 * cw, 3);
    console_select_release();
    int click = !console_has_selection();
    console_clipboard_set("ab\xC3\xA4", 4);
    keyboard_deliver(0x16);
    int k1 = keyboard_getchar(), k2 = keyboard_getchar(), k3 = keyboard_getchar(), k4 = keyboard_getchar(), k5 = keyboard_getchar();
    int paste = k1 == 'a' && k2 == 'b' && k3 == 0xC3 && k4 == 0xA4 && k5 < 0;

    console_clear();
    title("Markieren, Kopieren, Einfuegen");
    check("Ziehen mit der Maus markiert (Farben vertauscht)", active && hl_in && hl_out);
    check("Ctrl-C kopiert die Markierung (statt abzubrechen) und hebt sie auf", copy1 && no_ctrl_c && unhl);
    check("Markierung ueber zwei Zeilen, rueckwaerts, mit Umlauten", copy2);
    check("Einfacher Klick markiert nichts", click);
    check("Ctrl-V fuegt die Zwischenablage (UTF-8) als Eingabe ein", paste);
}

/* Eine Taste (HID-Usage) mit Umschaltern druecken und das Ergebnis vergleichen */
static int key_is(uint8_t usage, int shift, int ctrl, int altgr, int caps, const char *want)
{
    char got[16];
    int n = 0, c;
    while (keyboard_getchar() >= 0) /* keymap_key liefert sofort: keine Wartezeit noetig */
        ;
    keymap_key(usage, shift, ctrl, altgr, caps);
    while ((c = keyboard_getchar()) >= 0)
        if (n < (int)sizeof(got) - 1)
            got[n++] = (char)c;
    got[n] = 0;
    return strcmp(got, want) == 0;
}

void test_keymap(void)
{
    title("Tastaturlayouts");
    char tmp[64];
    drain_keys(tmp, sizeof(tmp));
    const char *before = keymap_name();

    check("Unbekanntes Layout abgelehnt", keymap_set("xx") != 0 && strcmp(keymap_name(), before) == 0);

    keymap_set("us");
    check("US: y, Shift+2 = @, Shift+' = \", Strg+C = 3",
          key_is(0x1C, 0, 0, 0, 0, "y") && key_is(0x1F, 1, 0, 0, 0, "@") && key_is(0x34, 1, 0, 0, 0, "\"") &&
          key_is(0x06, 0, 1, 0, 0, "\x03"));

    keymap_set("de");
    check("DE: Y/Z vertauscht, ß, ü, ö, ä, Shift+ä = Ä",
          key_is(0x1C, 0, 0, 0, 0, "z") && key_is(0x1D, 0, 0, 0, 0, "y") && key_is(0x2D, 0, 0, 0, 0, "\xC3\x9F") &&
          key_is(0x2F, 0, 0, 0, 0, "\xC3\xBC") && key_is(0x33, 0, 0, 0, 0, "\xC3\xB6") && key_is(0x34, 1, 0, 0, 0, "\xC3\x84"));
    check("DE: Shift+2 = \", Shift+3 = §, Shift+7 = /, - . , < >",
          key_is(0x1F, 1, 0, 0, 0, "\"") && key_is(0x20, 1, 0, 0, 0, "\xC2\xA7") && key_is(0x24, 1, 0, 0, 0, "/") &&
          key_is(0x38, 0, 0, 0, 0, "-") && key_is(0x64, 0, 0, 0, 0, "<") && key_is(0x64, 1, 0, 0, 0, ">"));
    check("DE: AltGr+Q = @, AltGr+E = €, AltGr+8 = [, AltGr+< = |, Strg+Alt+Q = @",
          key_is(0x14, 0, 0, 1, 0, "@") && key_is(0x08, 0, 0, 1, 0, "\xE2\x82\xAC") && key_is(0x25, 0, 0, 1, 0, "[") &&
          key_is(0x64, 0, 0, 1, 0, "|") && key_is(0x14, 0, 1, 2, 0, "@"));
    check("DE: Feststelltaste wirkt auch auf Umlaute (ä -> Ä, Shift -> ä), nicht auf Ziffern",
          key_is(0x34, 0, 0, 0, 1, "\xC3\x84") && key_is(0x34, 1, 0, 0, 1, "\xC3\xA4") && key_is(0x1E, 0, 0, 0, 1, "1"));
    int dead1 = key_is(0x35, 0, 0, 0, 0, "") && key_is(0x04, 0, 0, 0, 0, "\xC3\xA2");         /* ^ a = â */
    int dead2 = key_is(0x2E, 0, 0, 0, 0, "") && key_is(0x08, 1, 0, 0, 0, "\xC3\x89");         /* ´ E = É */
    int dead3 = key_is(0x2E, 1, 0, 0, 0, "") && key_is(0x2C, 0, 0, 0, 0, "`");                /* ` Leertaste = ` */
    int dead4 = key_is(0x35, 0, 0, 0, 0, "") && key_is(0x1B, 0, 0, 0, 0, "^x");               /* ^ x = ^x */
    check("DE: Tottasten (^a = â, ´E = É, `+Leertaste = `, ^x = ^x)", dead1 && dead2 && dead3 && dead4);

    /* PS/2-Weg: Scancodes ueber den 8042 einspeisen */
    drain_keys(tmp, sizeof(tmp));
    keyboard_inject_scancode(0x15); keyboard_inject_scancode(0x95); /* Y-Position */
    keyboard_inject_scancode(0x1A); keyboard_inject_scancode(0x9A); /* [-Position = ü */
    keyboard_inject_scancode(0xE0); keyboard_inject_scancode(0x38); /* AltGr gedrueckt */
    keyboard_inject_scancode(0x12); keyboard_inject_scancode(0x92); /* E -> € */
    keyboard_inject_scancode(0xE0); keyboard_inject_scancode(0xB8); /* AltGr los */
    keyboard_inject_scancode(0x56); keyboard_inject_scancode(0xD6); /* ISO-Taste links neben Y = < */
    drain_keys(tmp, sizeof(tmp));
    check("DE ueber PS/2: z, ü, AltGr+E = €, <", strcmp(tmp, "z\xC3\xBC\xE2\x82\xAC<") == 0);

    keymap_set("uk");
    check("UK: Shift+2 = \", Shift+3 = £, Shift+' = @, # und ~, \\ neben Z",
          key_is(0x1F, 1, 0, 0, 0, "\"") && key_is(0x20, 1, 0, 0, 0, "\xC2\xA3") && key_is(0x34, 1, 0, 0, 0, "@") &&
          key_is(0x32, 0, 0, 0, 0, "#") && key_is(0x32, 1, 0, 0, 0, "~") && key_is(0x64, 0, 0, 0, 0, "\\"));

    keymap_set(before);
    drain_keys(tmp, sizeof(tmp));
}

static void click(void)
{
    mouse_report(1, 0, 0, 0);
    mouse_report(0, 0, 0, 0);
}

void test_multiclick(void)
{
    title("Doppel- und Dreifachklick");
    char tmp[64];
    drain_keys(tmp, sizeof(tmp));
    int cw = 8 * (int)console_scale(), ch = 16 * (int)console_scale();
    console_clear();
    for (const char *c = "\nabc /mnt/usb0/datei.txt xyz   \n"; *c; c++)
        console_putc(*c);
    mouse_click_reset(); /* fruehere Klicks zaehlen nicht mit */
    mouse_goto(10 * cw + 2, ch + 2);

    uint32_t len;
    click();
    click();                 /* Doppelklick: Wort (Pfad bis zum Leerzeichen) */
    int word_sel = console_has_selection();
    keyboard_deliver(3);
    const char *t = console_clipboard(&len);
    int word = word_sel && len == 19 && memcmp(t, "/mnt/usb0/datei.txt", 19) == 0;

    mouse_click_reset();
    click();
    click();
    click();                 /* Dreifachklick: ganze Zeile (ohne Leerzeichen am Ende) */
    keyboard_deliver(3);
    t = console_clipboard(&len);
    int line = len == 27 && memcmp(t, "abc /mnt/usb0/datei.txt xyz", 27) == 0;

    thread_sleep_ms(450);
    click();                 /* nach einer Pause: wieder ein einfacher Klick, markiert nichts */
    int single = !console_has_selection();

    mouse_goto(40 * cw + 2, ch + 2); /* Doppelklick auf leeren Bereich: nichts */
    mouse_click_reset();
    click();
    click();
    int empty = !console_has_selection();
    drain_keys(tmp, sizeof(tmp));

    console_clear();
    title("Doppel- und Dreifachklick");
    check("Doppelklick markiert das Wort (ganzer Pfad)", word);
    check("Dreifachklick markiert die ganze Zeile", line);
    check("Nach einer Pause zaehlt ein Klick wieder als Einfachklick", single);
    check("Doppelklick auf leeren Bereich markiert nichts", empty);
}
