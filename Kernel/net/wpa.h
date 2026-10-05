#ifndef NET_WPA_H
#define NET_WPA_H

#include <stdint.h>

/* WPA2-Personal (PSK) fuer Stationen: 4-Wege-Handshake und Gruppenschluessel-Wechsel (IEEE 802.11i, EAPOL-Key mit
 * Descriptor-Version 2: HMAC-SHA1-MIC, AES Key Wrap), Paarschluessel und Gruppenschluessel mit CCMP. Unabhaengig von
 * der Karte: der Treiber reicht empfangene EAPOL-Rahmen herein, schickt die Antwort und baut danach die Schluessel ein,
 * die in events gemeldet werden. */

#define WPA_EV_PTK 1 /* Paarschluessel (tk) jetzt einbauen - nach dem Senden der Antwort */
#define WPA_EV_GTK 2 /* Gruppenschluessel (gtk, gtk_idx, gtk_rsc) einbauen */

#define EAPOL_KEY_HDR 99 /* EAPOL-Kopf (4) + Key-Descriptor bis einschliesslich Laenge der Schluesseldaten */

typedef struct {
    uint8_t  pmk[32];
    uint8_t  aa[6], spa[6];      /* Adresse des AP (Authenticator) und die eigene (Supplicant) */
    uint8_t  anonce[32], snonce[32];
    uint8_t  ptk[48];            /* KCK 0..15 (MIC), KEK 16..31 (Schluesseldaten), TK 32..47 (CCMP) */
    uint8_t  ie[64];             /* eigenes RSN-Element, genau wie in der Assoc-Anfrage */
    uint32_t ie_len;
    uint64_t replay;             /* hoechster angenommener Replay-Zaehler */
    int      have_replay, have_anonce;
    uint8_t  gtk[32];
    uint32_t gtk_len;
    int      gtk_idx;
    uint8_t  gtk_rsc[6];         /* Paketnummer, ab der Gruppenrahmen gelten (Byte 0 = niedrigstes) */
    int      events;             /* WPA_EV_*, setzt der Aufrufer nach dem Einbauen zurueck */
    int      done;               /* 4-Wege-Handshake abgeschlossen */
    char     err[64];            /* warum der letzte Rahmen abgelehnt wurde */
} Wpa;

/* PMK aus Passphrase (8..63 Zeichen) und SSID; 64 Hex-Zeichen werden direkt als PMK genommen. 0 oder -1 */
int  wpa_pmk(const char *pass, const uint8_t *ssid, uint32_t ssid_len, uint8_t pmk[32]);

/* Vor der Assoziation: Zustand anlegen. snonce: 32 zufaellige Bytes */
void wpa_start(Wpa *w, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t *ie,
               uint32_t ie_len, const uint8_t snonce[32]);

/* Empfangener EAPOL-Rahmen (ab dem EAPOL-Kopf, also nach LLC/SNAP). Ergebnis: Laenge der Antwort in out (ebenfalls ab
 * dem EAPOL-Kopf), 0 = keine Antwort noetig, -1 = abgelehnt (Grund in err). */
int  wpa_rx(Wpa *w, const uint8_t *frame, uint32_t len, uint8_t *out, uint32_t max);

/* PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) | max(AA,SPA) | min(ANonce,SNonce) | max(ANonce,SNonce)) */
void wpa_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
                    const uint8_t snonce[32], uint8_t ptk[48]);
/* MIC eines EAPOL-Key-Rahmens (MIC-Feld muss 0 sein) */
void wpa_mic(const uint8_t kck[16], const uint8_t *frame, uint32_t len, uint8_t mic[16]);

#endif
