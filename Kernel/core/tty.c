#include "core/tty.h"
#include "arch/x86_64/cpu.h"
#include "drivers/keyboard.h"
#include "lib/kprintf.h"
#include "core/process.h"
#include "lib/string.h"
#include "console/console.h"

#define LINE_MAX 256

static volatile int      raw_mode;
static volatile uint32_t fg_pgid;

/* Zeilenmodus: angefangene Zeile und fertige Zeile, die read() noch abholt */
static char line[LINE_MAX];
static int  line_len;
static char ready[LINE_MAX];
static int  ready_len, ready_pos;

void tty_set_mode(int raw)
{
    raw_mode = raw != 0;
    line_len = ready_len = ready_pos = 0; /* halbe Zeilen beim Moduswechsel verwerfen */
}

void tty_set_fg(uint32_t pgid)  { fg_pgid = pgid; }
uint32_t tty_get_fg(void)       { return fg_pgid; }

int tty_interrupt(void)
{
    if (!fg_pgid)
        return 0;
    process_kill_pgid(fg_pgid);
    return 1;
}

int64_t tty_write(const void *buf, uint64_t len)
{
    const char *s = buf;
    for (uint64_t i = 0; i < len;) {
        uint64_t f = irq_save(); /* kleine Stuecke am Stueck, damit sich Ausgaben nicht vermischen */
        for (int n = 0; n < 128 && i < len; n++, i++)
            kputc(s[i]);
        irq_restore(f);
    }
    return (int64_t)len;
}

static void echo(char c)
{
    uint64_t f = irq_save();
    kputc(c);
    irq_restore(f);
}

/* Naechstes Zeichen von der Tastatur; blockiert. -1 = unterbrochen (Prozess gekillt). */
static int next_key(void)
{
    for (;;) {
        /* Waehrend ein Grafikprogramm den Bildschirm hat (z.B. der Desktop), gehoeren ihm die Tasten: eine Shell auf der
         * unsichtbaren Konsole wartet so lange, statt ihm Tasten wegzunehmen */
        if (console_gfx_active() && !console_gfx_owner(process_pid(process_current()))) {
            if (process_wait_tick() != 0)
                return -1;
            continue;
        }
        int c = keyboard_getchar();
        if (c == KEY_ALT) { /* Alt-Kombination: gehoert den Grafikprogrammen */
            keyboard_getchar();
            continue;
        }
        if (c == KEY_MODS) { /* Umschalttasten + Taste: ohne Alt bleibt die Taste */
            int mods = keyboard_getchar(), k = keyboard_getchar();
            if (mods < 0 || k < 0 || (mods & 2))
                continue;
            return k;
        }
        if (c >= 0)
            return c;
        if (process_wait_tick() != 0)
            return -1;
    }
}

static int64_t read_raw(char *out, uint64_t len)
{
    int c = next_key();
    if (c < 0)
        return ERR_INTR;
    out[0] = (char)c;
    uint64_t n = 1;
    /* Was sonst schon bereitliegt, gleich mitnehmen (z.B. eingefuegter Text) */
    while (n < len && (c = keyboard_getchar()) >= 0)
        out[n++] = (char)c;
    return (int64_t)n;
}

static int64_t read_cooked(char *out, uint64_t len)
{
    for (;;) {
        if (ready_pos < ready_len) { /* Rest einer fertigen Zeile ausliefern */
            uint64_t n = (uint64_t)(ready_len - ready_pos);
            if (n > len)
                n = len;
            memcpy(out, ready + ready_pos, n);
            ready_pos += (int)n;
            if (ready_pos >= ready_len)
                ready_len = ready_pos = 0;
            return (int64_t)n;
        }

        int c = next_key();
        if (c < 0)
            return ERR_INTR;

        if (c == '\n') {
            echo('\n');
            line[line_len++] = '\n';
            memcpy(ready, line, (size_t)line_len);
            ready_len = line_len;
            ready_pos = 0;
            line_len = 0;
        } else if (c == '\b' || c == 0x7F) {
            if (line_len > 0) {
                line_len--;
                while (line_len > 0 && ((unsigned char)line[line_len] & 0xC0) == 0x80)
                    line_len--; /* ganzes UTF-8-Zeichen */
                echo('\b');
                echo(' ');
                echo('\b');
            }
        } else if (c == 0x04) { /* Ctrl-D */
            if (line_len == 0)
                return 0; /* Dateiende */
            memcpy(ready, line, (size_t)line_len); /* Zeile ohne Zeilenende abgeben */
            ready_len = line_len;
            ready_pos = 0;
            line_len = 0;
        } else if (c == 0x03) { /* Ctrl-C ohne Vordergrundgruppe: Zeile verwerfen */
            kprintf("^C\n");
            line_len = 0;
        } else if (((c >= 32 && c < 127) || (c >= 0x80 && c < 0xF5)) && line_len < LINE_MAX - 5) {
            line[line_len++] = (char)c;
            echo((char)c);
        }
        /* Sondertasten (0xF5..0xFF) und andere Steuerzeichen ignoriert der Zeilenmodus */
    }
}

int64_t tty_read(void *buf, uint64_t len)
{
    if (len == 0)
        return 0;
    return raw_mode ? read_raw(buf, len) : read_cooked(buf, len);
}
