#ifndef IGD_H
#define IGD_H

#include <stdint.h>
#include "boot_info.h"

/* Intel-Grafik im Prozessor (IGD, PCI 00:02.0), Generation 9: Skylake, Kaby Lake, Coffee Lake (UHD Graphics 630
 * u.a.), Comet Lake. Stufe 1: erkennen und nur lesen, was die UEFI-Firmware eingerichtet hat (Pipes, Ebenen,
 * Anschluesse) und welche Pipe den GOP-Framebuffer zeigt. Es wird nichts geschrieben.
 * Registerangaben nach Intels "Programmer's Reference Manual" fuer Skylake/Kaby Lake (Vol. 2c) und dem Linux-i915. */

typedef struct {
    int      present;    /* Intel-GPU gefunden */
    int      gen9;       /* bekannte Gen9-GPU: nur dann werden Register gelesen */
    uint16_t device;
    const char *name;
    uint64_t mmio;       /* BAR0 (GTTMMADR): Register ab 0, globale GTT ab der Haelfte */
    uint64_t mmio_size;
    uint64_t aperture;   /* BAR2 (GMADR): CPU-Fenster in den Grafikspeicher (ueber die GTT) */
    int      scanout_pipe; /* Pipe (0 = A), die den GOP-Framebuffer zeigt, -1 = unbekannt */
    uint32_t scanout_surf; /* deren PLANE_SURF (Adresse im Grafik-Adressraum) */
} IgdInfo;

void igd_init(const BootInfo *info);
const IgdInfo *igd_info(void);

#endif
