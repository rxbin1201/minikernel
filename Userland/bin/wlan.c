#include "libc.h"

/* wlan [info]   WLAN-Karte (Intel AX200) und Bluetooth: was der Kernel erkannt hat, die zerlegte Firmware aus
 *               /firmware und das Bluetooth-Geraet am USB (8087:0029).
 * wlan wake     Stufe 2a: Karte aufwecken (Takt, Zugriff auf die inneren Register), eine Kennung lesen
 * wlan load     Stufe 2b: Firmware laden und auf ihre erste Nachricht (ALIVE) warten */

static const char *hw_type(unsigned rev)
{
    unsigned t = (rev >> 4) & 0xFFF;
    return t == 0x34 ? "AX200 (22000 \"cc\")" : t == 0x32 ? "9260" : t == 0x33 ? "Qu" : t == 0x42 ? "AX210 (\"so\")" : "unbekannt";
}

void _start(int argc, char **argv)
{
    WlanInfo wi;
    if (argc > 1 && strcmp(argv[1], "load") == 0) {
        s64 r = sys_wlan_load();
        sys_wlan_info(&wi);
        if (r == ERR_NOENT) {
            printf("wlan: keine Karte\n");
            sys_exit(1);
        }
        if (!wi.dmar_found)
            printf("IOMMU:     keine (keine DMAR-Tabelle)\n");
        else
            printf("IOMMU:     %u Einheit(en), %u aktiv, %u abgeschaltet, DMAR-Flags %#x%s\n", wi.iommu_units,
                   wi.iommu_active, wi.iommu_off, wi.dmar_flags, wi.dmar_flags & 4 ? " (DMA-Schutz beim Booten)" : "");
        for (unsigned i = 0; i < wi.iommu_units && i < 4; i++)
            printf("           %u: GSTS %#010x PMEN %#010x\n", i, wi.iommu_gsts[i], wi.iommu_pmen[i]);
        printf("LTR:       %#010x -> %#010x\n", wi.ltr_before, wi.ltr_after);
        printf("Firmware gestartet, gewartet %u ms, CSR_INT %#010x, Status %u\n", wi.load_ms, wi.load_int, wi.load_status);
        if (wi.load_alive)
            printf("Erste Nachricht: Befehl %#x, Gruppe %#x, Laenge %u, Status %#x%s\n", wi.alive_cmd, wi.alive_group,
                   wi.alive_len, wi.alive_status,
                   wi.alive_cmd == 1 && wi.alive_status == 0xCAFE ? "  -> ALIVE, die Firmware laeuft" : "");
        else
            printf("Keine Nachricht der Firmware: Ladestatus %#x, Befehlszaehler UMAC %#x, LMAC %#x\n"
                   "(Verlauf: dmesg | grep iwl)\n", wi.st_load, wi.st_umac_pc, wi.st_lmac_pc);
        sys_exit(r == 0 ? 0 : 1);
    }
    if (argc > 1 && strcmp(argv[1], "wake") == 0) {
        s64 r = sys_wlan_wake();
        sys_wlan_info(&wi);
        if (r == ERR_NOENT) {
            printf("wlan: keine Karte\n");
            sys_exit(1);
        }
        printf("Bereit:    %s (HW_IF_CONFIG %#010x)\n", wi.wake_ready == 1 ? "ja (NIC_READY)"
               : wi.wake_ready == 2 ? "ja (nach PREPARE)" : "NEIN", wi.hwif_after);
        printf("Takt:      %s\n", wi.wake_clock ? "bereit" : "KEINER (nach INIT_DONE)");
        printf("CSR_RESET: %#010x -> %#010x (inneren Prozessor freigegeben)\n", wi.reset_before, wi.reset_after);
        printf("Zugriff:   %s nach %u us\n", wi.wake_access ? "bekommen" : "NICHT bekommen", wi.wake_us);
        printf("GP_CNTRL:  %#010x danach\n", wi.gp_after);
        printf("WFPM_CTRL: %#010x -> %#010x (Bit 31: Peripherie-Register ein)\n", wi.wfpm_before, wi.wfpm_after);
        printf("Ladestatus der Firmware: %#010x   Status CPU 1: %#010x\n", wi.prph_load, wi.prph_cpu1);
        printf("CNVI-ID:   %#010x\n", wi.cnvi_id);
        printf("CSR_INT:   %#010x nach den Lesezugriffen (Bit 29 = Hardware-Fehler)\n", wi.int_after);
        sys_exit(r == 0 ? 0 : 1);
    }
    if (sys_wlan_info(&wi) != 0) {
        fprintf(2, "wlan: der Kernel kennt kein WLAN (SYS_WLAN fehlt)\n");
        sys_exit(1);
    }
    int tty = sys_isatty(1) != 0;
    const char *b = tty ? C_BOLD : "", *r = tty ? C_RESET : "";
    printf("%sWLAN%s\n", b, r);
    if (wi.present) {
        printf("  Karte:     Intel %04x:%04x, Subsystem %04x:%04x, PCI %02x:%02x.%u, BAR0 %#llx\n", wi.vendor, wi.device,
               wi.sub_vendor, wi.sub_device, wi.bus, wi.dev, wi.fn, (unsigned long long)wi.bar);
        printf("  HW_REV:    %#010x  Typ %#x = %s, Schritt %c\n", wi.hw_rev, (wi.hw_rev >> 4) & 0xFFF, hw_type(wi.hw_rev),
               'A' + ((wi.hw_rev >> 2) & 3));
        printf("  RF_ID:     %#010x\n", wi.rf_id);
        printf("  GP_CNTRL:  %#010x   HW_IF_CONFIG: %#010x\n", wi.gp_cntrl, wi.hw_if_config);
    } else {
        printf("  Karte:     keine Intel AX200 (8086:2723) gefunden\n");
    }
    if (wi.fw_found) {
        printf("  Firmware:  /firmware/%s\n", wi.fw_name);
        printf("             %s, API %u, Version %u.%x.%u\n", wi.fw_human, wi.fw_api, wi.fw_major, wi.fw_minor, wi.fw_local);
        printf("             Abschnitte: %u LMAC, %u UMAC, %u Paging (%u KiB), %u CPU(s), Paging-Speicher %u KiB\n",
               wi.fw_lmac, wi.fw_umac, wi.fw_paging, wi.fw_bytes / 1024, wi.fw_cpus, wi.fw_paging_bytes / 1024);
        printf("             %u Faehigkeiten, %u API-Merkmale, %u Kanaele je Suche\n", wi.fw_capa,
               wi.fw_api_flags, wi.fw_scan_channels);
    }
    printf("  Zustand:   %s\n", wi.state);

    printf("%sBluetooth%s\n", b, r);
    int found = 0;
    UsbInfo u;
    for (u64 i = 0; sys_usbinfo(i, &u) == 0; i++)
        if (u.vid == 0x8087) {
            printf("  Geraet:    USB %04x:%04x '%s' an Port %s, Klasse %#x\n", u.vid, u.pid, u.name, u.path, u.cls);
            found = 1;
        }
    if (!found)
        printf("  Geraet:    kein Intel-Bluetooth am USB (8087:0029)\n");
    Stat st;
    if (sys_stat("/firmware/ibt-20-1-3.sfi", &st) == 0)
        printf("  Firmware:  /firmware/ibt-20-1-3.sfi, %llu Bytes\n", (unsigned long long)st.size);
    else
        printf("  Firmware:  /firmware/ibt-20-1-3.sfi fehlt\n");
    sys_exit(0);
}
