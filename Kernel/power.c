#include "power.h"
#include "acpi.h"
#include "blk.h"
#include "cpu.h"
#include "kprintf.h"

static void __attribute__((noreturn)) halt_forever(void)
{
    cpu_cli();
    for (;;)
        cpu_hlt();
}

void power_off(void)
{
    kprintf("\nSystem wird ausgeschaltet...\n");
    blk_flush_all();
    acpi_shutdown(); /* kehrt nur zurueck, wenn es nicht geklappt hat */
    kprintf("Ausschalten per ACPI hat nicht funktioniert. Du kannst den Rechner jetzt ausschalten.\n");
    halt_forever();
}

void power_reboot(void)
{
    kprintf("\nSystem wird neu gestartet...\n");
    blk_flush_all();
    acpi_reboot(); /* kehrt nur zurueck, wenn es nicht geklappt hat */
    kprintf("Neustart hat nicht funktioniert. Bitte den Rechner selbst neu starten.\n");
    halt_forever();
}
