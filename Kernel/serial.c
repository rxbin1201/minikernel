#include "serial.h"
#include "io.h"

#define COM1 0x3F8

static int present; /* 1, wenn am COM1-Port wirklich ein UART haengt */

void serial_init(void)
{
    /* Scratch-Register testen: ohne UART (z.B. Rechner ohne COM-Port) lesen wir nichts zurueck und sparen uns die
     * langsamen Port-Zugriffe fuer jedes Zeichen */
    outb(COM1 + 7, 0xAE);
    present = inb(COM1 + 7) == 0xAE;
    if (!present)
        return;

    outb(COM1 + 1, 0x00); /* Interrupts aus */
    outb(COM1 + 3, 0x80); /* DLAB an */
    outb(COM1 + 0, 0x01); /* Divisor 1 -> 115200 Baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1, DLAB aus */
    outb(COM1 + 2, 0xC7); /* FIFO an */
    outb(COM1 + 4, 0x03); /* DTR + RTS */
}

void serial_putc(char c)
{
    if (!present)
        return;
    /* Bounded warten: haengt der UART, soll der Kernel nicht stehen bleiben */
    for (int spins = 0; !(inb(COM1 + 5) & 0x20); spins++)
        if (spins > 100000)
            return;
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s);
    }
}

void serial_put_hex(uint64_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(digits[(v >> i) & 0xF]);
}

void serial_put_dec(uint64_t v)
{
    char buf[21];
    int i = 20;
    buf[i] = 0;
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    } while (v);
    serial_puts(&buf[i]);
}
