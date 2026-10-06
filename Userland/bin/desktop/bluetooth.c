/* Desktop: Bluetooth in der Taskleiste - Zustand, Suchen und Verbinden im Hintergrund, Inhalt des Bluetooth-Menues.
 *
 * Suchen (8 s, beim ersten Mal dazu das Laden der Firmware) und Verbinden blockieren im Kernel; das erledigt ein
 * eigener Thread, die Hauptschleife holt das Ergebnis ab und fragt sonst einmal je Sekunde den Zustand. Das Menue
 * zeigt die klassischen Geraete der letzten Suche (Audio zuerst); ein Klick verbindet - steht die Verbindung, richtet
 * der Kernel A2DP ein, und aller Ton geht an das Geraet. Schluessel und das zuletzt verbundene Geraet stehen in
 * bt_keys.cfg (settings.h); damit verbindet sich der Desktop beim Start. */

#include "desktop.h"
#include "thread.h"

int           bt_ok;     /* Bluetooth-Geraet am USB: Knopf in der Taskleiste */
static BtConn bc;        /* Zustand der Verbindung */
static BtDev  devs[BT_MENU_MAX];
static int    ndev;
static unsigned char last_addr[6], req_addr[6];
static char   last_name[48], req_name[48];
static int    have_last;

static volatile int bt_job; /* 0 nichts, 1 suchen, 2 verbinden, 3 trennen */
static volatile int bt_job_done;
static volatile s64 bt_job_result;
static int          bt_tid;
static char         bt_note[48];
static s64          bt_last_us;

/* Art eines klassischen Geraets aus der Class of Device; audio = 1 fuer Lautsprecher, Kopfhoerer usw. */
static const char *dev_kind(unsigned cod, int *audio)
{
    unsigned major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x3F;
    *audio = major == 4;
    switch (major) {
    case 1: return "Computer";
    case 2: return "Telefon";
    case 4: return minor == 1 ? "Headset" : minor == 2 ? "Freisprechen" : minor == 5 ? "Lautsprecher" :
                   minor == 6 ? "Kopfh\xC3\xB6rer" : "Audio";
    case 5: return "Eingabe";
    case 6: return "Drucker";
    case 7: return "Uhr";
    default: return "";
    }
}

static void *job_thread(void *arg)
{
    int j = (int)(long)arg;
    s64 r;
    if (j == 1) {
        r = sys_bt_scan(8);
    } else if (j == 2) {
        unsigned char l[6];
        char ln[48];
        bt_cfg_load(l, ln); /* Schluessel an den Kernel */
        r = sys_bt_connect(req_addr);
    } else {
        r = sys_bt_disconnect();
    }
    bt_job_result = r;
    __atomic_store_n(&bt_job_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static int start_job(int j)
{
    if (bt_job)
        return -1;
    bt_job_done = 0;
    bt_job = j;
    int t = thread_create(job_thread, (void *)(long)j);
    if (t < 0) {
        bt_job = 0;
        return -1;
    }
    bt_tid = t;
    bt_last_us = 0;
    return 0;
}

/* klassische Geraete der letzten Suche mit Namen: Audio zuerst, dann nach Signal */
static void load_devs(void)
{
    ndev = 0;
    BtDev d;
    for (u64 i = 0; i < 64 && sys_bt_dev(i, &d) == 0; i++) {
        if (d.kind != BT_KIND_BREDR || !d.name_len || ndev >= BT_MENU_MAX)
            continue;
        devs[ndev++] = d;
    }
    for (int i = 1; i < ndev; i++)
        for (int k = i; k > 0; k--) {
            int a1, a2;
            dev_kind(devs[k].cod, &a1);
            dev_kind(devs[k - 1].cod, &a2);
            if (a1 * 1000 + devs[k].rssi <= a2 * 1000 + devs[k - 1].rssi)
                break;
            BtDev t = devs[k];
            devs[k] = devs[k - 1];
            devs[k - 1] = t;
        }
}

void btd_init(void)
{
    BtInfo b;
    bt_ok = sys_bt_info(&b) == 0 && b.present;
    if (bt_ok)
        have_last = bt_cfg_load(last_addr, last_name) == 0;
}

static void connect_to(const unsigned char *addr, const char *name)
{
    memcpy(req_addr, addr, 6);
    snprintf(req_name, sizeof(req_name), "%s", name);
    if (start_job(2) == 0)
        bc.state = BT_CONN_CONNECTING;
}

void btd_tick(void)
{
    if (!bt_ok)
        return;
    int changed = 0;
    if (bt_job && __atomic_load_n(&bt_job_done, __ATOMIC_ACQUIRE)) {
        thread_join(bt_tid, 0);
        int j = bt_job;
        damage_menu();
        bt_job = 0;
        if (j == 1) {
            load_devs();
        } else if (j == 2 && bt_job_result == 0) { /* merken: damit verbindet sich der Desktop beim Start */
            bt_cfg_save(req_addr, req_name);
            memcpy(last_addr, req_addr, 6);
            snprintf(last_name, sizeof(last_name), "%s", req_name);
            have_last = 1;
        }
        changed = 1;
        bt_last_us = 0;
    }
    if (bt_last_us && now_us - bt_last_us < (bt_job == 2 ? 250000 : 1000000) && !changed)
        return;
    bt_last_us = now_us;
    BtConn c;
    if (sys_bt_conn(&c) != 0)
        return;
    if (bt_job == 2 && c.state != BT_CONN_READY)
        c.state = BT_CONN_CONNECTING;
    if (c.state != bc.state || c.a2dp_state != bc.a2dp_state || changed)
        damage_dock();
    if (menu_open == 7) {
        damage_menu();
        changed = 1;
    }
    bc = c;
    if (changed && menu_open == 7)
        damage_menu();

    static int auto_done; /* einmal beim Start: mit dem zuletzt verbundenen Geraet verbinden */
    if (!auto_done) {
        auto_done = 1;
        if (have_last && bc.state != BT_CONN_READY && !bt_job)
            connect_to(last_addr, last_name);
    }
}

/* ---------- fuer die Taskleiste ---------- */

int btd_icon(void) /* 0 getrennt, 1 verbinde, 2 verbunden, 3 spielt */
{
    if (bc.state == BT_CONN_READY)
        return bc.a2dp_state == 2 ? 3 : 2;
    return bc.state == BT_CONN_CONNECTING || bt_job == 2 ? 1 : 0;
}

static const char *conn_name(void)
{
    for (int i = 0; i < ndev; i++)
        if (memcmp(devs[i].addr, bc.addr, 6) == 0)
            return devs[i].name;
    if (memcmp(bc.addr, req_addr, 6) == 0 && req_name[0])
        return req_name;
    if (memcmp(bc.addr, last_addr, 6) == 0 && last_name[0])
        return last_name;
    return "Ger\xC3\xA4t";
}

const char *btd_device_name(void)
{
    return bc.state == BT_CONN_READY ? conn_name() : 0;
}

const char *btd_hover_name(char *buf, int max)
{
    if (bc.state == BT_CONN_READY)
        snprintf(buf, max, "Bluetooth: %s", conn_name());
    else
        snprintf(buf, max, "Bluetooth");
    return buf;
}

/* ---------- Menue ---------- */

static MenuItem items[BT_MENU_MAX + 12];
static char st_txt[48], dev_txt[48], audio_txt[48], msg_txt[44], keys[BT_MENU_MAX][24];

void btd_menu_opened(void)
{
    bt_note[0] = 0;
    if (!bt_job)
        load_devs();
    if (!ndev && !bt_job && bc.state != BT_CONN_CONNECTING) /* noch nie gesucht: gleich suchen */
        start_job(1);
}

const MenuItem *btd_menu(int *count)
{
    int n = 0;
    int st = bc.state;
    snprintf(st_txt, sizeof(st_txt), "%s", st == BT_CONN_READY ? "Verbunden" : st == BT_CONN_CONNECTING ?
             "Verbinde \xE2\x80\xA6" : st == BT_CONN_FAILED ? "Fehlgeschlagen" : "Bereit");
    items[n++] = (MenuItem){"Bluetooth", A_HEAD, st_txt};
    if (st == BT_CONN_READY || st == BT_CONN_CONNECTING) {
        snprintf(dev_txt, sizeof(dev_txt), "%s", st == BT_CONN_CONNECTING && req_name[0] ? req_name : conn_name());
        items[n++] = (MenuItem){"Ger\xC3\xA4t", A_INFO, dev_txt};
    }
    if (st == BT_CONN_READY) {
        snprintf(audio_txt, sizeof(audio_txt), "%s", bc.a2dp_state == 2 ? "spielt (SBC 48 kHz)" :
                 bc.a2dp_state == 1 ? "bereit" : "\xE2\x80\x93");
        items[n++] = (MenuItem){"Audio", A_INFO, audio_txt};
    } else if (st == BT_CONN_FAILED && bc.msg[0]) {
        int len = (int)strlen(bc.msg);
        snprintf(msg_txt, sizeof(msg_txt), len > 34 ? "%.33s\xE2\x80\xA6" : "%s", bc.msg);
        items[n++] = (MenuItem){"Grund", A_INFO, msg_txt};
    }
    if (bt_note[0])
        items[n++] = (MenuItem){"Hinweis", A_INFO, bt_note};
    items[n++] = (MenuItem){"", A_SEP, 0};
    for (int i = 0; i < ndev; i++) {
        int audio;
        const char *kind = dev_kind(devs[i].cod, &audio);
        int cur = st == BT_CONN_READY && memcmp(devs[i].addr, bc.addr, 6) == 0;
        snprintf(keys[i], sizeof(keys[i]), "%s", cur ? "verbunden" : kind);
        items[n++] = (MenuItem){devs[i].name, A_BT_DEV + i, keys[i]};
    }
    if (!ndev)
        items[n++] = (MenuItem){bt_job == 1 ? "Suche l\xC3\xA4uft \xE2\x80\xA6" : "Keine Ger\xC3\xA4te gefunden", A_INFO, ""};
    items[n++] = (MenuItem){"", A_SEP, 0};
    if (st == BT_CONN_READY)
        items[n++] = (MenuItem){"Trennen", A_BT_DISC, 0};
    items[n++] = (MenuItem){"Ger\xC3\xA4te suchen", A_BT_SCAN, bt_job == 1 ? "l\xC3\xA4uft \xE2\x80\xA6" : 0};
    *count = n;
    return items;
}

/* Aktion aus dem Bluetooth-Menue; 1 = Menue bleibt offen */
int btd_action(int a)
{
    damage_menu();
    menu_hover = -1;
    bt_note[0] = 0;
    int stay = 1;
    if (bt_job) {
        snprintf(bt_note, sizeof(bt_note), "%s", bt_job == 1 ? "Suche l\xC3\xA4uft noch" : "Bitte warten \xE2\x80\xA6");
    } else if (a == A_BT_SCAN) {
        start_job(1);
    } else if (a == A_BT_DISC) {
        start_job(3);
        stay = 0;
    } else if (a >= A_BT_DEV && a < A_BT_DEV + ndev) {
        BtDev *d = &devs[a - A_BT_DEV];
        if (bc.state == BT_CONN_READY && memcmp(d->addr, bc.addr, 6) == 0) {
            /* schon verbunden */
        } else if (bc.state == BT_CONN_READY) {
            snprintf(bt_note, sizeof(bt_note), "erst trennen");
        } else {
            connect_to(d->addr, d->name);
        }
    }
    if (stay)
        damage_menu();
    damage_dock();
    return stay;
}

int btd_dev_audio(int i) /* fuer das Symbol in der Zeile */
{
    int audio = 0;
    if (i >= 0 && i < ndev)
        dev_kind(devs[i].cod, &audio);
    return audio;
}
