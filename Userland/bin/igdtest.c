#include "libc.h"

/* igdtest [cursor|blit|info|edid|scale|mode|dp|dpmode|dptrain|output [b|c|d]|vblank]: testet den Intel-Grafiktreiber auf echter Hardware.
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
 *   igdtest vblank  nichts Sichtbares: Bildwechsel-Interrupts eine Sekunde lang zaehlen und die Wartezeiten messen
 *   igdtest bltmode N  Bild-Updates per Blitter umschalten (0 = aus, 1-6 Testmodi, siehe Ausgabe)
 *   igdtest render  nichts Sichtbares: Render-Engine starten (Ring, PIPE_CONTROL, Batch-Buffer, Zeitmessung)
 *   igdtest gpgpu   Programme auf den Recheneinheiten: Assembler-Probe, Fuellen, Kopieren und Mischen (jeweils gegen
 *                   die CPU geprueft), Tempo bei Bildschirmgroesse; an der Konsole danach ein halbtransparentes
 *                   Farbfeld ueber dem Text (5 s)
 *   igdtest comp [on|off]  Zusammensetzen des Desktops auf der GPU: Zustand und Messwerte (GPU gegen CPU) bzw. an/aus
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
    } else if (argc > 1 && strcmp(argv[1], "vblank") == 0) {
        op = 12;
        what = "Bildwechsel-Interrupts zaehlen";
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
    } else if (argc > 1 && strcmp(argv[1], "bltmode") == 0) {
        static const char *const modes[7] = {
            "aus: die CPU kopiert die Bild-Updates",
            "Blitter, Cache-Steuerung (MOCS) wie vorgefunden",
            "wie 1, Programmbild vorher aus dem CPU-Cache zurueckschreiben",
            "Blitter, MOCS Write-Through",
            "wie 3, Programmbild vorher zurueckschreiben",
            "Blitter, MOCS uncached (Standard)",
            "wie 5, Programmbild vorher zurueckschreiben",
        };
        if (argc < 3 || argv[2][0] < '0' || argv[2][0] > '6' || argv[2][1]) {
            printf("Aufruf: igdtest bltmode N\n");
            for (int i = 0; i < 7; i++)
                printf("  %d  %s\n", i, modes[i]);
            printf("Danach Fenster ziehen, oeffnen und schliessen: bleiben Striche im Bild?\n");
            sys_exit(2);
        }
        int m = argv[2][0] - '0';
        s64 r = sys_gpu((u64)(13 | m << 8));
        printf("igdtest: Modus %d (%s): %s\n", m, modes[m], r == 0 ? "gesetzt" : "geht nicht, siehe dmesg | grep igdblt");
        sys_exit(r == 0 ? 0 : 1);
    } else if (argc > 1 && strcmp(argv[1], "comp") == 0) {
        int set = argc < 3 ? 2 : strcmp(argv[2], "on") == 0 ? 1 : strcmp(argv[2], "off") == 0 ? 0 : -1;
        if (set < 0) {
            printf("Aufruf: igdtest comp [on|off]\n");
            sys_exit(2);
        }
        static char buf[2048];
        u64 pos = ~0ULL;
        s64 n;
        sys_klog(&pos, buf, 0); /* nur die Zeilen ab jetzt ausgeben */
        sys_gpu(set == 2 ? 17 : (u64)(16 | set << 8));
        while ((n = sys_klog(&pos, buf, sizeof(buf))) > 0)
            write_all(1, buf, (size_t)n);
        sys_exit(0);
    } else if (argc > 1 && strcmp(argv[1], "gpgpu") == 0) {
        op = 15;
        what = "Programme auf den Recheneinheiten, an der Konsole ein halbtransparentes Farbfeld";
    } else if (argc > 1 && strcmp(argv[1], "render") == 0) {
        op = 14;
        what = "Render-Engine, nichts Sichtbares";
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
        fprintf(2, "Aufruf: igdtest [cursor|blit|info|edid|scale|mode|dp|dpmode|dptrain|output [b|c|d]|vblank|bltmode N|render|gpgpu|comp [on|off]]\n");
        sys_exit(2);
    }
    printf("igdtest: startet in 1 s (%s) ...\n", what);
    sys_sleep_ms(1000);
    s64 r = sys_gpu((u64)op);
    printf("igdtest: %s (Ergebnis %lld). Details: dmesg | grep igd\n", r == 0 ? "OK" : "FEHLER", (long long)r);
    sys_exit(r == 0 ? 0 : 1);
}
