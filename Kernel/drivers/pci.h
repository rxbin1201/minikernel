#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 64

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if, revision;
    uint16_t sub_vendor, sub_device;
    uint32_t bar[6];
    char     driver[16]; /* Name des Treibers, der das Geraet benutzt ("" = keiner) */
} PciDevice;

/* Eintrag fuer SYS_PCIINFO (gleiches Layout in Userland/user.h) */
typedef struct {
    uint8_t  bus, dev, fn, class_code, subclass, prog_if, revision, pad;
    uint16_t vendor, device, sub_vendor, sub_device;
    uint32_t bar[6];
    char     driver[16];
} PciInfo;

/* Durchsucht alle Busse per Konfigurationsmechanismus #1 (Ports 0xCF8/0xCFC) und gibt die Geraete aus. */
int pci_scan(void);

/* Sucht das index-te Geraet mit dieser Vendor/Device-ID. 0 = gefunden. */
int pci_find(uint16_t vendor, uint16_t device, PciDevice *out);

/* Sucht das index-te Geraet mit dieser Klasse/Unterklasse (prog_if < 0: egal). 0 = gefunden. */
int pci_find_class(uint8_t class_code, uint8_t subclass, int prog_if, unsigned index, PciDevice *out);

/* Basisadresse eines Memory-BARs (32 oder 64 Bit). 0, wenn der BAR ein I/O-BAR oder leer ist. */
uint64_t pci_bar_mem(const PciDevice *d, int bar);

uint32_t pci_read32(const PciDevice *d, uint8_t offset);
void     pci_write32(const PciDevice *d, uint8_t offset, uint32_t value);

/* Aktiviert I/O-Zugriff (io), Memory-Zugriff (mem) und Bus-Mastering (DMA) des Geraets. */
void pci_enable(const PciDevice *d, int io, int mem, int bus_master);

/* Speicher und Bus-Master auf allen Bruecken zwischen Bus 0 und dem Geraet einschalten - sonst kommt DMA eines
 * Geraets hinter einer Bruecke (PCIe-Steckplatz) nicht im RAM an. Gibt die Zahl der geaenderten Bruecken zurueck. */
int pci_enable_upstream(const PciDevice *d);

/* Vermerkt, welcher Treiber das Geraet benutzt (fuer lspci) */
void pci_set_driver(const PciDevice *d, const char *name);

/* Offset der Capability mit dieser ID im Konfigurationsraum, 0 = nicht vorhanden */
uint8_t pci_find_cap(const PciDevice *d, uint8_t id);

/* Nachrichten-Interrupts: das Geraet schickt seinen Interrupt direkt als Vektor an den Local APIC mit apic_id.
 * Bevorzugt MSI-X (Eintrag 0), sonst MSI; die Interrupt-Leitung (INTx) wird abgeschaltet.
 * 2 = MSI-X, 1 = MSI, 0 = keins von beiden (dann weiter pollen). */
int pci_enable_msi(const PciDevice *d, uint8_t vector, uint32_t apic_id);
/* Nur MSI (fuer Geraete, deren MSI-X erst eigens programmiert werden muesste); 1 = an, 0 = nicht moeglich */
int pci_enable_msi_only(const PciDevice *d, uint8_t vector, uint32_t apic_id);

/* index-tes Geraet fuer SYS_PCIINFO; 0 = ok, -1 = Ende */
int pci_info(unsigned index, PciInfo *out);

#endif
