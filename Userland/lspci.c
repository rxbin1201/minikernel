#include "libc.h"

/* lspci: listet die PCI-Geraete mit Hersteller, Art und (falls bekannt) Modell sowie dem Kernel-Treiber.
 *   lspci      Uebersicht
 *   lspci -v   zusaetzlich Revision, Subsystem-ID und Speicherbereiche (BARs)
 *   lspci -n   nur Nummern (Hersteller:Geraet), ohne Namen */

typedef struct {
    unsigned short id;
    const char    *name;
} Name;

static const Name vendors[] = {
    {0x8086, "Intel"},     {0x8087, "Intel"},       {0x1022, "AMD"},         {0x1002, "AMD/ATI"},    {0x10DE, "NVIDIA"},
    {0x10EC, "Realtek"},   {0x14E4, "Broadcom"},    {0x168C, "Qualcomm Atheros"}, {0x1969, "Qualcomm Atheros"},
    {0x17CB, "Qualcomm"},  {0x14C3, "MediaTek"},    {0x1814, "Ralink"},      {0x1B21, "ASMedia"},    {0x1B4B, "Marvell"},
    {0x197B, "JMicron"},   {0x1106, "VIA"},         {0x1912, "Renesas"},     {0x104C, "Texas Instruments"},
    {0x144D, "Samsung"},   {0x15B7, "SanDisk/WD"},  {0x1987, "Phison"},      {0x1C5C, "SK hynix"},   {0x126F, "Silicon Motion"},
    {0x1E0F, "KIOXIA"},    {0x1179, "Toshiba"},     {0x2646, "Kingston"},    {0x1344, "Micron"},     {0x1CC1, "ADATA"},
    {0x1E4B, "MAXIO"},     {0x1D97, "Shenzhen Longsys"}, {0x1CC4, "Union Memory"}, {0x10B5, "PLX/Broadcom"},
    {0x1217, "O2 Micro"},  {0x1AF4, "Red Hat (virtio)"}, {0x1B36, "Red Hat (QEMU)"}, {0x1234, "QEMU/Bochs"},
    {0x15AD, "VMware"},    {0x80EE, "VirtualBox"},  {0x1414, "Microsoft"},   {0x1D6B, "Linux Foundation"},
};

/* Bekannte Intel-Geraete, vor allem Netzwerkkarten (die der Kernel mit dem e1000-Treiber betreibt) */
static const Name intel[] = {
    {0x100E, "82540EM Gigabit Ethernet"}, {0x100F, "82545EM Gigabit Ethernet"}, {0x10D3, "82574L Gigabit Ethernet"},
    {0x10F6, "82574LA Gigabit Ethernet"}, {0x153A, "Ethernet Connection I217-LM"}, {0x153B, "Ethernet Connection I217-V"},
    {0x155A, "Ethernet Connection I218-LM"}, {0x1559, "Ethernet Connection I218-V"}, {0x15A0, "Ethernet Connection (2) I218-LM"},
    {0x15A1, "Ethernet Connection (2) I218-V"}, {0x15A2, "Ethernet Connection (3) I218-LM"}, {0x15A3, "Ethernet Connection (3) I218-V"},
    {0x156F, "Ethernet Connection I219-LM"}, {0x1570, "Ethernet Connection I219-V"}, {0x15B7, "Ethernet Connection (2) I219-LM"},
    {0x15B8, "Ethernet Connection (2) I219-V"}, {0x15B9, "Ethernet Connection (3) I219-LM"}, {0x15D7, "Ethernet Connection (4) I219-LM"},
    {0x15D8, "Ethernet Connection (4) I219-V"}, {0x15E3, "Ethernet Connection (5) I219-LM"}, {0x15D6, "Ethernet Connection (5) I219-V"},
    {0x15BD, "Ethernet Connection (6) I219-LM"}, {0x15BE, "Ethernet Connection (6) I219-V"}, {0x15BB, "Ethernet Connection (7) I219-LM"},
    {0x15BC, "Ethernet Connection (7) I219-V"}, {0x15DF, "Ethernet Connection (8) I219-LM"}, {0x15E0, "Ethernet Connection (8) I219-V"},
    {0x15E1, "Ethernet Connection (9) I219-LM"}, {0x15E2, "Ethernet Connection (9) I219-V"}, {0x0D4E, "Ethernet Connection (10) I219-LM"},
    {0x0D4F, "Ethernet Connection (10) I219-V"}, {0x0D4C, "Ethernet Connection (11) I219-LM"}, {0x0D4D, "Ethernet Connection (11) I219-V"},
    {0x0D53, "Ethernet Connection (12) I219-LM"}, {0x0D55, "Ethernet Connection (12) I219-V"}, {0x15FB, "Ethernet Connection (13) I219-LM"},
    {0x15FC, "Ethernet Connection (13) I219-V"}, {0x15F9, "Ethernet Connection (14) I219-LM"}, {0x15FA, "Ethernet Connection (14) I219-V"},
    {0x15F4, "Ethernet Connection (15) I219-LM"}, {0x15F5, "Ethernet Connection (15) I219-V"}, {0x1A1E, "Ethernet Connection (16) I219-LM"},
    {0x1A1F, "Ethernet Connection (16) I219-V"}, {0x1A1C, "Ethernet Connection (17) I219-LM"}, {0x1A1D, "Ethernet Connection (17) I219-V"},
    {0x550A, "Ethernet Connection (18) I219-LM"}, {0x550B, "Ethernet Connection (18) I219-V"}, {0x550C, "Ethernet Connection (19) I219-LM"},
    {0x550D, "Ethernet Connection (19) I219-V"}, {0x550E, "Ethernet Connection (20) I219-LM"}, {0x550F, "Ethernet Connection (20) I219-V"},
    {0x5510, "Ethernet Connection (21) I219-LM"}, {0x5511, "Ethernet Connection (21) I219-V"}, {0x0DC7, "Ethernet Connection (22) I219-LM"},
    {0x0DC8, "Ethernet Connection (22) I219-V"}, {0x0DC5, "Ethernet Connection (23) I219-LM"}, {0x0DC6, "Ethernet Connection (23) I219-V"},
    {0x57A0, "Ethernet Connection (24) I219-LM"}, {0x57A1, "Ethernet Connection (24) I219-V"},
    {0x1533, "I210 Gigabit Ethernet"}, {0x1539, "I211 Gigabit Ethernet"}, {0x15F3, "Ethernet Controller I225-V"},
    {0x125C, "Ethernet Controller I226-V"}, {0x125B, "Ethernet Controller I226-LM"}, {0x10C9, "82576 Gigabit Ethernet"},
    {0x1237, "440FX Host-Bridge (QEMU)"}, {0x7000, "PIIX3 ISA-Bridge (QEMU)"}, {0x7010, "PIIX3 IDE (QEMU)"},
    {0x7113, "PIIX4 ACPI (QEMU)"}, {0x29C0, "Q35 Host-Bridge (QEMU)"}, {0x2922, "ICH9 AHCI"}, {0x2918, "ICH9 LPC"},
};

static const Name redhat[] = {
    {0x1000, "virtio Netzwerk"}, {0x1001, "virtio Block"}, {0x1041, "virtio Netzwerk"}, {0x1042, "virtio Block"},
    {0x1050, "virtio GPU"}, {0x000D, "QEMU xHCI"}, {0x0010, "QEMU NVMe"}, {0x0001, "QEMU PCI-Bridge"},
};

static const char *find(const Name *t, int n, unsigned id)
{
    for (int i = 0; i < n; i++)
        if (t[i].id == id)
            return t[i].name;
    return 0;
}

#define N(t) ((int)(sizeof(t) / sizeof(t[0])))

static const char *class_name(const PciInfo *p)
{
    unsigned c = p->class_code, s = p->subclass;
    switch (c) {
    case 0x01:
        return s == 0x00 ? "SCSI" : s == 0x01 ? "IDE" : s == 0x04 ? "RAID" : s == 0x06 ? "SATA (AHCI)" :
               s == 0x08 ? "NVMe-SSD" : "Massenspeicher";
    case 0x02: return s == 0x00 ? "Netzwerk (Ethernet)" : s == 0x80 ? "Netzwerk (WLAN o.ae.)" : "Netzwerk";
    case 0x03: return s == 0x00 ? "Grafik (VGA)" : s == 0x02 ? "Grafik (3D)" : "Grafik";
    case 0x04: return s == 0x03 ? "Audio (HD Audio)" : s == 0x01 ? "Audio" : "Multimedia";
    case 0x05: return "Speicher-Controller";
    case 0x06:
        return s == 0x00 ? "Host-Bridge" : s == 0x01 ? "ISA-Bridge (LPC)" : s == 0x04 ? "PCI-Bridge" : "Bridge";
    case 0x07: return s == 0x00 ? "Serielle Schnittstelle" : "Kommunikation";
    case 0x08: return s == 0x05 ? "SD-Kartenleser" : "System";
    case 0x09: return "Eingabegeraet";
    case 0x0B: return "Prozessor";
    case 0x0C:
        if (s == 0x03)
            return p->prog_if == 0x30 ? "USB (xHCI, USB 3)" : p->prog_if == 0x20 ? "USB (EHCI, USB 2)" :
                   p->prog_if == 0x10 ? "USB (OHCI)" : p->prog_if == 0x00 ? "USB (UHCI)" : "USB";
        return s == 0x05 ? "SMBus" : s == 0x80 ? "Serieller Bus" : "Serieller Bus";
    case 0x0D: return s == 0x11 ? "Bluetooth" : "Drahtlos";
    case 0x10: return "Verschluesselung";
    case 0x11: return "Signalverarbeitung";
    case 0x12: return "Beschleuniger";
    case 0x13: return "Instrumentierung";
    default:   return "Sonstiges";
    }
}

void _start(int argc, char **argv)
{
    int verbose = 0, numeric = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            numeric = 1;
        } else {
            fprintf(2, "Aufruf: lspci [-v] [-n]\n");
            sys_exit(1);
        }
    }
    int tty = sys_isatty(1) != 0;
    PciInfo p;
    int n = 0;
    for (u64 i = 0; sys_pciinfo(i, &p) == 0; i++, n++) {
        const char *vname = find(vendors, N(vendors), p.vendor);
        const char *dname = p.vendor == 0x8086 ? find(intel, N(intel), p.device) :
                            (p.vendor == 0x1AF4 || p.vendor == 0x1B36) ? find(redhat, N(redhat), p.device) : 0;
        int net = p.class_code == 0x02;
        printf("%02x:%02x.%x  %s%04x:%04x%s", p.bus, p.dev, p.fn, tty ? C_YELLOW : "", p.vendor, p.device, tty ? C_RESET : "");
        if (!numeric) {
            printf("  %s%-22s%s  %s", tty && net ? C_CYAN : "", class_name(&p), tty && net ? C_RESET : "", vname ? vname : "?");
            if (dname)
                printf(" %s", dname);
        } else {
            printf("  Klasse %02x.%02x.%02x", p.class_code, p.subclass, p.prog_if);
        }
        if (p.driver[0])
            printf("  %s[%s]%s", tty ? C_GREEN : "", p.driver, tty ? C_RESET : "");
        printf("\n");
        if (verbose) {
            printf("          Klasse %02x.%02x.%02x, Revision %02x, Subsystem %04x:%04x\n", p.class_code, p.subclass,
                   p.prog_if, p.revision, p.sub_vendor, p.sub_device);
            for (int b = 0; b < 6; b++) {
                unsigned v = p.bar[b];
                if (!v)
                    continue;
                if (v & 1) {
                    printf("          BAR%d: I/O-Ports ab 0x%x\n", b, v & ~3u);
                } else if (((v >> 1) & 3) == 2 && b < 5) {
                    printf("          BAR%d: Speicher ab 0x%x%08x (64 Bit)\n", b, p.bar[b + 1], v & ~15u);
                    b++;
                } else {
                    printf("          BAR%d: Speicher ab 0x%x\n", b, v & ~15u);
                }
            }
        }
    }
    if (!n)
        printf("keine PCI-Geraete\n");
    else if (!verbose)
        printf("%s%d Geraete. Treiber in [ ]; 'lspci -v' zeigt Details.%s\n", tty ? C_DIM : "", n, tty ? C_RESET : "");
    sys_exit(0);
}
