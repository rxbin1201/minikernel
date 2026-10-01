/* Einstiegspunkt des Kernels (kmain, aufgerufen von arch/x86_64/entry.S): Hardware und Dienste initialisieren, bei
 * Bedarf die Selbsttests (tests/) ausfuehren, dann das erste Programm (init=, Standard /bin/sh) starten. */

#include "drivers/serial.h"
#include "console/console.h"
#include "lib/kprintf.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/smp.h"
#include "drivers/keyboard.h"
#include "core/sched.h"
#include "mm/kstack.h"
#include "core/process.h"
#include "drivers/pci.h"
#include "drivers/block/blk.h"
#include "fs/fs.h"
#include "core/cmdline.h"
#include "arch/x86_64/power.h"
#include "drivers/usb/usb.h"
#include "net/net.h"
#include "drivers/mouse.h"
#include "drivers/keymap.h"
#include "drivers/rtc.h"
#include "drivers/video.h"
#include "drivers/gpu/igd.h"
#include "drivers/sound/hda.h"
#include "tests/selftest.h"

/* ---------- Datentraeger ---------- */

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

/* Aufgerufen von entry.S auf dem eigenen Kernel-Stack. */
void kmain(BootInfo *info)
{
    smp_early_init(); /* Per-CPU-Daten der Boot-CPU (GS), Big Kernel Lock */
    serial_init();
    console_init(&info->fb);
    kprintf("Kernel gestartet (Framebuffer %ux%u @ %#lx)\n", info->fb.width, info->fb.height, (unsigned long)info->fb.base);

    gdt_init(smp_cpu(0));
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
    fpu_init();   /* FPU/SSE fuer Programme (die weiteren CPUs uebernehmen CR0/CR4 beim Start) */
    sched_init();
    smp_init(); /* weitere CPUs: laufen ab jetzt Threads aus der gemeinsamen Run-Queue */
    console_start_thread(); /* blaettert im Verlauf (Shift+Bild hoch/runter), zeichnet den Mauszeiger */
    mouse_init();
    rtc_init();
    init_storage(info);
    igd_init(info); /* Intel-Grafik: vorerst nur erkennen und auslesen */
    hda_init();     /* Ton: Intel High Definition Audio */
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
        if (pid > 0) {
            /* ohne Zeitgrenze: eine zweite Shell, waehrend die erste noch laeuft, wuerde ihr die Tasten wegnehmen */
            while (process_wait(pid, 0, 0, 0, 3600 * 1000) == -1 && process_poll(pid, 0) == 0)
                ;
        } else {
            kprintf("init '%s' laesst sich nicht starten\n", init);
            thread_sleep_ms(1000);
        }
        init = "/bin/sh";
    }
}
