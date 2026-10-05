#ifndef BOOT_INFO_H
#define BOOT_INFO_H

/* Wird vom Bootloader befuellt und dem Kernel als erstes Argument (RDI) uebergeben.
 * Nur Typen mit fester Groesse, damit Bootloader (MS ABI) und Kernel (SysV) es gleich sehen. */

typedef struct {
    unsigned long long base;            /* Framebuffer-Adresse */
    unsigned long long size;            /* Groesse in Bytes */
    unsigned int       width;
    unsigned int       height;
    unsigned int       pixels_per_line; /* Pitch in Pixeln (32 bpp) */
} BootFramebuffer;

/* Layout identisch zu EFI_MEMORY_DESCRIPTOR */
typedef struct {
    unsigned int       type;            /* 7 = EfiConventionalMemory */
    unsigned int       pad;
    unsigned long long physical_start;
    unsigned long long virtual_start;
    unsigned long long page_count;      /* 4-KiB-Seiten */
    unsigned long long attribute;
} BootMemoryDescriptor;

typedef struct {
    unsigned int width, height;
} BootVideoMode;

#define BOOT_MAX_MODES 48

typedef struct {
    BootFramebuffer    fb;
    void              *memory_map;      /* Array von BootMemoryDescriptor */
    unsigned long long memory_map_size; /* Gesamtgroesse in Bytes */
    unsigned long long descriptor_size; /* Schrittweite pro Eintrag (NICHT sizeof) */
    unsigned int       descriptor_version;
    void              *rsdp;            /* ACPI-RSDP aus der UEFI-Konfigurationstabelle (oder 0) */
    void              *module;          /* geladene Datei \initrd.tar (RAM-Disk mit den User-Programmen) oder 0 */
    unsigned long long module_size;
    char               cmdline[256];    /* Inhalt von \cmdline.txt (optional), sonst leer */
    unsigned int       mode_count;      /* verfuegbare Grafikmodi (32 bpp BGR), nach Aufloesung dedupliziert */
    unsigned int       mode_current;    /* Index des benutzten Modus in modes[] */
    BootVideoMode      modes[BOOT_MAX_MODES];
    unsigned long long kernel_size;     /* Groesse von \kernel.elf: damit findet der Kernel sein Boot-Volume wieder */
    /* Startlogo der Firmware (ACPI-Tabelle BGRT), vom Bootloader kopiert: BMP-Datei, Lage links oben in dem Modus,
     * in dem die Firmware es gezeichnet hat (logo_scr_w x logo_scr_h). logo = 0: keins */
    void              *logo;
    unsigned long long logo_size;
    unsigned int       logo_x, logo_y, logo_scr_w, logo_scr_h;
} BootInfo;

#endif
