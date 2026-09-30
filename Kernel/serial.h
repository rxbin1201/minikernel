#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c); /* roh, ohne \n -> \r\n */
void serial_puts(const char *s);
void serial_put_hex(uint64_t v);
void serial_put_dec(uint64_t v);

#endif
