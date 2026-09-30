/* Selbsttests: VFS, Datenplatte, USB, weitere Datentraeger */

#include "lib/kprintf.h"
#include "lib/string.h"
#include "drivers/block/blk.h"
#include "mm/pmm.h"
#include "fs/fs.h"
#include "drivers/usb/usb.h"
#include "drivers/rtc.h"
#include "tests/selftest.h"

void test_usb(void)
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
    if (n) { /* jedes Kommando beim Erkennen der Geraete loest ein Ereignis und damit einen Interrupt aus */
        kprintf("  (%lu xHCI-Interrupts per MSI-X/MSI)\n", (unsigned long)usb_irq_count());
        check("xHCI meldet Ereignisse per Interrupt (MSI-X/MSI)", usb_irq_count() > 0);
    }
    /* Massenspeicher: das Ende der Transfers landet beim Interrupter ohne Interrupts (der Aufrufer wartet selbst) */
    for (int i = 0; i < blk_count(); i++) {
        BlkDev *d = blk_get(i);
        if (!d || memcmp(d->name, "usb", 3) != 0)
            continue;
        uint8_t *buf = blk_dma_alloc(8 * 512);
        uint64_t irq0 = usb_irq_count();
        int ok = buf != 0;
        for (int k = 0; ok && k < 25; k++)
            ok = blk_read(d, (uint64_t)k * 8, 8, buf) == 0;
        uint64_t irqs = usb_irq_count() - irq0;
        kprintf("  (%s: 25 x 8 Sektoren gelesen, %lu Interrupts dabei)\n", d->name, (unsigned long)irqs);
        check("USB-Massenspeicher: Lesen klappt", ok);
        check("USB-Massenspeicher: Transfers loesen keine Interrupts aus", irqs <= 2);
        if (buf)
            pmm_free_frame((uint64_t)buf); /* blk_dma_alloc: 4 KiB = ein Frame */
        break;
    }
    for (unsigned i = 0; i < n; i++) {
        usb_device_info(i, &info);
        check(info.name, info.driver != 0 || info.cls == 9); /* jedes Geraet hat einen Treiber (Hubs sind bekannt unbenutzt) */
    }
}

void test_vfs(BootInfo *info)
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
    check("readdir(/) enthaelt bin, etc, share und README.txt",
          vfs_readdir("/", 3) != 0 && vfs_readdir("/", 4) == 0 && vfs_lookup("/share/klang.wav"));
}

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

void test_disk(void)
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
void test_foreign(void)
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
