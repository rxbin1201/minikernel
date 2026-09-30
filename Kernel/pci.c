#include "pci.h"
#include "io.h"
#include "kprintf.h"

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC

static PciDevice devices[PCI_MAX_DEVICES];
static int device_count;

static uint32_t cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(CONFIG_ADDRESS, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    return inl(CONFIG_DATA);
}

static void cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    outl(CONFIG_ADDRESS, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    outl(CONFIG_DATA, v);
}

uint32_t pci_read32(const PciDevice *d, uint8_t offset)
{
    return cfg_read(d->bus, d->dev, d->fn, offset);
}

void pci_write32(const PciDevice *d, uint8_t offset, uint32_t value)
{
    cfg_write(d->bus, d->dev, d->fn, offset, value);
}

void pci_enable(const PciDevice *d, int io, int mem, int bus_master)
{
    /* Kommando-Register (untere 16 Bit); die Status-Bits darueber sind "write 1 to clear", also 0 schreiben */
    uint32_t cmd = pci_read32(d, 0x04) & 0xFFFF;
    if (io)
        cmd |= 1u << 0;
    if (mem)
        cmd |= 1u << 1;
    if (bus_master)
        cmd |= 1u << 2;
    pci_write32(d, 0x04, cmd);
}

static void add_function(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t id)
{
    if (device_count >= PCI_MAX_DEVICES)
        return;
    PciDevice *d = &devices[device_count++];
    d->bus = bus;
    d->dev = dev;
    d->fn = fn;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    uint32_t cls = cfg_read(bus, dev, fn, 0x08);
    d->class_code = cls >> 24;
    d->subclass   = (cls >> 16) & 0xFF;
    d->prog_if    = (cls >> 8) & 0xFF;
    d->revision   = cls & 0xFF;
    uint32_t sub = cfg_read(bus, dev, fn, 0x2C);
    d->sub_vendor = sub & 0xFFFF;
    d->sub_device = sub >> 16;
    d->driver[0] = 0;
    for (int i = 0; i < 6; i++)
        d->bar[i] = cfg_read(bus, dev, fn, 0x10 + 4 * i);
}

void pci_set_driver(const PciDevice *d, const char *name)
{
    for (int i = 0; i < device_count; i++) {
        PciDevice *e = &devices[i];
        if (e->bus == d->bus && e->dev == d->dev && e->fn == d->fn) {
            int k = 0;
            for (; name[k] && k < (int)sizeof(e->driver) - 1; k++)
                e->driver[k] = name[k];
            e->driver[k] = 0;
        }
    }
}

int pci_info(unsigned index, PciInfo *out)
{
    if (index >= (unsigned)device_count)
        return -1;
    const PciDevice *d = &devices[index];
    out->bus = d->bus;
    out->dev = d->dev;
    out->fn = d->fn;
    out->class_code = d->class_code;
    out->subclass = d->subclass;
    out->prog_if = d->prog_if;
    out->revision = d->revision;
    out->pad = 0;
    out->vendor = d->vendor;
    out->device = d->device;
    out->sub_vendor = d->sub_vendor;
    out->sub_device = d->sub_device;
    for (int i = 0; i < 6; i++)
        out->bar[i] = d->bar[i];
    for (int i = 0; i < (int)sizeof(out->driver); i++)
        out->driver[i] = d->driver[i];
    return 0;
}

int pci_scan(void)
{
    device_count = 0;
    for (unsigned bus = 0; bus < 256; bus++) {
        for (unsigned dev = 0; dev < 32; dev++) {
            uint32_t id = cfg_read(bus, dev, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF)
                continue; /* kein Geraet */
            uint32_t header = cfg_read(bus, dev, 0, 0x0C);
            unsigned functions = (header & 0x00800000) ? 8 : 1; /* Multi-Function-Bit */
            for (unsigned fn = 0; fn < functions; fn++) {
                id = cfg_read(bus, dev, fn, 0x00);
                if ((id & 0xFFFF) != 0xFFFF)
                    add_function(bus, dev, fn, id);
            }
        }
    }

    kprintf("pci: %d Geraete\n", device_count);
    for (int i = 0; i < device_count; i++) {
        const PciDevice *d = &devices[i];
        kprintf("  %02x:%02x.%u  %04x:%04x  Klasse %02x.%02x\n", d->bus, d->dev, d->fn, d->vendor, d->device,
                d->class_code, d->subclass);
    }
    return device_count;
}

int pci_find_class(uint8_t class_code, uint8_t subclass, int prog_if, unsigned index, PciDevice *out)
{
    unsigned seen = 0;
    for (int i = 0; i < device_count; i++) {
        const PciDevice *d = &devices[i];
        if (d->class_code != class_code || d->subclass != subclass || (prog_if >= 0 && d->prog_if != prog_if))
            continue;
        if (seen++ == index) {
            *out = *d;
            return 0;
        }
    }
    return -1;
}

uint64_t pci_bar_mem(const PciDevice *d, int bar)
{
    uint32_t lo = d->bar[bar];
    if (lo & 1)
        return 0; /* I/O-BAR */
    uint64_t base = lo & ~0xFULL;
    if (((lo >> 1) & 3) == 2 && bar < 5) /* 64-Bit-BAR: obere Haelfte im naechsten Register */
        base |= (uint64_t)d->bar[bar + 1] << 32;
    return base;
}

int pci_find(uint16_t vendor, uint16_t device, PciDevice *out)
{
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor == vendor && devices[i].device == device) {
            *out = devices[i];
            return 0;
        }
    }
    return -1;
}
