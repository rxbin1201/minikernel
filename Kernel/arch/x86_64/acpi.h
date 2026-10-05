#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_ISOS    16
#define ACPI_MAX_CPUS    64

typedef struct {
    uint8_t  id;
    uint32_t addr;
    uint32_t gsi_base;
} AcpiIoApic;

/* Interrupt Source Override: ISA-IRQ `irq` liegt tatsaechlich an GSI `gsi`. */
typedef struct {
    uint8_t  irq;
    uint32_t gsi;
    uint16_t flags; /* Bit 0-1: Polaritaet (3 = low), Bit 2-3: Trigger (3 = level) */
} AcpiIso;

typedef struct {
    int      valid;
    uint64_t lapic_addr;
    int      cpu_count;                  /* aktivierte Local APICs */
    uint8_t  cpu_apic_id[ACPI_MAX_CPUS];
    int      ioapic_count;
    AcpiIoApic ioapics[ACPI_MAX_IOAPICS];
    int      iso_count;
    AcpiIso  isos[ACPI_MAX_ISOS];
} AcpiInfo;

/* Liest RSDP -> XSDT/RSDT -> MADT. Gibt 0 bei Erfolg zurueck. */
int acpi_init(void *rsdp);

const AcpiInfo *acpi_info(void);

/* Tabelle mit der Signatur sig (4 Zeichen, z.B. "DMAR") samt Kopf, Laenge nach *len (darf 0 sein); 0 = keine */
const void *acpi_table(const char *sig, uint32_t *len);

/* Ausschalten (ACPI S5) bzw. Neustart. Beides versucht mehrere Wege nacheinander (ACPI-Register, dann bekannte
 * Emulator-/Chipsatz-Ports) und kehrt nur zurueck, wenn keiner geholfen hat. Interrupts werden abgeschaltet. */
void acpi_shutdown(void);
void acpi_reboot(void);

#endif
