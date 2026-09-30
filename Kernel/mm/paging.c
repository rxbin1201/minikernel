#include "mm/paging.h"
#include "arch/x86_64/spinlock.h"
#include "mm/pmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include <stdint.h>

#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_PWT     (1ULL << 3)
#define PTE_PCD     (1ULL << 4)
#define PTE_HUGE    (1ULL << 7) /* im PD-Eintrag: 2-MiB-Seite */
#define PTE_PAT_2M  (1ULL << 12) /* PAT-Bit bei 2-MiB-Seiten (bei 4-KiB-Seiten ist es Bit 7) */
#define PTE_PAT_4K  (1ULL << 7)
#define MSR_PAT     0x277
#define PTE_NX      (1ULL << 63)

#define ADDR_MASK_4K 0x000FFFFFFFFFF000ULL
#define ADDR_MASK_2M 0x000FFFFFFFE00000ULL
/* Flag-Bits, die wir beim Aufteilen/Aendern von Eintraegen erhalten */
#define FLAG_MASK    (PTE_WRITE | PTE_USER | PTE_PWT | PTE_PCD | PTE_NX)

#define SIZE_4K     4096ULL
#define SIZE_2M     (2ULL * 1024 * 1024)
#define MAX_PD      32          /* statischer Pool fuer die Anfangs-Mappings: je 1 GiB */

#define MSR_EFER    0xC0000080
#define EFER_NXE    (1ULL << 11)
#define CR0_WP      (1ULL << 16)

static uint64_t pml4[512]            __attribute__((aligned(4096)));
static uint64_t pdpt[512]            __attribute__((aligned(4096)));
static uint64_t pd_pool[MAX_PD][512] __attribute__((aligned(4096)));
static unsigned pd_used;
static int      nx_supported;
static int      pat_wc;       /* PAT-Eintrag 4 ist Write-Combining */

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];

/* ---------- CPU-Helfer ---------- */

static inline void invlpg(uint64_t virt)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline void flush_tlb(void)
{
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}

/* Programmiert PAT-Eintrag 4 auf Write-Combining (Seiten mit gesetztem PAT-Bit, ohne PCD/PWT). Der Framebuffer wird so
 * mit gebuendelten Schreibzugriffen beschrieben statt jeden Pixel einzeln uncached (viel schneller, v.a. unter VMware). */
static void setup_pat(void)
{
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & (1u << 16)))
        return;
    uint64_t pat = rdmsr(MSR_PAT);
    pat = (pat & ~(0xFFULL << 32)) | (0x01ULL << 32); /* PA4 = WC (0x01) */
    wrmsr(MSR_PAT, pat);
    pat_wc = 1;
}

/* Weitere CPUs: gleiche PAT wie die Boot-CPU (EFER.NXE, CR0.WP und CR3 setzt schon der Startcode) */
void paging_ap_init(void)
{
    if (pat_wc)
        wrmsr(MSR_PAT, (rdmsr(MSR_PAT) & ~(0xFFULL << 32)) | (0x01ULL << 32));
}

static void enable_nx(void)
{
    uint32_t eax = 0x80000000, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (eax < 0x80000001)
        return;
    eax = 0x80000001;
    __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & (1u << 20)))
        return;
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    nx_supported = 1;
}

/* ---------- Stufe 1: statisches Identity-Mapping ---------- */

static uint64_t *get_static_pd(unsigned pdpt_index)
{
    if (pdpt[pdpt_index] & PTE_PRESENT)
        return (uint64_t *)(pdpt[pdpt_index] & ADDR_MASK_4K);
    if (pd_used == MAX_PD)
        return 0;
    uint64_t *pd = pd_pool[pd_used++];
    pdpt[pdpt_index] = (uint64_t)pd | PTE_PRESENT | PTE_WRITE;
    return pd;
}

/* Mappt [start, end) 1:1. Bereits vorhandene Eintraege bleiben unveraendert. */
static void map_range_2m(uint64_t start, uint64_t end, uint64_t extra_flags)
{
    for (uint64_t a = start & ~(SIZE_2M - 1); a < end; a += SIZE_2M) {
        unsigned pdpt_index = a >> 30;
        if (pdpt_index >= 512) {
            kprintf("paging: Adresse ausserhalb 512 GiB ignoriert: %#lx\n", a);
            return;
        }
        uint64_t *pd = get_static_pd(pdpt_index);
        if (!pd) {
            kprintf("paging: PD-Pool voll, Bereich nicht gemappt: %#lx\n", a);
            return;
        }
        unsigned pd_index = (a >> 21) & 511;
        if (!(pd[pd_index] & PTE_PRESENT))
            pd[pd_index] = a | PTE_PRESENT | PTE_WRITE | PTE_HUGE | extra_flags;
    }
}

void paging_init(const BootInfo *info)
{
    enable_nx();
    setup_pat();

    pml4[0] = (uint64_t)pdpt | PTE_PRESENT | PTE_WRITE;

    /* Framebuffer zuerst und uncached (MMIO), damit die generischen Mappings ihn nicht ueberschreiben */
    map_range_2m(info->fb.base, info->fb.base + info->fb.size, pat_wc ? PTE_PAT_2M : (PTE_PCD | PTE_PWT));

    /* Alles, was UEFI kennt (Kernel, Stack, Runtime Services, ACPI, MMIO ...) */
    const uint8_t *p = info->memory_map;
    for (uint64_t off = 0; off + sizeof(BootMemoryDescriptor) <= info->memory_map_size;
         off += info->descriptor_size) {
        const BootMemoryDescriptor *d = (const BootMemoryDescriptor *)(p + off);
        map_range_2m(d->physical_start, d->physical_start + d->page_count * SIZE_4K, 0);
    }

    kprintf("paging: PML4 @ %p, %u GiB-Bereiche gemappt, NX %s\n", pml4, pd_used,
            nx_supported ? "an" : "nicht verfuegbar");

    as_switch((AddressSpace *)pml4);
}

/* ---------- Stufe 2: 4-KiB-Verwaltung ---------- */

static uint64_t table_frames; /* Frames, die gerade als Page Table dienen (fuer paging_table_frames) */

/* Aenderungen im Kernel-Bereich (in allen Adressraeumen gemeinsam) laufen unter paging_lock, damit z.B. der Heap auch
 * ohne Big Kernel Lock Seiten einblenden kann. Den User-Bereich eines Adressraums aendert nur sein Prozess (unter
 * dem BKL). */
static Spinlock paging_lock = SPINLOCK_INIT("paging");

static int is_kernel_va(uint64_t virt)
{
    return (virt >> 39) != (USER_BASE >> 39);
}

/* Frischer, genullter Frame fuer eine Page Table (PMM-Frames sind identity-mapped). */
static uint64_t *table_alloc(void)
{
    uint64_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    memset((void *)frame, 0, SIZE_4K);
    __atomic_add_fetch(&table_frames, 1, __ATOMIC_RELAXED);
    return (uint64_t *)frame;
}

static void table_free(uint64_t *t)
{
    pmm_free_frame((uint64_t)t);
    __atomic_sub_fetch(&table_frames, 1, __ATOMIC_RELAXED);
}

uint64_t paging_table_frames(void)
{
    return table_frames;
}

/* Zaehlt Aenderungen, nach denen andere CPUs ihren TLB leeren muessen (siehe bkl_acquire) */
static volatile uint64_t kernel_gen;

uint64_t paging_kernel_gen(void)
{
    return kernel_gen;
}

/* Folgt einem Eintrag zur naechsten Tabelle, legt sie bei Bedarf an. */
static uint64_t *next_table(uint64_t *entry, int create, uint64_t user)
{
    if (*entry & PTE_PRESENT) {
        if (*entry & PTE_HUGE)
            return 0; /* 1-GiB-Seiten kommen bei uns nicht vor */
        if (user)
            *entry |= PTE_USER;
        return (uint64_t *)(*entry & ADDR_MASK_4K);
    }
    if (!create)
        return 0;
    uint64_t *t = table_alloc();
    if (!t)
        return 0;
    *entry = (uint64_t)t | PTE_PRESENT | PTE_WRITE | user;
    return t;
}

/* Teilt eine 2-MiB-Seite in 512 gleichartige 4-KiB-Seiten. */
static uint64_t *split_huge(uint64_t *pde)
{
    uint64_t *pt = table_alloc();
    if (!pt)
        return 0;
    uint64_t base  = *pde & ADDR_MASK_2M;
    uint64_t flags = *pde & FLAG_MASK;
    if (*pde & PTE_PAT_2M)
        flags |= PTE_PAT_4K;
    for (unsigned i = 0; i < 512; i++)
        pt[i] = (base + i * SIZE_4K) | PTE_PRESENT | flags;
    /* Auf Zwischenebenen bleibt NX aus (sonst wirkt es auf alle Unterseiten), W bleibt an. */
    *pde = (uint64_t)pt | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    flush_tlb();
    return pt;
}

/* Liefert den PTE-Zeiger fuer virt. Teilt 2-MiB-Seiten auf; legt fehlende Tabellen nur mit create an. */
static uint64_t *walk(uint64_t *root, uint64_t virt, int create, uint64_t user)
{
    uint64_t *t3 = next_table(&root[(virt >> 39) & 511], create, user);
    if (!t3)
        return 0;
    uint64_t *t2 = next_table(&t3[(virt >> 30) & 511], create, user);
    if (!t2)
        return 0;
    uint64_t *pde = &t2[(virt >> 21) & 511];
    uint64_t *pt;
    if ((*pde & PTE_PRESENT) && (*pde & PTE_HUGE))
        pt = split_huge(pde);
    else
        pt = next_table(pde, create, user);
    if (!pt)
        return 0;
    return &pt[(virt >> 12) & 511];
}

static uint64_t sanitize(uint64_t flags)
{
    flags &= FLAG_MASK;
    if (!nx_supported)
        flags &= ~PTE_NX;
    return flags;
}

/* ---------- Adressraeume ---------- */

AddressSpace *as_kernel(void)  { return (AddressSpace *)pml4; }

/* Direkt aus CR3: jede CPU hat ihren eigenen geladenen Adressraum */
AddressSpace *as_current(void)
{
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return (AddressSpace *)(cr3 & ADDR_MASK_4K);
}

void as_switch(AddressSpace *as)
{
    __asm__ __volatile__("mov %0, %%cr3" : : "r"((uint64_t)as) : "memory");
}

/* Neuer Adressraum: teilt sich alle Kernel-Eintraege (Top-Level-Slots), der User-Bereich ist leer. */
AddressSpace *as_create(void)
{
    uint64_t *root = table_alloc();
    if (!root)
        return 0;
    memcpy(root, pml4, SIZE_4K);
    root[USER_BASE >> 39] = 0;
    return (AddressSpace *)root;
}

/* Gibt alle User-Seiten samt Frames sowie die Page Tables des User-Bereichs und die PML4 frei. */
void as_destroy(AddressSpace *as)
{
    uint64_t *root = (uint64_t *)as;
    if (!root || root == pml4)
        return;

    uint64_t e4 = root[USER_BASE >> 39];
    if (e4 & PTE_PRESENT) {
        uint64_t *t3 = (uint64_t *)(e4 & ADDR_MASK_4K);
        for (unsigned i = 0; i < 512; i++) {
            if (!(t3[i] & PTE_PRESENT))
                continue;
            uint64_t *t2 = (uint64_t *)(t3[i] & ADDR_MASK_4K);
            for (unsigned j = 0; j < 512; j++) {
                if (!(t2[j] & PTE_PRESENT))
                    continue;
                uint64_t *pt = (uint64_t *)(t2[j] & ADDR_MASK_4K);
                for (unsigned k = 0; k < 512; k++)
                    if (pt[k] & PTE_PRESENT)
                        pmm_free_frame(pt[k] & ADDR_MASK_4K);
                table_free(pt);
            }
            table_free(t2);
        }
        table_free(t3);
    }
    table_free(root);
}

/* Kopiert alle User-Seiten (fuer fork): gleiche Adressen und Rechte, aber eigene Frames mit kopiertem Inhalt. */
AddressSpace *as_clone(AddressSpace *src)
{
    AddressSpace *dst = as_create();
    if (!dst)
        return 0;
    uint64_t e4 = ((uint64_t *)src)[USER_BASE >> 39];
    if (!(e4 & PTE_PRESENT))
        return dst;

    uint64_t *t3 = (uint64_t *)(e4 & ADDR_MASK_4K);
    for (unsigned i = 0; i < 512; i++) {
        if (!(t3[i] & PTE_PRESENT))
            continue;
        uint64_t *t2 = (uint64_t *)(t3[i] & ADDR_MASK_4K);
        for (unsigned j = 0; j < 512; j++) {
            if (!(t2[j] & PTE_PRESENT) || (t2[j] & PTE_HUGE))
                continue;
            uint64_t *pt = (uint64_t *)(t2[j] & ADDR_MASK_4K);
            for (unsigned k = 0; k < 512; k++) {
                if (!(pt[k] & PTE_PRESENT))
                    continue;
                uint64_t va = (USER_BASE & ~((1ULL << 39) - 1)) | ((uint64_t)i << 30) | ((uint64_t)j << 21) | ((uint64_t)k << 12);
                uint64_t frame = pmm_alloc_frame();
                if (!frame || (memcpy((void *)frame, (void *)(pt[k] & ADDR_MASK_4K), SIZE_4K),
                               as_map(dst, va, frame, pt[k] & FLAG_MASK) != 0)) {
                    if (frame)
                        pmm_free_frame(frame);
                    as_destroy(dst);
                    return 0;
                }
            }
        }
    }
    return dst;
}

/* Legt den Top-Level-Eintrag fuer einen Kernel-Bereich sofort an. Muss vor dem ersten as_create passieren,
 * weil spaeter neu entstehende Kernel-Slots nicht in bestehende Adressraeume kopiert werden. */
void paging_reserve_kernel_slot(uint64_t virt)
{
    next_table(&pml4[(virt >> 39) & 511], 1, 0);
}

static int map_locked(AddressSpace *as, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pte = walk((uint64_t *)as, virt, 1, flags & PAGE_USER);
    if (!pte || (*pte & PTE_PRESENT))
        return -1;
    *pte = (phys & ADDR_MASK_4K) | PTE_PRESENT | sanitize(flags);
    invlpg(virt);
    return 0;
}

static int unmap_locked(AddressSpace *as, uint64_t virt)
{
    uint64_t *pte = walk((uint64_t *)as, virt, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;
    *pte = 0;
    invlpg(virt);
    if (is_kernel_va(virt))
        __atomic_add_fetch(&kernel_gen, 1, __ATOMIC_RELEASE); /* Kernel-Bereich: gilt fuer alle CPUs */
    return 0;
}

static int set_flags_locked(AddressSpace *as, uint64_t virt, uint64_t flags)
{
    uint64_t *pte = walk((uint64_t *)as, virt, 0, flags & PAGE_USER);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;
    *pte = (*pte & ADDR_MASK_4K) | PTE_PRESENT | sanitize(flags);
    invlpg(virt);
    if (is_kernel_va(virt))
        __atomic_add_fetch(&kernel_gen, 1, __ATOMIC_RELEASE);
    return 0;
}

int as_map(AddressSpace *as, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!is_kernel_va(virt))
        return map_locked(as, virt, phys, flags);
    uint64_t f = spin_lock(&paging_lock);
    int r = map_locked(as, virt, phys, flags);
    spin_unlock(&paging_lock, f);
    return r;
}

int as_unmap(AddressSpace *as, uint64_t virt)
{
    if (!is_kernel_va(virt))
        return unmap_locked(as, virt);
    uint64_t f = spin_lock(&paging_lock);
    int r = unmap_locked(as, virt);
    spin_unlock(&paging_lock, f);
    return r;
}

int as_set_flags(AddressSpace *as, uint64_t virt, uint64_t flags)
{
    if (!is_kernel_va(virt))
        return set_flags_locked(as, virt, flags);
    uint64_t f = spin_lock(&paging_lock);
    int r = set_flags_locked(as, virt, flags);
    spin_unlock(&paging_lock, f);
    return r;
}

int paging_map(uint64_t virt, uint64_t phys, uint64_t flags)   { return as_map(as_kernel(), virt, phys, flags); }
int paging_unmap(uint64_t virt)                                { return as_unmap(as_kernel(), virt); }
int paging_set_flags(uint64_t virt, uint64_t flags)            { return as_set_flags(as_kernel(), virt, flags); }
int paging_translate(uint64_t virt, uint64_t *phys, uint64_t *flags)
{
    return as_translate(as_kernel(), virt, phys, flags);
}

int as_translate(AddressSpace *as, uint64_t virt, uint64_t *phys, uint64_t *flags)
{
    /* Nur lesen, nichts aufteilen oder anlegen */
    uint64_t e = ((uint64_t *)as)[(virt >> 39) & 511];
    if (!(e & PTE_PRESENT))
        return 0;
    uint64_t *t3 = (uint64_t *)(e & ADDR_MASK_4K);
    e = t3[(virt >> 30) & 511];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;
    uint64_t *t2 = (uint64_t *)(e & ADDR_MASK_4K);
    e = t2[(virt >> 21) & 511];
    if (!(e & PTE_PRESENT))
        return 0;

    uint64_t base;
    if (e & PTE_HUGE) {
        base = (e & ADDR_MASK_2M) + (virt & (SIZE_2M - 1) & ~(SIZE_4K - 1));
    } else {
        uint64_t *pt = (uint64_t *)(e & ADDR_MASK_4K);
        e = pt[(virt >> 12) & 511];
        if (!(e & PTE_PRESENT))
            return 0;
        base = e & ADDR_MASK_4K;
    }
    if (phys)
        *phys = base | (virt & (SIZE_4K - 1));
    if (flags)
        *flags = e & FLAG_MASK;
    return 1;
}

int paging_map_mmio(uint64_t phys, uint64_t size)
{
    uint64_t first = phys & ~(SIZE_4K - 1);
    uint64_t end   = (phys + size + SIZE_4K - 1) & ~(SIZE_4K - 1);

    /* Der User-Slot (127,5 - 128 TiB) gehoert den Prozessen; MMIO dort waere in deren Adressraeumen nicht sichtbar. */
    if (first < USER_END && end > USER_BASE) {
        kprintf("paging: MMIO @ %#lx liegt im User-Bereich, nicht mappbar\n", phys);
        return -1;
    }
    /* Adressen jenseits von 512 GiB brauchen einen neuen Top-Level-Slot; er muss vor dem ersten Prozess entstehen,
     * sonst sehen bestehende Adressraeume ihn nicht (Treiber werden deshalb vor dem Start von Prozessen initialisiert). */
    if (end > (512ULL << 30))
        paging_reserve_kernel_slot(first);

    for (uint64_t a = first; a < end; a += SIZE_4K) {
        uint64_t flags = PAGE_WRITE | PAGE_NX | PAGE_NOCACHE;
        if (paging_translate(a, 0, 0)) {
            /* schon (z.B. per 2-MiB-Seite aus der UEFI-Memory-Map) gemappt: Seite aufteilen, uncached machen */
            if (paging_set_flags(a, flags) != 0)
                return -1;
        } else if (paging_map(a, a, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

static void set_range_flags(const char *start, const char *end, uint64_t flags)
{
    for (uint64_t a = (uint64_t)start; a < (uint64_t)end; a += SIZE_4K)
        paging_set_flags(a, flags);
}

void paging_harden(void)
{
    /* 1. Kernel-Sektionen einzeln absichern. Das teilt die 2-MiB-Seite des Kernels auf;
     *    die Seiten bleiben dabei erst mal so wie vorher (RWX), text laeuft also weiter. */
    set_range_flags(__text_start,   __text_end,   0);
    set_range_flags(__rodata_start, __rodata_end, PAGE_NX);
    set_range_flags(__data_start,   __data_end,   PAGE_WRITE | PAGE_NX);

    /* 2. Alles andere nicht ausfuehrbar machen (nur ausserhalb von text) */
    if (nx_supported) {
        for (unsigned i = 0; i < 512; i++) {
            if (!(pdpt[i] & PTE_PRESENT))
                continue;
            uint64_t *pd = (uint64_t *)(pdpt[i] & ADDR_MASK_4K);
            for (unsigned j = 0; j < 512; j++) {
                if (!(pd[j] & PTE_PRESENT))
                    continue;
                if (pd[j] & PTE_HUGE) {
                    pd[j] |= PTE_NX;
                    continue;
                }
                uint64_t *pt = (uint64_t *)(pd[j] & ADDR_MASK_4K);
                for (unsigned k = 0; k < 512; k++) {
                    uint64_t va = ((uint64_t)i << 30) | ((uint64_t)j << 21) | ((uint64_t)k << 12);
                    if ((pt[k] & PTE_PRESENT) && !(va >= (uint64_t)__text_start && va < (uint64_t)__text_end))
                        pt[k] |= PTE_NX;
                }
            }
        }
    }

    /* 3. Seite 0 ausblenden: Null-Pointer-Zugriffe loesen jetzt einen Page Fault aus */
    paging_unmap(0);

    /* 4. Auch der Kernel darf read-only Seiten nicht mehr beschreiben */
    uint64_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0 | CR0_WP) : "memory");

    flush_tlb();
    kprintf("paging: gehaertet (text=RX, rodata=R, data=RW+NX, Seite 0 unmapped, WP an)\n");
}
