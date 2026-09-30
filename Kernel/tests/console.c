/* Selbsttests: Konsole, Textausgabe, Unicode, Verlauf, Grafikmodi */

#include "drivers/serial.h"
#include "console/console.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "core/sched.h"
#include "lib/utf8.h"
#include "drivers/video.h"
#include "tests/selftest.h"

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Prueft ksnprintf gegen den erwarteten String */
#define CHECK_FMT(expected, ...)                                   \
    do {                                                           \
        char buf_[128];                                            \
        ksnprintf(buf_, sizeof(buf_), __VA_ARGS__);                \
        if (!streq(buf_, expected)) {                              \
            fmt_ok = 0;                                            \
            kprintf("  erwartet \"%s\", bekommen \"%s\"\n", expected, buf_); \
        }                                                          \
    } while (0)

void test_kprintf(void)
{
    title("kprintf");
    int fmt_ok = 1;

    CHECK_FMT("42|   42|42   |00042", "%d|%5d|%-5d|%05d", 42, 42, 42, 42);
    CHECK_FMT("-2147483648", "%d", -2147483647 - 1);
    CHECK_FMT("+7  8", "%+d % d", 7, 8);
    CHECK_FMT("ff FF 0xff 0xdeadbeef", "%x %X %#x %#lx", 255, 255, 255, 0xDEADBEEFUL);
    CHECK_FMT("18446744073709551615", "%llu", 18446744073709551615ULL);
    CHECK_FMT("0x0000000000001000", "%#018lx", 0x1000UL);
    CHECK_FMT("0x0 0x1234", "%p %p", (void *)0, (void *)0x1234);
    CHECK_FMT("abc|       abc|abc       |ab", "%s|%10s|%-10s|%.2s", "abc", "abc", "abc", "abc");
    const char *volatile nullp = 0;
    CHECK_FMT("(null)", "%s", nullp);
    CHECK_FMT("x-  y|100%", "%c-%3c|100%%", 'x', 'y');
    CHECK_FMT("   12|12   ", "%*d|%-*d", 5, 12, 5, 12);
    CHECK_FMT("007", "%.3d", 7);
    CHECK_FMT("255 -1", "%hhu %hhd", 255, -1);
    check("Formatierung", fmt_ok);

    char small[5];
    int n = ksnprintf(small, sizeof(small), "%s", "abcdefgh");
    check("ksnprintf kuerzt und terminiert", n == 8 && streq(small, "abcd"));

    kprintf("  Ausgabe: %s, %d, %#x, %c\n", "Hallo", -42, 255, '!');
}

/* Nicht leere Zelle (col, row)? */
static int cell_has_ink(uint32_t col, uint32_t row)
{
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 8; x++)
            if (console_read_pixel(col * 8 + x, row * 16 + y) != 0)
                return 1;
    return 0;
}

/* Kommt die Farbe (0x00RRGGBB) in der Zelle vor? */
static int cell_has_color(uint32_t col, uint32_t row, uint32_t color)
{
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 8; x++)
            if (console_read_pixel(col * 8 + x, row * 16 + y) == color)
                return 1;
    return 0;
}

/* Liest den Framebuffer zurueck: zeichnet ein Glyph als ASCII-Art auf Serial und prueft das Scrolling. */
void test_console(void)
{
    title("Konsole");
    uint32_t col, row;

    /* Jeder Abschnitt (loeschen, schreiben, auslesen) laeuft mit ausgeschalteten Interrupts: sonst kann eine Meldung
     * eines anderen Threads (z.B. "net: eth0: Verbindung hergestellt") dazwischen die gepruefte Zelle ueberschreiben */
    uint64_t f = irq_save();
    console_clear(); /* oben anfangen: sonst kann die Ausgabe unten scrollen und das Glyph verschieben (haengt von der Zahl der Boot-Zeilen ab) */
    title("Konsole");
    console_get_cursor(&col, &row);
    kprintf("H");
    kprintf("\n  Zelle (%u,%u) als ASCII-Art (nur Serial):\n", col, row);
    for (uint32_t y = 0; y < 16; y++) {
        serial_puts("    ");
        for (uint32_t x = 0; x < 8; x++)
            serial_putc(console_read_pixel(col * 8 + x, row * 16 + y) ? '#' : '.');
        serial_puts("\n");
    }
    int glyph_ok = cell_has_ink(col, row);
    irq_restore(f);

    /* Scrolling: bis zum unteren Rand und darueber hinaus schreiben. Ergebnisse erst nach dem
     * Auslesen ausgeben, weil die Ausgabe selbst wieder scrollt. */
    f = irq_save();
    console_clear();
    for (uint32_t i = 0; i < console_rows() + 3; i++)
        console_putc('\n');
    console_putc('S');
    uint32_t scol, srow; /* gescrollt wird in Viertel-Bildschirm-Schritten: der Cursor steht nicht zwingend unten */
    console_get_cursor(&scol, &srow);
    int last_ok  = srow > 0 && cell_has_ink(0, srow);
    int above_ok = srow > 0 && !cell_has_ink(0, srow - 1);
    irq_restore(f);

    /* ANSI-Farben: X hellrot, Y auf blauem Grund, Z wieder in der Standardfarbe; die Escape-Folgen belegen keine Zellen */
    f = irq_save();
    console_clear();
    for (const char *c = "\x1b[1;31mX\x1b[0m\x1b[44mY\x1b[0mZ"; *c; c++)
        console_putc(*c);
    uint32_t ecol, erow;
    console_get_cursor(&ecol, &erow);
    int esc_cols = ecol == 3;
    int red_ok = cell_has_color(0, 0, 0xF14C4C);
    int blue_ok = cell_has_color(1, 0, 0x2472C8);
    int default_ok = cell_has_color(2, 0, 0xC0C0C0) && !cell_has_color(2, 0, 0xF14C4C);
    irq_restore(f);

    console_clear();
    title("Konsole");
    check("Glyph gezeichnet", glyph_ok);
    check("ANSI: Escape-Folgen werden nicht ausgegeben", esc_cols);
    check("ANSI: ESC[1;31m = hellrot", red_ok);
    check("ANSI: ESC[44m = blauer Hintergrund", blue_ok);
    check("ANSI: ESC[0m stellt die Standardfarbe wieder her", default_ok);
    check("Scroll: letzte Zeile beschrieben", last_ok);
    check("Scroll: Zeile darueber leer", above_ok);
}

/* Misst, wie schnell die Konsole Text ausgibt und scrollt (nur die Konsole, ohne Serial) */
void test_console_speed(void)
{
    title("Konsolen-Geschwindigkeit");
    console_clear();
    const int lines = 300;
    uint64_t t0 = time_ms();
    for (int i = 0; i < lines; i++) {
        static const char text[] = "Zeile zum Testen der Scroll-Geschwindigkeit: 0123456789 abcdefghijklmnopqrstuvwxyz\n";
        for (const char *c = text; *c; c++)
            console_putc(*c);
    }
    uint64_t ms = time_ms() - t0;
    console_clear();
    title("Konsolen-Geschwindigkeit");
    kprintf("  %d Zeilen in %lu ms (%lu Zeilen/s)\n", lines, ms, ms ? lines * 1000 / ms : 0);
    check("Konsole schnell genug (300 Zeilen unter 1 s)", ms < 1000);
}

/* Zellen (Pixel) zweier Spalten in Zeile 0 identisch? */
static int cells_equal(uint32_t col_a, uint32_t col_b)
{
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 8; x++)
            if (console_read_pixel(col_a * 8 + x, y) != console_read_pixel(col_b * 8 + x, y))
                return 0;
    return 1;
}

void test_unicode(void)
{
    title("Unicode (UTF-8)");
    const char *s = "a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80";
    uint32_t a = utf8_next(&s), b = utf8_next(&s), c = utf8_next(&s), d = utf8_next(&s), e = utf8_next(&s);
    check("utf8_next: a, ae-Umlaut, Euro, Emoji, Ende", a == 'a' && b == 0xE4 && c == 0x20AC && d == 0x1F600 && e == 0);

    const char *bad = "\xC3(\xC0\x80";
    uint32_t b1 = utf8_next(&bad), b2 = utf8_next(&bad), b3 = utf8_next(&bad);
    check("Ungueltiges UTF-8 (abgebrochen, Ueberlaenge) wird zu U+FFFD", b1 == UNI_REPLACEMENT && b2 == '(' && b3 == UNI_REPLACEMENT);
    check("utf8_valid", utf8_valid("Pr\xC3\xBC" "fung") && !utf8_valid("\xE2\x82") && !utf8_valid("\xED\xA0\x80") && utf8_valid(""));

    uint16_t u16[8];
    int n = utf8_to_utf16("\xF0\x9F\x98\x80" "\xC3\xA4", u16, 8);
    char back[16];
    int bl = utf16_to_utf8(u16, n, back, sizeof(back));
    check("UTF-16: Ersatzpaar und Rueckweg", n == 3 && u16[0] == 0xD83D && u16[1] == 0xDE00 && u16[2] == 0xE4 &&
                                             bl == 6 && memcmp(back, "\xF0\x9F\x98\x80\xC3\xA4", 6) == 0);
    check("Grossbuchstaben (ASCII, Latin-1, Griechisch, Kyrillisch)",
          uni_upper('a') == 'A' && uni_upper(0xE4) == 0xC4 && uni_upper(0xDF) == 0xDF && uni_upper(0x3B1) == 0x391 &&
          uni_upper(0x430) == 0x410 && uni_upper(0xFF) == 0x178 && uni_upper(0x20AC) == 0x20AC);

    /* Konsole: Umlaute, Euro und ungueltige Bytes belegen je eine Zelle */
    console_clear();
    for (const char *p = "\xC3\xA4\xC3\xB6\xE2\x82\xAC\xFF?\xC3(x"; *p; p++)
        console_putc(*p);
    uint32_t col, row;
    console_get_cursor(&col, &row);
    int width_ok = col == 8 && row == 0; /* ae oe Euro | 0xFF ? | 0xC3 ( x -> FFFD ( x */
    int ink_ok = cell_has_ink(0, 0) && cell_has_ink(1, 0) && cell_has_ink(2, 0);
    int distinct_ok = !cells_equal(0, 4) && !cells_equal(0, 1) && !cells_equal(2, 4);
    int replace_ok = cells_equal(3, 4) && cells_equal(5, 4); /* ungueltige Folgen erscheinen wie '?' */
    console_clear();
    title("Unicode (UTF-8)");
    check("Konsole: eine Zelle je Zeichen (ae, oe, Euro, ungueltige Bytes)", width_ok);
    check("Konsole: Umlaute und Euro haben eigene Glyphen (nicht '?')", ink_ok && distinct_ok);
    check("Konsole: ungueltiges UTF-8 erscheint als '?'", replace_ok);
}

/* Zahl aus den Ziffern einer Zeile "Znnnnn" im sichtbaren Bild (Zeile row) */
static int line_number(uint32_t row)
{
    int v = 0;
    for (uint32_t c = 1; c <= 5; c++) {
        uint32_t ch = console_debug_char(c, row);
        if (ch < '0' || ch > '9')
            return -1;
        v = v * 10 + (int)(ch - '0');
    }
    return v;
}

static void put_line(int n)
{
    char l[16];
    int len = ksnprintf(l, sizeof(l), "Z%05d\n", n);
    for (int i = 0; i < len; i++)
        console_putc(l[i]);
}

void test_scrollback(void)
{
    title("Konsole: Verlauf (Scrollback)");
    console_clear();
    uint32_t r = console_rows();
    for (int i = 0; i < (int)(r * 3); i++)
        put_line(i);
    uint32_t hist = console_history_lines();
    int top = line_number(0);

    console_view_scroll(5);
    int back5 = line_number(0);
    int ink_ok = cell_has_ink(1, 0);
    uint64_t off5 = console_view_offset();
    console_view_scroll(-3);
    int back2 = line_number(0);
    console_view_live();
    int live = line_number(0);

    console_view_scroll(4);
    int top4 = line_number(0);
    console_putc('x'); /* neue Ausgabe laesst die Ansicht stehen ... */
    uint64_t after_output = console_view_offset();
    for (int i = 0; i < (int)r; i++) /* ... auch wenn dabei gescrollt wird: dieselben Zeilen bleiben sichtbar */
        put_line(1000 + i);
    int still = line_number(0) == top4 && console_view_offset() > 4;
    console_live_request(); /* ein Tastendruck springt zurueck zum Ende (ueber den Konsolen-Thread) */
    thread_sleep_ms(60);
    uint64_t after_key = console_view_offset();
    console_view_scroll(4);
    console_view_scroll(1 << 30);
    uint64_t at_top = console_view_offset();
    hist = console_history_lines(); /* durch die zusaetzlichen Zeilen gewachsen */
    console_view_scroll(-(1 << 30));
    uint64_t at_end = console_view_offset();

    console_clear();
    title("Konsole: Verlauf (Scrollback)");
    check("Aus dem Bild gescrollte Zeilen landen im Verlauf", hist >= r && top > 0);
    check("Blaettern: 5 Zeilen zurueck zeigt die Zeile 5 davor (und zeichnet sie)", off5 == 5 && back5 == top - 5 && ink_ok);
    check("Blaettern: 3 Zeilen vor", back2 == top - 2);
    check("Zurueck zum Ende (live)", live == top);
    check("Neue Ausgabe laesst die Ansicht stehen (auch beim Scrollen)", after_output == 4 && still);
    check("Tastendruck springt zum Ende zurueck", after_key == 0);
    check("Blaettern ist auf den Verlauf begrenzt", at_top == hist && at_end == 0);
}

void test_video(BootInfo *info)
{
    title("Grafik");
    kprintf("  Bildschirm %ux%u, %u Grafikmodi, Schrift x%u, Textfeld %ux%u\n", info->fb.width, info->fb.height,
            video_mode_count(), console_scale(), console_cols(), console_rows());
    VideoInfo vi;
    int have_cur = 0, sane = 1;
    for (unsigned i = 0; video_mode_info(i, &vi) == 0; i++) {
        sane &= vi.width >= 320 && vi.height >= 200;
        if (vi.current)
            have_cur = vi.width == info->fb.width && vi.height == info->fb.height;
    }
    check("Bootloader meldet Grafikmodi (oder die Firmware kennt nur einen)", video_mode_count() > 0 && sane);
    check("Der aktuelle Modus stimmt mit dem Framebuffer ueberein", have_cur);
    check("Textfeld passt in den Bildschirm", console_cols() * 8 * console_scale() <= info->fb.width &&
                                              console_rows() * 16 * console_scale() <= info->fb.height);

    /* Moduswechsel (igd_mode_set): Konsole auf eine kleinere Groesse und zurueck */
    uint32_t w0 = console_width_px(), h0 = console_height_px(), c0 = console_cols(), r0 = console_rows();
    int small = console_resize(w0 / 2, h0 / 2) == 0;
    uint32_t cs = console_cols(), rs = console_rows(), ws = console_width_px();
    kprintf("  verkleinert auf %ux%u: Textfeld %ux%u\n", ws, console_height_px(), cs, rs);
    int back = console_resize(w0, h0) == 0;
    check("Konsole passt sich einer kleineren Aufloesung an", small && ws == w0 / 2 &&
                                                              cs == (w0 / 2) / (8 * console_scale()) && rs < r0);
    check("... und kommt zur alten Groesse zurueck", back && console_width_px() == w0 && console_height_px() == h0 &&
                                                     console_cols() == c0 && console_rows() == r0);
    check("Groesser als der Framebuffer geht nicht", console_resize(w0, h0 + 64) != 0 && console_height_px() == h0);
}
