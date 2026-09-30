#ifndef CMDLINE_H
#define CMDLINE_H

/* Kernel-Kommandozeile: optionale Textdatei \cmdline.txt auf der EFI-Systempartition, vom Bootloader gelesen.
 * Woerter sind durch Leerzeichen getrennt; "schluessel=wert" liefert cmdline_get("schluessel").
 * Bekannte Optionen:
 *   selftest        fuehrt beim Start alle Selbsttests aus (dauert 20-30 s)
 *   init=<pfad>     startet dieses Programm statt /bin/sh (z.B. init=/bin/poweroff) */

void cmdline_init(const char *line);
int cmdline_has(const char *word);       /* Wort kommt vor (auch als schluessel=...) */
const char *cmdline_get(const char *key); /* Wert hinter "key=" oder NULL */

#endif
