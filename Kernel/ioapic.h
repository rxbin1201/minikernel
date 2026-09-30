#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>

/* Mappt alle IOAPICs aus der MADT und maskiert ihre Eintraege. 0 = ok. */
int ioapic_init(void);

/* Leitet ISA-IRQ `irq` (unter Beachtung der MADT-Overrides) als `vector` an den Boot-CPU. 0 = ok. */
int ioapic_route_isa_irq(uint8_t irq, uint8_t vector);

#endif
