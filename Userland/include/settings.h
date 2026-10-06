#ifndef SETTINGS_H
#define SETTINGS_H

/* Einstellungen des Desktops: das Programm "Einstellungen" schreibt sie, der Desktop liest sie beim Start und wenn
 * es ihn darum bittet (WP_SETTINGS). Datei settings.cfg mit Zeilen "schluessel=wert" auf dem Boot-Volume neben
 * cmdline.txt, ohne Boot-Volume auf /disk. Aufloesung und Tastaturlayout stehen wie bisher in cmdline.txt (der
 * Kernel braucht sie beim Start). */

typedef struct {
    int ui_scale;  /* Groesse der Oberflaeche in Prozent: 0 = automatisch (ab 1300 Pixel Hoehe 125), 100, 125, 150 */
    int cursor;    /* Mauszeiger in Prozent: 0 = automatisch, 100-250 */
    int dock;      /* Taskleiste in Prozent: 85 klein, 100 normal, 125 gross */
    int seconds;   /* Uhr in der Taskleiste mit Sekunden */
    int date;      /* Datum unter der Uhrzeit */
    int wallpaper; /* Farbthema des Hintergrunds (ui_wallpaper, 0 = Abendrot) */
} Settings;

void        settings_default(Settings *s);
int         settings_load(Settings *s);       /* 0 = gelesen; sonst gelten die Standardwerte */
int         settings_save(const Settings *s); /* 0 = gespeichert */
const char *settings_file(void);              /* Pfad der Datei, "" = kein Ort zum Speichern */

/* Zuletzt verbundenes WLAN (wlan.cfg neben settings.cfg, Zeilen ssid=... und pass=...): der Desktop verbindet sich
 * beim Start damit. Das Passwort steht im Klartext darin - wie bei wpa_supplicant.conf. */
int wlan_cfg_load(char ssid[33], char pass[65]); /* 0 = gelesen */
int wlan_cfg_save(const char *ssid, const char *pass);

/* Bluetooth (bt_keys.cfg neben settings.cfg): Verbindungsschluessel ("aa:bb:cc:dd:ee:ff art schluessel-hex") und das
 * zuletzt verbundene Geraet ("last aa:bb:cc:dd:ee:ff Name"). bt_cfg_load gibt die Schluessel an den Kernel und liefert
 * das letzte Geraet (0 = bekannt); bt_cfg_save schreibt die Schluessel des Kernels und das Geraet (last = 0: das alte
 * bleibt). */
int bt_parse_addr(const char *s, unsigned char a[6]);
int bt_cfg_load(unsigned char last[6], char last_name[48]);
int bt_cfg_save(const unsigned char *last, const char *last_name);

#endif
