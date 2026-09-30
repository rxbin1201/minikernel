#include "arch/x86_64/acpi.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"
#include "mm/paging.h"
#include "lib/string.h"

typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) Rsdp;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) SdtHeader;

typedef struct {
    SdtHeader header;
    uint32_t  lapic_addr;
    uint32_t  flags;
    /* danach Eintraege: type (u8), length (u8), Daten */
} __attribute__((packed)) Madt;

static AcpiInfo info;

/* Aus der FADT (Signatur "FACP") und dem _S5_-Objekt der DSDT */
static struct {
    uint32_t pm1a_cnt, pm1b_cnt; /* I/O-Ports der Power-Management-Kontrollregister */
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    int      have_s5;
    uint8_t  slp_typ_a, slp_typ_b;
    int      reset_valid;
    uint8_t  reset_space;        /* 0 = Speicher, 1 = I/O-Port */
    uint64_t reset_addr;
    uint8_t  reset_value;
} power;

const AcpiInfo *acpi_info(void)
{
    return &info;
}

static uint8_t checksum(const void *p, uint64_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;
    while (len--)
        sum += *b++;
    return sum;
}

/* Alle Seiten von [addr, addr+len) gemappt? Schuetzt vor Page Faults durch kaputte Zeiger. */
static int range_mapped(uint64_t addr, uint64_t len)
{
    for (uint64_t a = addr & ~4095ULL; a < addr + len; a += 4096)
        if (!paging_translate(a, 0, 0))
            return 0;
    return 1;
}

static const SdtHeader *map_table(uint64_t addr)
{
    if (!addr || !range_mapped(addr, sizeof(SdtHeader)))
        return 0;
    const SdtHeader *h = (const SdtHeader *)addr;
    if (h->length < sizeof(SdtHeader) || !range_mapped(addr, h->length))
        return 0;
    if (checksum(h, h->length) != 0) {
        kprintf("acpi: Checksumme falsch bei %.4s @ %#lx\n", h->signature, addr);
        return 0;
    }
    return h;
}

/* Sucht in der DSDT das Objekt "\_S5_" (Package mit den SLP_TYP-Werten fuer den Schlafzustand S5 = ausgeschaltet).
 * Bytes: NameOp (0x08), Name "_S5_", PackageOp (0x12), PkgLength, Elementanzahl, dann die Werte
 * (jeweils optional mit BytePrefix 0x0A). Das ist der uebliche Minimalweg, ohne einen AML-Interpreter zu brauchen. */
static void find_s5(uint64_t dsdt_addr)
{
    const SdtHeader *d = map_table(dsdt_addr);
    if (!d)
        return;
    const uint8_t *p = (const uint8_t *)d;
    for (uint32_t i = 36; i + 12 < d->length; i++) {
        if (memcmp(p + i, "_S5_", 4) != 0)
            continue;
        if (!(p[i - 1] == 0x08 || (p[i - 1] == '\\' && p[i - 2] == 0x08)))
            continue;
        const uint8_t *q = p + i + 4;
        if (*q != 0x12)
            continue;
        q++;
        q += ((*q >> 6) & 3) + 1; /* PkgLength: obere 2 Bit = Anzahl der Folgebytes */
        q++;                      /* Anzahl der Elemente */
        if (*q == 0x0A)
            q++;
        power.slp_typ_a = *q++;
        if (*q == 0x0A)
            q++;
        power.slp_typ_b = *q;
        power.have_s5 = 1;
        return;
    }
}

static void parse_fadt(const SdtHeader *h)
{
    const uint8_t *b = (const uint8_t *)h;
    if (h->length < 116)
        return;

    uint64_t dsdt = *(const uint32_t *)(b + 40);
    power.smi_cmd     = *(const uint32_t *)(b + 48);
    power.acpi_enable = b[52];
    power.pm1a_cnt    = *(const uint32_t *)(b + 64);
    power.pm1b_cnt    = *(const uint32_t *)(b + 68);
    if (h->length >= 148 && *(const uint64_t *)(b + 140)) /* X_DSDT (64 Bit) hat Vorrang */
        dsdt = *(const uint64_t *)(b + 140);

    if (h->length >= 129 && (*(const uint32_t *)(b + 112) & (1u << 10))) { /* RESET_REG_SUP */
        power.reset_space = b[116];
        power.reset_addr  = *(const uint64_t *)(b + 120);
        power.reset_value = b[128];
        power.reset_valid = 1;
    }
    find_s5(dsdt);
}

static void parse_madt(const Madt *madt)
{
    info.lapic_addr = madt->lapic_addr;

    const uint8_t *p   = (const uint8_t *)madt + sizeof(Madt);
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (p + 2 <= end && p[1] >= 2) {
        uint8_t type = p[0], len = p[1];
        if (p + len > end)
            break;

        switch (type) {
        case 0: /* Processor Local APIC: acpi_id, apic_id, flags */
            if (len >= 8) {
                uint32_t flags = *(const uint32_t *)(p + 4);
                if ((flags & 3) && info.cpu_count < ACPI_MAX_CPUS) /* enabled oder online-capable */
                    info.cpu_apic_id[info.cpu_count++] = p[3];
            }
            break;
        case 1: /* I/O APIC: id, reserved, addr, gsi_base */
            if (len >= 12 && info.ioapic_count < ACPI_MAX_IOAPICS) {
                AcpiIoApic *io = &info.ioapics[info.ioapic_count++];
                io->id       = p[2];
                io->addr     = *(const uint32_t *)(p + 4);
                io->gsi_base = *(const uint32_t *)(p + 8);
            }
            break;
        case 2: /* Interrupt Source Override: bus, source, gsi, flags */
            if (len >= 10 && info.iso_count < ACPI_MAX_ISOS) {
                AcpiIso *iso = &info.isos[info.iso_count++];
                iso->irq   = p[3];
                iso->gsi   = *(const uint32_t *)(p + 4);
                iso->flags = *(const uint16_t *)(p + 8);
            }
            break;
        case 5: /* Local APIC Address Override */
            if (len >= 12)
                info.lapic_addr = *(const uint64_t *)(p + 4);
            break;
        }
        p += len;
    }
}

int acpi_init(void *rsdp_ptr)
{
    memset(&info, 0, sizeof(info));

    uint64_t rsdp_addr = (uint64_t)rsdp_ptr;
    if (!rsdp_addr || !range_mapped(rsdp_addr, sizeof(Rsdp))) {
        kprintf("acpi: kein (gueltiger) RSDP\n");
        return -1;
    }
    const Rsdp *rsdp = rsdp_ptr;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || checksum(rsdp, 20) != 0) {
        kprintf("acpi: RSDP ungueltig\n");
        return -1;
    }

    /* XSDT (64-Bit-Zeiger) bevorzugen, sonst RSDT (32-Bit) */
    int use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_addr;
    const SdtHeader *root = map_table(use_xsdt ? rsdp->xsdt_addr : rsdp->rsdt_addr);
    if (!root) {
        kprintf("acpi: %s nicht lesbar\n", use_xsdt ? "XSDT" : "RSDT");
        return -1;
    }

    uint32_t entry_size = use_xsdt ? 8 : 4;
    uint32_t entries    = (root->length - sizeof(SdtHeader)) / entry_size;
    const uint8_t *tbl  = (const uint8_t *)root + sizeof(SdtHeader);

    kprintf("acpi: rev %u, %s mit %u Tabellen:", rsdp->revision, use_xsdt ? "XSDT" : "RSDT", entries);
    const Madt *madt = 0;
    const SdtHeader *fadt = 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t addr = use_xsdt ? *(const uint64_t *)(tbl + i * 8) : *(const uint32_t *)(tbl + i * 4);
        const SdtHeader *h = map_table(addr);
        if (!h)
            continue;
        kprintf(" %.4s", h->signature);
        if (memcmp(h->signature, "APIC", 4) == 0 && h->length >= sizeof(Madt))
            madt = (const Madt *)h;
        if (memcmp(h->signature, "FACP", 4) == 0)
            fadt = h;
    }
    kprintf("\n");

    if (!madt) {
        kprintf("acpi: keine MADT gefunden\n");
        return -1;
    }
    parse_madt(madt);
    if (fadt)
        parse_fadt(fadt);
    info.valid = 1;

    kprintf("acpi: LAPIC @ %#lx, %d CPU(s), %d IOAPIC(s), %d Override(s)\n",
            info.lapic_addr, info.cpu_count, info.ioapic_count, info.iso_count);
    return 0;
}

/* ---------- Ausschalten / Neustart ---------- */

static void delay_ms(uint32_t ms)
{
    (void)WAIT_UNTIL(0, ms); /* wartet die volle Zeit (Bedingung ist nie wahr), auch mit ausgeschalteten Interrupts */
}

void acpi_shutdown(void)
{
    cpu_cli();

    kprintf("acpi: Ausschalten, PM1a %#x, %s (SLP_TYP %u/%u)\n", power.pm1a_cnt,
            power.have_s5 ? "_S5_ gefunden" : "kein _S5_ gefunden", power.slp_typ_a, power.slp_typ_b);
    if (power.pm1a_cnt && power.have_s5) {
        /* Ist ACPI noch nicht aktiv (SCI_EN, Bit 0), zuerst beim BIOS anfordern; unter UEFI ist es das meist schon */
        if (!(inw((uint16_t)power.pm1a_cnt) & 1) && power.smi_cmd && power.acpi_enable) {
            outb((uint16_t)power.smi_cmd, power.acpi_enable);
            (void)WAIT_UNTIL(inw((uint16_t)power.pm1a_cnt) & 1, 3000);
        }
        outw((uint16_t)power.pm1a_cnt, (uint16_t)((power.slp_typ_a << 10) | (1u << 13))); /* SLP_TYP + SLP_EN */
        if (power.pm1b_cnt)
            outw((uint16_t)power.pm1b_cnt, (uint16_t)((power.slp_typ_b << 10) | (1u << 13)));
        delay_ms(500);
    }

    kprintf("acpi: ACPI hat nicht ausgeschaltet, versuche Emulator-Ports\n");
    /* Bekannte Ports von Emulatoren, falls ACPI nichts bewirkt hat (QEMU/Bochs/VirtualBox) */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    delay_ms(500);
}

void acpi_reboot(void)
{
    cpu_cli();

    /* 1. ACPI-Reset-Register (aus der FADT) */
    kprintf("acpi: Neustart, Reset-Register %s\n", power.reset_valid ? "vorhanden" : "nicht vorhanden");
    if (power.reset_valid) {
        if (power.reset_space == 1) {
            outb((uint16_t)power.reset_addr, power.reset_value);
        } else if (power.reset_space == 0 && paging_map_mmio(power.reset_addr, 1) == 0) {
            *(volatile uint8_t *)power.reset_addr = power.reset_value;
        }
        delay_ms(500);
    }

    kprintf("acpi: Reset-Register hat nicht gereicht, versuche Chipsatz-Reset (0xCF9)\n");
    /* 2. Reset-Steuerregister des Chipsatzes: erst Reset anfordern, dann ausloesen (harter Reset) */
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    delay_ms(500);

    kprintf("acpi: versuche Tastaturcontroller-Reset\n");
    /* 3. Tastaturcontroller: Reset-Leitung pulsen */
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
        ;
    outb(0x64, 0xFE);
    delay_ms(500);

    kprintf("acpi: letzte Moeglichkeit: Triple Fault\n");
    /* 4. Letzte Moeglichkeit: Triple Fault mit leerer IDT */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) empty = {0, 0};
    __asm__ __volatile__("lidt %0; int3" : : "m"(empty));
}
