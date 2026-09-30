#include "libc.h"

/* igdtest [cursor|blit|info|edid|scale|mode|dp|dpmode|dptrain|output [b|c|d]]: testet den Intel-Grafiktreiber auf echter Hardware.
 *   igdtest         Page-Flipping: das Bild ist etwa 2 Sekunden invertiert, flackert dann kurz und kommt zurueck
 *   igdtest cursor  Hardware-Mauszeiger: ein Pfeil kreist 3 Sekunden um die Bildmitte und verschwindet wieder
 *   igdtest blit    Blitter: farbige Rechtecke erscheinen nacheinander, dann scrollt das Bild fuenfmal nach oben;
 *                   danach wird die Konsole neu gezeichnet
 *   igdtest info    nichts Sichtbares: Zaehler der Bild-Updates seit dem Start, Vergleich der Kopierwege
 *   igdtest edid    nichts Sichtbares: Monitordaten (EDID) und der von der Firmware eingestellte Modus
 *   igdtest scale   zwei Testbilder je 4 s in kleinerer Aufloesung, vom Skalierer hochgerechnet: erst bildschirmfuellend,
 *                   dann 16:9 in der Mitte mit schwarzen Raendern
 *   igdtest mode    echter Moduswechsel: jeden per HDMI moeglichen Modus des Monitors ca. 8 s mit Testbild, dann
 *                   zurueck (der Monitor wird bei jedem Wechsel kurz schwarz)
 *   igdtest dp      nichts Sichtbares: DisplayPort-Geraete an den Anschluessen suchen, Faehigkeiten und EDID lesen
 *   igdtest dpmode  Moduswechsel per DisplayPort: die Modi des Monitors (hoechste Bildrate zuerst) je ca. 8 s, dann zurueck
 *   igdtest dptrain DP-Verbindung abschalten (1 s schwarz) und selbst neu einmessen, dann Bild wieder an
 *   igdtest output  Anschluesse B-D anzeigen; "igdtest output c" legt das Bild von Grund auf auf Port C, nach 12 s zurueck
 * Die Messwerte stehen im Kernel-Log: danach "dmesg > /disk/igd.txt" und die Datei schicken. */
void _start(int argc, char **argv)
{
    int op = 1;
    const char *what = "Bildschirm wird kurz invertiert";
    if (argc > 1 && strcmp(argv[1], "cursor") == 0) {
        op = 2;
        what = "ein Pfeil kreist 3 s um die Bildmitte";
    } else if (argc > 1 && strcmp(argv[1], "scale") == 0) {
        op = 6;
        what = "zwei Testbilder, vom Skalierer hochgerechnet";
    } else if (argc > 1 && strcmp(argv[1], "output") == 0) {
        int port = 0;
        if (argc > 2 && (argv[2][0] | 0x20) >= 'b' && (argv[2][0] | 0x20) <= 'd' && !argv[2][1])
            port = (argv[2][0] | 0x20) - 'a';
        op = 11 | port << 8;
        what = port ? "Bild auf einen anderen Anschluss, nach 12 s zurueck" : "Anschluesse anzeigen";
    } else if (argc > 1 && strcmp(argv[1], "dptrain") == 0) {
        op = 10;
        what = "DP-Verbindung aus und selbst neu einmessen, der Monitor wird kurz schwarz";
    } else if (argc > 1 && strcmp(argv[1], "dpmode") == 0) {
        op = 9;
        what = "Moduswechsel per DisplayPort, der Monitor wird dabei eventuell kurz schwarz";
    } else if (argc > 1 && strcmp(argv[1], "mode") == 0) {
        op = 7;
        what = "Moduswechsel, der Monitor wird dabei jeweils kurz schwarz";
    } else if (argc > 1 && strcmp(argv[1], "blit") == 0) {
        op = 3;
        what = "farbige Rechtecke, dann scrollt das Bild";
    } else if (argc > 1 && (strcmp(argv[1], "info") == 0 || strcmp(argv[1], "edid") == 0 || strcmp(argv[1], "dp") == 0)) {
        int info = strcmp(argv[1], "info") == 0, dp = strcmp(argv[1], "dp") == 0;
        s64 r = sys_gpu(info ? 4 : dp ? 8 : 5);
        printf("igdtest: %s (Ergebnis %lld). Details: dmesg | grep %s\n", r == 0 ? "OK" : "FEHLER", (long long)r,
               info ? "igdinfo" : dp ? "igd" : "igdmode");
        sys_exit(r == 0 ? 0 : 1);
    } else if (argc > 1) {
        fprintf(2, "Aufruf: igdtest [cursor|blit|info|edid|scale|mode|dp|dpmode|dptrain|output [b|c|d]]\n");
        sys_exit(2);
    }
    printf("igdtest: startet in 1 s (%s) ...\n", what);
    sys_sleep_ms(1000);
    s64 r = sys_gpu((u64)op);
    printf("igdtest: %s (Ergebnis %lld). Details: dmesg | grep igd\n", r == 0 ? "OK" : "FEHLER", (long long)r);
    sys_exit(r == 0 ? 0 : 1);
}
