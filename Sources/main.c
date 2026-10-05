#include <efi.h>
#include <efilib.h>
#include "boot_info.h"

#define KERNEL_PATH L"\\kernel.elf"
#define PAGE_SIZE   4096ULL

/* Meldungen nur mit "verbose" in cmdline.txt (sonst zeigt der Kernel gleich seine Startanimation); Fehler immer */
static int verbose;
#define LOG(...) do { if (verbose) Print(__VA_ARGS__); } while (0)

/* Minimaler ELF64-Support */
#define PT_LOAD 1

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

static EFI_STATUS read_at(EFI_FILE_HANDLE file, UINT64 offset, UINTN size, VOID *buf)
{
    EFI_STATUS st = uefi_call_wrapper(file->SetPosition, 2, file, offset);
    if (EFI_ERROR(st))
        return st;

    UINTN n = size;
    st = uefi_call_wrapper(file->Read, 3, file, &n, buf);
    if (!EFI_ERROR(st) && n != size)
        st = EFI_LOAD_ERROR;
    return st;
}

static EFI_STATUS load_kernel(EFI_HANDLE image, UINT64 *entry, UINT64 *file_size)
{
    EFI_STATUS st;
    EFI_LOADED_IMAGE *loaded;
    EFI_FILE_HANDLE root, file;

    st = uefi_call_wrapper(BS->HandleProtocol, 3, image, &LoadedImageProtocol, (VOID **)&loaded);
    if (EFI_ERROR(st))
        return st;

    root = LibOpenRoot(loaded->DeviceHandle);
    if (!root)
        return EFI_NOT_FOUND;

    st = uefi_call_wrapper(root->Open, 5, root, &file, KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) {
        Print(L"Kernel %s nicht gefunden: %r\r\n", KERNEL_PATH, st);
        return st;
    }
    LOG(L"Kernel gefunden\r\n");
    EFI_FILE_INFO *kfi = LibFileInfo(file);
    if (kfi) {
        *file_size = kfi->FileSize;
        FreePool(kfi);
    }

    /* ELF-Header pruefen */
    Elf64_Ehdr eh;
    st = read_at(file, 0, sizeof(eh), &eh);
    if (EFI_ERROR(st))
        return st;

    if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F' ||
        eh.e_ident[4] != 2 /* 64 bit */ || eh.e_ident[5] != 1 /* little endian */ ||
        eh.e_machine != 0x3E /* x86-64 */ || eh.e_phentsize != sizeof(Elf64_Phdr)) {
        Print(L"Kein gueltiger x86-64 ELF\r\n");
        return EFI_UNSUPPORTED;
    }
    LOG(L"ELF-Header gueltig, Entry: 0x%lx\r\n", eh.e_entry);

    /* Program Headers lesen */
    UINTN ph_size = (UINTN)eh.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *ph = AllocatePool(ph_size);
    if (!ph)
        return EFI_OUT_OF_RESOURCES;
    st = read_at(file, eh.e_phoff, ph_size, ph);
    if (EFI_ERROR(st))
        return st;

    /* Gesamten physischen Bereich aller PT_LOAD-Segmente bestimmen,
     * damit Segmente, die sich eine Seite teilen, keine Doppel-Allokation ausloesen. */
    UINT64 lo = ~0ULL, hi = 0;
    for (UINT16 i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (ph[i].p_paddr < lo)
            lo = ph[i].p_paddr;
        if (ph[i].p_paddr + ph[i].p_memsz > hi)
            hi = ph[i].p_paddr + ph[i].p_memsz;
    }
    if (lo >= hi)
        return EFI_LOAD_ERROR;

    lo &= ~(PAGE_SIZE - 1);
    UINTN pages = (UINTN)((hi - lo + PAGE_SIZE - 1) / PAGE_SIZE);

    EFI_PHYSICAL_ADDRESS addr = lo;
    st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(st)) {
        Print(L"Kernel-Speicher @ 0x%lx nicht allokierbar: %r\r\n", lo, st);
        return st;
    }
    ZeroMem((VOID *)lo, pages * PAGE_SIZE);

    /* Segmente laden (BSS ist durch ZeroMem bereits genullt) */
    for (UINT16 i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0)
            continue;
        st = read_at(file, ph[i].p_offset, (UINTN)ph[i].p_filesz, (VOID *)ph[i].p_paddr);
        if (EFI_ERROR(st))
            return st;
    }

    uefi_call_wrapper(file->Close, 1, file);
    FreePool(ph);

    LOG(L"Kernel geladen @ 0x%lx (%d Seiten)\r\n", lo, pages);
    *entry = eh.e_entry;
    return EFI_SUCCESS;
}

/* Laedt eine ganze Datei in EfiLoaderData-Speicher (bleibt nach ExitBootServices erhalten). */
static EFI_STATUS load_module(EFI_HANDLE image, CHAR16 *path, VOID **out, UINT64 *out_size)
{
    EFI_LOADED_IMAGE *loaded;
    EFI_FILE_HANDLE root, file;

    EFI_STATUS st = uefi_call_wrapper(BS->HandleProtocol, 3, image, &LoadedImageProtocol, (VOID **)&loaded);
    if (EFI_ERROR(st))
        return st;
    root = LibOpenRoot(loaded->DeviceHandle);
    if (!root)
        return EFI_NOT_FOUND;

    st = uefi_call_wrapper(root->Open, 5, root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st))
        return st;

    EFI_FILE_INFO *fi = LibFileInfo(file);
    if (!fi)
        return EFI_LOAD_ERROR;
    UINT64 size = fi->FileSize;
    FreePool(fi);

    EFI_PHYSICAL_ADDRESS addr;
    UINTN pages = (UINTN)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(st))
        return st;

    st = read_at(file, 0, (UINTN)size, (VOID *)addr);
    uefi_call_wrapper(file->Close, 1, file);
    if (EFI_ERROR(st))
        return st;

    *out = (VOID *)addr;
    *out_size = size;
    return EFI_SUCCESS;
}

/* Grafikmodi einsammeln (nur 32 bpp BGR, wie der Kernel sie zeichnet) und einen waehlen: Kommandozeile "mode=1920x1080"
 * (naechstliegende Aufloesung), "mode=max" (groesste) oder nichts (der aktuelle Modus der Firmware). */
static UINT32 mode_num[BOOT_MAX_MODES];

static const CHAR8 *find_option(const CHAR8 *cmd, const char *key)
{
    UINTN kl = 0;
    while (key[kl])
        kl++;
    for (UINTN i = 0; cmd[i]; i++) {
        if (i && cmd[i - 1] != ' ')
            continue;
        UINTN k = 0;
        while (k < kl && cmd[i + k] == (CHAR8)key[k])
            k++;
        if (k == kl)
            return cmd + i + kl;
    }
    return NULL;
}

static UINT32 parse_uint(const CHAR8 **p)
{
    UINT32 v = 0;
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (UINT32)(*(*p)++ - '0');
    return v;
}

static void select_mode(BootInfo *info)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    if (EFI_ERROR(LibLocateProtocol(&GraphicsOutputProtocol, (VOID **)&gop)))
        return;

    UINT32 n = 0;
    INT32 cur = -1;
    for (UINT32 i = 0; i < gop->Mode->MaxMode && n < BOOT_MAX_MODES; i++) {
        UINTN sz;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, i, &sz, &mi)))
            continue;
        UINT32 w = mi->HorizontalResolution, h = mi->VerticalResolution;
        int ok = mi->PixelFormat == PixelBlueGreenRedReserved8BitPerColor && mi->PixelsPerScanLine >= w;
        FreePool(mi);
        if (!ok)
            continue;
        int dup = -1;
        for (UINT32 j = 0; j < n; j++)
            if (info->modes[j].width == w && info->modes[j].height == h)
                dup = (INT32)j;
        if (dup >= 0) {
            if (i == gop->Mode->Mode)
                mode_num[dup] = i; /* der aktuelle Modus gewinnt bei gleicher Aufloesung */
            if (i == gop->Mode->Mode)
                cur = dup;
            continue;
        }
        info->modes[n].width = w;
        info->modes[n].height = h;
        mode_num[n] = i;
        if (i == gop->Mode->Mode)
            cur = (INT32)n;
        n++;
    }
    info->mode_count = n;
    if (!n)
        return;

    INT32 want = cur;
    const CHAR8 *opt = find_option((const CHAR8 *)info->cmdline, "mode=");
    if (opt && opt[0] == 'm' && opt[1] == 'a' && opt[2] == 'x') {
        want = 0;
        for (UINT32 j = 1; j < n; j++)
            if ((UINT64)info->modes[j].width * info->modes[j].height > (UINT64)info->modes[want].width * info->modes[want].height)
                want = (INT32)j;
    } else if (opt && opt[0] >= '0' && opt[0] <= '9') {
        const CHAR8 *p = opt;
        UINT32 w = parse_uint(&p), h = 0;
        if (*p == 'x')
            p++, h = parse_uint(&p);
        UINT64 target = (UINT64)w * h, best = ~0ULL;
        for (UINT32 j = 0; j < n; j++) { /* naechstliegende Aufloesung (gleiche Flaeche zaehlt am wenigsten Abstand) */
            UINT64 a = (UINT64)info->modes[j].width * info->modes[j].height;
            UINT64 d = a > target ? a - target : target - a;
            if (info->modes[j].width == w && info->modes[j].height == h)
                d = 0;
            if (d < best) {
                best = d;
                want = (INT32)j;
            }
        }
    }
    if (want < 0) { /* der aktuelle Modus ist nicht 32 bpp BGR: den groessten passenden nehmen */
        want = 0;
        for (UINT32 j = 1; j < n; j++)
            if ((UINT64)info->modes[j].width * info->modes[j].height > (UINT64)info->modes[want].width * info->modes[want].height)
                want = (INT32)j;
    }
    if (want != cur) {
        EFI_STATUS st = uefi_call_wrapper(gop->SetMode, 2, gop, mode_num[want]);
        if (EFI_ERROR(st)) {
            Print(L"Grafikmodus %dx%d nicht umschaltbar (%r)\r\n", info->modes[want].width, info->modes[want].height, st);
            want = cur >= 0 ? cur : 0;
        }
    }
    info->mode_current = (UINT32)want;
    LOG(L"Grafikmodus: %dx%d (%d verfuegbar)\r\n", info->modes[want].width, info->modes[want].height, (int)n);
}

/* Startlogo der Firmware (BGRT) in eigenen Speicher kopieren (EfiLoaderData: der Kernel gibt ihn nicht frei), damit
 * der Kernel es an derselben Stelle weiter zeigen kann. Lage gilt fuer den Modus vor select_mode (scr_w x scr_h). */
static void copy_boot_logo(BootInfo *info, const UINT8 *rsdp, UINT32 scr_w, UINT32 scr_h)
{
    if (!rsdp || rsdp[0] != 'R' || rsdp[1] != 'S' || rsdp[2] != 'D')
        return;
    UINT64 sdt = rsdp[15] >= 2 ? *(const UINT64 *)(rsdp + 24) : *(const UINT32 *)(rsdp + 16);
    UINTN esz = rsdp[15] >= 2 ? 8 : 4;
    const UINT8 *h = (const UINT8 *)(UINTN)sdt;
    if (!h)
        return;
    UINT32 len = *(const UINT32 *)(h + 4);
    for (UINTN off = 36; off + esz <= len; off += esz) {
        UINT64 a = esz == 8 ? *(const UINT64 *)(h + off) : *(const UINT32 *)(h + off);
        const UINT8 *t = (const UINT8 *)(UINTN)a;
        if (!t || t[0] != 'B' || t[1] != 'G' || t[2] != 'R' || t[3] != 'T' || *(const UINT32 *)(t + 4) < 56)
            continue;
        const UINT8 *bmp = (const UINT8 *)(UINTN)*(const UINT64 *)(t + 40);
        if (t[39] != 0 || !bmp || bmp[0] != 'B' || bmp[1] != 'M') /* Bildtyp 0 = BMP */
            return;
        UINT32 size = *(const UINT32 *)(bmp + 2);
        if (size < 54 || size > (16u << 20))
            return;
        EFI_PHYSICAL_ADDRESS dst;
        if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData,
                                        (UINTN)((size + PAGE_SIZE - 1) / PAGE_SIZE), &dst)))
            return;
        CopyMem((VOID *)(UINTN)dst, (VOID *)bmp, size);
        info->logo = (VOID *)(UINTN)dst;
        info->logo_size = size;
        info->logo_x = *(const UINT32 *)(t + 48);
        info->logo_y = *(const UINT32 *)(t + 52);
        info->logo_scr_w = scr_w;
        info->logo_scr_h = scr_h;
        LOG(L"Startlogo der Firmware: %d Bytes bei %d,%d\r\n", (int)size, (int)info->logo_x, (int)info->logo_y);
        return;
    }
}

/* Ohne "verbose": Bildschirm schwarz und das Startlogo wieder an seine Stelle (Text der Firmware weg, nach einem
 * Moduswechsel an derselben relativen Stelle - wie spaeter der Kernel in Kernel/console/splash.c) */
static void draw_boot_logo(const BootInfo *info)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    if (EFI_ERROR(LibLocateProtocol(&GraphicsOutputProtocol, (VOID **)&gop)))
        return;
    UINT32 w = gop->Mode->Info->HorizontalResolution, h = gop->Mode->Info->VerticalResolution;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = {0, 0, 0, 0};
    uefi_call_wrapper(gop->Blt, 10, gop, &black, EfiBltVideoFill, 0, 0, 0, 0, (UINTN)w, (UINTN)h, 0);
    const UINT8 *b = info->logo;
    if (!b)
        return;
    INT32 bw = *(const INT32 *)(b + 18), bh = *(const INT32 *)(b + 22);
    UINT32 bpp = (UINT32)(b[28] | b[29] << 8) / 8, off = *(const UINT32 *)(b + 10);
    UINT32 ah = (UINT32)(bh < 0 ? -bh : bh), rb = ((UINT32)bw * bpp + 3) & ~3u;
    if (bw <= 0 || bw > 4096 || ah == 0 || ah > 4096 || (bpp != 3 && bpp != 4) ||
        (UINT64)off + (UINT64)rb * ah > info->logo_size)
        return;
    UINT32 k = w >= 2 * info->logo_scr_w && h >= 2 * info->logo_scr_h ? 2 : 1, ow = (UINT32)bw * k, oh = ah * k;
    INT64 x = info->logo_x, y = info->logo_y;
    if (w != info->logo_scr_w || h != info->logo_scr_h) {
        x = (INT64)(((UINT64)info->logo_x * 2 + (UINT64)bw) * w / (2 * (UINT64)info->logo_scr_w)) - (INT64)ow / 2;
        y = (INT64)(((UINT64)info->logo_y * 2 + ah) * h / (2 * (UINT64)info->logo_scr_h)) - (INT64)oh / 2;
    }
    if (x < 0 || y < 0 || x + ow > w || y + oh > h)
        return;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *px = AllocatePool((UINTN)ow * oh * sizeof(*px));
    if (!px)
        return;
    for (UINT32 yy = 0; yy < oh; yy++) {
        UINT32 sy = yy / k, row = bh < 0 ? sy : ah - 1 - sy;
        const UINT8 *src = b + off + (UINT64)row * rb;
        for (UINT32 xx = 0; xx < ow; xx++) {
            const UINT8 *p = src + (xx / k) * bpp;
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL *d = &px[(UINT64)yy * ow + xx];
            d->Blue = p[0];
            d->Green = p[1];
            d->Red = p[2];
            d->Reserved = 0;
        }
    }
    uefi_call_wrapper(gop->Blt, 10, gop, px, EfiBltBufferToVideo, 0, 0, (UINTN)x, (UINTN)y, (UINTN)ow, (UINTN)oh, 0);
    FreePool(px);
}

static EFI_STATUS get_framebuffer(BootFramebuffer *fb)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_STATUS st = LibLocateProtocol(&GraphicsOutputProtocol, (VOID **)&gop);
    if (EFI_ERROR(st))
        return st;

    fb->base            = gop->Mode->FrameBufferBase;
    fb->size            = gop->Mode->FrameBufferSize;
    fb->width           = gop->Mode->Info->HorizontalResolution;
    fb->height          = gop->Mode->Info->VerticalResolution;
    fb->pixels_per_line = gop->Mode->Info->PixelsPerScanLine;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

    static BootInfo info; /* static: bleibt nach ExitBootServices gueltig */

    /* Optionale Kommandozeile fuer den Kernel (siehe Kernel/core/cmdline.h), zuerst: "verbose" schaltet die Meldungen
     * hier ein, "mode=" wird gleich hier ausgewertet */
    VOID *cmd = NULL;
    UINT64 cmd_size = 0;
    if (!EFI_ERROR(load_module(ImageHandle, L"\\cmdline.txt", &cmd, &cmd_size)) && cmd) {
        UINT64 n = cmd_size < sizeof(info.cmdline) - 1 ? cmd_size : sizeof(info.cmdline) - 1;
        for (UINT64 i = 0; i < n; i++) {
            CHAR8 ch = ((CHAR8 *)cmd)[i];
            info.cmdline[i] = ch < ' ' ? ' ' : ch; /* Zeilenumbrueche usw. werden zu Leerzeichen */
        }
        info.cmdline[n] = 0;
        const CHAR8 *v = find_option((const CHAR8 *)info.cmdline, "verbose");
        verbose = v && (*v == 0 || *v == ' ');
    }
    if (verbose) { /* sonst bleibt das Bild der Firmware (ihr Logo) stehen, bis der Kernel es uebernimmt */
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
        Print(L"Bootloader gestartet\r\nKommandozeile geladen\r\n");
    }

    UINT64 entry;
    UINT64 kernel_size = 0;
    EFI_STATUS st = load_kernel(ImageHandle, &entry, &kernel_size);
    info.kernel_size = kernel_size;
    if (EFI_ERROR(st)) {
        Print(L"Kernel laden fehlgeschlagen: %r\r\n", st);
        goto halt;
    }

    /* ACPI-RSDP: bevorzugt 2.0 (XSDT), sonst 1.0 */
    EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
    VOID *rsdp = NULL;
    if (EFI_ERROR(LibGetSystemConfigurationTable(&acpi20, &rsdp)))
        LibGetSystemConfigurationTable(&AcpiTableGuid, &rsdp);
    info.rsdp = rsdp;
    BootFramebuffer before;
    if (!EFI_ERROR(get_framebuffer(&before)))
        copy_boot_logo(&info, (const UINT8 *)rsdp, before.width, before.height);

    select_mode(&info);
    if (!verbose)
        draw_boot_logo(&info);
    st = get_framebuffer(&info.fb);
    if (EFI_ERROR(st)) {
        Print(L"Kein GOP-Framebuffer: %r\r\n", st);
        goto halt;
    }
    LOG(L"Framebuffer: 0x%lx, %dx%d\r\n", info.fb.base, info.fb.width, info.fb.height);

    /* Optionales User-Programm; ohne die Datei startet der Kernel trotzdem */
    VOID *module = NULL;
    UINT64 module_size = 0;
    st = load_module(ImageHandle, L"\\initrd.tar", &module, &module_size);
    if (EFI_ERROR(st))
        Print(L"initrd.tar nicht geladen (%r)\r\n", st);
    else
        LOG(L"initrd.tar geladen: %ld Bytes @ 0x%lx\r\n", module_size, (UINT64)(UINTN)module);
    info.module = module;
    info.module_size = module_size;

    LOG(L"ACPI RSDP: 0x%lx\r\n", (UINT64)(UINTN)rsdp);
    LOG(L"Springe zum Kernel...\r\n");

    /* Memory Map holen und Boot Services beenden. Zwischen GetMemoryMap und
     * ExitBootServices darf nichts mehr allokiert/ausgegeben werden, sonst wird der MapKey ungueltig. */
    UINTN map_key, desc_size, entries;
    UINT32 desc_ver;
    EFI_MEMORY_DESCRIPTOR *map = LibMemoryMap(&entries, &map_key, &desc_size, &desc_ver);
    st = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(st)) {
        /* Map hat sich geaendert -> einmal neu holen */
        map = LibMemoryMap(&entries, &map_key, &desc_size, &desc_ver);
        st = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
        if (EFI_ERROR(st))
            goto halt; /* Print geht hier nicht mehr sicher */
    }

    info.memory_map         = map;
    info.memory_map_size    = entries * desc_size;
    info.descriptor_size    = desc_size;
    info.descriptor_version = desc_ver;

    /* Kernel ist SysV-ABI: BootInfo* landet in RDI */
    ((void (*)(BootInfo *))entry)(&info);

halt:
    for (;;)
        __asm__ __volatile__("hlt");
    return EFI_SUCCESS;
}
