#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "boot_info.h"

/* Flags fuer paging_map / paging_set_flags (Present ist implizit). */
#define PAGE_WRITE   (1ULL << 1)
#define PAGE_USER    (1ULL << 2)
#define PAGE_NOCACHE ((1ULL << 3) | (1ULL << 4)) /* PWT | PCD */
#define PAGE_NX      (1ULL << 63)                /* wird ignoriert, wenn die CPU kein NX kann */
#define PAGE_SHARED  (1ULL << 9)  /* frei verwendbares PTE-Bit: Frame gehoert einem Shared-Memory-Objekt (shm.c);
                                   * as_destroy gibt ihn nicht frei, as_clone (fork) uebernimmt die Seite nicht */
#define PAGE_COW     (1ULL << 10) /* frei verwendbares PTE-Bit: Copy-on-Write nach fork. Die Seite ist schreibgeschuetzt
                                   * und teilt sich ihren Frame vielleicht mit anderen Adressraeumen (pmm_ref); beim
                                   * ersten Schreiben bekommt sie eine eigene Kopie (as_cow_resolve) */

/* User-Bereich: ein eigener Top-Level-Slot (PML4-Index 255, 127,5 - 128 TiB), pro Prozess privat. Er liegt weit oberhalb
 * aller realen physischen Adressen (auch hoher PCI-BARs, die die Firmware bei 64-Bit-Fenstern gern weit oben vergibt).
 * Alles darunter (Slot 0: Identity-Mapping mit dem Kernel) und die Kernel-Slots fuer Heap und Stacks
 * sind in jedem Adressraum identisch, aber ohne PAGE_USER fuer Ring 3 unzugaenglich. */
#define USER_BASE 0x7F8000000000ULL   /* PML4-Slot 255: ganz oben im unteren Adressraum (wie unter Linux) */
#define USER_END  0x800000000000ULL

typedef struct AddressSpace AddressSpace; /* in Wahrheit: Zeiger auf die PML4 */

AddressSpace *as_kernel(void);
AddressSpace *as_current(void);           /* aktuell in CR3 */
AddressSpace *as_create(void);            /* teilt den Kernel-Teil, User-Bereich leer; NULL bei Speichermangel */
void          as_destroy(AddressSpace *as); /* gibt User-Seiten (samt Frames) und Tabellen frei */
void          as_switch(AddressSpace *as);  /* CR3 laden */
AddressSpace *as_clone(AddressSpace *src);  /* Kopie aller User-Seiten (fork); NULL bei Speichermangel */
/* fork mit Copy-on-Write: das Kind bekommt dieselben Frames, beschreibbare Seiten werden in beiden Adressraeumen
 * schreibgeschuetzt und PAGE_COW. Nur, wenn kein anderer Thread src gerade benutzt (leert den TLB dieser CPU). */
AddressSpace *as_clone_cow(AddressSpace *src);
/* Schreibzugriff auf eine PAGE_COW-Seite: eigene Kopie (oder, wenn sonst niemand den Frame hat, einfach wieder
 * beschreibbar). 1 = erledigt, 0 = keine Copy-on-Write-Seite, -1 = kein Speicher. Nur fuer den eigenen Adressraum. */
int           as_cow_resolve(AddressSpace *as, uint64_t virt);
int           as_cow_break_all(AddressSpace *as); /* alle PAGE_COW-Seiten aufloesen (vor dem zweiten Thread); 0 / -1 */

int as_map(AddressSpace *as, uint64_t virt, uint64_t phys, uint64_t flags);
int as_unmap(AddressSpace *as, uint64_t virt);
int as_set_flags(AddressSpace *as, uint64_t virt, uint64_t flags);
int as_translate(AddressSpace *as, uint64_t virt, uint64_t *phys, uint64_t *flags);

/* Legt den Top-Level-Eintrag fuer einen Kernel-Bereich an (vor dem ersten as_create aufrufen). */
void paging_reserve_kernel_slot(uint64_t virt);

/* Stufe 1: Baut die Anfangs-Page-Tables (Identity-Mapping, 2-MiB-Seiten) und aktiviert sie.
 * Braucht noch keinen PMM. */
void paging_init(const BootInfo *info);

/* Stufe 2 (nach pmm_init): Ab hier sind 4-KiB-Operationen moeglich. Setzt Kernel-Sektionen auf
 * passende Rechte (text = RX, rodata = R, data/bss = RW+NX), macht den restlichen Speicher NX,
 * blendet Seite 0 aus und aktiviert CR0.WP. */
void paging_harden(void);

/* Mappt eine 4-KiB-Seite. 0 = ok, -1 = schon gemappt oder kein Speicher fuer Page Tables.
 * 2-MiB-Seiten werden bei Bedarf automatisch aufgeteilt. */
int paging_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* Entfernt das Mapping. 0 = ok, -1 = war nicht gemappt. Der Frame wird NICHT freigegeben. */
int paging_unmap(uint64_t virt);

/* Aendert die Flags einer gemappten Seite. 0 = ok, -1 = nicht gemappt. */
int paging_set_flags(uint64_t virt, uint64_t flags);

/* Blendet MMIO-Bereiche (APIC, IOAPIC ...) 1:1 ein: beschreibbar, uncached, NX. 0 = ok. */
int paging_map_mmio(uint64_t phys, uint64_t size);

/* 1 = gemappt (phys/flags werden gefuellt, beide duerfen NULL sein), 0 = nicht gemappt. */
int paging_translate(uint64_t virt, uint64_t *phys, uint64_t *flags);

/* Frames, die gerade als Page Table dienen (Kernel-Tabellen bleiben, die von Adressraeumen gibt as_destroy frei) */
uint64_t paging_table_frames(void);

/* Auf einer weiteren CPU: PAT wie auf der Boot-CPU einstellen */
void paging_ap_init(void);

/* Zaehler, der bei jedem Ausblenden/Umstellen einer Kernel-Seite steigt: andere CPUs leeren dann ihren TLB */
uint64_t paging_kernel_gen(void);

#endif
