/* Selbsttests: Paging, Speicher, Heap, Interrupts, Scheduler, Uhr */

#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/apic.h"
#include "drivers/keyboard.h"
#include "core/sched.h"
#include "core/process.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/cpu.h"
#include "mm/paging.h"
#include "drivers/rtc.h"
#include "tests/selftest.h"

void test_paging(void)
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

void test_pmm(void)
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

void test_heap(void)
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

void test_interrupts(void)
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

void test_sched(void)
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

void test_rtc(void)
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

/* Stresstest fuer die Spinlocks von Heap, Paging und PMM: Kernel-Threads geben den Big Kernel Lock ab und rufen
 * gleichzeitig auf mehreren CPUs kmalloc/kfree (auch grosse Bloecke, damit der Heap waechst) und pmm_alloc/free auf.
 * Jeder Block traegt ein Muster, das beim Freigeben geprueft wird. Ohne BKL kein kprintf (die Konsole braucht ihn). */
#define STRESS_ROUNDS 20000
#define STRESS_SLOTS  16

static volatile int stress_done, stress_bad, stress_cpus_mask;

static void stress_thread(void *arg)
{
    uint32_t id = (uint32_t)(uint64_t)arg, seed = id * 2654435761u + 1;
    uint8_t *ptr[STRESS_SLOTS] = {0};
    uint32_t len[STRESS_SLOTS] = {0};
    int bad = 0;

    cpu_cli();
    bkl_release(); /* ab hier laeuft der Thread parallel zu den anderen */
    cpu_sti();
    for (int i = 0; i < STRESS_ROUNDS; i++) {
        __atomic_or_fetch(&stress_cpus_mask, 1 << this_cpu()->index, __ATOMIC_RELAXED);
        int k = i % STRESS_SLOTS;
        uint8_t mark = (uint8_t)(id * 16 + k);
        if (ptr[k]) {
            for (uint32_t j = 0; j < len[k]; j++)
                if (ptr[k][j] != mark) {
                    bad++;
                    break;
                }
            kfree(ptr[k]);
        }
        seed = seed * 1103515245u + 12345u;
        len[k] = i % 997 == 0 ? 70000 : 16 + (seed >> 8) % 3000;
        ptr[k] = kmalloc(len[k]);
        if (!ptr[k]) {
            bad++;
            continue;
        }
        memset(ptr[k], mark, len[k]);
        if (i % 8 == 0) {
            uint64_t fr = pmm_alloc_frame();
            if (!fr) {
                bad++;
            } else {
                *(volatile uint64_t *)fr = id;
                for (int j = 0; j < 50; j++)
                    __asm__ __volatile__("pause");
                if (*(volatile uint64_t *)fr != id) /* zwei CPUs mit demselben Frame? */
                    bad++;
                pmm_free_frame(fr);
            }
        }
    }
    for (int k = 0; k < STRESS_SLOTS; k++)
        kfree(ptr[k]);
    cpu_cli();
    bkl_acquire(); /* thread_exit braucht ihn wieder */
    stress_bad += bad;
    stress_done++;
}

static void smp_stress(unsigned n)
{
    unsigned k = n < 4 ? n : 4;
    uint64_t frames = pmm_free_frame_count(), heap0 = heap_total_bytes(), tables0 = paging_table_frames();
    stress_done = stress_bad = stress_cpus_mask = 0;
    uint64_t t0 = time_ms();
    for (unsigned i = 0; i < k; i++)
        thread_create("stress", stress_thread, (void *)(uint64_t)(i + 1));
    for (int w = 0; w < 3000 && stress_done < (int)k; w++)
        thread_sleep_ms(10);
    unsigned cpus = 0;
    for (unsigned i = 0; i < SMP_MAX_CPUS; i++)
        if (stress_cpus_mask & (1 << i))
            cpus++;
    kprintf("  (%u Threads x %d Runden kmalloc/kfree/pmm ohne BKL auf %u CPU(s): %lu ms, %d Fehler)\n", k,
            STRESS_ROUNDS, cpus, (unsigned long)(time_ms() - t0), stress_bad);
    check("Stresstest ohne BKL: alle Threads fertig", stress_done == (int)k);
    check("Stresstest ohne BKL: lief auf mehreren CPUs", cpus >= 2);
    check("Stresstest ohne BKL: kein Block/Frame doppelt vergeben oder ueberschrieben", stress_bad == 0);
    check("Stresstest ohne BKL: Heap konsistent", heap_check());
    uint64_t grown = (heap_total_bytes() - heap0) / 4096 + (paging_table_frames() - tables0); /* Heap gibt nichts zurueck */
    check("Stresstest ohne BKL: alle Frames zurueck", frames - pmm_free_frame_count() == grown);
}

/* Mehrere CPUs: alle gestarteten CPUs laufen, und User-Programme rechnen wirklich gleichzeitig. Dazu laufen einige
 * /bin/burn nebeneinander; die Timer-Ticks im User-Mode je CPU zeigen, wo gerechnet wurde. */
void test_smp(void)
{
    title("Mehrere CPUs (SMP)");
    unsigned n = smp_cpu_count();
    int online = 1;
    for (unsigned i = 0; i < n; i++)
        if (!smp_cpu(i)->online)
            online = 0;
    kprintf("  %u CPU(s)\n", n);
    check("Alle gestarteten CPUs laufen", online);
    if (n < 2) {
        kprintf("  (nur eine CPU, Test der Parallelitaet uebersprungen)\n");
        return;
    }

    unsigned k = n < 4 ? n : 4, busy = 0;
    uint64_t before[SMP_MAX_CPUS], sum = 0, bkl_before = 0, bkl_after = 0;
    for (unsigned i = 0; i < n; i++) {
        before[i] = smp_cpu(i)->ticks_user;
        bkl_before += smp_cpu(i)->bkl_timer;
    }
    int pids[4], started = 1;
    for (unsigned i = 0; i < k; i++)
        if ((pids[i] = process_spawn("/bin/burn", "burn 1500", 0)) <= 0)
            started = 0;
    thread_sleep_ms(1600); /* am Stueck schlafen: process_wait fragt jeden Tick nach und wuerde dafuer Wechsel ausloesen */
    for (unsigned i = 0; i < k; i++)
        if (pids[i] > 0)
            process_wait(pids[i], 0, 0, 0, 10000);
    for (unsigned i = 0; i < n; i++) {
        uint64_t d = smp_cpu(i)->ticks_user - before[i];
        sum += d;
        if (d >= 50) /* mindestens eine halbe Sekunde im User-Mode */
            busy++;
        bkl_after += smp_cpu(i)->bkl_timer;
    }
    uint64_t bkl_ticks = bkl_after - bkl_before;
    kprintf("  (%u x burn 1500 ms: %lu User-Ticks, %u CPU(s) mit mindestens 50; davon %lu Timer-Ticks mit BKL)\n", k,
            (unsigned long)sum, busy, (unsigned long)bkl_ticks);
    check("burn-Prozesse gestartet", started);
    check("User-Programme laufen auf mehreren CPUs gleichzeitig", busy >= 2);
    check("Rechenzeit mehr als eine CPU schaffen koennte", sum > 200); /* eine CPU: hoechstens 150 Ticks in 1,5 s */
    /* Vorher holte jeder Tick den BKL (bkl_ticks = sum). Jetzt nur noch, wenn ein aufgewachter Thread laufen muss und
     * keine CPU frei ist; mit freien CPUs werden rechnende gar nicht mehr unterbrochen. */
    check("Timer rechnender CPUs holt den BKL nicht bei jedem Tick", bkl_ticks * 4 < sum * 3);
    if (n > k)
        check("Mit freien CPUs werden rechnende kaum unterbrochen", bkl_ticks * 10 < sum);

    smp_stress(n);
}
