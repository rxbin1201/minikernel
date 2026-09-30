#include "ioapic.h"
#include "acpi.h"
#include "apic.h"
#include "kprintf.h"
#include "paging.h"

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WIN    0x10
#define IOAPIC_VER    0x01
#define IOAPIC_REDTBL 0x10

#define REDIR_MASKED     (1u << 16)
#define REDIR_LEVEL      (1u << 15)
#define REDIR_ACTIVE_LOW (1u << 13)

static uint32_t ioapic_read(const AcpiIoApic *io, uint32_t reg)
{
    volatile uint32_t *base = (volatile uint32_t *)(uint64_t)io->addr;
    base[IOAPIC_REGSEL / 4] = reg;
    return base[IOAPIC_WIN / 4];
}

static void ioapic_write(const AcpiIoApic *io, uint32_t reg, uint32_t v)
{
    volatile uint32_t *base = (volatile uint32_t *)(uint64_t)io->addr;
    base[IOAPIC_REGSEL / 4] = reg;
    base[IOAPIC_WIN / 4] = v;
}

static uint32_t max_redirection(const AcpiIoApic *io)
{
    return (ioapic_read(io, IOAPIC_VER) >> 16) & 0xFF; /* Index des letzten Eintrags */
}

int ioapic_init(void)
{
    const AcpiInfo *acpi = acpi_info();
    if (!acpi->valid || acpi->ioapic_count == 0) {
        kprintf("ioapic: keiner gefunden\n");
        return -1;
    }

    for (int i = 0; i < acpi->ioapic_count; i++) {
        const AcpiIoApic *io = &acpi->ioapics[i];
        if (paging_map_mmio(io->addr, 4096) != 0) {
            kprintf("ioapic: MMIO @ %#x nicht mappbar\n", io->addr);
            return -1;
        }
        uint32_t last = max_redirection(io);
        for (uint32_t n = 0; n <= last; n++)
            ioapic_write(io, IOAPIC_REDTBL + 2 * n, REDIR_MASKED | 0xFF);
        kprintf("ioapic: ID %u @ %#x, GSI %u-%u\n", io->id, io->addr, io->gsi_base, io->gsi_base + last);
    }
    return 0;
}

int ioapic_route_isa_irq(uint8_t irq, uint8_t vector)
{
    const AcpiInfo *acpi = acpi_info();

    /* ISA-Standard: GSI = IRQ, Flanke, active high; die MADT kann das ueberschreiben */
    uint32_t gsi = irq;
    uint32_t low = vector;
    for (int i = 0; i < acpi->iso_count; i++) {
        if (acpi->isos[i].irq != irq)
            continue;
        gsi = acpi->isos[i].gsi;
        if ((acpi->isos[i].flags & 3) == 3)
            low |= REDIR_ACTIVE_LOW;
        if (((acpi->isos[i].flags >> 2) & 3) == 3)
            low |= REDIR_LEVEL;
    }

    for (int i = 0; i < acpi->ioapic_count; i++) {
        const AcpiIoApic *io = &acpi->ioapics[i];
        uint32_t last = max_redirection(io);
        if (gsi < io->gsi_base || gsi > io->gsi_base + last)
            continue;
        uint32_t n = gsi - io->gsi_base;
        ioapic_write(io, IOAPIC_REDTBL + 2 * n + 1, apic_id() << 24); /* Ziel: Boot-CPU */
        ioapic_write(io, IOAPIC_REDTBL + 2 * n, low);                 /* zuletzt: entmaskiert */
        return 0;
    }
    kprintf("ioapic: kein IOAPIC fuer GSI %u\n", gsi);
    return -1;
}
