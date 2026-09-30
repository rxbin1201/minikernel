#include "boot_info.h"
#include "serial.h"
#include "console.h"
#include "kprintf.h"
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "cpu.h"
#include "acpi.h"
#include "apic.h"
#include "ioapic.h"
#include "keyboard.h"
#include "sched.h"
#include "kstack.h"
#include "process.h"
#include "syscall.h"
#include "vfs.h"
#include "pci.h"
#include "blk.h"
#include "fat32.h"
#include "fs.h"
#include "cmdline.h"
#include "power.h"
#include "usb.h"
#include "net.h"
#include "mouse.h"
#include "keymap.h"
#include "tty.h"
#include "rtc.h"
#include "utf8.h"
#include "video.h"

#define COLOR_DEFAULT 0x00C0C0C0
#define COLOR_OK      0x0000FF00
#define COLOR_FAIL    0x00FF3030
#define COLOR_TITLE   0x0060C0FF

static void check(const char *name, int ok)
{
    kprintf("  %s: ", name);
    console_set_color(ok ? COLOR_OK : COLOR_FAIL, 0);
    kprintf("%s\n", ok ? "OK" : "FEHLER");
    console_set_color(COLOR_DEFAULT, 0);
}

static void title(const char *name)
{
    console_set_color(COLOR_TITLE, 0);
    kprintf("[%s]\n", name);
    console_set_color(COLOR_DEFAULT, 0);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Prueft ksnprintf gegen den erwarteten String */
#define CHECK_FMT(expected, ...)                                   \
    do {                                                           \
        char buf_[128];                                            \
        ksnprintf(buf_, sizeof(buf_), __VA_ARGS__);                \
        if (!streq(buf_, expected)) {                              \
            fmt_ok = 0;                                            \
            kprintf("  erwartet \"%s\", bekommen \"%s\"\n", expected, buf_); \
        }                                                          \
    } while (0)

static void test_kprintf(void)
{
    title("kprintf");
    int fmt_ok = 1;

    CHECK_FMT("42|   42|42   |00042", "%d|%5d|%-5d|%05d", 42, 42, 42, 42);
    CHECK_FMT("-2147483648", "%d", -2147483647 - 1);
    CHECK_FMT("+7  8", "%+d % d", 7, 8);
    CHECK_FMT("ff FF 0xff 0xdeadbeef", "%x %X %#x %#lx", 255, 255, 255, 0xDEADBEEFUL);
    CHECK_FMT("18446744073709551615", "%llu", 18446744073709551615ULL);
    CHECK_FMT("0x0000000000001000", "%#018lx", 0x1000UL);
    CHECK_FMT("0x0 0x1234", "%p %p", (void *)0, (void *)0x1234);
    CHECK_FMT("abc|       abc|abc       |ab", "%s|%10s|%-10s|%.2s", "abc", "abc", "abc", "abc");
    const char *volatile nullp = 0;
    CHECK_FMT("(null)", "%s", nullp);
    CHECK_FMT("x-  y|100%", "%c-%3c|100%%", 'x', 'y');
    CHECK_FMT("   12|12   ", "%*d|%-*d", 5, 12, 5, 12);
    CHECK_FMT("007", "%.3d", 7);
    CHECK_FMT("255 -1", "%hhu %hhd", 255, -1);
    check("Formatierung", fmt_ok);

    char small[5];
    int n = ksnprintf(small, sizeof(small), "%s", "abcdefgh");
    check("ksnprintf kuerzt und terminiert", n == 8 && streq(small, "abcd"));

    kprintf("  Ausgabe: %s, %d, %#x, %c\n", "Hallo", -42, 255, '!');
}

/* Nicht leere Zelle (col, row)? */
static int cell_has_ink(uint32_t col, uint32_t row)
{
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 8; x++)
            if (console_read_pixel(col * 8 + x, row * 16 + y) != 0)
                return 1;
    return 0;
}

/* Kommt die Farbe (0x00RRGGBB) in der Zelle vor? */
static int cell_has_color(uint32_t col, uint32_t row, uint32_t color)
{
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 8; x++)
            if (console_read_pixel(col * 8 + x, row * 16 + y) == color)
                return 1;
    return 0;
}

/* Liest den Framebuffer zurueck: zeichnet ein Glyph als ASCII-Art auf Serial und prueft das Scrolling. */
static void test_console(void)
{
    title("Konsole");
    uint32_t col, row;

    console_clear(); /* oben anfangen: sonst kann die Ausgabe unten scrollen und das Glyph verschieben (haengt von der Zahl der Boot-Zeilen ab) */
    title("Konsole");
    console_get_cursor(&col, &row);
    kprintf("H");
    kprintf("\n  Zelle (%u,%u) als ASCII-Art (nur Serial):\n", col, row);
    for (uint32_t y = 0; y < 16; y++) {
        serial_puts("    ");
        for (uint32_t x = 0; x < 8; x++)
            serial_putc(console_read_pixel(col * 8 + x, row * 16 + y) ? '#' : '.');
        serial_puts("\n");
    }
    int glyph_ok = cell_has_ink(col, row);

    /* Scrolling: bis zum unteren Rand und darueber hinaus schreiben. Ergebnisse erst nach dem
     * Auslesen ausgeben, weil die Ausgabe selbst wieder scrollt. */
    console_clear();
    for (uint32_t i = 0; i < console_rows() + 3; i++)
        console_putc('\n');
    console_putc('S');
    uint32_t scol, srow; /* gescrollt wird in Viertel-Bildschirm-Schritten: der Cursor steht nicht zwingend unten */
    console_get_cursor(&scol, &srow);
    int last_ok  = srow > 0 && cell_has_ink(0, srow);
    int above_ok = srow > 0 && !cell_has_ink(0, srow - 1);

    /* ANSI-Farben: X hellrot, Y auf blauem Grund, Z wieder in der Standardfarbe; die Escape-Folgen belegen keine Zellen */
    console_clear();
    for (const char *c = "\x1b[1;31mX\x1b[0m\x1b[44mY\x1b[0mZ"; *c; c++)
        console_putc(*c);
    uint32_t ecol, erow;
    console_get_cursor(&ecol, &erow);
    int esc_cols = ecol == 3;
    int red_ok = cell_has_color(0, 0, 0xF14C4C);
    int blue_ok = cell_has_color(1, 0, 0x2472C8);
    int default_ok = cell_has_color(2, 0, 0xC0C0C0) && !cell_has_color(2, 0, 0xF14C4C);

    console_clear();
    title("Konsole");
    check("Glyph gezeichnet", glyph_ok);
    check("ANSI: Escape-Folgen werden nicht ausgegeben", esc_cols);
    check("ANSI: ESC[1;31m = hellrot", red_ok);
    check("ANSI: ESC[44m = blauer Hintergrund", blue_ok);
    check("ANSI: ESC[0m stellt die Standardfarbe wieder her", default_ok);
    check("Scroll: letzte Zeile beschrieben", last_ok);
    check("Scroll: Zeile darueber leer", above_ok);
}

/* Misst, wie schnell die Konsole Text ausgibt und scrollt (nur die Konsole, ohne Serial) */
static void test_console_speed(void)
{
    title("Konsolen-Geschwindigkeit");
    console_clear();
    const int lines = 300;
    uint64_t t0 = time_ms();
    for (int i = 0; i < lines; i++) {
        static const char text[] = "Zeile zum Testen der Scroll-Geschwindigkeit: 0123456789 abcdefghijklmnopqrstuvwxyz\n";
        for (const char *c = text; *c; c++)
            console_putc(*c);
    }
    uint64_t ms = time_ms() - t0;
    console_clear();
    title("Konsolen-Geschwindigkeit");
    kprintf("  %d Zeilen in %lu ms (%lu Zeilen/s)\n", lines, ms, ms ? lines * 1000 / ms : 0);
    check("Konsole schnell genug (300 Zeilen unter 1 s)", ms < 1000);
}

/* Dezimalzahl am Anfang eines Textes */
static uint64_t atoi_u(const char *s)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

static void test_usb(void)
{
    title("USB");
    UsbInfo info;
    unsigned n = 0;
    static const char *drivers[] = {"kein Treiber", "Tastatur", "Massenspeicher", "Hub", "Maus"};
    while (usb_device_info(n, &info) == 0) {
        kprintf("  Port %s: %s, Speed %u, %s\n", info.path, info.name, info.speed, info.driver < 5 ? drivers[info.driver] : "?");
        n++;
    }
    kprintf("  %u USB-Geraet(e) erkannt\n", n); /* ohne angeschlossene Geraete/Controller ist 0 in Ordnung */
    for (unsigned i = 0; i < n; i++) {
        usb_device_info(i, &info);
        check(info.name, info.driver != 0 || info.cls == 9); /* jedes Geraet hat einen Treiber (Hubs sind bekannt unbenutzt) */
    }
}

static void test_paging(void)
{
    title("Paging");
    extern char __text_start[], __rodata_start[], __data_start[];
    uint64_t phys, fl;

    check("Seite 0 nicht gemappt", !paging_translate(0, 0, 0));
    check("text = RX", paging_translate((uint64_t)__text_start, &phys, &fl) && !(fl & (PAGE_WRITE | PAGE_NX)));
    check("rodata = R+NX", paging_translate((uint64_t)__rodata_start, &phys, &fl) && !(fl & PAGE_WRITE) && (fl & PAGE_NX));
    check("data = RW+NX", paging_translate((uint64_t)__data_start, &phys, &fl) && (fl & PAGE_WRITE) && (fl & PAGE_NX));

    const uint64_t vtest = 0x20000000000ULL; /* 2 TiB, nicht der Heap-Bereich */
    uint64_t frame = pmm_alloc_frame();
    int r = paging_map(vtest, frame, PAGE_WRITE | PAGE_NX);
    *(volatile uint64_t *)vtest = 0xCAFEBABE;
    check("map 2 TiB -> Frame", r == 0 && *(volatile uint64_t *)frame == 0xCAFEBABE &&
                                paging_translate(vtest, &phys, 0) && phys == frame);
    check("Doppeltes map abgelehnt", paging_map(vtest, frame, 0) == -1);
    check("unmap", paging_unmap(vtest) == 0 && !paging_translate(vtest, 0, 0));
    pmm_free_frame(frame);

#ifdef TEST_NULL_FAULT
    kprintf("Schreibe auf Adresse 0...\n");
    *(volatile uint64_t *)0 = 1;
#endif
#ifdef TEST_RODATA_FAULT
    kprintf("Schreibe in rodata...\n");
    *(volatile uint64_t *)__rodata_start = 1;
#endif
}

static void test_pmm(void)
{
    title("PMM");
    uint64_t free_before = pmm_free_frame_count();

    uint64_t a = pmm_alloc_frame();
    uint64_t b = pmm_alloc_frame();
    kprintf("  alloc: %#lx, %#lx\n", a, b);
    *(volatile uint64_t *)a = 0xDEADBEEF;
    check("Frame schreibbar", *(volatile uint64_t *)a == 0xDEADBEEF);

    uint64_t run = pmm_alloc_frames(16);
    kprintf("  alloc 16 zusammenhaengend: %#lx\n", run);

    pmm_free_frame(a);
    pmm_free_frame(b);
    pmm_free_frames(run, 16);
    pmm_free_frame(a); /* absichtlicher Double Free -> Warnung */

    check("Bilanz", pmm_free_frame_count() == free_before);
    check("Frame wird wiederverwendet", pmm_alloc_frame() == a);
    pmm_free_frame(a);
}

static void test_heap(void)
{
    title("Heap");
    uint64_t used0 = heap_used_bytes();

    uint8_t *p1 = kmalloc(100), *p2 = kmalloc(200), *p3 = kmalloc(300);
    check("16-Byte ausgerichtet", p1 && p2 && p3 && (((uint64_t)p1 | (uint64_t)p2 | (uint64_t)p3) & 15) == 0);
    memset(p1, 0xAA, 100);
    memset(p2, 0xBB, 200);
    memset(p3, 0xCC, 300);
    check("Bloecke ueberlappen nicht", p1[99] == 0xAA && p2[0] == 0xBB && p2[199] == 0xBB && p3[0] == 0xCC);

    kfree(p2);
    kfree(p1); /* verschmilzt mit p2 */
    uint8_t *p4 = kmalloc(300);
    check("Coalescing (Block wiederverwendet)", p4 == p1);

    size_t big_size = 1 << 20;
    uint8_t *big = kmalloc(big_size);
    check("1 MiB (Heap waechst)", big != 0 && heap_total_bytes() >= big_size);
    if (big) {
        memset(big, 1, big_size);
        check("1 MiB beschreibbar", big[0] == 1 && big[big_size - 1] == 1);
    }

    uint8_t *z = kcalloc(64, 8);
    int zero = z != 0;
    for (int i = 0; z && i < 512; i++)
        zero &= z[i] == 0;
    check("kcalloc nullt", zero);

    uint8_t *re = kmalloc(10);
    memset(re, 0x5A, 10);
    re = krealloc(re, 5000);
    check("krealloc behaelt Daten", re != 0 && re[0] == 0x5A && re[9] == 0x5A);

    kfree(p4); /* p4 == p1 */
    kfree(p3);
    kfree(big);
    kfree(z);
    kfree(re);
    kfree(p4); /* absichtlicher Double Free -> Warnung */

    check("Bilanz", heap_used_bytes() == used0);
    check("konsistent", heap_check());
    kprintf("  Heap: %lu KiB gemappt\n", heap_total_bytes() / 1024);
}

static void test_interrupts(void)
{
    title("Interrupts");
    check("ACPI/MADT gelesen", acpi_info()->valid);
    check("IOAPIC gefunden", acpi_info()->ioapic_count > 0);

    uint64_t t0 = apic_ticks();
    apic_sleep_ms(200);
    uint64_t dt = apic_ticks() - t0;
    kprintf("  200 ms warten -> %lu Ticks\n", dt);
    check("Timer-Ticks (100 Hz)", dt >= 18 && dt <= 24);

    /* Tastendruecke ueber den 8042 einspeisen: 'a', dann Shift+'a' (Scancode-Set 1) */
    static const unsigned char keys[] = {0x1E, 0x9E, 0x2A, 0x1E, 0x9E, 0xAA};
    for (unsigned i = 0; i < sizeof(keys); i++) {
        keyboard_inject_scancode(keys[i]);
        apic_sleep_ms(20);
    }
    int c1 = keyboard_getchar();
    int c2 = keyboard_getchar();
    check("Tastatur: 'a'", c1 == 'a');
    check("Tastatur: Shift+a = 'A'", c2 == 'A');
    check("Tastaturpuffer leer", keyboard_getchar() == -1);
}

static Mutex          counter_lock = MUTEX_INIT;
static volatile int   shared_counter;
static volatile int   workers_done;
static volatile int   flag_set, spinner_done;

static void worker(void *arg)
{
    int n = (int)(uint64_t)arg;
    for (int i = 0; i < 200; i++) {
        mutex_lock(&counter_lock);
        int v = shared_counter;
        if (i % 50 == 0)
            thread_yield(); /* mit gehaltenem Mutex abgeben: die anderen muessen blockieren */
        shared_counter = v + 1;
        mutex_unlock(&counter_lock);
    }
    kprintf("  worker %d fertig (Thread %u)\n", n, thread_id(thread_current()));
    mutex_lock(&counter_lock);
    workers_done++;
    mutex_unlock(&counter_lock);
}

/* Dreht sich, bis ein anderer Thread das Flag setzt: klappt nur mit Praeemption */
static void spinner(void *arg)
{
    (void)arg;
    while (!flag_set)
        ;
    spinner_done = 1;
}

static void setter(void *arg)
{
    (void)arg;
    thread_sleep_ms(30);
    flag_set = 1;
}

static void quick(void *arg)
{
    (void)arg;
}

static int wait_for(volatile int *v, int target, int timeout_ms)
{
    for (int waited = 0; *v != target && waited < timeout_ms; waited += 10)
        thread_sleep_ms(10);
    return *v == target;
}

static void test_sched(void)
{
    title("Scheduler");

    /* 1. Mutex: 3 Worker zaehlen zusammen auf 600 */
    for (int i = 1; i <= 3; i++) {
        char name[16];
        ksnprintf(name, sizeof(name), "worker%d", i);
        thread_create(name, worker, (void *)(uint64_t)i);
    }
    check("Worker beendet", wait_for(&workers_done, 3, 2000));
    check("Mutex: Zaehler = 600", shared_counter == 600);

    /* 2. Praeemption: Spinner haengt, bis Setter (schlaeft 30 ms) das Flag setzt */
    thread_create("spinner", spinner, 0);
    thread_create("setter", setter, 0);
    check("Praeemption (Busy-Wait wird unterbrochen)", wait_for(&spinner_done, 1, 1000));

    /* 3. Schlafen */
    uint64_t t0 = apic_ticks();
    thread_sleep_ms(100);
    uint64_t dt = apic_ticks() - t0;
    kprintf("  100 ms schlafen -> %lu Ticks\n", dt);
    check("thread_sleep_ms", dt >= 10 && dt <= 12);

    /* 4. Viele kurzlebige Threads: Stacks muessen wieder freigegeben werden */
    uint64_t heap_before = heap_used_bytes();
    for (int i = 0; i < 20; i++)
        thread_create("quick", quick, 0);
    thread_sleep_ms(100); /* Idle-Thread raeumt auf */
    check("Threads beendet und aufgeraeumt", heap_used_bytes() == heap_before && heap_check());

    kprintf("  Kontextwechsel bisher: %lu\n", sched_switch_count());
    sched_dump();
}

#ifdef TEST_STACK_OVERFLOW
static void recurse(int n)
{
    volatile char pad[256];
    pad[0] = (char)n;
    recurse(n + 1);
    pad[1] = pad[0];
}

static void overflow_thread(void *arg)
{
    (void)arg;
    recurse(0);
}
#endif


/* Startet ein Programm aus der initrd und wartet auf das Ende. 0 = ok. */
static int run_user(const char *path, const char *cmdline, int *code, int *faulted)
{
    int pid = process_spawn(path, cmdline, 0);
    if (pid < 0)
        return -1;
    return process_wait(pid, 0, code, faulted, 5000);
}

/* Legt Text direkt in den Tastaturpuffer, als waere er getippt (sofort; fuer die Shell-Tests) */
static void feed(const char *s)
{
    for (; *s; s++)
        keyboard_deliver((unsigned char)*s);
}

/* Tippt einen Text ueber den 8042 ein (Make + Break je Zeichen, Shift wenn noetig). */
static void type_string(const char *s)
{
    for (; *s; s++) {
        int shift;
        int sc = keyboard_scancode_for(*s, &shift);
        if (sc < 0)
            continue;
        if (shift)
            keyboard_inject_scancode(0x2A);
        keyboard_inject_scancode((unsigned char)sc);
        keyboard_inject_scancode((unsigned char)(sc | 0x80));
        if (shift)
            keyboard_inject_scancode(0xAA);
        thread_sleep_ms(30);
    }
}

static void test_vfs(BootInfo *info)
{
    title("Dateisystem (initrd)");
    if (!info->module) {
        check("initrd.tar vom Bootloader geladen", 0);
        return;
    }
    int n = vfs_count();
    kprintf("  initrd: %lu Bytes, %d Eintraege\n", (unsigned long)info->module_size, n);
    check("initrd eingelesen", n > 0);

    const VfsNode *sh = vfs_lookup("/bin/sh");
    check("/bin/sh gefunden (ELF-Magic)", sh && !sh->is_dir && sh->size > 64 && sh->data[0] == 0x7F && sh->data[1] == 'E');
    check("Pfad-Normalisierung ('bin//./hello/')", vfs_lookup("bin//./hello/") == vfs_lookup("/bin/hello") && vfs_lookup("/bin/hello"));
    check("Nicht vorhandene Datei", vfs_lookup("/gibt/es/nicht") == 0);
    const VfsNode *motd = vfs_lookup("/etc/motd");
    check("/etc/motd lesbar", motd && motd->size > 10 && motd->data[0] == 'W');

    unsigned count = 0;
    kprintf("  /bin:");
    for (const VfsNode *e; (e = vfs_readdir("/bin", count)); count++)
        kprintf(" %s", vfs_basename(e->path));
    kprintf("\n");
    check("readdir(/bin) listet die Programme", count >= 5);
    check("readdir(/) enthaelt bin, etc und README.txt",
          vfs_readdir("/", 2) != 0 && vfs_readdir("/", 3) == 0);
}

/* ---------- Datentraeger ---------- */

static uint8_t pat(uint32_t i, uint8_t seed)
{
    return (uint8_t)(i * 31 + seed);
}

/* Schreibt len Bytes Muster (Index start+i, Startwert seed) in Bloecken zu `chunk` Bytes (chunk <= 512). */
static int write_pattern(const char *path, int flags, uint32_t start, uint32_t len, uint32_t chunk, uint8_t seed)
{
    FsFile f;
    if (fs_open(path, flags, &f) != 0)
        return -1;
    uint8_t buf[512];
    for (uint32_t off = 0; off < len;) {
        uint32_t n = len - off < chunk ? len - off : chunk;
        for (uint32_t i = 0; i < n; i++)
            buf[i] = pat(start + off + i, seed);
        if (fs_write(&f, buf, n) != (int64_t)n) {
            fs_close(&f);
            return -1;
        }
        off += n;
    }
    fs_close(&f);
    return 0;
}

/* Prueft Inhalt und Laenge: Byte i ist pat(i, seed), fuer i < first_n stattdessen pat(i, first_seed). */
static int verify_pattern(const char *path, uint32_t len, uint8_t seed, uint32_t first_n, uint8_t first_seed)
{
    FsFile f;
    if (fs_open(path, FAT_O_RDONLY, &f) != 0)
        return 0;
    uint8_t buf[777]; /* krumme Blockgroesse, damit Sektor- und Clustergrenzen ungleichmaessig getroffen werden */
    uint32_t off = 0;
    int64_t n;
    int ok = 1;
    while ((n = fs_read(&f, buf, sizeof(buf))) > 0) {
        for (int64_t i = 0; i < n; i++)
            if (buf[i] != pat(off + (uint32_t)i, off + (uint32_t)i < first_n ? first_seed : seed))
                ok = 0;
        off += (uint32_t)n;
    }
    fs_close(&f);
    return ok && n == 0 && off == len;
}

static int ieq(const char *a, const char *b)
{
    for (;;) { /* wie das Dateisystem: Schreibweise egal, auch bei Umlauten */
        uint32_t x = utf8_next(&a), y = utf8_next(&b);
        if (uni_upper(x) != uni_upper(y))
            return 0;
        if (!x)
            return 1;
    }
}

static int dir_has(const char *dir, const char *name, uint64_t *size)
{
    FsDirEnt e;
    for (unsigned i = 0; fs_readdir(dir, i, &e) == 0; i++) {
        if (ieq(e.name, name)) {
            if (size)
                *size = e.size;
            return 1;
        }
    }
    return 0;
}

static void test_disk(void)
{
    title("Datentraeger (virtio-blk + FAT32)");
    check("Blockgeraet gefunden (virtio-blk, AHCI oder NVMe)", blk_count() > 0);

    /* Nur ein Volume mit dem Label MINIKERNEL wird benutzt: der Kernel schreibt nie auf fremde Partitionen */
    int dv = fs_disk_volume();
    if (dv < 0) {
        check("FAT32-Volume 'MINIKERNEL' gemountet", 0);
        return;
    }
    FatVolumeInfo vinfo;
    fat_volume_info(dv, &vinfo);
    kprintf("  Volume auf %s: Label '%s', %u Cluster a %u Bytes\n", vinfo.device, vinfo.label, fat_total_clusters(dv),
            fat_cluster_bytes(dv));
    check("FAT32-Volume 'MINIKERNEL' gemountet", 1);

    /* Rohzugriff auf das Geraet, auf dem das Volume liegt */
    BlkDev *dev = 0;
    for (int i = 0; i < blk_count(); i++)
        if (strcmp(blk_get(i)->name, vinfo.device) == 0)
            dev = blk_get(i);
    static uint8_t big[200 * 512], rbuf[1024];
    /* 200 Sektoren am Stueck: mehr als der Bounce-Puffer (64 Sektoren) -> mehrere Anfragen */
    check("Grosser Lesevorgang (200 Sektoren)", dev && blk_read(dev, 0, 200, big) == 0);
    check("Lesen hinter dem Ende wird abgelehnt", dev && blk_read(dev, dev->sectors, 1, rbuf) != 0);

    /* Nur auf dem emulierten virtio-Geraet wird roh geschrieben (letzte zwei Sektoren, danach wiederhergestellt).
     * Auf AHCI/NVMe (evtl. echte Hardware) wuerde das eine GPT-Sicherungstabelle am Plattenende treffen. */
    if (dev && dev->name[0] == 'v') {
        static uint8_t saved[1024], wbuf[1024];
        uint64_t last = dev->sectors - 2;
        int io_ok = blk_read(dev, last, 2, saved) == 0;
        for (int i = 0; i < 1024; i++)
            wbuf[i] = pat(i, 0x5A);
        io_ok = io_ok && blk_write(dev, last, 2, wbuf) == 0 && blk_read(dev, last, 2, rbuf) == 0 &&
                memcmp(wbuf, rbuf, 1024) == 0;
        io_ok = io_ok && blk_write(dev, last, 2, saved) == 0;
        check("Rohe Sektoren schreiben und lesen (nur virtio)", io_ok);
    }

    /* Von einem anderen Programm (tools/mkdisk.py) geschriebene Dateien */
    FsFile f;
    int seed_ok = fs_open("/disk/seed.txt", FAT_O_RDONLY, &f) == 0; /* Kleinschreibung: Namen sind case-insensitive */
    uint8_t buf[700];
    uint32_t off = 0;
    int64_t n;
    while (seed_ok && (n = fs_read(&f, buf, sizeof(buf))) > 0) {
        for (int64_t i = 0; i < n; i++)
            if (buf[i] != (uint8_t)((off + i) * 7 + 3))
                seed_ok = 0;
        off += (uint32_t)n;
    }
    if (seed_ok)
        fs_close(&f);
    check("SEED.TXT lesen (5000 Bytes ueber mehrere Cluster)", seed_ok && off == 5000);

    int note_ok = fs_open("/disk/DOCS/note.txt", FAT_O_RDONLY, &f) == 0;
    if (note_ok) {
        int64_t nn = fs_read(&f, buf, sizeof(buf));
        note_ok = nn > 10 && memcmp(buf, "Diese Datei", 11) == 0;
        fs_close(&f);
    }
    check("DOCS/NOTE.TXT im Unterverzeichnis lesen", note_ok);
    check("readdir(/disk) zeigt SEED.TXT und DOCS", dir_has("/disk", "SEED.TXT", 0) && dir_has("/disk", "DOCS", 0));

    /* Schreiben */
    uint32_t free0 = fat_free_clusters(dv);
    uint64_t size = 0;

    check("Datei anlegen und 10000 Bytes schreiben (Bloecke zu 333)",
          write_pattern("/disk/T1.BIN", FAT_O_WRONLY | FAT_O_CREAT, 0, 10000, 333, 1) == 0);
    check("... zurueckgelesen und Groesse stimmt", verify_pattern("/disk/T1.BIN", 10000, 1, 0, 0) &&
                                                    dir_has("/disk", "T1.BIN", &size) && size == 10000);
    check("Anhaengen (O_APPEND) von 100 Bytes",
          write_pattern("/disk/T1.BIN", FAT_O_WRONLY | FAT_O_APPEND, 10000, 100, 100, 1) == 0 &&
          verify_pattern("/disk/T1.BIN", 10100, 1, 0, 0));
    check("Anfang ueberschreiben (10 Bytes), Rest bleibt",
          write_pattern("/disk/T1.BIN", FAT_O_WRONLY, 0, 10, 10, 2) == 0 && verify_pattern("/disk/T1.BIN", 10100, 1, 10, 2));
    check("Abschneiden (O_TRUNC) gibt die Cluster frei", write_pattern("/disk/T1.BIN", FAT_O_WRONLY | FAT_O_TRUNC, 0, 0, 1, 0) == 0 &&
                                                          dir_has("/disk", "T1.BIN", &size) && size == 0 &&
                                                          fat_free_clusters(dv) == free0);
    check("Datei loeschen", fs_unlink("/disk/T1.BIN") == 0 && !dir_has("/disk", "T1.BIN", 0) && fat_free_clusters(dv) == free0);

    /* Verzeichnisse */
    check("mkdir", fs_mkdir("/disk/SUB") == 0 && dir_has("/disk", "SUB", 0));
    check("Datei im Unterverzeichnis", write_pattern("/disk/sub/a.txt", FAT_O_WRONLY | FAT_O_CREAT, 0, 300, 100, 9) == 0 &&
                                        verify_pattern("/disk/SUB/A.TXT", 300, 9, 0, 0) && dir_has("/disk/SUB", "A.TXT", 0));
    check("rmdir auf nicht leeres Verzeichnis abgelehnt", fs_unlink("/disk/SUB") == ERR_NOTEMPTY);
    check("Aufraeumen (Datei, dann Verzeichnis)", fs_unlink("/disk/SUB/A.TXT") == 0 && fs_unlink("/disk/SUB") == 0 &&
                                                   !dir_has("/disk", "SUB", 0));

    /* Fehlerfaelle */
    FsFile tmp;
    check("Langer Name (kein 8.3) wird mit LFN angelegt", write_pattern("/disk/Langer Dateiname.txt", FAT_O_WRONLY | FAT_O_CREAT, 0, 100, 100, 4) == 0 &&
                                                          dir_has("/disk", "Langer Dateiname.txt", 0) &&
                                                          verify_pattern("/disk/LANGER~1.TXT", 100, 4, 0, 0) &&
                                                          fs_unlink("/disk/langer dateiname.TXT") == 0 && !dir_has("/disk", "Langer Dateiname.txt", 0));
    {
        uint64_t sz = 0;
        FsStat st;
        const char *uname = "/disk/Pr\xC3\xBC" "fung \xC3\xA4\xC3\xB6\xC3\xBC \xC3\x9F.txt";
        const char *upper = "/disk/PR\xC3\x9C" "FUNG \xC3\x84\xC3\x96\xC3\x9C \xC3\x9F.TXT";
        int ok = write_pattern(uname, FAT_O_WRONLY | FAT_O_CREAT, 0, 64, 64, 7) == 0 &&
                 dir_has("/disk", "Pr\xC3\xBC" "fung \xC3\xA4\xC3\xB6\xC3\xBC \xC3\x9F.txt", &sz) && sz == 64 &&
                 verify_pattern(upper, 64, 7, 0, 0);
        check("Umlaute im Dateinamen (UTF-8 <-> UTF-16), Suche ohne Beachtung der Schreibweise", ok);
        uint64_t now = rtc_now();
        check("Zeitstempel: neue Datei hat die aktuelle RTC-Zeit (+-4 s)",
              !rtc_valid() || (fs_stat(uname, &st) == 0 && st.mtime + 4 >= now && st.mtime <= now + 4));
        check("Umlaut-Datei loeschen", fs_unlink(uname) == 0 && !dir_has("/disk", "Pr\xC3\xBC" "fung \xC3\xA4\xC3\xB6\xC3\xBC \xC3\x9F.txt", 0));
    }
    check("Ungueltiger Name (Sonderzeichen) abgelehnt", fs_open("/disk/a*b.txt", FAT_O_WRONLY | FAT_O_CREAT, &tmp) == ERR_INVAL);
    check("Nicht vorhandene Datei", fs_open("/disk/nichtda.txt", FAT_O_RDONLY, &tmp) == ERR_NOENT);
    check("Verzeichnis als Datei oeffnen", fs_open("/disk/DOCS", FAT_O_RDONLY, &tmp) == ERR_ISDIR);
    check("mkdir auf existierenden Namen", fs_mkdir("/disk/DOCS") == ERR_EXIST);
    check("initrd ist nur lesbar", fs_open("/etc/motd", FAT_O_WRONLY, &tmp) == ERR_ROFS && fs_mkdir("/neu") == ERR_ROFS);

    /* Verzeichnis waechst ueber mehrere Cluster (16 Eintraege pro Cluster): 40 Dateien anlegen und wieder loeschen */
    uint32_t free_mid = 0;
    int many_ok = 1;
    for (int pass = 0; pass < 2; pass++) {
        char name[32];
        for (int i = 0; i < 40; i++) {
            ksnprintf(name, sizeof(name), "/disk/F%02d.TXT", i);
            many_ok &= write_pattern(name, FAT_O_WRONLY | FAT_O_CREAT, 0, 50 + i, 50, (uint8_t)i) == 0;
        }
        for (int i = 0; i < 40; i++) {
            ksnprintf(name, sizeof(name), "/disk/F%02d.TXT", i);
            many_ok &= verify_pattern(name, 50 + i, (uint8_t)i, 0, 0);
        }
        for (int i = 0; i < 40; i++) {
            ksnprintf(name, sizeof(name), "/disk/F%02d.TXT", i);
            many_ok &= fs_unlink(name) == 0;
        }
        if (pass == 0)
            free_mid = fat_free_clusters(dv);
    }
    check("40 Dateien anlegen/pruefen/loeschen (Verzeichnis waechst)", many_ok && !dir_has("/disk", "F00.TXT", 0));
    check("Kein Cluster-Leck beim zweiten Durchlauf", fat_free_clusters(dv) == free_mid);

    /* Persistenz: Zaehler in einer Datei, der bei jedem Start hochgezaehlt wird */
    int count = 0;
    if (fs_open("/disk/BOOTCNT.TXT", FAT_O_RDONLY, &f) == 0) {
        char digits[16] = {0};
        int64_t nn = fs_read(&f, digits, sizeof(digits) - 1);
        for (int64_t i = 0; i < nn && digits[i] >= '0' && digits[i] <= '9'; i++)
            count = count * 10 + (digits[i] - '0');
        fs_close(&f);
    }
    count++;
    char line[16];
    int len = ksnprintf(line, sizeof(line), "%d\n", count);
    int wr = fs_open("/disk/BOOTCNT.TXT", FAT_O_WRONLY | FAT_O_CREAT | FAT_O_TRUNC, &f) == 0 &&
             fs_write(&f, line, len) == len;
    fs_close(&f);
    kprintf("  Bootzaehler auf der Platte: %d (bleibt ueber Neustarts erhalten)\n", count);
    check("Bootzaehler geschrieben", wr);
}

/* Liest eine Datei ganz (hoechstens max-1 Bytes) und haengt ein 0 an. Liefert die Laenge oder -1. */
static int slurp(const char *path, char *buf, int max)
{
    FsFile f;
    if (fs_open(path, FAT_O_RDONLY, &f) != 0)
        return -1;
    int n = 0;
    int64_t r;
    while (n < max - 1 && (r = fs_read(&f, buf + n, (uint64_t)(max - 1 - n))) > 0)
        n += (int)r;
    fs_close(&f);
    buf[n] = 0;
    return n;
}

static int wait_for_file(const char *dir, const char *name, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (dir_has(dir, name, 0))
            return 1;
        thread_sleep_ms(50);
    }
    return dir_has(dir, name, 0);
}

static int strstr_(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (memcmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

static int count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (*s == '\n')
            n++;
    return n;
}

static void test_user(BootInfo *info)
{
    title("User-Mode");
    if (!info->module) {
        check("initrd.tar vom Bootloader geladen", 0);
        return;
    }
    int code = 0, faulted = 0;

#ifdef TEST_STACK_OVERFLOW
    kprintf("  Kernel-Stack-Ueberlauf provozieren...\n");
    thread_create("overflow", overflow_thread, 0);
    thread_sleep_ms(500);
#endif

    check("hello: Exit-Code 42", run_user("/bin/hello", "hello eins zwei", &code, &faulted) == 0 && code == 42 && !faulted);
    thread_sleep_ms(50); /* Idle-Thread raeumt Adressraum und Stack auf */
    uint64_t frames_before = pmm_free_frame_count();

    check("crash null -> Prozess beendet", run_user("/bin/crash", "crash null", &code, &faulted) == 0 && faulted);
    check("crash kread -> Prozess beendet", run_user("/bin/crash", "crash kread", &code, &faulted) == 0 && faulted);
    check("crash priv -> Prozess beendet", run_user("/bin/crash", "crash priv", &code, &faulted) == 0 && faulted);
    check("crash badptr -> Syscall-Fehler statt Absturz",
          run_user("/bin/crash", "crash badptr", &code, &faulted) == 0 && !faulted && code == 7);
    check("crash unmapped (nach munmap) -> Prozess beendet",
          run_user("/bin/crash", "crash unmapped", &code, &faulted) == 0 && faulted);
    check("memtest (brk, mmap, malloc)", run_user("/bin/memtest", "memtest", &code, &faulted) == 0 && code == 0 && !faulted);
    check("Nicht vorhandenes Programm", run_user("/bin/gibtsnicht", "x", &code, &faulted) == -1);

    /* Taste 'x' einspeisen, keytest liest sie per SYS_GETCHAR */
    int pid = process_spawn("/bin/keytest", "keytest", 0);
    thread_sleep_ms(100);
    type_string("x");
    check("keytest liest 'x'", pid > 0 && process_wait(pid, 0, &code, &faulted, 3000) == 0 && code == 'x');

    /* fork/exec/Pipes/dup2/kill sowie lseek/rename/stat/chdir werden von eigenen Programmen geprueft */
    check("forktest (fork, exec, Pipe, dup2, kill)", run_user("/bin/forktest", "forktest", &code, &faulted) == 0 && code == 0 && !faulted);
    check("fstest (lseek, rename, stat, chdir, getcwd)", run_user("/bin/fstest", "fstest", &code, &faulted) == 0 && code == 0 && !faulted);

    /* Die Shell: Befehle tippen (Tastatur-Injektion), die Ergebnisse stehen danach als Dateien auf /disk.
     * Prueft Pipes, Umleitungen, Anfuehrungszeichen, relative Pfade, Verlauf (Pfeil hoch) und Ctrl-C. */
    pid = process_spawn("/bin/sh", "sh", 0);
    thread_sleep_ms(200);
    feed("help\n");
    feed("echo hallo > /disk/o1.txt\n");
    feed("cat /disk/o1.txt | wc > /disk/o2.txt\n");
    feed("seq 1 5 | grep 3 >> /disk/o1.txt\n");
    feed("ls /bin | head -n 3 > /disk/o3.txt\n");
    feed("cd /disk\n");
    feed("pwd > o4.txt\n");
    feed("echo x >> o6.txt\n");
    keyboard_deliver(KEY_UP);                 /* Pfeil hoch: die letzte Zeile aus dem Verlauf zurueckholen */
    feed("\n");                /* ... und noch einmal ausfuehren */
    feed("echo \"zwei  Woerter\" > o7.txt\n");
    feed("mv o1.txt o5.txt\n");
    feed("cat < o5.txt | grep hallo > o8.txt\n");
    feed("gibtsnicht\n");      /* unbekannter Befehl: Fehlermeldung, Shell laeuft weiter */
    feed("echo > sync1.txt\n");
    wait_for_file("/disk", "SYNC1.TXT", 20000); /* die Shell ist bereit */
    feed("sleep 30\n");
    thread_sleep_ms(600);             /* Shell muss forken/exec'en und die Vordergrundgruppe setzen */
    uint64_t t_sleep = apic_ticks();
    keyboard_deliver(3);                  /* bricht "sleep 30" ab, nicht die Shell */
    feed("echo nach-ctrl-c > o9.txt\n");
    wait_for_file("/disk", "O9.TXT", 5000);
    uint64_t sleep_ticks = apic_ticks() - t_sleep; /* so lange dauerte es von Ctrl-C bis die Shell wieder Befehle ausfuehrte */
    feed("wc > o11.txt\n");     /* wc liest von der Tastatur (Zeilenmodus des Terminals) */
    thread_sleep_ms(500);
    feed("eins zwei\n");
    keyboard_deliver(4);                   /* Dateiende */
    /* Variablen, ;, &&, ||, Hintergrundjobs, Skripte und Tab-Vervollstaendigung */
    feed("X=hallo; echo \"v=$X\" > p1.txt\n");
    feed("false && echo nein > p2.txt || echo ja > p2.txt\n");
    feed("echo a > p3.txt; echo b >> p3.txt\n");
    feed("echo bg > p5.txt &\n");
    feed("wait\n");
    feed("rm p6.txt\n");        /* Rest eines frueheren, abgebrochenen Laufs */
    feed("echo 'echo $1-$# >> p6.txt' > s.sh\n");
    feed("sh s.sh abc\n");
    feed("./s.sh xyz\n");
    feed("echo tab > p8.txt\n");
    feed("cat p8.t\t > p9.txt\n");
    feed("seq 1 20 | tail -n 3 > q2.txt\n");
    feed("seq 1 3 | less > q3.txt\n");
    feed("seq 1 300 > q4.txt\n");
    feed("cat q4.txt | wc > q5.txt\n"); /* eine lange Datei kommt vollstaendig durch cat */
    uint64_t date_before = rtc_now(), date_ms0 = time_ms();
    feed("date +s > q6.txt\n");
    feed("date -s '2031-05-06 07:08:09'\n");
    feed("date +s > q7.txt\n");
    feed("resolution > q8.txt\n");
    feed("keymap > qc.txt\n");
    feed("keymap xx 2> qd.txt\n");
    feed("resolution -d /disk 800x600 > q9.txt\n");
    feed("resolution -d /disk -s 3 >> q9.txt\n");
    feed("cat cmdline.txt > qa.txt\n");
    feed("resolution -d /disk auto -s auto >> q9.txt\n");
    feed("cat cmdline.txt > qb.txt\n");
    feed("rm cmdline.txt\n");
    thread_sleep_ms(300);
    console_clipboard_set("echo Gr\xC3\xBC\xC3\x9F > pv.txt\n", 21); /* Ctrl-V in der Shell (mit Umlauten) */
    keyboard_deliver(0x16);
    thread_sleep_ms(1500);
    console_clipboard_set("echo rechts > pw.txt\n", 21);             /* rechte Maustaste fuegt ein */
    mouse_report(2, 0, 0, 0);
    mouse_report(0, 0, 0, 0);
    thread_sleep_ms(1500);
    feed("if true\n");                 /* mehrzeilig: die Shell liest weiter, bis 'fi' kommt */
    feed("then echo ml > ml.txt\n");
    feed("fi\n");
    feed("gruss() { echo hi $1; }\n");  /* Funktion bleibt fuer spaetere Zeilen erhalten */
    feed("gruss du > fn.txt\n");
    feed("ps > o10.txt\n");

    check("Shell hat alle Befehle abgearbeitet", wait_for_file("/disk", "O10.TXT", 25000));
    thread_sleep_ms(500);

    char data[256];
    check("echo > datei", slurp("/disk/O5.TXT", data, sizeof(data)) == 8 && strcmp(data, "hallo\n3\n") == 0); /* o1 wurde in o5 umbenannt, '>>' hat angehaengt */
    check("Pipe: cat | wc", slurp("/disk/O2.TXT", data, sizeof(data)) > 0 && strcmp(data, "1 1 6\n") == 0);
    check("Pipe: ls | head -n 3", slurp("/disk/O3.TXT", data, sizeof(data)) > 0 && count_lines(data) == 3);
    check("cd + pwd + relativer Pfad", slurp("/disk/O4.TXT", data, sizeof(data)) > 0 && strcmp(data, "/disk\n") == 0);
    check("Verlauf: Pfeil hoch + Enter wiederholt den Befehl", slurp("/disk/O6.TXT", data, sizeof(data)) > 0 && strcmp(data, "x\nx\n") == 0);
    check("Anfuehrungszeichen: ein Argument mit zwei Leerzeichen", slurp("/disk/O7.TXT", data, sizeof(data)) > 0 && strcmp(data, "zwei  Woerter\n") == 0);
    check("Eingabeumleitung: cat < datei | grep", slurp("/disk/O8.TXT", data, sizeof(data)) > 0 && strcmp(data, "hallo\n") == 0);
    check("Ctrl-C beendet sleep, die Shell laeuft weiter", wait_for_file("/disk", "O9.TXT", 100) && sleep_ticks < 1000);
    kprintf("  (nach Ctrl-C war die Shell in %lu Ticks wieder bereit; sleep 30 haette 3000 gedauert)\n", sleep_ticks);
    check("Terminal-Zeilenmodus: wc liest von der Tastatur bis Ctrl-D", slurp("/disk/O11.TXT", data, sizeof(data)) > 0 && strcmp(data, "1 2 10\n") == 0);
    check("ps listet die Prozesse", slurp("/disk/O10.TXT", data, sizeof(data)) > 0 && strstr_(data, "PID") && strstr_(data, "sh"));

    check("Variable: X=hallo; echo \"v=$X\"", slurp("/disk/P1.TXT", data, sizeof(data)) > 0 && strcmp(data, "v=hallo\n") == 0);
    check("&& und ||: false && a || b", slurp("/disk/P2.TXT", data, sizeof(data)) > 0 && strcmp(data, "ja\n") == 0);
    check("; trennt Befehle", slurp("/disk/P3.TXT", data, sizeof(data)) > 0 && strcmp(data, "a\nb\n") == 0);
    check("Hintergrundjob (&) und wait", slurp("/disk/P5.TXT", data, sizeof(data)) > 0 && strcmp(data, "bg\n") == 0);
    check("Skript: sh datei args und ./datei.sh", slurp("/disk/P6.TXT", data, sizeof(data)) > 0 && strcmp(data, "abc-1\nxyz-1\n") == 0);
    check("Tab-Vervollstaendigung (cat p8.t<Tab>)", slurp("/disk/P9.TXT", data, sizeof(data)) > 0 && strcmp(data, "tab\n") == 0);

    check("tail -n 3", slurp("/disk/Q2.TXT", data, sizeof(data)) > 0 && strcmp(data, "18\n19\n20\n") == 0);
    check("less ohne Terminal wirkt wie cat", slurp("/disk/Q3.TXT", data, sizeof(data)) > 0 && strcmp(data, "1\n2\n3\n") == 0);
    check("cat einer langen Datei (300 Zeilen) ist vollstaendig", slurp("/disk/Q5.TXT", data, sizeof(data)) > 0 && strcmp(data, "300 300 1092\n") == 0);

    {
        /* date liest die Uhr, date -s stellt sie (2031-05-06 07:08:09); danach wird sie wieder zurueckgestellt */
        int64_t d1 = slurp("/disk/Q6.TXT", data, sizeof(data)) > 0 ? (int64_t)atoi_u(data) : -1;
        int64_t d2 = slurp("/disk/Q7.TXT", data, sizeof(data)) > 0 ? (int64_t)atoi_u(data) : -1;
        check("date +s liefert die RTC-Zeit", !rtc_valid() || (d1 >= (int64_t)date_before && d1 <= (int64_t)date_before + 90));
        check("date -s stellt die Uhr (2031-05-06 07:08:09 = 1935817689)", d2 >= 1935817689 && d2 <= 1935817689 + 5);
        rtc_set(date_before + (time_ms() - date_ms0) / 1000);
    }
    {
        check("resolution: listet die Grafikmodi", slurp("/disk/Q8.TXT", data, sizeof(data)) > 0 && strstr_(data, "Verfuegbare Grafikmodi") &&
                                                   strstr_(data, "aktuell"));
        int64_t qa = slurp("/disk/QA.TXT", data, sizeof(data));
        check("resolution: schreibt mode= und scale= in die cmdline.txt", qa > 0 && strcmp(data, "mode=800x600 scale=3\n") == 0);
        int64_t qb = slurp("/disk/QB.TXT", data, sizeof(data));
        check("resolution: auto entfernt die Eintraege wieder", qb > 0 && strcmp(data, "\n") == 0);
    }
    check("Einfuegen in die Shell mit Ctrl-V (UTF-8)", slurp("/disk/PV.TXT", data, sizeof(data)) > 0 && strcmp(data, "Gr\xC3\xBC\xC3\x9F\n") == 0);
    check("Einfuegen mit der rechten Maustaste", slurp("/disk/PW.TXT", data, sizeof(data)) > 0 && strcmp(data, "rechts\n") == 0);
    check("Mehrzeilige Eingabe (if ... fi ueber drei Zeilen)", slurp("/disk/ML.TXT", data, sizeof(data)) > 0 && strcmp(data, "ml\n") == 0);
    check("Funktion aus einer frueheren Zeile", slurp("/disk/FN.TXT", data, sizeof(data)) > 0 && strcmp(data, "hi du\n") == 0);
    feed("rm ml.txt fn.txt\n");
    feed("rm pv.txt pw.txt\n");
    check("keymap zeigt das Layout", slurp("/disk/QC.TXT", data, sizeof(data)) > 0 && strstr_(data, "Tastaturlayout: us"));
    check("keymap lehnt unbekannte Layouts ab und bleibt bei us", strcmp(keymap_name(), "us") == 0);
    feed("rm q8.txt q9.txt qa.txt qb.txt qc.txt qd.txt\n");
    feed("rm q2.txt q3.txt q4.txt q5.txt q6.txt q7.txt\n");
    feed("rm o5.txt o2.txt o3.txt o4.txt o6.txt o7.txt o8.txt o9.txt o10.txt o11.txt p1.txt p2.txt p3.txt p5.txt p6.txt p8.txt p9.txt s.sh sync1.txt\n");
    feed("cd /\n");
    feed("exit\n");
    check("Shell beendet sich mit exit (Code 0)",
          pid > 0 && process_wait(pid, 0, &code, &faulted, 15000) == 0 && code == 0 && !faulted);
    check("Testdateien wieder geloescht", !dir_has("/disk", "O5.TXT", 0) && !dir_has("/disk", "O10.TXT", 0) &&
                                          !dir_has("/disk", "O9.TXT", 0) && !dir_has("/disk", "O11.TXT", 0));

    thread_sleep_ms(100);
    check("Keine Frames verloren (Adressraeume/Stacks freigegeben)", pmm_free_frame_count() == frames_before);
    check("Kernel-Heap konsistent", heap_check());
}

static void halt_forever(void)
{
    cpu_cli();
    for (;;)
        cpu_hlt();
}

/* ACPI, APIC (Timer), IOAPIC und Tastatur. Ohne Timer laeuft kein Scheduler: dann geht es nicht weiter. */
static int init_interrupts(BootInfo *info)
{
    if (acpi_init(info->rsdp) != 0) {
        kprintf("FEHLER: ACPI/MADT nicht lesbar\n");
        return -1;
    }
    pic_disable();
    if (apic_init(acpi_info()->lapic_addr) != 0) {
        kprintf("FEHLER: Local APIC/Timer nicht nutzbar\n");
        return -1;
    }
    if (ioapic_init() != 0)
        kprintf("WARNUNG: kein IOAPIC, Tastatur-Interrupts fehlen\n");
    else if (keyboard_init() != 0)
        kprintf("WARNUNG: Tastatur-IRQ nicht geroutet\n");
    cpu_sti();
    return 0;
}

/* initrd (/) und, falls vorhanden, das FAT32-Volume "MINIKERNEL" (/disk) */
static void init_storage(BootInfo *info)
{
    if (info->module) {
        int n = vfs_init(info->module, info->module_size);
        kprintf("initrd: %lu Bytes, %d Eintraege\n", (unsigned long)info->module_size, n);
    } else {
        kprintf("WARNUNG: kein initrd geladen\n");
    }

    pci_scan();
    blk_init();
    usb_init(); /* USB-Massenspeicher melden sich hier als weitere Blockgeraete an */
    if (blk_count() == 0) {
        kprintf("Kein Blockgeraet gefunden, /disk ist nicht verfuegbar\n");
        return;
    }
    /* Alle FAT-Volumes einbinden. Beschrieben wird nur das Volume mit dem Label MINIKERNEL (/disk), der Rest ist
     * nur lesbar (/mnt/<geraet>): der Kernel schreibt nie auf fremde Datentraeger. */
    fat_set_default_readonly(cmdline_has("fsro")); /* "fsro" in cmdline.txt: fremde Volumes nur lesbar */
    fs_init();
    int dv = fs_disk_volume();
    if (dv >= 0) {
        FatVolumeInfo vi;
        fat_volume_info(dv, &vi);
        kprintf("/disk: Volume '%s' auf %s, %lu MiB\n", vi.label, vi.device,
                (unsigned long)fat_total_clusters(dv) * fat_cluster_bytes(dv) / (1024 * 1024));
    } else {
        kprintf("Kein FAT32-Volume 'MINIKERNEL' gefunden, /disk ist nicht verfuegbar\n");
    }
}

/* Schreibtests auf einem Volume unter /mnt (FAT12/16/32 und exFAT): Dateien, lange Namen, Verzeichnisse, Wachstum von
 * Verzeichnissen, Umbenennen, keine Cluster-Lecks */
static void foreign_write_tests(const char *base)
{
    char p[192], q[192], name[96];
    uint64_t size = 0;
    FsFile f;

    uint32_t free0 = fs_free_clusters(base);
    ksnprintf(p, sizeof(p), "%s/T1.BIN", base);
    check("Schreiben: Datei anlegen (10000 Bytes in Bloecken zu 333)",
          write_pattern(p, FAT_O_WRONLY | FAT_O_CREAT, 0, 10000, 333, 1) == 0 && verify_pattern(p, 10000, 1, 0, 0) &&
          dir_has(base, "T1.BIN", &size) && size == 10000);
    check("Anhaengen (O_APPEND)", write_pattern(p, FAT_O_WRONLY | FAT_O_APPEND, 10000, 100, 100, 1) == 0 &&
                                  verify_pattern(p, 10100, 1, 0, 0));
    check("Anfang ueberschreiben, Rest bleibt", write_pattern(p, FAT_O_WRONLY, 0, 10, 10, 2) == 0 &&
                                                verify_pattern(p, 10100, 1, 10, 2));
    check("Abschneiden (O_TRUNC) gibt die Cluster frei",
          write_pattern(p, FAT_O_WRONLY | FAT_O_TRUNC, 0, 0, 1, 0) == 0 && dir_has(base, "T1.BIN", &size) && size == 0 &&
          fs_free_clusters(base) == free0);
    check("Datei loeschen", fs_unlink(p) == 0 && !dir_has(base, "T1.BIN", 0) && fs_free_clusters(base) == free0);

    /* Lange Namen */
    ksnprintf(p, sizeof(p), "%s/Ein langer Dateiname mit Leerzeichen.txt", base);
    ksnprintf(q, sizeof(q), "%s/ein LANGER dateiname MIT leerzeichen.TXT", base);
    check("Lange Namen: anlegen, exakt aufgelistet, ohne Beachtung der Schreibweise lesbar",
          write_pattern(p, FAT_O_WRONLY | FAT_O_CREAT, 0, 300, 100, 3) == 0 &&
          dir_has(base, "Ein langer Dateiname mit Leerzeichen.txt", &size) && size == 300 && verify_pattern(q, 300, 3, 0, 0));
    ksnprintf(q, sizeof(q), "%s/klein.txt", base); /* passt in 8.3, klein geschrieben: Windows zeigt es klein */
    check("Kurzer Kleinbuchstaben-Name", write_pattern(q, FAT_O_WRONLY | FAT_O_CREAT, 0, 10, 10, 5) == 0 &&
                                         dir_has(base, "klein.txt", 0));
    check("Lange Namen: loeschen", fs_unlink(p) == 0 && fs_unlink(q) == 0 && !dir_has(base, "Ein langer Dateiname mit Leerzeichen.txt", 0) &&
                                   !dir_has(base, "klein.txt", 0) && fs_free_clusters(base) == free0);
    ksnprintf(p, sizeof(p), "%s/a*b.txt", base);
    check("Ungueltiger Name abgelehnt", fs_open(p, FAT_O_WRONLY | FAT_O_CREAT, &f) == ERR_INVAL);

    /* Umlaute (Namen sind UTF-8, auf der Platte UTF-16) und Zeitstempel */
    ksnprintf(p, sizeof(p), "%s/Gr\xC3\xB6\xC3\x9F" "e \xC3\x9C" "bung \xE2\x82\xAC.txt", base);
    ksnprintf(q, sizeof(q), "%s/GR\xC3\x96\xC3\x9F" "E \xC3\xBC" "BUNG \xE2\x82\xAC.TXT", base);
    FsStat ust;
    uint64_t unow = rtc_now();
    check("Umlaute und Euro im Namen: anlegen, exakt aufgelistet, ohne Beachtung der Schreibweise lesbar",
          write_pattern(p, FAT_O_WRONLY | FAT_O_CREAT, 0, 90, 30, 2) == 0 &&
          dir_has(base, "Gr\xC3\xB6\xC3\x9F" "e \xC3\x9C" "bung \xE2\x82\xAC.txt", &size) && size == 90 &&
          verify_pattern(q, 90, 2, 0, 0));
    check("Zeitstempel: Datei hat die aktuelle RTC-Zeit (+-4 s)",
          !rtc_valid() || (fs_stat(p, &ust) == 0 && ust.mtime + 4 >= unow && ust.mtime <= unow + 4));
    check("Umlaut-Datei loeschen", fs_unlink(p) == 0 && fs_free_clusters(base) == free0);

    /* Verzeichnisse */
    ksnprintf(p, sizeof(p), "%s/Neuer Ordner", base);
    ksnprintf(q, sizeof(q), "%s/Neuer Ordner/innen.bin", base);
    check("mkdir mit langem Namen", fs_mkdir(p) == 0 && dir_has(base, "Neuer Ordner", 0) && fs_mkdir(p) == ERR_EXIST);
    check("Datei im Unterverzeichnis", write_pattern(q, FAT_O_WRONLY | FAT_O_CREAT, 0, 2000, 500, 6) == 0 &&
                                       verify_pattern(q, 2000, 6, 0, 0) && dir_has(p, "innen.bin", 0));
    check("rmdir auf nicht leeres Verzeichnis abgelehnt", fs_unlink(p) == ERR_NOTEMPTY);
    check("Aufraeumen (Datei, dann Verzeichnis)", fs_unlink(q) == 0 && fs_unlink(p) == 0 && !dir_has(base, "Neuer Ordner", 0) &&
                                                  fs_free_clusters(base) == free0);

    /* Ein Verzeichnis waechst ueber mehrere Cluster (lange Namen belegen mehrere Eintraege je Datei) */
    ksnprintf(p, sizeof(p), "%s/WACHSTUM", base);
    int grow_ok = fs_mkdir(p) == 0;
    for (int i = 0; i < 60 && grow_ok; i++) {
        ksnprintf(name, sizeof(name), "%s/Datei mit langem Namen Nummer %02d.txt", p, i);
        grow_ok = write_pattern(name, FAT_O_WRONLY | FAT_O_CREAT, 0, 20 + (uint32_t)i, 20, (uint8_t)i) == 0;
    }
    for (int i = 0; i < 60 && grow_ok; i++) {
        ksnprintf(name, sizeof(name), "Datei mit langem Namen Nummer %02d.txt", i);
        ksnprintf(q, sizeof(q), "%s/%s", p, name);
        grow_ok = dir_has(p, name, &size) && size == 20 + (uint64_t)i && verify_pattern(q, 20 + (uint32_t)i, (uint8_t)i, 0, 0);
    }
    check("60 Dateien mit langen Namen (Verzeichnis waechst) anlegen und pruefen", grow_ok);
    for (int i = 0; i < 60; i++) {
        ksnprintf(name, sizeof(name), "%s/Datei mit langem Namen Nummer %02d.txt", p, i);
        grow_ok &= fs_unlink(name) == 0;
    }
    check("... und wieder loeschen, Verzeichnis entfernen: keine verlorenen Cluster",
          grow_ok && !dir_has(p, "Datei mit langem Namen Nummer 00.txt", 0) && fs_unlink(p) == 0 &&
          fs_free_clusters(base) == free0);

    /* Umbenennen und Verschieben */
    ksnprintf(p, sizeof(p), "%s/REN1.BIN", base);
    ksnprintf(q, sizeof(q), "%s/Neuer Name mit Text.bin", base);
    check("rename Datei (kurz -> lang)", write_pattern(p, FAT_O_WRONLY | FAT_O_CREAT, 0, 700, 100, 8) == 0 && fs_rename(p, q) == 0 &&
                                         !dir_has(base, "REN1.BIN", 0) && dir_has(base, "Neuer Name mit Text.bin", &size) &&
                                         size == 700 && verify_pattern(q, 700, 8, 0, 0));
    ksnprintf(p, sizeof(p), "%s/SUBR", base);
    ksnprintf(name, sizeof(name), "%s/SUBR/verschoben.bin", base);
    check("rename in ein anderes Verzeichnis", fs_mkdir(p) == 0 && fs_rename(q, name) == 0 && !dir_has(base, "Neuer Name mit Text.bin", 0) &&
                                               verify_pattern(name, 700, 8, 0, 0));
    ksnprintf(q, sizeof(q), "%s/SUBR/Tiefer Ordner", base);
    char dst[192], inner[192];
    ksnprintf(dst, sizeof(dst), "%s/Ordner oben", base);
    ksnprintf(inner, sizeof(inner), "%s/Ordner oben/drin.bin", base);
    check("rename eines Verzeichnisses in ein anderes Verzeichnis (samt Inhalt)",
          fs_mkdir(q) == 0 && write_pattern(name, FAT_O_WRONLY, 0, 0, 1, 0) == 0 && fs_rename(q, dst) == 0 &&
          write_pattern(inner, FAT_O_WRONLY | FAT_O_CREAT, 0, 50, 50, 9) == 0 && verify_pattern(inner, 50, 9, 0, 0) &&
          dir_has(dst, "drin.bin", 0) && !dir_has(p, "Tiefer Ordner", 0));
    ksnprintf(q, sizeof(q), "%s/SUBR/INTO", base);
    check("Verzeichnis nicht in sich selbst verschieben", fs_rename(p, q) == ERR_INVAL);
    check("rename auf vorhandenen Namen abgelehnt", fs_rename(name, dst) == ERR_EXIST);
    check("Aufraeumen nach rename", fs_unlink(inner) == 0 && fs_unlink(dst) == 0 && fs_unlink(name) == 0 && fs_unlink(p) == 0 &&
                                    fs_free_clusters(base) == free0);

    /* Anhaengen an eine vorhandene (bei exFAT zusammenhaengende) Datei */
    ksnprintf(p, sizeof(p), "%s/BIG.BIN", base);
    uint8_t tail[100];
    for (int i = 0; i < 100; i++)
        tail[i] = 0x5A;
    int app_ok = fs_open(p, FAT_O_WRONLY | FAT_O_APPEND, &f) == 0 && fs_write(&f, tail, 100) == 100;
    if (app_ok)
        fs_close(&f);
    uint8_t chk[8];
    app_ok = app_ok && dir_has(base, "BIG.BIN", &size) && size == 20100 && fs_open(p, FAT_O_RDONLY, &f) == 0 &&
             fs_seek(&f, 19996, 0) == 19996 && fs_read(&f, chk, 8) == 8 && chk[0] == (uint8_t)(19996 * 13 + 5) &&
             chk[3] == (uint8_t)(19999 * 13 + 5) && chk[4] == 0x5A && chk[7] == 0x5A;
    check("Anhaengen an BIG.BIN (20000 Bytes, ueber die Clustergrenze hinaus)", app_ok);
}

/* Weitere Datentraeger (tools/mkstick.py): nur lesbar, lange Namen. Ohne angeschlossenen Test-Stick wird nichts geprueft. */
static void test_foreign(void)
{
    title("Weitere Datentraeger (/mnt, nur lesbar)");
    fs_rescan();

    char base[64] = "";
    FsDirEnt e;
    for (unsigned i = 0; fs_readdir("/mnt", i, &e) == 0; i++) {
        char p[96];
        ksnprintf(p, sizeof(p), "/mnt/%s", e.name);
        if (dir_has(p, "HELLO.TXT", 0)) {
            memcpy(base, p, strlen(p) + 1);
            break;
        }
    }
    if (!base[0]) {
        kprintf("  (kein Test-Stick angeschlossen, uebersprungen)\n");
        return;
    }
    kprintf("  Test-Stick unter %s\n", base);

    char path[160];
    FsFile f;
    char buf[64];
    ksnprintf(path, sizeof(path), "%s/Lange Datei mit Leerzeichen.txt", base);
    int ok = fs_open(path, FAT_O_RDONLY, &f) == 0;
    int64_t n = ok ? fs_read(&f, buf, sizeof(buf)) : -1;
    if (ok)
        fs_close(&f);
    check("Datei mit langem Namen lesen", n == 39 && memcmp(buf, "Inhalt der langen Datei.", 24) == 0);

    int is_exfat = dir_has(base, "EXFAT.MRK", 0); /* exFAT kennt keine Kurznamen */
    if (!is_exfat) {
        ksnprintf(path, sizeof(path), "%s/LANGED~1.TXT", base);
        check("... auch ueber den Kurznamen", fs_open(path, FAT_O_RDONLY, &f) == 0);
    }

    ksnprintf(path, sizeof(path), "%s/lange datei MIT leerzeichen.TXT", base);
    check("Namen sind case-insensitiv", fs_open(path, FAT_O_RDONLY, &f) == 0);

    ksnprintf(path, sizeof(path), "%s/Bilder und Notizen/Notiz Nummer eins.txt", base);
    ok = fs_open(path, FAT_O_RDONLY, &f) == 0;
    n = ok ? fs_read(&f, buf, sizeof(buf)) : -1;
    check("Unterverzeichnis mit langem Namen", n > 20 && memcmp(buf, "Notiz im Unterverzeichnis", 25) == 0);

    ksnprintf(path, sizeof(path), "%s/BIG.BIN", base);
    int big_ok = fs_open(path, FAT_O_RDONLY, &f) == 0;
    uint8_t chunk[700];
    uint32_t off = 0;
    while (big_ok && (n = fs_read(&f, chunk, sizeof(chunk))) > 0) {
        for (int64_t i = 0; i < n; i++)
            if (chunk[i] != (uint8_t)((off + i) * 13 + 5))
                big_ok = 0;
        off += (uint32_t)n;
    }
    check("BIG.BIN (20000 Bytes ueber mehrere Cluster) stimmt", big_ok && off == 20000);

    uint64_t size = 0;
    ksnprintf(path, sizeof(path), "%s", base);
    check("readdir zeigt lange Namen, Kleinschreibung (NT-Flag) und Verzeichnisse",
          dir_has(path, "Lange Datei mit Leerzeichen.txt", &size) && size == 39 && dir_has(path, "readme.txt", 0) &&
          dir_has(path, "Bilder und Notizen", 0));
    check("Geloeschte Eintraege (samt LFN-Resten) erscheinen nicht", !dir_has(path, "Geloeschte Datei.txt", 0) &&
                                                                       !dir_has(path, "GELOES~1.TXT", 0));
    FsStat st;
    check("stat unter /mnt", fs_stat(base, &st) == 0 && st.is_dir && fs_stat("/mnt", &st) == 0 && st.is_dir);

    if (dir_has(base, "DIRTY.MRK", 0)) { /* nicht sauber getrenntes Volume: nur lesbar */
        ksnprintf(path, sizeof(path), "%s/NEU.TXT", base);
        check("Dirty-Volume: Anlegen abgelehnt (ROFS)", fs_open(path, FAT_O_WRONLY | FAT_O_CREAT, &f) == ERR_ROFS);
        ksnprintf(path, sizeof(path), "%s/HELLO.TXT", base);
        check("Dirty-Volume: Schreiben/Loeschen/mkdir abgelehnt", fs_open(path, FAT_O_WRONLY, &f) == ERR_ROFS &&
                                                                  fs_unlink(path) == ERR_ROFS && fs_mkdir("/mnt/xx") != 0);
        return;
    }
    foreign_write_tests(base);

    if (is_exfat) { /* exFAT-spezifisch: Dateien ohne FAT-Kette, fragmentierte Dateien, gueltige Laenge, Nicht-ASCII-Namen */
        ksnprintf(path, sizeof(path), "%s/FRAG.BIN", base);
        int frag_ok = fs_open(path, FAT_O_RDONLY, &f) == 0;
        off = 0;
        while (frag_ok && (n = fs_read(&f, chunk, sizeof(chunk))) > 0) {
            for (int64_t i = 0; i < n; i++)
                if (chunk[i] != (uint8_t)((off + i) * 7 + 3))
                    frag_ok = 0;
            off += (uint32_t)n;
        }
        check("exFAT: fragmentierte Datei (FAT-Kette, 5000 Bytes)", frag_ok && off == 5000);

        ksnprintf(path, sizeof(path), "%s/SPARSE.BIN", base);
        int sp_ok = fs_open(path, FAT_O_RDONLY, &f) == 0 && dir_has(base, "SPARSE.BIN", &size) && size == 3000;
        off = 0;
        while (sp_ok && (n = fs_read(&f, chunk, sizeof(chunk))) > 0) {
            for (int64_t i = 0; i < n; i++) {
                uint32_t pos = off + (uint32_t)i;
                uint8_t want = pos < 1000 ? (uint8_t)(pos * 5 + 1) : 0;
                if (chunk[i] != want)
                    sp_ok = 0;
            }
            off += (uint32_t)n;
        }
        check("exFAT: hinter der gueltigen Laenge (1000 von 3000) wird Null gelesen", sp_ok && off == 3000);

        int seek_ok = 1;
        ksnprintf(path, sizeof(path), "%s/BIG.BIN", base);
        if (fs_open(path, FAT_O_RDONLY, &f) == 0 && fs_seek(&f, 12345, 0) == 12345) {
            uint8_t b[4];
            seek_ok = fs_read(&f, b, 4) == 4 && b[0] == (uint8_t)(12345 * 13 + 5) && b[3] == (uint8_t)(12348 * 13 + 5);
        } else {
            seek_ok = 0;
        }
        check("exFAT: seek mitten in eine zusammenhaengende Datei", seek_ok);
        check("exFAT: Name mit Umlaut (von Fremdprogramm angelegt) wird als UTF-8 gelesen", dir_has(base, "B\xC3\xA4r.txt", 0) &&
                                                                                            dir_has(base, "B\xC3\x84R.TXT", 0));
    }
}

/* Zellen (Pixel) zweier Spalten in Zeile 0 identisch? */
static int cells_equal(uint32_t col_a, uint32_t col_b)
{
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 8; x++)
            if (console_read_pixel(col_a * 8 + x, y) != console_read_pixel(col_b * 8 + x, y))
                return 0;
    return 1;
}

static void test_unicode(void)
{
    title("Unicode (UTF-8)");
    const char *s = "a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80";
    uint32_t a = utf8_next(&s), b = utf8_next(&s), c = utf8_next(&s), d = utf8_next(&s), e = utf8_next(&s);
    check("utf8_next: a, ae-Umlaut, Euro, Emoji, Ende", a == 'a' && b == 0xE4 && c == 0x20AC && d == 0x1F600 && e == 0);

    const char *bad = "\xC3(\xC0\x80";
    uint32_t b1 = utf8_next(&bad), b2 = utf8_next(&bad), b3 = utf8_next(&bad);
    check("Ungueltiges UTF-8 (abgebrochen, Ueberlaenge) wird zu U+FFFD", b1 == UNI_REPLACEMENT && b2 == '(' && b3 == UNI_REPLACEMENT);
    check("utf8_valid", utf8_valid("Pr\xC3\xBC" "fung") && !utf8_valid("\xE2\x82") && !utf8_valid("\xED\xA0\x80") && utf8_valid(""));

    uint16_t u16[8];
    int n = utf8_to_utf16("\xF0\x9F\x98\x80" "\xC3\xA4", u16, 8);
    char back[16];
    int bl = utf16_to_utf8(u16, n, back, sizeof(back));
    check("UTF-16: Ersatzpaar und Rueckweg", n == 3 && u16[0] == 0xD83D && u16[1] == 0xDE00 && u16[2] == 0xE4 &&
                                             bl == 6 && memcmp(back, "\xF0\x9F\x98\x80\xC3\xA4", 6) == 0);
    check("Grossbuchstaben (ASCII, Latin-1, Griechisch, Kyrillisch)",
          uni_upper('a') == 'A' && uni_upper(0xE4) == 0xC4 && uni_upper(0xDF) == 0xDF && uni_upper(0x3B1) == 0x391 &&
          uni_upper(0x430) == 0x410 && uni_upper(0xFF) == 0x178 && uni_upper(0x20AC) == 0x20AC);

    /* Konsole: Umlaute, Euro und ungueltige Bytes belegen je eine Zelle */
    console_clear();
    for (const char *p = "\xC3\xA4\xC3\xB6\xE2\x82\xAC\xFF?\xC3(x"; *p; p++)
        console_putc(*p);
    uint32_t col, row;
    console_get_cursor(&col, &row);
    int width_ok = col == 8 && row == 0; /* ae oe Euro | 0xFF ? | 0xC3 ( x -> FFFD ( x */
    int ink_ok = cell_has_ink(0, 0) && cell_has_ink(1, 0) && cell_has_ink(2, 0);
    int distinct_ok = !cells_equal(0, 4) && !cells_equal(0, 1) && !cells_equal(2, 4);
    int replace_ok = cells_equal(3, 4) && cells_equal(5, 4); /* ungueltige Folgen erscheinen wie '?' */
    console_clear();
    title("Unicode (UTF-8)");
    check("Konsole: eine Zelle je Zeichen (ae, oe, Euro, ungueltige Bytes)", width_ok);
    check("Konsole: Umlaute und Euro haben eigene Glyphen (nicht '?')", ink_ok && distinct_ok);
    check("Konsole: ungueltiges UTF-8 erscheint als '?'", replace_ok);
}

static void test_rtc(void)
{
    title("Uhr (RTC)");
    check("RTC liefert eine gueltige Zeit", rtc_valid());
    if (!rtc_valid())
        return;
    uint64_t t0 = rtc_now();
    DateTime dt;
    unix_to_datetime(t0, &dt);
    kprintf("  Uhr: %04d-%02d-%02d %02d:%02d:%02d\n", dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
    check("Datum plausibel (2020..2099)", dt.year >= 2020 && dt.year <= 2099 && dt.month >= 1 && dt.month <= 12 && dt.day >= 1);

    DateTime x = {2000, 2, 29, 12, 34, 56, 0}, y;
    unix_to_datetime(datetime_to_unix(&x), &y);
    check("Kalender: 2000-02-29 12:34:56 = 951827696 (Dienstag)",
          datetime_to_unix(&x) == 951827696 && y.year == 2000 && y.month == 2 && y.day == 29 && y.hour == 12 && y.min == 34 &&
          y.sec == 56 && y.wday == 2);
    DateTime z = {2026, 9, 29, 0, 0, 0, 0};
    unix_to_datetime(datetime_to_unix(&z), &y);
    int wday_ok = y.wday == 2;
    unix_to_datetime(4102444799ULL, &y);
    check("Kalender: 2026-09-29 ist ein Dienstag, 4102444799 = 2099-12-31 23:59:59",
          wday_ok && y.year == 2099 && y.month == 12 && y.day == 31 && y.hour == 23 && y.min == 59 && y.sec == 59);

    /* Zeit in die RTC schreiben, neu einlesen (CMOS) und wieder zurueckstellen */
    uint64_t target = 1893456000ULL + 12 * 3600 + 34 * 60 + 56; /* 2030-01-01 12:34:56 */
    check("RTC schreiben und neu einlesen", rtc_set(target) == 0 && (rtc_init(), 1) && rtc_now() >= target && rtc_now() <= target + 3);
    rtc_set(t0 + 1);

    uint16_t dd, tt;
    dos_now(&dd, &tt);
    uint64_t back = dos_to_unix(dd, tt);
    check("DOS-Zeitstempel: hin und zurueck (auf 2 s genau)", back != 0 && back + 4 >= rtc_now() && back <= rtc_now() + 1);
}

/* Zahl aus den Ziffern einer Zeile "Znnnnn" im sichtbaren Bild (Zeile row) */
static int line_number(uint32_t row)
{
    int v = 0;
    for (uint32_t c = 1; c <= 5; c++) {
        uint32_t ch = console_debug_char(c, row);
        if (ch < '0' || ch > '9')
            return -1;
        v = v * 10 + (int)(ch - '0');
    }
    return v;
}

static void put_line(int n)
{
    char l[16];
    int len = ksnprintf(l, sizeof(l), "Z%05d\n", n);
    for (int i = 0; i < len; i++)
        console_putc(l[i]);
}

static void test_scrollback(void)
{
    title("Konsole: Verlauf (Scrollback)");
    console_clear();
    uint32_t r = console_rows();
    for (int i = 0; i < (int)(r * 3); i++)
        put_line(i);
    uint32_t hist = console_history_lines();
    int top = line_number(0);

    console_view_scroll(5);
    int back5 = line_number(0);
    int ink_ok = cell_has_ink(1, 0);
    uint64_t off5 = console_view_offset();
    console_view_scroll(-3);
    int back2 = line_number(0);
    console_view_live();
    int live = line_number(0);

    console_view_scroll(4);
    int top4 = line_number(0);
    console_putc('x'); /* neue Ausgabe laesst die Ansicht stehen ... */
    uint64_t after_output = console_view_offset();
    for (int i = 0; i < (int)r; i++) /* ... auch wenn dabei gescrollt wird: dieselben Zeilen bleiben sichtbar */
        put_line(1000 + i);
    int still = line_number(0) == top4 && console_view_offset() > 4;
    console_live_request(); /* ein Tastendruck springt zurueck zum Ende (ueber den Konsolen-Thread) */
    thread_sleep_ms(60);
    uint64_t after_key = console_view_offset();
    console_view_scroll(4);
    console_view_scroll(1 << 30);
    uint64_t at_top = console_view_offset();
    hist = console_history_lines(); /* durch die zusaetzlichen Zeilen gewachsen */
    console_view_scroll(-(1 << 30));
    uint64_t at_end = console_view_offset();

    console_clear();
    title("Konsole: Verlauf (Scrollback)");
    check("Aus dem Bild gescrollte Zeilen landen im Verlauf", hist >= r && top > 0);
    check("Blaettern: 5 Zeilen zurueck zeigt die Zeile 5 davor (und zeichnet sie)", off5 == 5 && back5 == top - 5 && ink_ok);
    check("Blaettern: 3 Zeilen vor", back2 == top - 2);
    check("Zurueck zum Ende (live)", live == top);
    check("Neue Ausgabe laesst die Ansicht stehen (auch beim Scrollen)", after_output == 4 && still);
    check("Tastendruck springt zum Ende zurueck", after_key == 0);
    check("Blaettern ist auf den Verlauf begrenzt", at_top == hist && at_end == 0);
}

static void test_mouse(void)
{
    title("Maus");
    kprintf("  %s\n", mouse_attached() ? "USB-Maus angeschlossen" : "keine USB-Maus angeschlossen (Ereignisse werden simuliert)");
    int scale = (int)console_scale();
    MouseInfo mi;
    if (mouse_attached()) { /* echte Maus: der Zeiger muss schon auf dem Bildschirm stehen (Spitze + (1,2) ist ein weisser Pixel) */
        mouse_get(&mi);
        thread_sleep_ms(50);
        check("Angeschlossene Maus: Zeiger steht auf dem Bildschirm",
              console_debug_fb_pixel((uint32_t)mi.x + 1, (uint32_t)mi.y + 2) == 0xFFFFFF);
    }

    /* Meldungen wie von einem Treiber: Bewegung mit Beschleunigung, Tasten, Rad */
    mouse_report(0, 0, 0, 0);
    mouse_get(&mi);
    int x0 = mi.x, y0 = mi.y;
    unsigned ev0 = mi.events;
    mouse_report(1, 4, -3, 2);
    mouse_get(&mi);
    int moved_ok = mi.x == x0 + 8 * scale && mi.y == y0 - 6 * scale && mi.buttons == 1 && mi.wheel == 2 && mi.events == ev0 + 1;
    MouseInfo again;
    mouse_get(&again);
    int wheel_reset = again.wheel == 0;
    thread_sleep_ms(60); /* das Rad hat ein Blaettern im Verlauf angefordert: zurueck zum Ende */
    console_view_live();

    for (int i = 0; i < 300; i++)
        mouse_report(0, -127, -127, 0);
    mouse_get(&mi);
    int corner_tl = mi.x == 0 && mi.y == 0;
    for (int i = 0; i < 300; i++)
        mouse_report(0, 127, 127, 0);
    mouse_get(&mi);
    int corner_br = mi.x == (int)console_width_px() - 1 && mi.y == (int)console_height_px() - 1;
    mouse_report(0, 0, 0, 0);

    /* Zeiger zeichnen: nur im Framebuffer, nicht im Abbild; ueber Text liegend bleibt er sichtbar; ausblendbar */
    console_clear();
    console_cursor_set(48, 48, 1);
    int drawn = console_debug_fb_pixel(49, 50) == 0xFFFFFF && console_read_pixel(49, 50) != 0xFFFFFF;
    for (const char *c = "\x1b[4;7H#"; *c; c++) /* Zelle (6,3) = Pixel 48..55 x 48..63 liegt unter dem Zeiger */
        console_putc(*c);
    int over_text = console_debug_fb_pixel(49, 50) == 0xFFFFFF;
    console_cursor_set(48, 48, 0);
    int hidden = console_debug_fb_pixel(49, 50) == console_read_pixel(49, 50);
    console_clear();
    console_cursor_set(mi.x, mi.y, mouse_attached());
    title("Maus");
    check("Bewegung mit Beschleunigung, Tasten, Rad", moved_ok);
    check("Radwert wird beim Abholen zurueckgesetzt", wheel_reset);
    check("Position bleibt im Bildschirm (Ecken)", corner_tl && corner_br);
    check("Zeiger wird in den Framebuffer gezeichnet (nicht ins Abbild)", drawn);
    check("Zeiger bleibt sichtbar, wenn darunter Text gezeichnet wird", over_text);
    check("Zeiger ausblenden stellt den Hintergrund wieder her", hidden);
}

static void test_clipboard(void)
{
    title("Markieren, Kopieren, Einfuegen");
    while (keyboard_getchar() >= 0) /* Tastaturpuffer leeren */
        ;
    int cw = 8 * (int)console_scale(), ch = 16 * (int)console_scale();
    console_clear();
    for (const char *c = "Zeile eins\nHallo Maus 123\nGr\xC3\xBC\xC3\x9F" "e   \n"; *c; c++)
        console_putc(*c);

    /* Zeile 1, Spalten 0..9 ("Hallo Maus") markieren */
    console_select_press(1, ch + 1);
    console_select_move(9 * cw + 1, ch + 1);
    console_select_release();
    int active = console_has_selection();
    int hl_in = console_read_pixel(0, (uint32_t)ch) == 0xC0C0C0;             /* markiert: Hintergrund in Textfarbe */
    int hl_out = console_read_pixel((uint32_t)(11 * cw), (uint32_t)ch) == 0; /* dahinter nicht */
    keyboard_deliver(3);                                                      /* Ctrl-C kopiert */
    uint32_t len;
    const char *t = console_clipboard(&len);
    int copy1 = len == 10 && memcmp(t, "Hallo Maus", 10) == 0;
    int no_ctrl_c = keyboard_getchar() < 0 && !console_has_selection();
    int unhl = console_read_pixel(0, (uint32_t)ch) == 0;

    /* Ueber zwei Zeilen, rueckwaerts gezogen, mit Umlauten und Leerzeichen am Zeilenende */
    console_select_press(10 * cw + 1, 2 * ch + 1);
    console_select_move(6 * cw + 1, ch + 1);
    keyboard_deliver(3);
    t = console_clipboard(&len);
    int copy2 = len == 16 && memcmp(t, "Maus 123\nGr\xC3\xBC\xC3\x9F" "e", 16) == 0;

    /* Ein einfacher Klick markiert nichts; Ctrl-V fuegt die Zwischenablage als Eingabe ein */
    console_select_press(3 * cw, 3);
    console_select_release();
    int click = !console_has_selection();
    console_clipboard_set("ab\xC3\xA4", 4);
    keyboard_deliver(0x16);
    int k1 = keyboard_getchar(), k2 = keyboard_getchar(), k3 = keyboard_getchar(), k4 = keyboard_getchar(), k5 = keyboard_getchar();
    int paste = k1 == 'a' && k2 == 'b' && k3 == 0xC3 && k4 == 0xA4 && k5 < 0;

    console_clear();
    title("Markieren, Kopieren, Einfuegen");
    check("Ziehen mit der Maus markiert (Farben vertauscht)", active && hl_in && hl_out);
    check("Ctrl-C kopiert die Markierung (statt abzubrechen) und hebt sie auf", copy1 && no_ctrl_c && unhl);
    check("Markierung ueber zwei Zeilen, rueckwaerts, mit Umlauten", copy2);
    check("Einfacher Klick markiert nichts", click);
    check("Ctrl-V fuegt die Zwischenablage (UTF-8) als Eingabe ein", paste);
}

/* Liest, was im Tastaturpuffer steht (bis max-1 Bytes) */
static int drain_keys(char *out, int max)
{
    int n = 0, c;
    thread_sleep_ms(30); /* PS/2: der Interrupt muss die Bytes erst abholen */
    while ((c = keyboard_getchar()) >= 0)
        if (n < max - 1)
            out[n++] = (char)c;
    out[n] = 0;
    return n;
}

/* Eine Taste (HID-Usage) mit Umschaltern druecken und das Ergebnis vergleichen */
static int key_is(uint8_t usage, int shift, int ctrl, int altgr, int caps, const char *want)
{
    char got[16];
    int n = 0, c;
    while (keyboard_getchar() >= 0) /* keymap_key liefert sofort: keine Wartezeit noetig */
        ;
    keymap_key(usage, shift, ctrl, altgr, caps);
    while ((c = keyboard_getchar()) >= 0)
        if (n < (int)sizeof(got) - 1)
            got[n++] = (char)c;
    got[n] = 0;
    return strcmp(got, want) == 0;
}

static void test_keymap(void)
{
    title("Tastaturlayouts");
    char tmp[64];
    drain_keys(tmp, sizeof(tmp));
    const char *before = keymap_name();

    check("Unbekanntes Layout abgelehnt", keymap_set("xx") != 0 && strcmp(keymap_name(), before) == 0);

    keymap_set("us");
    check("US: y, Shift+2 = @, Shift+' = \", Strg+C = 3",
          key_is(0x1C, 0, 0, 0, 0, "y") && key_is(0x1F, 1, 0, 0, 0, "@") && key_is(0x34, 1, 0, 0, 0, "\"") &&
          key_is(0x06, 0, 1, 0, 0, "\x03"));

    keymap_set("de");
    check("DE: Y/Z vertauscht, ß, ü, ö, ä, Shift+ä = Ä",
          key_is(0x1C, 0, 0, 0, 0, "z") && key_is(0x1D, 0, 0, 0, 0, "y") && key_is(0x2D, 0, 0, 0, 0, "\xC3\x9F") &&
          key_is(0x2F, 0, 0, 0, 0, "\xC3\xBC") && key_is(0x33, 0, 0, 0, 0, "\xC3\xB6") && key_is(0x34, 1, 0, 0, 0, "\xC3\x84"));
    check("DE: Shift+2 = \", Shift+3 = §, Shift+7 = /, - . , < >",
          key_is(0x1F, 1, 0, 0, 0, "\"") && key_is(0x20, 1, 0, 0, 0, "\xC2\xA7") && key_is(0x24, 1, 0, 0, 0, "/") &&
          key_is(0x38, 0, 0, 0, 0, "-") && key_is(0x64, 0, 0, 0, 0, "<") && key_is(0x64, 1, 0, 0, 0, ">"));
    check("DE: AltGr+Q = @, AltGr+E = €, AltGr+8 = [, AltGr+< = |, Strg+Alt+Q = @",
          key_is(0x14, 0, 0, 1, 0, "@") && key_is(0x08, 0, 0, 1, 0, "\xE2\x82\xAC") && key_is(0x25, 0, 0, 1, 0, "[") &&
          key_is(0x64, 0, 0, 1, 0, "|") && key_is(0x14, 0, 1, 2, 0, "@"));
    check("DE: Feststelltaste wirkt auch auf Umlaute (ä -> Ä, Shift -> ä), nicht auf Ziffern",
          key_is(0x34, 0, 0, 0, 1, "\xC3\x84") && key_is(0x34, 1, 0, 0, 1, "\xC3\xA4") && key_is(0x1E, 0, 0, 0, 1, "1"));
    int dead1 = key_is(0x35, 0, 0, 0, 0, "") && key_is(0x04, 0, 0, 0, 0, "\xC3\xA2");         /* ^ a = â */
    int dead2 = key_is(0x2E, 0, 0, 0, 0, "") && key_is(0x08, 1, 0, 0, 0, "\xC3\x89");         /* ´ E = É */
    int dead3 = key_is(0x2E, 1, 0, 0, 0, "") && key_is(0x2C, 0, 0, 0, 0, "`");                /* ` Leertaste = ` */
    int dead4 = key_is(0x35, 0, 0, 0, 0, "") && key_is(0x1B, 0, 0, 0, 0, "^x");               /* ^ x = ^x */
    check("DE: Tottasten (^a = â, ´E = É, `+Leertaste = `, ^x = ^x)", dead1 && dead2 && dead3 && dead4);

    /* PS/2-Weg: Scancodes ueber den 8042 einspeisen */
    drain_keys(tmp, sizeof(tmp));
    keyboard_inject_scancode(0x15); keyboard_inject_scancode(0x95); /* Y-Position */
    keyboard_inject_scancode(0x1A); keyboard_inject_scancode(0x9A); /* [-Position = ü */
    keyboard_inject_scancode(0xE0); keyboard_inject_scancode(0x38); /* AltGr gedrueckt */
    keyboard_inject_scancode(0x12); keyboard_inject_scancode(0x92); /* E -> € */
    keyboard_inject_scancode(0xE0); keyboard_inject_scancode(0xB8); /* AltGr los */
    keyboard_inject_scancode(0x56); keyboard_inject_scancode(0xD6); /* ISO-Taste links neben Y = < */
    drain_keys(tmp, sizeof(tmp));
    check("DE ueber PS/2: z, ü, AltGr+E = €, <", strcmp(tmp, "z\xC3\xBC\xE2\x82\xAC<") == 0);

    keymap_set("uk");
    check("UK: Shift+2 = \", Shift+3 = £, Shift+' = @, # und ~, \\ neben Z",
          key_is(0x1F, 1, 0, 0, 0, "\"") && key_is(0x20, 1, 0, 0, 0, "\xC2\xA3") && key_is(0x34, 1, 0, 0, 0, "@") &&
          key_is(0x32, 0, 0, 0, 0, "#") && key_is(0x32, 1, 0, 0, 0, "~") && key_is(0x64, 0, 0, 0, 0, "\\"));

    keymap_set(before);
    drain_keys(tmp, sizeof(tmp));
}

/* Setzt den Mauszeiger auf Pixel (x, y): erst in die Ecke, dann in kleinen Schritten (ohne Beschleunigung) */
static void mouse_goto(int x, int y)
{
    for (int i = 0; i < 400; i++)
        mouse_report(0, -127, -127, 0);
    int s = (int)console_scale();
    for (int i = 0; i < x / s; i++)
        mouse_report(0, 1, 0, 0);
    for (int i = 0; i < y / s; i++)
        mouse_report(0, 0, 1, 0);
}

static void click(void)
{
    mouse_report(1, 0, 0, 0);
    mouse_report(0, 0, 0, 0);
}

static void test_multiclick(void)
{
    title("Doppel- und Dreifachklick");
    char tmp[64];
    drain_keys(tmp, sizeof(tmp));
    int cw = 8 * (int)console_scale(), ch = 16 * (int)console_scale();
    console_clear();
    for (const char *c = "\nabc /mnt/usb0/datei.txt xyz   \n"; *c; c++)
        console_putc(*c);
    mouse_click_reset(); /* fruehere Klicks zaehlen nicht mit */
    mouse_goto(10 * cw + 2, ch + 2);

    uint32_t len;
    click();
    click();                 /* Doppelklick: Wort (Pfad bis zum Leerzeichen) */
    int word_sel = console_has_selection();
    keyboard_deliver(3);
    const char *t = console_clipboard(&len);
    int word = word_sel && len == 19 && memcmp(t, "/mnt/usb0/datei.txt", 19) == 0;

    mouse_click_reset();
    click();
    click();
    click();                 /* Dreifachklick: ganze Zeile (ohne Leerzeichen am Ende) */
    keyboard_deliver(3);
    t = console_clipboard(&len);
    int line = len == 27 && memcmp(t, "abc /mnt/usb0/datei.txt xyz", 27) == 0;

    thread_sleep_ms(450);
    click();                 /* nach einer Pause: wieder ein einfacher Klick, markiert nichts */
    int single = !console_has_selection();

    mouse_goto(40 * cw + 2, ch + 2); /* Doppelklick auf leeren Bereich: nichts */
    mouse_click_reset();
    click();
    click();
    int empty = !console_has_selection();
    drain_keys(tmp, sizeof(tmp));

    console_clear();
    title("Doppel- und Dreifachklick");
    check("Doppelklick markiert das Wort (ganzer Pfad)", word);
    check("Dreifachklick markiert die ganze Zeile", line);
    check("Nach einer Pause zaehlt ein Klick wieder als Einfachklick", single);
    check("Doppelklick auf leeren Bereich markiert nichts", empty);
}

static void test_video(BootInfo *info)
{
    title("Grafik");
    kprintf("  Bildschirm %ux%u, %u Grafikmodi, Schrift x%u, Textfeld %ux%u\n", info->fb.width, info->fb.height,
            video_mode_count(), console_scale(), console_cols(), console_rows());
    VideoInfo vi;
    int have_cur = 0, sane = 1;
    for (unsigned i = 0; video_mode_info(i, &vi) == 0; i++) {
        sane &= vi.width >= 320 && vi.height >= 200;
        if (vi.current)
            have_cur = vi.width == info->fb.width && vi.height == info->fb.height;
    }
    check("Bootloader meldet Grafikmodi (oder die Firmware kennt nur einen)", video_mode_count() > 0 && sane);
    check("Der aktuelle Modus stimmt mit dem Framebuffer ueberein", have_cur);
    check("Textfeld passt in den Bildschirm", console_cols() * 8 * console_scale() <= info->fb.width &&
                                              console_rows() * 16 * console_scale() <= info->fb.height);
}

/* Alle Selbsttests (nur mit "selftest" in der Kommandozeile). Sie ueberschreiben Bildschirm und Testdateien. */
/* Startet den Editor mit einer Datei; die Tasten liegen vorher schon im Tastaturpuffer */
static int editor_run(const char *path, int *pid_out)
{
    char cmd[96];
    ksnprintf(cmd, sizeof(cmd), "edit %s", path);
    int pid = process_spawn("/bin/edit", cmd, 0);
    if (pid_out)
        *pid_out = pid;
    return pid;
}

static int editor_wait(int pid)
{
    int code = 0, faulted = 0;
    return pid > 0 && process_wait(pid, 0, &code, &faulted, 8000) == 0 && code == 0 && !faulted;
}

static int write_text(const char *path, const char *text)
{
    FsFile f;
    if (fs_open(path, FAT_O_WRONLY | FAT_O_CREAT | FAT_O_TRUNC, &f) != 0)
        return -1;
    int64_t n = (int64_t)strlen(text);
    int ok = fs_write(&f, text, (uint64_t)n) == n;
    fs_close(&f);
    return ok ? 0 : -1;
}

static void test_editor(void)
{
    title("Texteditor");
    char data[256], tmp[64];
    if (fs_disk_volume() < 0) {
        check("Editor-Tests brauchen /disk", 0);
        return;
    }
    drain_keys(tmp, sizeof(tmp));
    fs_unlink("/disk/ED1.TXT");
    fs_unlink("/disk/ED2.TXT");
    fs_unlink("/disk/ED3.TXT");

    /* 1. Neue Datei: tippen, Pfeil hoch, Ende, Suchen (ohne Beachtung der Schreibweise), speichern, beenden */
    feed("Hallo\nWelt");
    keyboard_deliver(KEY_UP);
    keyboard_deliver(KEY_END);
    feed("!");
    keyboard_deliver(6);             /* Strg+F */
    feed("welt\n");
    keyboard_deliver(KEY_LEFT);      /* Markierung aufheben, Cursor an den Anfang des Funds */
    feed("x");
    keyboard_deliver(19);            /* Strg+S */
    keyboard_deliver(17);            /* Strg+Q */
    int pid;
    int ok1 = editor_wait(editor_run("/disk/ED1.TXT", &pid));
    check("Tippen, Pfeiltasten, Suchen, Speichern", ok1 && slurp("/disk/ED1.TXT", data, sizeof(data)) > 0 &&
                                                    strcmp(data, "Hallo!\nxWelt\n") == 0);

    /* 2. Aenderung ohne Speichern: erstes Strg+Q warnt, das zweite beendet ohne zu speichern */
    feed("zzz");
    keyboard_deliver(17);
    keyboard_deliver(17);
    int ok2 = editor_wait(editor_run("/disk/ED1.TXT", 0));
    check("Beenden mit ungespeicherten Aenderungen fragt nach (Datei bleibt unveraendert)",
          ok2 && slurp("/disk/ED1.TXT", data, sizeof(data)) > 0 && strcmp(data, "Hallo!\nxWelt\n") == 0);

    /* 3. Umlaute und Backspace (ein ganzes UTF-8-Zeichen) */
    feed("Gr\xC3\xBC\xC3\x9F\xE2\x82\xAC\b");
    keyboard_deliver(19);
    keyboard_deliver(17);
    int ok3 = editor_wait(editor_run("/disk/ED2.TXT", 0));
    check("Umlaute tippen, Backspace loescht ein ganzes Zeichen", ok3 && slurp("/disk/ED2.TXT", data, sizeof(data)) > 0 &&
                                                              strcmp(data, "Gr\xC3\xBC\xC3\x9F\n") == 0);

    /* 4. Strg+K schneidet die Zeile aus (Zwischenablage), Strg+V fuegt sie woanders ein */
    feed("eins\nzwei");
    keyboard_deliver(KEY_UP);
    keyboard_deliver(11);            /* Strg+K */
    keyboard_deliver(KEY_END);
    feed("\n");
    int pid4 = editor_run("/disk/ED3.TXT", 0);
    thread_sleep_ms(1500);           /* erst muss der Editor Strg+K verarbeitet haben */
    uint32_t clen;
    const char *clip = console_clipboard(&clen);
    int clip_ok = clen == 5 && memcmp(clip, "eins\n", 5) == 0;
    keyboard_deliver(0x16);          /* Strg+V */
    keyboard_deliver(19);
    keyboard_deliver(17);
    int ok4 = editor_wait(pid4);
    check("Strg+K schneidet eine Zeile aus, Strg+V fuegt sie ein", ok4 && clip_ok &&
          slurp("/disk/ED3.TXT", data, sizeof(data)) > 0 && strcmp(data, "zwei\neins\n\n") == 0);

    /* 5. Windows-Zeilenenden bleiben erhalten */
    write_text("/disk/ED3.TXT", "a\r\nb\r\n");
    keyboard_deliver(KEY_END);
    feed("x");
    keyboard_deliver(19);
    keyboard_deliver(17);
    int ok5 = editor_wait(editor_run("/disk/ED3.TXT", 0));
    check("Windows-Zeilenenden (CRLF) bleiben erhalten", ok5 && slurp("/disk/ED3.TXT", data, sizeof(data)) > 0 &&
                                                         strcmp(data, "ax\r\nb\r\n") == 0);

    /* 6. Maus: Klick setzt den Cursor, rechte Taste fuegt ein; die Konsole markiert/blaettert waehrenddessen nicht */
    write_text("/disk/ED3.TXT", "abc\ndef\n");
    int pid6 = editor_run("/disk/ED3.TXT", 0);
    thread_sleep_ms(1500);           /* Editor laeuft und hat die Maus uebernommen */
    int cw = 8 * (int)console_scale(), ch = 16 * (int)console_scale();
    mouse_goto((4 + 2) * cw + 2, 2 * ch + 2); /* Textzeile 2 (Bildschirmzeile 2), Spalte 2 hinter der Zeilennummer */
    mouse_report(1, 0, 0, 0);
    mouse_report(0, 0, 0, 0);
    int no_console_sel = !console_has_selection();
    mouse_report(0, 0, 0, 3);        /* Rad: darf nicht im Konsolenverlauf blaettern */
    thread_sleep_ms(200);
    int no_console_scroll = console_view_offset() == 0;
    feed("Y");
    thread_sleep_ms(300);
    console_clipboard_set("Q", 1);
    mouse_report(2, 0, 0, 0);        /* rechte Taste: einfuegen */
    mouse_report(0, 0, 0, 0);
    thread_sleep_ms(300);
    keyboard_deliver(19);
    keyboard_deliver(17);
    int ok6 = editor_wait(pid6);
    check("Maus: Klick setzt den Cursor, rechte Taste fuegt ein", ok6 && slurp("/disk/ED3.TXT", data, sizeof(data)) > 0 &&
                                                                  strcmp(data, "abc\ndeYQf\n") == 0);
    check("Waehrend der Editor laeuft, markiert/blaettert die Konsole nicht", no_console_sel && no_console_scroll);
    console_select_press(1, 1); /* nach dem Beenden gehoert die Maus wieder der Konsole */
    console_select_move(3 * cw, 1);
    check("Nach dem Beenden gehoert die Maus wieder der Konsole", console_has_selection());
    console_select_clear();

    fs_unlink("/disk/ED1.TXT");
    fs_unlink("/disk/ED2.TXT");
    fs_unlink("/disk/ED3.TXT");
    drain_keys(tmp, sizeof(tmp));
    console_clear();
}

static int file_is(const char *path, const char *want)
{
    static char buf[2048];
    int64_t n = slurp(path, buf, sizeof(buf));
    return n >= 0 && strcmp(buf, want) == 0;
}

static int file_has(const char *path, const char *part)
{
    static char buf[2048];
    return slurp(path, buf, sizeof(buf)) > 0 && strstr_(buf, part);
}

static int run_sh(const char *cmdline)
{
    int pid = process_spawn("/bin/sh", cmdline, 0);
    int code = 0, faulted = 0;
    return pid > 0 && process_wait(pid, 0, &code, &faulted, 20000) == 0 && !faulted ? code : -1;
}

static void test_script(void)
{
    title("Shell-Skripte");
    if (fs_disk_volume() < 0) {
        check("Skript-Tests brauchen /disk", 0);
        return;
    }
    int rc = run_sh("sh /etc/tests/shell.sh eins zwei drei");
    const char *want =
        "gross\nfuenf\ni=3\nw=a\nw=b\nw=c\nn=2\nsumme=20\nrechnen: 2\nargs=3\nfak5=120\nloop=1\nloop=3\nret=7\n"
        "status=127\nzeilen=2 2 10\nvorgabe=std\nlaenge=1\np=eins 3 eins zwei drei\nnach_shift=zwei\nglob=SB.TXT\n"
        "quote='5' $x\nnegiert\nread=b\nende\n";
    static char got[2048];
    slurp("/disk/SR.TXT", got, sizeof(got));
    int same = strcmp(got, want) == 0;
    if (!same) { /* bei Abweichung zeigen, was herauskam */
        kprintf("  erwartet:\n%s  erhalten:\n%s", want, got);
    }
    check("if/elif/else, while, until, for, Funktionen, Rekursion, $(...), $((...))", rc == 0 && same);
    check("Fehlerausgabe umleiten (2>, 2>>, >&2)", file_has("/disk/SE.TXT", "fehler") &&
                                                 file_has("/disk/SE.TXT", "gibtsnicht: Befehl nicht gefunden"));
    fs_unlink("/disk/SR.TXT");
    fs_unlink("/disk/SE.TXT");
    fs_unlink("/disk/SB.TXT");

    /* Syntaxfehler werden mit Zeile gemeldet, unvollstaendige Skripte erkannt */
    write_text("/disk/SF.SH", "echo a\nif true; then\necho b\n");
    int rc_incomplete = run_sh("sh /disk/SF.SH");
    write_text("/disk/SF.SH", "echo a\nfi\n");
    int rc_syntax = run_sh("sh /disk/SF.SH");
    fs_unlink("/disk/SF.SH");
    check("Unvollstaendiges Skript und Syntaxfehler: Exit-Code 2", rc_incomplete == 2 && rc_syntax == 2);
}

static void test_tools(void)
{
    title("Werkzeuge");
    if (fs_disk_volume() < 0) {
        check("Werkzeug-Tests brauchen /disk", 0);
        return;
    }
    int rc = run_sh("sh /etc/tests/tools.sh");
    check("tools.sh laeuft durch", rc == 0);
    check("sort", file_is("/disk/T1.TXT", "a\na\nb\nc\n"));
    check("sort -r | uniq", file_is("/disk/T2.TXT", "c\nb\na\n"));
    check("uniq -c", file_is("/disk/T3.TXT", "      2 a\n      1 b\n      1 c\n"));
    check("sort -n", file_is("/disk/T4.TXT", "9\n10\n100\n"));
    check("diff (geaendert, hinzugefuegt, Exit-Code 1)", file_is("/disk/T5.TXT", "2c2\n< zwei\n---\n> ZWEI\n3a4\n> vier\nrc=1\n"));
    check("diff gleicher Dateien: keine Ausgabe, Exit-Code 0", file_is("/disk/T6.TXT", "rc=0\n"));
    check("find -name", file_is("/disk/T7.TXT", "FT/A.TXT\nFT/SUB/B.TXT\n"));
    check("find -type d", file_is("/disk/T8.TXT", "FT\nFT/SUB\n"));
    check("du -s -b", file_is("/disk/T9.TXT", "5        FT\n"));
    check("hexdump", file_has("/disk/TA.TXT", "00000000  41 42 43") && file_has("/disk/TA.TXT", "|ABC|") &&
                     file_has("/disk/TA.TXT", "00000003\n"));
    check("df zeigt /disk", file_has("/disk/TB.TXT", "/disk") && file_has("/disk/TB.TXT", "FAT32"));
    check("tree", file_has("/disk/TC.TXT", "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ") && file_has("/disk/TC.TXT", "1 Verzeichnisse, 2 Dateien"));
    check("cal (Februar 2026 beginnt am Sonntag, KW 5)", file_has("/disk/TD.TXT", "Februar 2026") &&
                                                       file_has("/disk/TD.TXT", " 5                     1 \n"));
    check("uptime", file_has("/disk/TE.TXT", "laeuft seit"));

    static const char *const files[] = {"U1", "T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "T9", "TA", "TB", "TC",
                                        "TD", "TE", "D1", "D2", 0};
    char path[32];
    for (int i = 0; files[i]; i++) {
        ksnprintf(path, sizeof(path), "/disk/%s.TXT", files[i]);
        fs_unlink(path);
    }
    fs_unlink("/disk/FT/SUB/B.TXT");
    fs_unlink("/disk/FT/SUB");
    fs_unlink("/disk/FT/A.TXT");
    fs_unlink("/disk/FT");
}

/* Netzwerk in QEMU ("-netdev user"): DHCP-Server und Gateway 10.0.2.2, DNS 10.0.2.3, Gast 10.0.2.15 */
static int net_wait_dhcp(NetInfo *ni, uint64_t timeout_ms)
{
    uint64_t t0 = time_ms();
    while (time_ms() - t0 < timeout_ms) {
        if (net_info(0, ni) == 0 && ni->dhcp == 2)
            return 1;
        thread_sleep_ms(50);
    }
    return 0;
}

static void test_net(void)
{
    title("Netzwerk");
    NetInfo ni;
    if (net_info(0, &ni) != 0) {
        kprintf("  (keine Netzwerkkarte - uebersprungen; QEMU mit NET=e1000 oder NET=e1000e starten)\n");
        return;
    }
    check("Netzwerkkarte gefunden, MAC-Adresse gelesen", (ni.mac[0] | ni.mac[1] | ni.mac[2] | ni.mac[3] | ni.mac[4] | ni.mac[5]) != 0);
    int bound = net_wait_dhcp(&ni, 10000);
    check("Verbindung steht", ni.link);
    static const uint8_t want_ip[4] = {10, 0, 2, 15}, want_mask[4] = {255, 255, 255, 0};
    static const uint8_t gw[4] = {10, 0, 2, 2}, dns[4] = {10, 0, 2, 3};
    check("DHCP: 10.0.2.15/24, Gateway 10.0.2.2, DNS 10.0.2.3", bound && memcmp(ni.ip, want_ip, 4) == 0 &&
          memcmp(ni.mask, want_mask, 4) == 0 && memcmp(ni.gateway, gw, 4) == 0 && memcmp(ni.dns, dns, 4) == 0);

    int64_t r = net_ping(gw, 1, 56, 2000);
    check("ping 10.0.2.2 antwortet", r >= 0);
    if (r >= 0)
        kprintf("  (Antwortzeit %lu us)\n", (unsigned long)(r & 0xFFFFFFFFFFLL));
    ArpInfo ai;
    int arp_gw = 0;
    for (unsigned i = 0; net_arp_info(i, &ai) == 0; i++)
        if (memcmp(ai.ip, gw, 4) == 0)
            arp_gw = 1;
    check("ARP-Tabelle kennt das Gateway", arp_gw);
    static const uint8_t nobody[4] = {10, 0, 2, 99};
    check("Nicht vorhandener Rechner: 'nicht erreichbar' (ARP ohne Antwort)", net_ping(nobody, 1, 56, 600) == ERR_HOSTUNREACH);
    static const uint8_t far[4] = {192, 0, 2, 1};
    uint8_t cfg[16] = {10, 0, 2, 20, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    net_set_static(0, cfg); /* ohne Gateway: fremde Netze unerreichbar */
    check("Feste Adresse ohne Gateway: fremdes Netz unerreichbar, lokales Netz geht",
          net_ping(far, 1, 56, 500) == ERR_NETUNREACH && net_ping(gw, 2, 56, 2000) >= 0);
    net_start_dhcp(0);
    check("DHCP erneut: wieder 10.0.2.15", net_wait_dhcp(&ni, 10000) && memcmp(ni.ip, want_ip, 4) == 0);

    if (fs_disk_volume() < 0)
        return;
    write_text("/disk/NET.SH", "lspci > /disk/N1.TXT\nifconfig -a > /disk/N2.TXT\nping -c 2 -s 1000 10.0.2.2 > /disk/N3.TXT\n");
    int rc = run_sh("sh /disk/NET.SH");
    check("lspci zeigt die Netzwerkkarte mit Treiber", rc == 0 && file_has("/disk/N1.TXT", "Netzwerk (Ethernet)") &&
                                                       file_has("/disk/N1.TXT", "[e1000]"));
    check("ifconfig zeigt Verbindung, Adresse, DHCP und ARP-Tabelle",
          file_has("/disk/N2.TXT", "1000 Mbit/s") && file_has("/disk/N2.TXT", "10.0.2.15/24  Gateway 10.0.2.2  DNS 10.0.2.3") &&
          file_has("/disk/N2.TXT", "DHCP:       von 10.0.2.2") && file_has("/disk/N2.TXT", "  10.0.2.2 "));
    check("ping-Programm (1000 Byte): 2 Antworten", file_has("/disk/N3.TXT", "2 gesendet, 2 empfangen, 0 % Verlust"));
    fs_unlink("/disk/NET.SH");
    fs_unlink("/disk/N1.TXT");
    fs_unlink("/disk/N2.TXT");
    fs_unlink("/disk/N3.TXT");

    /* UDP-Sockets (ueber die eigene Adresse, ohne Netz) */
    int err = 0;
    UdpSock *a = udp_open(7000, &err), *b = udp_open(0, &err);
    UdpSock *dup = udp_open(7000, &err);
    check("UDP: Socket auf Port 7000, zweiter auf demselben Port abgelehnt", a && b && !dup && err == ERR_EXIST);
    static const uint8_t lo[4] = {127, 0, 0, 1};
    uint8_t from[4], rb[64];
    uint16_t fport = 0;
    int sent = a && b ? udp_sendto(b, lo, 7000, "hallo", 5) : -1;
    int got = a ? udp_recvfrom(a, rb, sizeof(rb), from, &fport, 100) : -1;
    check("UDP: Datagramm an 127.0.0.1 kommt mit Absender-Port an", sent == 5 && got == 5 && memcmp(rb, "hallo", 5) == 0 &&
                                                                    b && fport == udp_local_port(b));
    check("UDP: leere Warteschlange -> ERR_AGAIN bzw. ERR_TIMEDOUT",
          a && udp_recvfrom(a, rb, sizeof(rb), 0, 0, 0) == ERR_AGAIN && udp_recvfrom(a, rb, sizeof(rb), 0, 0, 50) == ERR_TIMEDOUT);
    udp_close(b);

    /* Programm "udp send" gegen den Kernel-Socket als Echo-Server */
    write_text("/disk/NET.SH", "udp send 127.0.0.1 7000 hallo welt > /disk/N4.TXT\nnslookup localhost > /disk/N5.TXT\n");
    int pid = process_spawn("/bin/sh", "sh /disk/NET.SH", 0);
    got = a ? udp_recvfrom(a, rb, sizeof(rb) - 1, from, &fport, 5000) : -1;
    if (got > 0) {
        uint8_t echo[80];
        memcpy(echo, "echo: ", 6);
        memcpy(echo + 6, rb, (size_t)got);
        udp_sendto(a, from, fport, echo, (uint32_t)got + 6);
    }
    int code = -1, faulted = 0;
    if (pid > 0)
        process_wait(pid, 0, &code, &faulted, 20000);
    check("udp-Programm: senden, Antwort empfangen", got == 11 && code == 0 && file_has("/disk/N4.TXT", "11 Bytes an 127.0.0.1:7000") &&
                                                    file_has("/disk/N4.TXT", "echo: hallo welt"));
    check("nslookup localhost", file_has("/disk/N5.TXT", "Adresse: 127.0.0.1"));
    udp_close(a);
    fs_unlink("/disk/NET.SH");
    fs_unlink("/disk/N4.TXT");
    fs_unlink("/disk/N5.TXT");

    /* DNS ohne Netz */
    uint8_t ips[8][4];
    check("DNS: IP-Adresse und localhost ohne Anfrage", net_resolve("10.1.2.3", ips, 8) == 1 && ips[0][0] == 10 && ips[0][3] == 3 &&
                                                      net_resolve("LocalHost", ips, 8) == 1 && ips[0][0] == 127);
    check("DNS: ungueltige Namen abgelehnt", net_resolve("a..b", ips, 8) == ERR_INVAL && net_resolve("", ips, 8) == ERR_INVAL);

    /* Echtes DNS und NTP ueber QEMU (braucht Internet auf dem Host) */
    uint64_t t0 = time_ms();
    int n = net_resolve("google.de", ips, 8);
    if (n == ERR_TIMEDOUT || n == ERR_NETUNREACH || n == ERR_HOSTUNREACH) {
        kprintf("  (kein Internet: DNS/NTP ueber das Internet uebersprungen)\n");
        return;
    }
    uint64_t t1 = time_ms();
    int n2 = net_resolve("google.de", ips, 8);
    uint64_t t2 = time_ms();
    kprintf("  (google.de -> %u.%u.%u.%u, %lu ms; aus dem Zwischenspeicher %lu ms)\n", ips[0][0], ips[0][1], ips[0][2],
            ips[0][3], (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
    check("DNS: google.de aufgeloest, zweites Mal aus dem Zwischenspeicher", n > 0 && n2 == n && t2 - t1 < 5);
    check("DNS: unbekannter Name -> ERR_NOENT", net_resolve("gibt-es-nicht.invalid", ips, 8) == ERR_NOENT);
    NtpResult nr;
    int e = net_ntp(0, 0, &nr);
    if (e == 0)
        kprintf("  (NTP %s, Ebene %u, Laufzeit %u ms, Uhr weicht %ld ms ab)\n", nr.server, nr.stratum, nr.rtt_ms,
                (long)nr.offset_ms);
    check("NTP: Zeit von pool.ntp.org plausibel (nach 2026, Ebene 1-15)", e == 0 && nr.utc > 1767225600ULL &&
                                                                         nr.stratum >= 1 && nr.stratum <= 15);
}

/* Nur mit TESTS=netpeer und dem Python-Gegenrechner (scratchpad/peer.py) als DNS-, NTP- und UDP-Echo-Server auf 10.0.5.1 */
static void test_netpeer(void)
{
    title("Netzwerk gegen Test-Gegenstelle");
    NetInfo ni;
    uint64_t t0 = time_ms();
    while (time_ms() - t0 < 5000 && (net_info(0, &ni) != 0 || !ni.link))
        thread_sleep_ms(20);
    uint8_t ips[8][4];
    int n = net_resolve("alias.minikernel", ips, 8);
    check("DNS: CNAME-Kette, zwei A-Records", n == 2 && ips[0][0] == 10 && ips[0][1] == 1 && ips[0][2] == 2 && ips[0][3] == 3 &&
                                              ips[1][3] == 4);
    check("DNS: NXDOMAIN -> ERR_NOENT", net_resolve("nx.minikernel", ips, 8) == ERR_NOENT);
    check("DNS: Antwort mit Fehlercode -> ERR_IO", net_resolve("servfail.minikernel", ips, 8) == ERR_IO);
    check("DNS: Server antwortet nicht -> ERR_TIMEDOUT", net_resolve("stumm.minikernel", ips, 8) == ERR_TIMEDOUT);
    NtpResult nr;
    int e = net_ntp("10.0.5.1", 0, &nr);
    DateTime dt;
    unix_to_datetime(nr.local, &dt);
    /* Gegenstelle liefert 2030-06-15 12:00:00 UTC: Sommerzeit, also 14:00 MESZ */
    check("NTP: Zeit, Sommerzeit (MESZ = UTC+2), Ebene", e == 0 && nr.utc == 1907755200ULL && nr.tz_offset_s == 7200 &&
                                                        dt.hour == 14 && strcmp(nr.tz, "MESZ") == 0 && nr.stratum == 2);
    e = net_ntp("winter.minikernel", 0, &nr); /* 2030-01-15 12:00:00 UTC */
    unix_to_datetime(nr.local, &dt);
    check("NTP: Winterzeit (MEZ = UTC+1), Server per Name", e == 0 && nr.utc == 1894708800ULL && dt.hour == 13 &&
                                                           strcmp(nr.tz, "MEZ") == 0);
    check("NTP: ungueltige Antwort (Ebene 0) -> ERR_IO", net_ntp("kod.minikernel", 0, &nr) == ERR_IO);
    int err;
    UdpSock *s = udp_open(0, &err);
    static const uint8_t peer[4] = {10, 0, 5, 1};
    uint8_t buf[1500], big[1400], from[4];
    uint16_t fport;
    for (int i = 0; i < 1400; i++)
        big[i] = (uint8_t)(i * 13);
    int ok = s && udp_sendto(s, peer, 7, big, 1400) == 1400 && udp_recvfrom(s, buf, sizeof(buf), from, &fport, 2000) == 1400 &&
             memcmp(buf, big, 1400) == 0 && fport == 7;
    check("UDP: 1400 Byte an den Echo-Server und zurueck", ok);
    udp_close(s);
}

static void test_graphics(void)
{
    title("Grafik fuer Programme");
    char tmp[64];
    drain_keys(tmp, sizeof(tmp));

    /* Kernel-Ebene: Konsole pausiert, Blit landet im Framebuffer, Freigeben stellt die Konsole wieder her */
    console_clear();
    for (const char *c = "Konsole"; *c; c++)
        console_putc(*c);
    uint32_t before = console_debug_fb_pixel(8 * 7 * console_scale() + 3, 8);
    int acq = console_gfx_acquire(4242) == 0;
    int busy = console_gfx_acquire(4343) == -2;
    for (const char *c = "XXXXXXXXXX"; *c; c++) /* Ausgabe waehrend des Grafikmodus: nur ins Abbild */
        console_putc(*c);
    int hidden = console_debug_fb_pixel(8 * 7 * console_scale() + 3, 8) == before;
    static uint32_t red[16];
    for (int i = 0; i < 16; i++)
        red[i] = 0xFF0000;
    console_gfx_blit(red, 4, 100, 100, 4, 4);
    int blit = console_debug_fb_pixel(101, 101) == 0xFF0000;
    console_gfx_release(4242);
    int restored = console_debug_fb_pixel(101, 101) == console_read_pixel(101, 101) && !console_gfx_owner(4242);
    console_clear();
    title("Grafik fuer Programme");
    check("Bildschirm uebernehmen (ein zweites Programm wird abgewiesen)", acq && busy);
    check("Konsolenausgabe waehrend des Grafikmodus erscheint nicht", hidden);
    check("Bild kopieren (Blit) in den Framebuffer", blit);
    check("Freigeben stellt die Konsole wieder her", restored);

    if (fs_disk_volume() < 0)
        return;
    /* paint: Bild speichern (Strg+S) und beenden (Esc) */
    fs_unlink("/disk/GT.BMP");
    int pid = process_spawn("/bin/paint", "paint /disk/GT.BMP", 0);
    thread_sleep_ms(1500);
    int during = pid > 0 && !console_gfx_owner(0);
    keyboard_deliver(19);
    thread_sleep_ms(500);
    keyboard_deliver(0x1B);
    int code = 0, faulted = 0;
    int ended = pid > 0 && process_wait(pid, 0, &code, &faulted, 8000) == 0 && code == 0 && !faulted;
    FsStat st;
    uint64_t want = 54 + (((uint64_t)console_width_px() * 3 + 3) & ~3ULL) * (console_height_px() - 52 - 18);
    check("paint: speichert ein BMP und beendet sich", during && ended && fs_stat("/disk/GT.BMP", &st) == 0 && st.size == want);

    /* view zeigt das Bild und endet mit Esc */
    pid = process_spawn("/bin/view", "view /disk/GT.BMP", 0);
    thread_sleep_ms(1500);
    keyboard_deliver(0x1B);
    check("view: Bild anzeigen und beenden", pid > 0 && process_wait(pid, 0, &code, &faulted, 8000) == 0 && code == 0 && !faulted);
    fs_unlink("/disk/GT.BMP");

    /* desktop: im Terminal-Fenster laeuft eine Shell */
    fs_unlink("/disk/DT.TXT");
    pid = process_spawn("/bin/desktop", "desktop", 0);
    thread_sleep_ms(2500);
    feed("echo hallo desktop > /disk/DT.TXT\n");
    thread_sleep_ms(2500);
    static char data[64];
    int shell_ok = slurp("/disk/DT.TXT", data, sizeof(data)) > 0 && strcmp(data, "hallo desktop\n") == 0;
    process_kill_pid((uint32_t)pid);
    int killed = pid > 0 && process_wait(pid, 0, &code, &faulted, 8000) == 0;
    thread_sleep_ms(300);
    check("desktop: Terminal-Fenster fuehrt Befehle aus", shell_ok);
    check("desktop beendet: Konsole ist wieder da", killed && console_debug_fb_pixel(0, 0) == console_read_pixel(0, 0));
    fs_unlink("/disk/DT.TXT");
    drain_keys(tmp, sizeof(tmp));
}

/* Selbsttests: "selftest" fuehrt alle aus, "selftest=disk,user" nur die genannten Gruppen (Namen wie in der Liste
 * unten, z.B. console unicode mouse clipboard keymap disk usb foreign user). Hinter jeder Gruppe steht ihre Dauer. */
static const char *test_filter;

static int test_selected(const char *name)
{
    if (!test_filter || !test_filter[0])
        return 1;
    size_t n = strlen(name);
    for (const char *p = test_filter; *p;) {
        const char *e = p;
        while (*e && *e != ',')
            e++;
        if ((size_t)(e - p) == n && memcmp(p, name, n) == 0)
            return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

#define RUN(name, call)                                                                     \
    do {                                                                                    \
        if (test_selected(name)) {                                                          \
            uint64_t t0_ = time_ms();                                                       \
            call;                                                                           \
            kprintf("  (%s: %lu ms)\n", name, (unsigned long)(time_ms() - t0_));          \
        }                                                                                   \
    } while (0)

static void run_selftests(BootInfo *info)
{
    /* Die Tests tippen ueber US-Scancodes: waehrenddessen US-Layout, danach das gewaehlte wieder */
    char layout[KEYMAP_NAME_MAX + 1];
    const char *ln = keymap_name();
    memcpy(layout, ln, strlen(ln) + 1);
    keymap_set("us");
    test_filter = cmdline_get("selftest");
    uint64_t t_all = time_ms();
    RUN("console", test_console());
    RUN("unicode", test_unicode());
    RUN("scrollback", test_scrollback());
    RUN("mouse", test_mouse());
    RUN("clipboard", test_clipboard());
    RUN("keymap", test_keymap());
    RUN("multiclick", test_multiclick());
    RUN("video", test_video(info));
    RUN("rtc", test_rtc());
    RUN("kprintf", test_kprintf());
    RUN("paging", test_paging());
    RUN("pmm", test_pmm());
    RUN("heap", test_heap());
    RUN("interrupts", test_interrupts());
    RUN("console_speed", test_console_speed());
    RUN("sched", test_sched());
    RUN("vfs", test_vfs(info));
    RUN("disk", test_disk());
    RUN("usb", test_usb());
    RUN("foreign", test_foreign());
    RUN("user", test_user(info));
    RUN("editor", test_editor());
    RUN("script", test_script());
    RUN("tools", test_tools());
    RUN("graphics", test_graphics());
    RUN("net", test_net());
    if (test_filter && strstr_(test_filter, "netpeer")) /* nur ausdruecklich (braucht die Test-Gegenstelle) */
        RUN("netpeer", test_netpeer());
    kprintf("\nSelbsttests: %lu ms\n", (unsigned long)(time_ms() - t_all));
    keymap_set(layout);
}

/* Aufgerufen von entry.S auf dem eigenen Kernel-Stack. */
void kmain(BootInfo *info)
{
    serial_init();
    console_init(&info->fb);
    kprintf("Kernel gestartet (Framebuffer %ux%u @ %#lx)\n", info->fb.width, info->fb.height, (unsigned long)info->fb.base);

    gdt_init();
    kprintf("GDT + TSS geladen\n");

    idt_init();
    kprintf("IDT geladen\n");

    paging_init(info);
    __asm__ __volatile__("int3"); /* Exception-Rueckkehr testen */
    kprintf("int3 zurueckgekehrt\n");

    pmm_init(info);
    paging_harden();
    heap_init();
    kstack_init();
    console_enable_shadow(); /* ab hier scrollt die Konsole im RAM statt im (langsamen) Framebuffer */

    cmdline_init(info->cmdline);
    if (info->cmdline[0])
        kprintf("Kommandozeile: %s\n", info->cmdline);
    video_init(info);
    const char *kbd = cmdline_get("kbd"); /* Tastaturlayout: kbd=de, kbd=uk (Standard us) */
    if (kbd && keymap_set(kbd) != 0)
        kprintf("Unbekanntes Tastaturlayout '%s' (bekannt: %s)\n", kbd, keymap_list());
    const char *scale_opt = cmdline_get("scale"); /* Schriftvergroesserung: scale=1..4 (Standard: 2 ab etwa 2400 Pixel Breite) */
    if (scale_opt && scale_opt[0] >= '1' && scale_opt[0] <= '4')
        console_set_scale((uint32_t)(scale_opt[0] - '0'));
    kprintf("Bildschirm: %ux%u, %u Grafikmodi verfuegbar, Schrift x%u, Textfeld %ux%u\n", info->fb.width, info->fb.height,
            video_mode_count(), console_scale(), console_cols(), console_rows());

    if (init_interrupts(info) != 0) {
        kprintf("Ohne Timer kann der Kernel nicht weiterlaufen, angehalten.\n");
        halt_forever();
    }
    sched_init();
    console_start_thread(); /* blaettert im Verlauf (Shift+Bild hoch/runter), zeichnet den Mauszeiger */
    mouse_init();
    rtc_init();
    init_storage(info);
    net_init(); /* Netzwerkkarten; DHCP laeuft im Hintergrund */
    syscall_init();

    if (cmdline_has("selftest")) {
        run_selftests(info);
        if (!cmdline_has("keep")) { /* nach den Tests ausschalten: "make efi TESTS=1" kehrt sofort zurueck (KEEP=1 bleibt im System) */
            kprintf("\nSelbsttests beendet, schalte aus ('keep' in der Kommandozeile bleibt im System).\n");
            power_off();
        }
    }

    console_set_color(COLOR_TITLE, 0);
    kprintf("\nMiniKernel bereit. 'help' zeigt die Befehle; 'poweroff' und 'reboot' beenden das System.\n");
    console_set_color(COLOR_DEFAULT, 0);

    /* Erstes Programm: init=<pfad> aus der Kommandozeile, sonst die Shell. Beendet sich die Shell, startet sie der
     * Kernel neu (ein anderes init-Programm wird danach durch die Shell ersetzt). */
    const char *init = cmdline_get("init");
    if (!init)
        init = "/bin/sh";
    for (;;) {
        int pid = info->module ? process_spawn(init, init, 0) : -1;
        if (pid > 0)
            process_wait(pid, 0, 0, 0, 3600 * 1000);
        else {
            kprintf("init '%s' laesst sich nicht starten\n", init);
            thread_sleep_ms(1000);
        }
        init = "/bin/sh";
    }
}
