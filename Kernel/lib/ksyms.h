#ifndef KSYMS_H
#define KSYMS_H

#include <stdint.h>

/* Symboltabelle des Kernels (von tools/mksyms.py beim Bauen erzeugt) und Backtraces ueber die Framepointer-Kette
 * (der Kernel wird mit -fno-omit-frame-pointer gebaut). */

typedef struct {
    uint64_t addr;
    uint32_t name; /* Offset in ksym_names */
} KSym;

extern const unsigned ksym_count;
extern const KSym     ksym_table[];
extern const char     ksym_names[];

/* Name der Funktion, in der addr liegt, und der Abstand zu ihrem Anfang; NULL = unbekannt */
const char *ksym_lookup(uint64_t addr, uint64_t *offset);

/* Gibt "name+0x12" (oder die blosse Adresse) per kprintf aus */
void ksym_print(uint64_t addr);

/* Aufrufkette ab (rip, rbp) ausgeben, hoechstens max_frames Rahmen */
void backtrace_print(uint64_t rip, uint64_t rbp, int max_frames);

#endif
