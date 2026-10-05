#include "libc.h"

/* wlan [info]   WLAN-Karte (Intel AX200) und Bluetooth: was der Kernel erkannt hat, die zerlegte Firmware aus
 *               /firmware und das Bluetooth-Geraet am USB (8087:0029). Stufe 1 des Treibers: nur erkennen. */

static const char *hw_type(unsigned rev)
{
    unsigned t = (rev >> 4) & 0xFFF;
    return t == 0x34 ? "AX200 (22000 \"cc\")" : t == 0x32 ? "9260" : t == 0x33 ? "Qu" : t == 0x42 ? "AX210 (\"so\")" : "unbekannt";
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    WlanInfo wi;
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
