#include "libc.h"
#include "settings.h"

/* bt        Bluetooth (Stufe 1): Geraet am USB, Version der Intel-Firmware (Bootloader oder Betrieb),
 *           Boot-Parameter, passende Firmware-Datei in /firmware. Fragt das Geraet bei jedem Aufruf neu.
 * bt load   Stufe 2: Firmware aus /firmware laden (Secure Send) und starten (Intel Reset)
 * bt scan [SEKUNDEN]
 *           Stufe 3: Geraete in der Naehe suchen - klassisch (Inquiry) und Bluetooth LE gleichzeitig (Standard 8 s);
 *           laedt die Firmware und richtet HCI ein, falls noetig
 * bt connect ADRESSE|NAME
 *           Stufe 4: verbinden, koppeln (bzw. gespeicherten Schluessel nehmen), verschluesseln, AVDTP-Kanal oeffnen
 *           und die Audio-Endpunkte mit ihren Codecs zeigen. Schluessel und das Geraet landen in bt_keys.cfg neben
 *           settings.cfg: nicht jedes Mal neu koppeln, und der Desktop verbindet sich beim Start damit.
 * bt disconnect, bt status
 *           Stufe 5: ist die Verbindung da, richtet der Kernel A2DP ein (SBC 48 kHz Stereo); alles, was abgespielt
 *           wird (play, music, Spiele), geht dann an die Soundbar statt an die Soundkarte */

/* ---------- Verbindung ---------- */

static void show_sbc(const unsigned char *c)
{
    printf("SBC:");
    static const int fr[4] = {48000, 44100, 32000, 16000};
    for (int i = 0; i < 4; i++)
        if (c[0] & (0x10 << i))
            printf(" %d", fr[i]);
    printf(" Hz,%s%s%s%s", c[0] & 0x08 ? " mono" : "", c[0] & 0x04 ? " dual" : "", c[0] & 0x02 ? " stereo" : "",
           c[0] & 0x01 ? " joint" : "");
    printf(", Bloecke%s%s%s%s", c[1] & 0x80 ? " 4" : "", c[1] & 0x40 ? " 8" : "", c[1] & 0x20 ? " 12" : "",
           c[1] & 0x10 ? " 16" : "");
    printf(", Baender%s%s, %s%s, Bitpool %u-%u", c[1] & 0x08 ? " 4" : "", c[1] & 0x04 ? " 8" : "",
           c[1] & 0x02 ? "SNR " : "", c[1] & 0x01 ? "Loudness" : "", c[2], c[3]);
}

static void show_conn(void)
{
    BtConn c;
    if (sys_bt_conn(&c) != 0)
        return;
    static const char *const st[] = {"getrennt", "verbinde ...", "verbunden", "fehlgeschlagen"};
    static const char *const steps[] = {"-", "HCI", "Verbindung", "Koppeln", "Verschluesseln", "L2CAP",
                                        "AVDTP Discover", "AVDTP Faehigkeiten", "fertig"};
    printf("Verbindung: %s", c.state < 4 ? st[c.state] : "?");
    if (c.state != BT_CONN_IDLE || c.addr[0] | c.addr[1] | c.addr[2] | c.addr[3] | c.addr[4] | c.addr[5])
        printf(" (%02x:%02x:%02x:%02x:%02x:%02x)", c.addr[0], c.addr[1], c.addr[2], c.addr[3], c.addr[4], c.addr[5]);
    printf("\n");
    if (c.state == BT_CONN_FAILED)
        printf("Schritt:    %s (Fehler %d; Status Verbindung %#x, Koppeln %#x, Verschl. %#x)\n",
               c.step < 9 ? steps[c.step] : "?", c.error, c.conn_status, c.auth_status, c.enc_status);
    if (c.msg[0])
        printf("Meldung:    %s\n", c.msg);
    if (c.state == BT_CONN_READY) {
        printf("Link:       Handle %#x, %s, %s\n", c.handle, c.encrypted ? "verschluesselt" : "unverschluesselt",
               c.paired_new ? "neu gekoppelt" : "gespeicherter Schluessel");
        printf("AVDTP:      Kanal %#x <-> %#x, MTU der Gegenstelle %u\n", c.l2_local_cid, c.l2_remote_cid,
               c.l2_remote_mtu);
        for (int i = 0; i < c.n_seps; i++) {
            BtSep *s = &c.seps[i];
            printf("  Endpunkt %u: %s %s%s  ", s->seid, s->media == 0 ? "Audio" : s->media == 1 ? "Video" : "?",
                   s->tsep ? "Senke" : "Quelle", s->in_use ? " (belegt)" : "");
            if (s->codec == 0 && s->caps_len >= 4)
                show_sbc(s->caps);
            else if (s->codec == 2)
                printf("AAC");
            else if (s->codec == 0xFF && s->caps_len >= 6)
                printf("herstellereigen (Firma %02x%02x, Codec %02x%02x)", s->caps[1], s->caps[0], s->caps[5], s->caps[4]);
            else if (s->codec != 0xFE)
                printf("Codec %#x", s->codec);
            printf("\n");
        }
    }
    if (c.state == BT_CONN_READY) {
        static const char *const as[] = {"nicht eingerichtet", "bereit (pausiert)", "spielt"};
        printf("A2DP:       %s", c.a2dp_state < 3 ? as[c.a2dp_state] : "?");
        if (c.a2dp_seid)
            printf(" - Endpunkt %u, SBC 48 kHz Stereo, Bitpool %u, Medienkanal %#x (MTU %u)", c.a2dp_seid,
                   c.a2dp_bitpool, c.media_remote_cid, c.media_mtu);
        printf("\n            %u Pakete gesendet, %u Samples verworfen, %u Fehler\n", c.a2dp_packets, c.a2dp_dropped,
               c.a2dp_errors);
        printf("            %u Luecken (Programm zu langsam), %u Pakete mit Wartezeit ueber 30 ms (laengste %u ms)\n",
               c.a2dp_underruns, c.a2dp_stalls, c.a2dp_max_wait_ms);
    }
    printf("Zaehler:    ACL %u empfangen, %u gesendet; L2CAP %u\n", c.acl_rx, c.acl_tx, c.l2_rx);
}

static void connect_cmd(const char *target)
{
    unsigned char a[6];
    if (bt_parse_addr(target, a) != 0) { /* Name: in der letzten Suche nachsehen, sonst suchen */
        int found = 0;
        for (int pass = 0; pass < 2 && !found; pass++) {
            if (pass == 1) {
                printf("'%s' nicht in der letzten Suche - suche 8 s ...\n", target);
                if (sys_bt_scan(8) < 0)
                    break;
            }
            BtDev d;
            for (u64 i = 0; !found && sys_bt_dev(i, &d) == 0; i++)
                if (d.kind == BT_KIND_BREDR && d.name_len && strcmp(d.name, target) == 0) {
                    memcpy(a, d.addr, 6);
                    found = 1;
                }
        }
        if (!found) {
            printf("bt: kein klassisches Geraet namens '%s' gefunden (Adresse angeben: aa:bb:cc:dd:ee:ff)\n", target);
            sys_exit(1);
        }
    }
    unsigned char last[6];
    char last_name[48];
    bt_cfg_load(last, last_name);
    printf("Verbinde mit %02x:%02x:%02x:%02x:%02x:%02x ... (beim ersten Mal: Geraet in den Kopplungsmodus)\n", a[0],
           a[1], a[2], a[3], a[4], a[5]);
    s64 r = sys_bt_connect(a);
    BtConn c;
    sys_bt_conn(&c);
    if (r == 0) { /* Schluessel und "zuletzt verbunden" (damit verbindet sich der Desktop beim Start) */
        char name[48] = "";
        BtDev d;
        for (u64 i = 0; sys_bt_dev(i, &d) == 0; i++)
            if (memcmp(d.addr, a, 6) == 0 && d.name_len)
                snprintf(name, sizeof(name), "%s", d.name);
        bt_cfg_save(a, name[0] ? name : (strcmp(target, "") && bt_parse_addr(target, last) != 0 ? target : ""));
    }
    show_conn();
    if (r != 0)
        printf("            Verlauf: dmesg | grep bt\n");
    sys_exit(r == 0 ? 0 : 1);
}

/* Hauptklasse eines klassischen Geraets (Class of Device, Bits 8-12) */
static const char *cod_name(unsigned cod)
{
    static const char *const major[] = {"sonstiges", "Computer", "Telefon", "Netzwerk", "Audio/Video", "Eingabe",
                                        "Bildgebung", "Wearable", "Spielzeug", "Gesundheit"};
    unsigned m = (cod >> 8) & 0x1F;
    if (m == 5) { /* Eingabegeraete: Tastatur/Maus aus der Unterklasse */
        unsigned minor = (cod >> 6) & 3;
        return minor == 1 ? "Tastatur" : minor == 2 ? "Maus" : minor == 3 ? "Tastatur+Maus" : "Eingabe";
    }
    if (m == 4) {
        unsigned minor = (cod >> 2) & 0x3F;
        return minor == 1 ? "Headset" : minor == 2 ? "Freisprechen" : minor == 5 ? "Lautsprecher" :
               minor == 6 ? "Kopfhoerer" : "Audio";
    }
    return m < 10 ? major[m] : m == 31 ? "-" : "?";
}

/* LE: Kategorie des Erscheinungsbilds (Bits 6-15) */
static const char *appearance_name(unsigned a)
{
    switch (a >> 6) {
    case 0: return "-";
    case 1: return "Telefon";
    case 2: return "Computer";
    case 3: return "Uhr";
    case 5: return "Anzeige";
    case 10: return "Schluesselfinder";
    case 15:
        return (a & 0x3F) == 1 ? "Tastatur" : (a & 0x3F) == 2 ? "Maus" : (a & 0x3F) == 4 ? "Gamepad" : "Eingabe";
    case 33: return "Audio";
    case 37: return "Kopfhoerer";
    default: return "LE-Geraet";
    }
}

static void scan(int seconds)
{
    printf("Suche %d s nach Geraeten (klassisch und LE) ...\n", seconds);
    s64 n = sys_bt_scan((u64)seconds);
    if (n < 0) {
        BtInfo b;
        sys_bt_info(&b);
        printf("bt: Suche fehlgeschlagen (%lld): %s\n            Verlauf: dmesg | grep bt\n", (long long)n,
               b.dl_msg[0] ? b.dl_msg : b.state);
        sys_exit(1);
    }
    static BtDev d[64];
    int cnt = 0;
    for (u64 i = 0; cnt < 64 && sys_bt_dev(i, &d[cnt]) == 0; i++)
        cnt++;
    for (int i = 1; i < cnt; i++) /* benannte zuerst, dann nach Signal */
        for (int k = i; k > 0; k--) {
            int a = (d[k].name_len != 0) * 1000 + d[k].rssi, b = (d[k - 1].name_len != 0) * 1000 + d[k - 1].rssi;
            if (a <= b)
                break;
            BtDev t = d[k];
            d[k] = d[k - 1];
            d[k - 1] = t;
        }
    BtInfo b;
    sys_bt_info(&b);
    printf("%d Geraet(e) in %u ms\n", cnt, b.scan_ms);
    printf("  %-28s  %-17s  %-9s  %4s  %s\n", "Name", "Adresse", "Art", "dBm", "Typ");
    for (int i = 0; i < cnt; i++) {
        static const char *const kind[] = {"klassisch", "LE", "LE (zuf.)"};
        char rssi[8];
        if (d[i].rssi == -127)
            strcpy(rssi, "?");
        else
            snprintf(rssi, sizeof(rssi), "%d", d[i].rssi);
        printf("  %-28.28s  %02x:%02x:%02x:%02x:%02x:%02x  %-9s  %4s  %s\n", d[i].name_len ? d[i].name : "(ohne Namen)",
               d[i].addr[0], d[i].addr[1], d[i].addr[2], d[i].addr[3], d[i].addr[4], d[i].addr[5],
               d[i].kind < 3 ? kind[d[i].kind] : "?", rssi,
               d[i].kind == BT_KIND_BREDR ? cod_name(d[i].cod) : appearance_name(d[i].appearance));
    }
    sys_exit(0);
}

static void addr(const char *name, const unsigned char *a)
{
    printf("%-12s%02x:%02x:%02x:%02x:%02x:%02x\n", name, a[0], a[1], a[2], a[3], a[4], a[5]);
}

void _start(int argc, char **argv)
{
    int load = argc > 1 && strcmp(argv[1], "load") == 0;
    if (argc > 2 && strcmp(argv[1], "connect") == 0)
        connect_cmd(argv[2]);
    if (argc > 1 && strcmp(argv[1], "disconnect") == 0) {
        sys_bt_disconnect();
        show_conn();
        sys_exit(0);
    }
    if (argc > 1 && strcmp(argv[1], "status") == 0) {
        show_conn();
        sys_exit(0);
    }
    if (argc > 1 && strcmp(argv[1], "scan") == 0) {
        BtInfo bi;
        if (sys_bt_info(&bi) != 0 || !bi.present) {
            printf("Kein Bluetooth-Geraet am USB.\n");
            sys_exit(1);
        }
        scan(argc > 2 ? atoi(argv[2]) : 8);
    }
    BtInfo b;
    if (sys_bt_info(&b) != 0) {
        fprintf(2, "bt: der Kernel kennt kein Bluetooth (SYS_BT fehlt)\n");
        sys_exit(1);
    }
    if (!b.present) {
        printf("Kein Bluetooth-Geraet am USB (Klasse E0/01/01). lsusb zeigt die Geraete.\n");
        sys_exit(1);
    }
    if (load) {
        printf("Lade die Firmware (etwa 800 KiB), das dauert einige Sekunden ...\n");
        s64 lr = sys_bt_load();
        sys_bt_info(&b);
        printf("Laden:      %s\n", b.dl_msg);
        if (b.boot_addr)
            printf("Datei:      Boot-Adresse %#x, Build %u Woche %u 20%02u\n", b.boot_addr, b.file_build_num,
                   b.file_build_ww, b.file_build_yy);
        if (b.dl_frags)
            printf("Download:   %u Secure-Send-Befehle in %u ms, Ergebnis %s\n", b.dl_frags, b.dl_ms,
                   b.dl_result == 0 ? "ok" : b.dl_result == 0xFF ? "keins" : "FEHLER");
        if (b.booted && b.boot_ms)
            printf("Start:      Firmware meldet sich nach %u ms\n", b.boot_ms);
        if (lr != 0) {
            printf("            Verlauf: dmesg | grep bt\n");
            sys_exit(1);
        }
        printf("\n");
    }
    s64 r = sys_bt_query();
    sys_bt_info(&b);
    printf("Geraet:     USB %04x:%04x an Port %s\n", b.vid, b.pid, b.path);
    printf("Endpunkte:  Ereignisse %#x (%u Byte), Daten %#x/%#x (%u Byte)\n", b.ep_intr, b.mps_intr, b.ep_bulk_in,
           b.ep_bulk_out, b.mps_bulk);
    if (!b.ver_ok) {
        printf("Version:    keine Antwort (Fehler %lld, letzter Befehl %#06x: %d)\n", (long long)r, b.last_opcode,
               b.last_error);
        printf("            Verlauf: dmesg | grep bt\n");
        sys_exit(1);
    }
    static const char *mode[] = {"unbekannt", "Bootloader", "Betrieb"};
    printf("Intel:      Plattform %#x, Variante %#x (%u), Revision %u\n", b.hw_platform, b.hw_variant, b.hw_variant,
           b.hw_revision);
    printf("Firmware:   Variante %#x = %s, Revision %u, Build %u (Woche %u, 20%02u), Patch %u\n", b.fw_variant,
           b.mode < 3 ? mode[b.mode] : "?", b.fw_revision, b.fw_build_num, b.fw_build_ww, b.fw_build_yy, b.fw_patch_num);
    if (b.boot_ok) {
        printf("Boot:       Geraete-Revision %u, Secure Boot %s, Schluesseltyp %u (aus Kopf %u), OTP %u/%u/%u\n",
               b.dev_revid, b.secure_boot ? "an" : "aus", b.key_type, b.key_from_hdr, b.otp_format, b.otp_content,
               b.otp_patch);
        printf("            Sperren OTP %u, API %u, Debug %u; mindestens Build %u Woche %u 20%02u\n", b.otp_lock,
               b.api_lock, b.debug_lock, b.min_fw_build_nn, b.min_fw_build_cw, b.min_fw_build_yy);
        addr("BD-Adresse: ", b.otp_bdaddr);
    }
    if (b.mode == BT_MODE_OPERATIONAL) {
        addr("BD-Adresse: ", b.bdaddr);
        if (b.local_ok)
            printf("HCI:        Version %u, Revision %#x, LMP %u.%#x, Hersteller %#x\n", b.hci_version, b.hci_revision,
                   b.lmp_version, b.lmp_subversion, b.manufacturer);
    }
    printf("Datei:      /firmware/%s %s", b.fw_name, b.fw_found ? "" : "FEHLT");
    if (b.fw_found)
        printf("(%u Bytes)", b.fw_size);
    printf("\n");
    printf("Zustand:    %s\n", b.state);
    if (b.hci_ready)
        printf("HCI:        eingerichtet, %u DDC-Eintraege, ACL %u x %u Byte, LE %u x %u Byte\n", b.ddc_records,
               b.acl_pkts, b.acl_mtu, b.le_pkts, b.le_mtu);
    printf("Zaehler:    %u Befehle, %u Ereignisse (davon %u von Intel)\n", b.cmds, b.events, b.vendor_events);
    sys_exit(0);
}
