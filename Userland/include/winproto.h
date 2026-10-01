#ifndef WINPROTO_H
#define WINPROTO_H

/* Fensterprotokoll zwischen dem Desktop und seinen Fenster-Programmen.
 *
 * Der Desktop startet ein Programm mit zwei Pipes: aus Deskriptor 3 liest das Programm, was der Desktop schickt
 * (Begruessung, Tasten, Maus, Fokus, Schliessen, Bildtakt), auf Deskriptor 4 schreibt es seine Wuensche (Fenster
 * anlegen, Bereich geaendert, Titel, naechstes Bild). Alle Nachrichten sind WpMsg (64 Byte).
 *
 * Den Inhalt zeichnet das Programm in geteilten Speicher (SYS_SHM), den der Desktop mit einblendet; der Desktop zeichnet
 * Rahmen, Titelleiste und Schatten. Aendert der Benutzer die Groesse (nur bei WPF_RESIZABLE), schickt der Desktop
 * WP_RESIZE; das Programm legt einen neuen Puffer an, zeichnet hinein und meldet ihn mit WP_BUFFER (bis dahin zeigt
 * der Desktop den alten). Das alles erledigt die Grafikbibliothek (gfx.c): ein Programm, das unter dem
 * Desktop gfx_open() aufruft, bekommt ein Fenster statt des ganzen Bildschirms.
 *
 * Der Desktop wartet nie auf ein Programm: er schreibt nur, wenn die Pipe Platz hat (sonst verwirft er die Nachricht),
 * und liest nur ganze Nachrichten. */

#define WP_FD_IN  3          /* Programm liest (Desktop -> Programm) */
#define WP_FD_OUT 4          /* Programm schreibt (Programm -> Desktop) */
#define WP_MAGIC  0x314E4957 /* "WIN1" */

enum {
    /* Desktop -> Programm */
    WP_HELLO = 1, /* a = Bildschirmbreite, b = Hoehe, c = Massstab der Oberflaeche in Prozent, d = WP_MAGIC */
    WP_INPUT,     /* a..f = Event (type, key, x, y, button, wheel); x, y relativ zum Fensterinhalt */
    WP_FOCUS,     /* a = 1: Fenster ist jetzt aktiv, 0: nicht mehr */
    WP_CLOSE,     /* Schliessen-Knopf (das Programm beendet sich oder fragt nach; beim dritten Mal beendet es der Desktop) */
    WP_FRAME,     /* Antwort auf WP_WANT_FRAME: ein Bild ist gezeigt, das naechste kann kommen */
    WP_RESIZE,    /* a, b = neue Groesse des Inhalts */

    /* Programm -> Desktop */
    WP_CREATE = 16, /* a = Breite, b = Hoehe (Inhalt), c = Nummer des geteilten Speichers, d = WPF_*, text = Titel */
    WP_DAMAGE,      /* a, b, c, d = x, y, Breite, Hoehe: dieser Teil des Inhalts ist neu */
    WP_TITLE,       /* text = neuer Titel */
    WP_WANT_FRAME,  /* nach dem naechsten Bild WP_FRAME schicken (gfx_vsync) */
    WP_BUFFER,      /* a, b = Groesse, c = Nummer des neuen geteilten Speichers (nach WP_RESIZE) */
    WP_OPEN,        /* Datei/Ordner mit dem passenden Programm oeffnen (leerer Pfad: neues Textdokument): der Pfad kommt in
                     * Stuecken zu je hoechstens 32 Byte, a = Stelle im Pfad, c = Laenge des Stuecks, b = 1 beim letzten */
};

#define WPF_RESIZABLE 1 /* Groesse aenderbar (und zoombar) */
#define WP_PATH_MAX   256

typedef struct {
    int  type;
    int  a, b, c, d, e, f, g;
    char text[32];
} WpMsg;

#endif
