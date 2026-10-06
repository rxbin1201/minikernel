#include "libc.h"
#include "settings.h"

/* wlan [info]   WLAN-Karte (Intel AX200) und Bluetooth: was der Kernel erkannt hat, die zerlegte Firmware aus
 *               /firmware und das Bluetooth-Geraet am USB (8087:0029).
 * wlan wake     Stufe 2a: Karte aufwecken (Takt, Zugriff auf die inneren Register), eine Kennung lesen
 * wlan load     Stufe 2b/3: Firmware laden, ALIVE, erste Befehle (NVM, Land, Suche einrichten)
 * wlan scan     Stufe 4: Netze suchen (laedt die Firmware, falls noetig) und nach Signalstaerke sortiert zeigen
 * wlan connect NAME [PASSWORT]
 *               Stufe 5: mit dem Netz verbinden (offen oder WPA2-PSK); danach holt sich wlan0 per DHCP eine Adresse.
 *               Klappt es, merkt sich wlan.cfg das Netz (der Desktop verbindet sich beim Start damit);
 *               "wlan connect" ohne Namen nimmt das gemerkte
 * wlan disconnect, wlan status */

static const char *const sec_name[] = {"offen", "WEP", "WPA", "WPA2", "WPA3", "WPA2/3"};
static const char *const step_name[] = {"-", "Firmware", "Netz suchen", "Schluessel aus dem Passwort", "Kontexte",
                                        "Station", "Warteschlangen", "Zeitfenster", "Authentifizierung",
                                        "Assoziierung", "WPA2-Handshake", "verbunden"};

static void show_status(const WlanStatus *s)
{
    static const char *const st[] = {"getrennt", "verbinde ...", "verbunden", "fehlgeschlagen"};
    printf("Zustand:   %s\n", s->state < 4 ? st[s->state] : "?");
    if (s->ssid[0])
        printf("Netz:      '%s' (%02x:%02x:%02x:%02x:%02x:%02x), Kanal %u, %d dBm, %s\n", s->ssid, s->bssid[0],
               s->bssid[1], s->bssid[2], s->bssid[3], s->bssid[4], s->bssid[5], s->channel, s->signal,
               s->security < 6 ? sec_name[s->security] : "?");
    if (s->state == WLAN_ST_CONNECTED) {
        static const char *const mode[] = {"802.11a/g", "802.11n (HT)", "802.11ac (VHT)"};
        printf("Verbindung: AID %u, senden mit %u Mbit/s%s, aufgebaut in %u ms\n", s->aid, s->rate_kbps / 1000,
               s->tlc ? " (Rate waehlt die Firmware)" : " (fest)", s->connect_ms);
        printf("Funk:      %s, %u MHz, %u Datenstrom/-stroeme, bis %u Mbit/s, Block-Ack beim Empfang: %s\n",
               s->phy_mode < 3 ? mode[s->phy_mode] : "?", s->width ? s->width : 20, s->nss ? s->nss : 1,
               s->max_kbps / 1000, s->ba_rx ? "ja (AP sendet aggregiert)" : "nein");
    }
    if (s->state == WLAN_ST_FAILED)
        printf("Schritt:   %s (Fehler %d)\n", s->step < 12 ? step_name[s->step] : "?", s->error);
    if (s->msg[0])
        printf("Meldung:   %s\n", s->msg);
    if (s->rx_frames || s->tx_frames)
        printf("Rahmen:    %llu empfangen (%llu verworfen), %llu gesendet (%llu ohne Bestaetigung), %llu Schluesselwechsel\n",
               (unsigned long long)s->rx_frames, (unsigned long long)s->rx_dropped, (unsigned long long)s->tx_frames,
               (unsigned long long)s->tx_failed, (unsigned long long)s->rekeys);
}

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
        if (wi.load_alive) {
            static const char *steps[] = {"-", "INIT_EXTENDED_CFG", "NVM_ACCESS_COMPLETE", "INIT_COMPLETE", "NVM_GET_INFO",
                                          "TX_ANT_CONFIGURATION", "BT_CONFIG", "MCC_UPDATE", "SCAN_CFG", "fertig"};
            unsigned st = wi.init_step < 10 ? wi.init_step : 0;
            printf("Init:      %s (%u Pakete empfangen)\n", steps[st], wi.rx_packets);
            if (st != 9)
                printf("           bei diesem Schritt kam keine Antwort (Verlauf: dmesg | grep iwl)\n");
        }
        if (wi.init_step >= 9 && wi.mcc)
            printf("Land:      %c%c, %u Kanaele erlaubt\n", (char)(wi.mcc >> 8), (char)wi.mcc, wi.mcc_channels);
        if (wi.init_step >= 5) {
            unsigned k = wi.nvm_sku;
            printf("MAC:       %02x:%02x:%02x:%02x:%02x:%02x\n", wi.mac[0], wi.mac[1], wi.mac[2], wi.mac[3], wi.mac[4],
                   wi.mac[5]);
            printf("NVM:       Version %#x, %u MAC-Adressen, Antennen TX %#x RX %#x, %u Kanaele, LAR %s\n",
                   wi.nvm_version, wi.nvm_hw_addrs, wi.nvm_tx_chains, wi.nvm_rx_chains, wi.nvm_channels,
                   wi.nvm_lar ? "an" : "aus");
            printf("Kann:      %s%s%s%s%s%s\n", k & 1 ? "2,4 GHz " : "", k & 2 ? "5 GHz " : "", k & 4 ? "802.11n " : "",
                   k & 8 ? "802.11ac " : "", k & 16 ? "802.11ax " : "", k & 32 ? "(ohne MIMO)" : "");
        }
        sys_exit(r == 0 ? 0 : 1);
    }
    if (argc > 1 && strcmp(argv[1], "connect") == 0) {
        WlanConnect c;
        memset(&c, 0, sizeof(c));
        if (argc < 3) {
            if (wlan_cfg_load(c.ssid, c.pass) != 0) {
                fprintf(2, "Aufruf: wlan connect NAME [PASSWORT]   (Namen mit Leerzeichen in Anfuehrungszeichen)\n");
                sys_exit(1);
            }
        } else {
            if (strlen(argv[2]) > 32 || (argc > 3 && strlen(argv[3]) > 64)) {
                fprintf(2, "wlan: Name hoechstens 32, Passwort hoechstens 64 Zeichen\n");
                sys_exit(1);
            }
            strcpy(c.ssid, argv[2]);
            if (argc > 3)
                strcpy(c.pass, argv[3]);
        }
        printf("Verbinde mit '%s' ...\n", c.ssid);
        s64 r = sys_wlan_connect(&c);
        if (r == 0)
            wlan_cfg_save(c.ssid, c.pass);
        memset(&c, 0, sizeof(c));
        WlanStatus s;
        sys_wlan_status(&s);
        if (r == ERR_NOENT && s.step == 0) {
            printf("wlan: keine Karte\n");
            sys_exit(1);
        }
        show_status(&s);
        if (r == 0)
            printf("Die Adresse kommt per DHCP: ifconfig zeigt wlan0.\n");
        else
            printf("(Verlauf: dmesg | grep -e wlan -e iwl)\n");
        sys_exit(r == 0 ? 0 : 1);
    }
    if (argc > 1 && strcmp(argv[1], "disconnect") == 0) {
        s64 r = sys_wlan_disconnect();
        printf(r == 0 ? "getrennt\n" : "wlan: keine Karte\n");
        sys_exit(r == 0 ? 0 : 1);
    }
    if (argc > 1 && strcmp(argv[1], "status") == 0) {
        WlanStatus s;
        if (sys_wlan_status(&s) != 0) {
            fprintf(2, "wlan: der Kernel kennt kein WLAN\n");
            sys_exit(1);
        }
        show_status(&s);
        sys_exit(0);
    }
    if (argc > 1 && strcmp(argv[1], "scan") == 0) {
        printf("Suche auf 38 Kanaelen (2,4 und 5 GHz), dauert einige Sekunden ...\n");
        s64 r = sys_wlan_scan();
        if (r == ERR_NOENT) {
            printf("wlan: keine Karte\n");
            sys_exit(1);
        }
        if (r < 0) {
            sys_wlan_info(&wi);
            printf("wlan: Suche fehlgeschlagen (%lld; Init-Schritt %u) - Verlauf: dmesg | grep iwl\n", (long long)r,
                   wi.init_step);
            sys_exit(1);
        }
        static WlanNet n[64];
        int cnt = 0;
        for (u64 i = 0; cnt < 64 && sys_wlan_net(i, &n[cnt]) == 0; i++)
            cnt++;
        for (int i = 1; i < cnt; i++) /* nach Signal sortieren, staerkstes zuerst */
            for (int k = i; k > 0 && n[k].signal > n[k - 1].signal; k--) {
                WlanNet t = n[k];
                n[k] = n[k - 1];
                n[k - 1] = t;
            }
        sys_wlan_info(&wi);
        printf("%d Netz(e) in %u ms (%u Rahmen empfangen)\n", cnt, wi.scan_ms, wi.scan_frames);
        printf("  %-32s  %-17s  %5s  %5s  %s\n", "Name", "BSSID", "Kanal", "dBm", "Schutz");
        for (int i = 0; i < cnt; i++) {
            char name[40];
            if (n[i].ssid_len == 0 || n[i].ssid[0] == 0)
                strcpy(name, "(versteckt)");
            else
                snprintf(name, sizeof(name), "%s", n[i].ssid);
            printf("  %-32s  %02x:%02x:%02x:%02x:%02x:%02x  %5u  %5d  %s\n", name, n[i].bssid[0], n[i].bssid[1],
                   n[i].bssid[2], n[i].bssid[3], n[i].bssid[4], n[i].bssid[5], n[i].channel, n[i].signal,
                   n[i].security < 6 ? sec_name[n[i].security] : "?");
        }
        sys_exit(0);
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
        printf("  PCI:       Kommando/Status %#010x, %u Bruecke(n) fuer DMA freigeschaltet\n", wi.pci_cmd, wi.bridges_fixed);
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
