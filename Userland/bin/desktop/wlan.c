/* Desktop: WLAN in der Taskleiste - Zustand, Suchen und Verbinden im Hintergrund, Inhalt des WLAN-Menues.
 *
 * Suchen (einige Sekunden) und Verbinden (bis etwa 10 s) blockieren im Kernel; das erledigt ein eigener Thread, die
 * Hauptschleife fragt jedes Bild nach, ob er fertig ist, und sonst einmal je Sekunde (beim Verbinden oefter) den
 * Zustand. Ein Klick auf ein geschuetztes Netz fragt im Menue nach dem Passwort (Tippen, Enter verbindet); das
 * zuletzt erfolgreich verbundene Netz steht in wlan.cfg (settings.h), damit verbindet sich der Desktop beim Start. */

#include "desktop.h"
#include "thread.h"

int        wlan_ok;          /* Karte mit Firmware da: Knopf in der Taskleiste */
WlanStatus wl_st;
static NetInfo  wl_ni;       /* wlan0 im Netzwerk-Stack (Adresse) */
static int      wl_ni_ok;
static WlanNet  wl_nets[WL_MAX];
static int      wl_n;

static volatile int wl_job;  /* 0 nichts, 1 suchen, 2 verbinden, 3 trennen (laeuft im Thread) */
static volatile int wl_job_done;
static volatile s64 wl_job_result;
static int          wl_tid;
static WlanConnect  wl_req;
static char wl_saved_ssid[33], wl_saved_pass[65];
static char wl_failed[33];   /* das letzte Verbinden damit scheiterte: beim naechsten Klick nach dem Passwort fragen */
static char wl_note[48];     /* kurzer Hinweis im Menue (z.B. "Passwort zu kurz") */
static int  wl_pw_mode;      /* Passwort-Eingabe fuer wl_pw_ssid */
static char wl_pw_ssid[33], wl_pw[65];
static s64  wl_last_us;

static const char *const sec_name[] = {"offen", "WEP", "WPA", "WPA2", "WPA3", "WPA2/3"};

static int sec_ok(int sec) /* koennen wir uns damit verbinden? */
{
    return sec == WLAN_SEC_OPEN || sec == WLAN_SEC_WPA2 || sec == WLAN_SEC_WPA2_3;
}

/* ---------- Hintergrund ---------- */

static void *job_thread(void *arg)
{
    int j = (int)(long)arg;
    s64 r = j == 1 ? sys_wlan_scan() : j == 2 ? sys_wlan_connect(&wl_req) : sys_wlan_disconnect();
    wl_job_result = r;
    __atomic_store_n(&wl_job_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static int start_job(int j)
{
    if (wl_job)
        return -1;
    wl_job_done = 0;
    wl_job = j;
    int t = thread_create(job_thread, (void *)(long)j);
    if (t < 0) {
        wl_job = 0;
        return -1;
    }
    wl_tid = t;
    wl_last_us = 0; /* Zustand gleich neu holen */
    return 0;
}

static void connect_to(const char *ssid, const char *pass)
{
    memset(&wl_req, 0, sizeof(wl_req));
    strcpy(wl_req.ssid, ssid);
    strcpy(wl_req.pass, pass);
    if (start_job(2) == 0)
        wl_st.state = WLAN_ST_CONNECTING; /* sofort anzeigen, der Kernel meldet es erst gleich */
}

/* Netze der letzten Suche: je Name nur der staerkste AP, ohne versteckte, nach Signal sortiert */
static void load_nets(void)
{
    wl_n = 0;
    WlanNet n;
    for (u64 i = 0; i < 64 && sys_wlan_net(i, &n) == 0; i++) {
        if (!n.ssid_len || !n.ssid[0])
            continue;
        int k = 0;
        while (k < wl_n && strcmp(wl_nets[k].ssid, n.ssid) != 0)
            k++;
        if (k < wl_n) {
            if (n.signal > wl_nets[k].signal)
                wl_nets[k] = n;
            continue;
        }
        if (wl_n < WL_MAX)
            wl_nets[wl_n++] = n;
        else if (n.signal > wl_nets[WL_MAX - 1].signal)
            wl_nets[WL_MAX - 1] = n;
        for (int a = wl_n - 1; a > 0 && wl_nets[a].signal > wl_nets[a - 1].signal; a--) {
            WlanNet t = wl_nets[a];
            wl_nets[a] = wl_nets[a - 1];
            wl_nets[a - 1] = t;
        }
    }
}

void wlan_init(void)
{
    WlanInfo wi;
    wlan_ok = sys_wlan_info(&wi) == 0 && wi.present && wi.fw_found;
    if (wlan_ok)
        wlan_cfg_load(wl_saved_ssid, wl_saved_pass);
}

/* jedes Bild: fertige Hintergrundarbeit abholen; einmal je Sekunde (beim Verbinden oefter) den Zustand */
void wlan_tick(void)
{
    if (!wlan_ok)
        return;
    int changed = 0;
    if (wl_job && __atomic_load_n(&wl_job_done, __ATOMIC_ACQUIRE)) {
        thread_join(wl_tid, 0);
        int j = wl_job;
        damage_menu(); /* alte Groesse des Menues */
        wl_job = 0;
        if (j == 1) {
            load_nets();
        } else if (j == 2) {
            if (wl_job_result == 0) {
                wlan_cfg_save(wl_req.ssid, wl_req.pass);
                strcpy(wl_saved_ssid, wl_req.ssid);
                strcpy(wl_saved_pass, wl_req.pass);
                wl_failed[0] = 0;
            } else {
                strcpy(wl_failed, wl_req.ssid);
            }
            memset(wl_req.pass, 0, sizeof(wl_req.pass));
        }
        wl_last_us = 0;
        changed = 1;
    }
    if (wl_last_us && now_us - wl_last_us < (wl_job == 2 ? 250000 : 1000000) && !changed)
        return;
    wl_last_us = now_us;
    WlanStatus s;
    if (sys_wlan_status(&s) != 0)
        return;
    if (wl_job == 2 && s.state != WLAN_ST_CONNECTING && s.state != WLAN_ST_CONNECTED)
        s.state = WLAN_ST_CONNECTING; /* Thread hat den Kernel noch nicht erreicht */
    NetInfo ni;
    int ok = 0;
    for (u64 i = 0; i < 4 && sys_netinfo(i, &ni) == 0; i++)
        if (strncmp(ni.name, "wlan", 4) == 0) {
            ok = 1;
            break;
        }
    int had_ip = wl_ni_ok && (wl_ni.ip[0] | wl_ni.ip[1] | wl_ni.ip[2] | wl_ni.ip[3]);
    int has_ip = ok && (ni.ip[0] | ni.ip[1] | ni.ip[2] | ni.ip[3]);
    if (s.state != wl_st.state || s.step != wl_st.step || had_ip != has_ip || changed)
        damage_dock();
    if (menu_open == 3) {
        damage_menu();
        changed = 1;
    }
    wl_st = s;
    wl_ni = ni;
    wl_ni_ok = ok;
    if (changed && menu_open == 3)
        damage_menu(); /* neue Groesse */

    /* einmal beim Start: mit dem gemerkten Netz verbinden */
    static int auto_done;
    if (!auto_done) {
        auto_done = 1;
        if (wl_saved_ssid[0] && wl_st.state != WLAN_ST_CONNECTED && !wl_job)
            connect_to(wl_saved_ssid, wl_saved_pass);
    }
}

/* ---------- fuer die Taskleiste ---------- */

int wlan_icon(int *level) /* 0 getrennt, 1 verbinde bzw. ohne Adresse, 2 verbunden; level 0..3 */
{
    int s = wl_st.signal;
    *level = s >= -55 ? 3 : s >= -67 ? 2 : s >= -78 ? 1 : 0;
    if (wl_st.state == WLAN_ST_CONNECTED)
        return wl_ni_ok && (wl_ni.ip[0] | wl_ni.ip[1] | wl_ni.ip[2] | wl_ni.ip[3]) ? 2 : 1;
    return wl_st.state == WLAN_ST_CONNECTING || wl_job == 2 ? 1 : 0;
}

const char *wlan_hover_name(char *buf, int max)
{
    if (wl_st.state == WLAN_ST_CONNECTED)
        snprintf(buf, max, "WLAN: %s", wl_st.ssid);
    else
        snprintf(buf, max, "WLAN");
    return buf;
}

int wlan_net_signal(int i)
{
    return i >= 0 && i < wl_n ? wl_nets[i].signal : -100;
}

/* ---------- Menue ---------- */

static MenuItem items[WL_MAX + 16];
static char st_txt[48], ip_txt[24], step_txt[40], msg_txt[44], pw_dots[200], net_keys[WL_MAX][16];

void wlan_menu_opened(void)
{
    wl_pw_mode = 0;
    wl_note[0] = 0;
    if (!wl_job)
        load_nets();
    if (!wl_n && !wl_job) /* noch nie gesucht: gleich suchen */
        start_job(1);
}

int wlan_pw_active(void)
{
    return menu_open == 3 && wl_pw_mode;
}


/* Text auf hoechstens n Zeichen kuerzen (an einer UTF-8-Grenze, mit ...) */
static void clip(char *dst, int max, const char *src, int n)
{
    int len = (int)strlen(src);
    if (len <= n) {
        snprintf(dst, max, "%s", src);
        return;
    }
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
        n--;
    snprintf(dst, max, "%.*s\xE2\x80\xA6", n, src);
}

const MenuItem *wlan_menu(int *count)
{
    int n = 0;
    if (wl_pw_mode) {
        items[n++] = (MenuItem){"Passwort f\xC3\xBCr", A_INFO, wl_pw_ssid};
        int k = 0;
        for (int i = 0; wl_pw[i] && k < (int)sizeof(pw_dots) - 4; i++) {
            memcpy(pw_dots + k, "\xE2\x80\xA2", 3); /* Punkt je Zeichen */
            k += 3;
        }
        pw_dots[k] = 0;
        items[n++] = (MenuItem){"", A_WLAN_PASS, pw_dots};
        if (wl_note[0])
            items[n++] = (MenuItem){"Hinweis", A_INFO, wl_note};
        items[n++] = (MenuItem){"Verbinden", A_WLAN_GO, "Enter"};
        items[n++] = (MenuItem){"Abbrechen", A_WLAN_BACK, 0};
        *count = n;
        return items;
    }
    int st = wl_st.state;
    snprintf(st_txt, sizeof(st_txt), "%s", st == WLAN_ST_CONNECTED ? "Verbunden" : st == WLAN_ST_CONNECTING ?
             "Verbinde \xE2\x80\xA6" : st == WLAN_ST_FAILED ? "Fehlgeschlagen" : "Getrennt");
    items[n++] = (MenuItem){"WLAN", A_HEAD, st_txt};
    if (st == WLAN_ST_CONNECTED) {
        items[n++] = (MenuItem){"Netz", A_INFO, wl_st.ssid};
        if (wl_ni_ok && (wl_ni.ip[0] | wl_ni.ip[1] | wl_ni.ip[2] | wl_ni.ip[3]))
            snprintf(ip_txt, sizeof(ip_txt), "%u.%u.%u.%u", wl_ni.ip[0], wl_ni.ip[1], wl_ni.ip[2], wl_ni.ip[3]);
        else
            snprintf(ip_txt, sizeof(ip_txt), "%s", wl_ni.dhcp == 1 ? "wird geholt \xE2\x80\xA6" : "keine");
        items[n++] = (MenuItem){"IP-Adresse", A_INFO, ip_txt};
        snprintf(step_txt, sizeof(step_txt), "%d dBm, %u Mbit/s", wl_st.signal, wl_st.rate_kbps / 1000);
        items[n++] = (MenuItem){"Signal", A_INFO, step_txt};
    } else if (st == WLAN_ST_CONNECTING) {
        static const char *const steps[] = {"Start", "Firmware", "Netz suchen", "Passwort", "Kontexte", "Station",
                                            "Warteschlangen", "Zeitfenster", "Authentifizierung", "Assoziierung",
                                            "Schl\xC3\xBCssel", "fertig"};
        items[n++] = (MenuItem){"Netz", A_INFO, wl_req.ssid[0] ? wl_req.ssid : wl_st.ssid};
        snprintf(step_txt, sizeof(step_txt), "%s", wl_st.step < 12 ? steps[wl_st.step] : "?");
        items[n++] = (MenuItem){"Schritt", A_INFO, step_txt};
    } else if (st == WLAN_ST_FAILED || (st == WLAN_ST_IDLE && wl_st.msg[0])) {
        clip(msg_txt, sizeof(msg_txt), wl_st.msg, 34);
        items[n++] = (MenuItem){st == WLAN_ST_FAILED ? "Grund" : "Zuletzt", A_INFO, msg_txt};
    }
    if (wl_note[0])
        items[n++] = (MenuItem){"Hinweis", A_INFO, wl_note};
    items[n++] = (MenuItem){"", A_SEP, 0};
    for (int i = 0; i < wl_n; i++) {
        const WlanNet *w = &wl_nets[i];
        int cur = st == WLAN_ST_CONNECTED && strcmp(w->ssid, wl_st.ssid) == 0;
        snprintf(net_keys[i], sizeof(net_keys[i]), "%s", cur ? "verbunden" : w->security < 6 ? sec_name[w->security] : "?");
        items[n++] = (MenuItem){w->ssid, A_WLAN_NET + i, net_keys[i]};
    }
    if (!wl_n)
        items[n++] = (MenuItem){wl_job == 1 ? "Suche l\xC3\xA4uft \xE2\x80\xA6" : "Keine Netze gefunden", A_INFO, ""};
    items[n++] = (MenuItem){"", A_SEP, 0};
    if (st == WLAN_ST_CONNECTED)
        items[n++] = (MenuItem){"WLAN trennen", A_WLAN_DISC, 0};
    items[n++] = (MenuItem){"Netze suchen", A_WLAN_SCAN, wl_job == 1 ? "l\xC3\xA4uft \xE2\x80\xA6" : 0};
    *count = n;
    return items;
}

static void note(const char *t)
{
    snprintf(wl_note, sizeof(wl_note), "%s", t);
}

/* Aktion aus dem WLAN-Menue; 1 = Menue bleibt offen */
int wlan_action(int a)
{
    damage_menu(); /* alte Groesse */
    menu_hover = -1;
    wl_note[0] = 0;
    int stay = 1;
    if (wl_job && a != A_WLAN_BACK) {
        note(wl_job == 1 ? "Suche l\xC3\xA4uft noch" : "Bitte warten \xE2\x80\xA6");
    } else if (a == A_WLAN_SCAN) {
        start_job(1);
    } else if (a == A_WLAN_DISC) {
        start_job(3);
        stay = 0;
    } else if (a == A_WLAN_BACK) {
        wl_pw_mode = 0;
    } else if (a == A_WLAN_GO) {
        int len = (int)strlen(wl_pw);
        if (len < 8 || len > 63) {
            note("8 bis 63 Zeichen");
        } else {
            wl_pw_mode = 0;
            connect_to(wl_pw_ssid, wl_pw);
            memset(wl_pw, 0, sizeof(wl_pw));
        }
    } else if (a >= A_WLAN_NET && a < A_WLAN_NET + wl_n) {
        const WlanNet *w = &wl_nets[a - A_WLAN_NET];
        if (wl_st.state == WLAN_ST_CONNECTED && strcmp(w->ssid, wl_st.ssid) == 0) {
            /* schon verbunden */
        } else if (!sec_ok(w->security)) {
            note(w->security == WLAN_SEC_WPA3 ? "WPA3 geht noch nicht" : "Verschl\xC3\xBCsselung geht nicht");
        } else if (w->security == WLAN_SEC_OPEN) {
            connect_to(w->ssid, "");
        } else if (strcmp(w->ssid, wl_saved_ssid) == 0 && strcmp(w->ssid, wl_failed) != 0) {
            connect_to(w->ssid, wl_saved_pass); /* gemerktes Passwort */
        } else {
            wl_pw_mode = 1;
            strcpy(wl_pw_ssid, w->ssid);
            memset(wl_pw, 0, sizeof(wl_pw));
        }
    }
    if (stay)
        damage_menu(); /* neue Groesse */
    damage_dock();
    return stay;
}

/* Tippen im Passwortfeld; 1 = verbraucht */
int wlan_menu_key(int k)
{
    if (!wlan_pw_active())
        return 0;
    int c = k & 0xFF, len = (int)strlen(wl_pw);
    damage_menu();
    if (c == '\n' || c == '\r')
        return wlan_action(A_WLAN_GO), 1;
    if (c == '\b' || c == 0x7F) {
        if (len)
            wl_pw[len - 1] = 0;
    } else if (c >= 32 && c < 127 && !(k & (KEY_MOD_ALT | KEY_MOD_CTRL))) {
        if (len < 63) {
            wl_pw[len] = (char)c;
            wl_pw[len + 1] = 0;
        }
    } else {
        return 0;
    }
    wl_note[0] = 0;
    damage_menu();
    return 1;
}
