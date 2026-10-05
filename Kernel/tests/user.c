/* Selbsttests: Userland: Programme, Editor, Skripte, Werkzeuge, Grafik */

#include "console/console.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "lib/string.h"
#include "arch/x86_64/apic.h"
#include "drivers/keyboard.h"
#include "core/sched.h"
#include "core/process.h"
#include "fs/fs.h"
#include "drivers/mouse.h"
#include "drivers/keymap.h"
#include "core/tty.h"
#include "drivers/rtc.h"
#include "tests/selftest.h"

/* Dezimalzahl am Anfang eines Textes */
static uint64_t atoi_u(const char *s)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

#ifdef TEST_STACK_OVERFLOW
static void recurse(int n)
{
    volatile char pad[256];
    pad[0] = (char)n;
    recurse(n + 1);
    pad[1] = pad[0];
}

static void overflow_thread(void *arg)
{
    (void)arg;
    recurse(0);
}
#endif

/* Startet ein Programm aus der initrd und wartet auf das Ende. 0 = ok. */
static int run_user(const char *path, const char *cmdline, int *code, int *faulted)
{
    int pid = process_spawn(path, cmdline, 0);
    if (pid < 0)
        return -1;
    return process_wait(pid, 0, code, faulted, 5000);
}

/* Legt Text direkt in den Tastaturpuffer, als waere er getippt (sofort; fuer die Shell-Tests) */
static void feed(const char *s)
{
    for (; *s; s++)
        keyboard_deliver((unsigned char)*s);
}

/* Tippt einen Text ueber den 8042 ein (Make + Break je Zeichen, Shift wenn noetig). */
static void type_string(const char *s)
{
    for (; *s; s++) {
        int shift;
        int sc = keyboard_scancode_for(*s, &shift);
        if (sc < 0)
            continue;
        if (shift)
            keyboard_inject_scancode(0x2A);
        keyboard_inject_scancode((unsigned char)sc);
        keyboard_inject_scancode((unsigned char)(sc | 0x80));
        if (shift)
            keyboard_inject_scancode(0xAA);
        thread_sleep_ms(30);
    }
}

static int wait_for_file(const char *dir, const char *name, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (dir_has(dir, name, 0))
            return 1;
        thread_sleep_ms(50);
    }
    return dir_has(dir, name, 0);
}

static int count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (*s == '\n')
            n++;
    return n;
}

void test_user(BootInfo *info)
{
    title("User-Mode");
    if (!info->module) {
        check("initrd.tar vom Bootloader geladen", 0);
        return;
    }
    int code = 0, faulted = 0;

#ifdef TEST_STACK_OVERFLOW
    kprintf("  Kernel-Stack-Ueberlauf provozieren...\n");
    thread_create("overflow", overflow_thread, 0);
    thread_sleep_ms(500);
#endif

    check("hello: Exit-Code 42", run_user("/bin/hello", "hello eins zwei", &code, &faulted) == 0 && code == 42 && !faulted);
    thread_sleep_ms(50); /* Idle-Thread raeumt Adressraum und Stack auf */
    /* Kernel-Heap und Kernel-Page-Tables wachsen beim ersten Gebrauch und geben nichts zurueck: das ist kein Leck */
    uint64_t frames_before = pmm_free_frame_count(), heap_before = heap_total_bytes(), tables_before = paging_table_frames();

    check("crash null -> Prozess beendet", run_user("/bin/crash", "crash null", &code, &faulted) == 0 && faulted);
    check("crash kread -> Prozess beendet", run_user("/bin/crash", "crash kread", &code, &faulted) == 0 && faulted);
    check("crash priv -> Prozess beendet", run_user("/bin/crash", "crash priv", &code, &faulted) == 0 && faulted);
    check("crash badptr -> Syscall-Fehler statt Absturz",
          run_user("/bin/crash", "crash badptr", &code, &faulted) == 0 && !faulted && code == 7);
    check("crash unmapped (nach munmap) -> Prozess beendet",
          run_user("/bin/crash", "crash unmapped", &code, &faulted) == 0 && faulted);
    check("memtest (brk, mmap, malloc)", run_user("/bin/memtest", "memtest", &code, &faulted) == 0 && code == 0 && !faulted);
    check("Nicht vorhandenes Programm", run_user("/bin/gibtsnicht", "x", &code, &faulted) == -1);

    /* Taste 'x' einspeisen, keytest liest sie per SYS_GETCHAR */
    int pid = process_spawn("/bin/keytest", "keytest", 0);
    thread_sleep_ms(100);
    type_string("x");
    check("keytest liest 'x'", pid > 0 && process_wait(pid, 0, &code, &faulted, 3000) == 0 && code == 'x');

    /* fork/exec/Pipes/dup2/kill sowie lseek/rename/stat/chdir werden von eigenen Programmen geprueft */
    check("forktest (fork, exec, Pipe, dup2, kill)", run_user("/bin/forktest", "forktest", &code, &faulted) == 0 && code == 0 && !faulted);
    check("fstest (lseek, rename, stat, chdir, getcwd)", run_user("/bin/fstest", "fstest", &code, &faulted) == 0 && code == 0 && !faulted);
    check("shmtest (geteilter Speicher, Freigabe, Dienste)", run_user("/bin/shmtest", "shmtest", &code, &faulted) == 0 && code == 0 && !faulted);
    /* Threads in Programmen: Sperre, parallel auf mehreren CPUs, Futex, malloc/munmap aus mehreren Threads, exit,
     * Absturz, kill und fork mit laufenden Threads (die Seiten muessen danach alle zurueck sein, siehe unten) */
    pid = process_spawn("/bin/threadtest", "threadtest", 0);
    check("threadtest (Threads in Programmen)",
          pid > 0 && process_wait(pid, 0, &code, &faulted, 60000) == 0 && code == 0 && !faulted);
    if (pid > 0 && code != 0)
        kprintf("  threadtest: Pruefung %d fehlgeschlagen\n", code);
    /* fork mit Copy-on-Write: getrennte Inhalte nach dem Schreiben, auch bei Schreibzugriffen des Kernels, ueber
     * Generationen und mit Threads danach (alle geteilten Frames muessen danach zurueck sein, siehe unten) */
    pid = process_spawn("/bin/cowtest", "cowtest", 0);
    check("cowtest (fork mit Copy-on-Write)",
          pid > 0 && process_wait(pid, 0, &code, &faulted, 60000) == 0 && code == 0 && !faulted);
    if (pid > 0 && code != 0)
        kprintf("  cowtest: Pruefung %d fehlgeschlagen\n", code);
    /* Eingeblendete Dateien: Seiten erst beim Zugriff aus der Datei (alle Frames muessen danach zurueck sein) */
    pid = process_spawn("/bin/mmaptest", "mmaptest", 0);
    check("mmaptest (Dateien einblenden)",
          pid > 0 && process_wait(pid, 0, &code, &faulted, 60000) == 0 && code == 0 && !faulted);
    if (pid > 0 && code != 0)
        kprintf("  mmaptest: Pruefung %d fehlgeschlagen\n", code);
    /* Grenzen: weit ueber den frueheren festen Tabellen, die neuen Obergrenzen greifen genau */
    pid = process_spawn("/bin/limittest", "limittest", 0);
    check("limittest (Deskriptoren, Threads, Prozesse, Stack, Einblendungen, Kommandozeile)",
          pid > 0 && process_wait(pid, 0, &code, &faulted, 120000) == 0 && code == 0 && !faulted);
    if (pid > 0 && code != 0)
        kprintf("  limittest: Pruefung %d fehlgeschlagen\n", code);
    /* MP3 dekodieren (play liest die eingeblendete Datei): 3 s Stereo, 44,1 kHz, zwei Sinustoene */
    {
        void *wav = 0;
        uint64_t wsize = 0, sum = 0, loud = 0;
        int ok = run_user("/bin/play", "play -w /disk/TON.WAV /share/ton.mp3", &code, &faulted) == 0 && code == 0 &&
                 fs_read_file("/disk/TON.WAV", &wav, &wsize) == 0 && wsize > 44;
        if (ok) {
            const int16_t *smp = (const int16_t *)((const uint8_t *)wav + 44);
            uint64_t n = (wsize - 44) / 2;
            for (uint64_t i = 0; i < n; i++) {
                sum = sum * 31 + (uint16_t)smp[i];
                loud += smp[i] > 2000 || smp[i] < -2000; /* ffmpeg-Sinus: Spitze etwa 4096 */
            }
            uint64_t frames = n / 2;
            kprintf("  play -w: %lu Abtastwerte je Kanal, Pruefsumme %#lx\n", (unsigned long)frames, (unsigned long)sum);
            ok = frames > 3 * 44100 - 3000 && frames < 3 * 44100 + 3000 && loud > n / 4;
        }
        kfree(wav);
        fs_unlink("/disk/TON.WAV");
        check("play: MP3 in WAV umwandeln (eingeblendete Datei)", ok);
    }

    /* Die Shell: Befehle tippen (Tastatur-Injektion), die Ergebnisse stehen danach als Dateien auf /disk.
     * Prueft Pipes, Umleitungen, Anfuehrungszeichen, relative Pfade, Verlauf (Pfeil hoch) und Ctrl-C. */
    pid = process_spawn("/bin/sh", "sh", 0);
    thread_sleep_ms(200);
    feed("help\n");
    feed("echo hallo > /disk/o1.txt\n");
    feed("cat /disk/o1.txt | wc > /disk/o2.txt\n");
    feed("seq 1 5 | grep 3 >> /disk/o1.txt\n");
    feed("ls /bin | head -n 3 > /disk/o3.txt\n");
    feed("cd /disk\n");
    feed("pwd > o4.txt\n");
    feed("echo x >> o6.txt\n");
    keyboard_deliver(KEY_UP);                 /* Pfeil hoch: die letzte Zeile aus dem Verlauf zurueckholen */
    feed("\n");                /* ... und noch einmal ausfuehren */
    feed("echo \"zwei  Woerter\" > o7.txt\n");
    feed("mv o1.txt o5.txt\n");
    feed("cat < o5.txt | grep hallo > o8.txt\n");
    feed("gibtsnicht\n");      /* unbekannter Befehl: Fehlermeldung, Shell laeuft weiter */
    feed("echo > sync1.txt\n");
    wait_for_file("/disk", "SYNC1.TXT", 20000); /* die Shell ist bereit */
    feed("sleep 30\n");
    thread_sleep_ms(600);             /* Shell muss forken/exec'en und die Vordergrundgruppe setzen */
    uint64_t t_sleep = apic_ticks();
    keyboard_deliver(3);                  /* bricht "sleep 30" ab, nicht die Shell */
    feed("echo nach-ctrl-c > o9.txt\n");
    wait_for_file("/disk", "O9.TXT", 5000);
    uint64_t sleep_ticks = apic_ticks() - t_sleep; /* so lange dauerte es von Ctrl-C bis die Shell wieder Befehle ausfuehrte */
    feed("wc > o11.txt\n");     /* wc liest von der Tastatur (Zeilenmodus des Terminals) */
    thread_sleep_ms(500);
    feed("eins zwei\n");
    keyboard_deliver(4);                   /* Dateiende */
    /* Variablen, ;, &&, ||, Hintergrundjobs, Skripte und Tab-Vervollstaendigung */
    feed("X=hallo; echo \"v=$X\" > p1.txt\n");
    feed("false && echo nein > p2.txt || echo ja > p2.txt\n");
    feed("echo a > p3.txt; echo b >> p3.txt\n");
    feed("echo bg > p5.txt &\n");
    feed("wait\n");
    feed("rm p6.txt\n");        /* Rest eines frueheren, abgebrochenen Laufs */
    feed("echo 'echo $1-$# >> p6.txt' > s.sh\n");
    feed("sh s.sh abc\n");
    feed("./s.sh xyz\n");
    feed("echo tab > p8.txt\n");
    feed("cat p8.t\t > p9.txt\n");
    feed("seq 1 20 | tail -n 3 > q2.txt\n");
    feed("seq 1 3 | less > q3.txt\n");
    feed("seq 1 300 > q4.txt\n");
    feed("cat q4.txt | wc > q5.txt\n"); /* eine lange Datei kommt vollstaendig durch cat */
    uint64_t date_before = rtc_now(), date_ms0 = time_ms();
    feed("date +s > q6.txt\n");
    feed("date -s '2031-05-06 07:08:09'\n");
    feed("date +s > q7.txt\n");
    feed("resolution > q8.txt\n");
    feed("keymap > qc.txt\n");
    feed("keymap xx 2> qd.txt\n");
    feed("resolution -d /disk 800x600 > q9.txt\n");
    feed("resolution -d /disk -s 3 >> q9.txt\n");
    feed("cat cmdline.txt > qa.txt\n");
    feed("resolution -d /disk auto -s auto >> q9.txt\n");
    feed("cat cmdline.txt > qb.txt\n");
    feed("rm cmdline.txt\n");
    thread_sleep_ms(300);
    console_clipboard_set("echo Gr\xC3\xBC\xC3\x9F > pv.txt\n", 21); /* Ctrl-V in der Shell (mit Umlauten) */
    keyboard_deliver(0x16);
    thread_sleep_ms(1500);
    console_clipboard_set("echo rechts > pw.txt\n", 21);             /* rechte Maustaste fuegt ein */
    mouse_report(2, 0, 0, 0);
    mouse_report(0, 0, 0, 0);
    thread_sleep_ms(1500);
    feed("if true\n");                 /* mehrzeilig: die Shell liest weiter, bis 'fi' kommt */
    feed("then echo ml > ml.txt\n");
    feed("fi\n");
    feed("gruss() { echo hi $1; }\n");  /* Funktion bleibt fuer spaetere Zeilen erhalten */
    feed("gruss du > fn.txt\n");
    feed("ps > o10.txt\n");

    check("Shell hat alle Befehle abgearbeitet", wait_for_file("/disk", "O10.TXT", 25000));
    thread_sleep_ms(500);

    char data[256];
    check("echo > datei", slurp("/disk/O5.TXT", data, sizeof(data)) == 8 && strcmp(data, "hallo\n3\n") == 0); /* o1 wurde in o5 umbenannt, '>>' hat angehaengt */
    check("Pipe: cat | wc", slurp("/disk/O2.TXT", data, sizeof(data)) > 0 && strcmp(data, "1 1 6\n") == 0);
    check("Pipe: ls | head -n 3", slurp("/disk/O3.TXT", data, sizeof(data)) > 0 && count_lines(data) == 3);
    check("cd + pwd + relativer Pfad", slurp("/disk/O4.TXT", data, sizeof(data)) > 0 && strcmp(data, "/disk\n") == 0);
    check("Verlauf: Pfeil hoch + Enter wiederholt den Befehl", slurp("/disk/O6.TXT", data, sizeof(data)) > 0 && strcmp(data, "x\nx\n") == 0);
    check("Anfuehrungszeichen: ein Argument mit zwei Leerzeichen", slurp("/disk/O7.TXT", data, sizeof(data)) > 0 && strcmp(data, "zwei  Woerter\n") == 0);
    check("Eingabeumleitung: cat < datei | grep", slurp("/disk/O8.TXT", data, sizeof(data)) > 0 && strcmp(data, "hallo\n") == 0);
    check("Ctrl-C beendet sleep, die Shell laeuft weiter", wait_for_file("/disk", "O9.TXT", 100) && sleep_ticks < 1000);
    kprintf("  (nach Ctrl-C war die Shell in %lu Ticks wieder bereit; sleep 30 haette 3000 gedauert)\n", sleep_ticks);
    check("Terminal-Zeilenmodus: wc liest von der Tastatur bis Ctrl-D", slurp("/disk/O11.TXT", data, sizeof(data)) > 0 && strcmp(data, "1 2 10\n") == 0);
    check("ps listet die Prozesse", slurp("/disk/O10.TXT", data, sizeof(data)) > 0 && strstr_(data, "PID") && strstr_(data, "sh"));

    check("Variable: X=hallo; echo \"v=$X\"", slurp("/disk/P1.TXT", data, sizeof(data)) > 0 && strcmp(data, "v=hallo\n") == 0);
    check("&& und ||: false && a || b", slurp("/disk/P2.TXT", data, sizeof(data)) > 0 && strcmp(data, "ja\n") == 0);
    check("; trennt Befehle", slurp("/disk/P3.TXT", data, sizeof(data)) > 0 && strcmp(data, "a\nb\n") == 0);
    check("Hintergrundjob (&) und wait", slurp("/disk/P5.TXT", data, sizeof(data)) > 0 && strcmp(data, "bg\n") == 0);
    check("Skript: sh datei args und ./datei.sh", slurp("/disk/P6.TXT", data, sizeof(data)) > 0 && strcmp(data, "abc-1\nxyz-1\n") == 0);
    check("Tab-Vervollstaendigung (cat p8.t<Tab>)", slurp("/disk/P9.TXT", data, sizeof(data)) > 0 && strcmp(data, "tab\n") == 0);

    check("tail -n 3", slurp("/disk/Q2.TXT", data, sizeof(data)) > 0 && strcmp(data, "18\n19\n20\n") == 0);
    check("less ohne Terminal wirkt wie cat", slurp("/disk/Q3.TXT", data, sizeof(data)) > 0 && strcmp(data, "1\n2\n3\n") == 0);
    check("cat einer langen Datei (300 Zeilen) ist vollstaendig", slurp("/disk/Q5.TXT", data, sizeof(data)) > 0 && strcmp(data, "300 300 1092\n") == 0);

    {
        /* date liest die Uhr, date -s stellt sie (2031-05-06 07:08:09); danach wird sie wieder zurueckgestellt */
        int64_t d1 = slurp("/disk/Q6.TXT", data, sizeof(data)) > 0 ? (int64_t)atoi_u(data) : -1;
        int64_t d2 = slurp("/disk/Q7.TXT", data, sizeof(data)) > 0 ? (int64_t)atoi_u(data) : -1;
        check("date +s liefert die RTC-Zeit", !rtc_valid() || (d1 >= (int64_t)date_before && d1 <= (int64_t)date_before + 90));
        check("date -s stellt die Uhr (2031-05-06 07:08:09 = 1935817689)", d2 >= 1935817689 && d2 <= 1935817689 + 5);
        rtc_set(date_before + (time_ms() - date_ms0) / 1000);
    }
    {
        check("resolution: listet die Grafikmodi", slurp("/disk/Q8.TXT", data, sizeof(data)) > 0 && strstr_(data, "Verfuegbare Grafikmodi") &&
                                                   strstr_(data, "aktuell"));
        int64_t qa = slurp("/disk/QA.TXT", data, sizeof(data));
        check("resolution: schreibt mode= und scale= in die cmdline.txt", qa > 0 && strcmp(data, "mode=800x600 scale=3\n") == 0);
        int64_t qb = slurp("/disk/QB.TXT", data, sizeof(data));
        check("resolution: auto entfernt die Eintraege wieder", qb > 0 && strcmp(data, "\n") == 0);
    }
    check("Einfuegen in die Shell mit Ctrl-V (UTF-8)", slurp("/disk/PV.TXT", data, sizeof(data)) > 0 && strcmp(data, "Gr\xC3\xBC\xC3\x9F\n") == 0);
    check("Einfuegen mit der rechten Maustaste", slurp("/disk/PW.TXT", data, sizeof(data)) > 0 && strcmp(data, "rechts\n") == 0);
    check("Mehrzeilige Eingabe (if ... fi ueber drei Zeilen)", slurp("/disk/ML.TXT", data, sizeof(data)) > 0 && strcmp(data, "ml\n") == 0);
    check("Funktion aus einer frueheren Zeile", slurp("/disk/FN.TXT", data, sizeof(data)) > 0 && strcmp(data, "hi du\n") == 0);
    feed("rm ml.txt fn.txt\n");
    feed("rm pv.txt pw.txt\n");
    check("keymap zeigt das Layout", slurp("/disk/QC.TXT", data, sizeof(data)) > 0 && strstr_(data, "Tastaturlayout: us"));
    check("keymap lehnt unbekannte Layouts ab und bleibt bei us", strcmp(keymap_name(), "us") == 0);
    feed("rm q8.txt q9.txt qa.txt qb.txt qc.txt qd.txt\n");
    feed("rm q2.txt q3.txt q4.txt q5.txt q6.txt q7.txt\n");
    feed("rm o5.txt o2.txt o3.txt o4.txt o6.txt o7.txt o8.txt o9.txt o10.txt o11.txt p1.txt p2.txt p3.txt p5.txt p6.txt p8.txt p9.txt s.sh sync1.txt\n");
    feed("cd /\n");
    feed("exit\n");
    check("Shell beendet sich mit exit (Code 0)",
          pid > 0 && process_wait(pid, 0, &code, &faulted, 15000) == 0 && code == 0 && !faulted);
    check("Testdateien wieder geloescht", !dir_has("/disk", "O5.TXT", 0) && !dir_has("/disk", "O10.TXT", 0) &&
                                          !dir_has("/disk", "O9.TXT", 0) && !dir_has("/disk", "O11.TXT", 0));

    int64_t missing = frames_missing(frames_before, heap_before, tables_before);
    if (missing)
        kprintf("  (%ld Frames fehlen, ohne Heap und Kernel-Page-Tables)\n", (long)missing);
    check("Keine Frames verloren (Adressraeume/Stacks freigegeben)", missing == 0);
    check("Kernel-Heap konsistent", heap_check());
}

/* Alle Selbsttests (nur mit "selftest" in der Kommandozeile). Sie ueberschreiben Bildschirm und Testdateien. */
/* Startet den Editor mit einer Datei; die Tasten liegen vorher schon im Tastaturpuffer */
static int editor_run(const char *path, int *pid_out)
{
    char cmd[96];
    ksnprintf(cmd, sizeof(cmd), "edit %s", path);
    int pid = process_spawn("/bin/edit", cmd, 0);
    if (pid_out)
        *pid_out = pid;
    return pid;
}

static int editor_wait(int pid)
{
    int code = 0, faulted = 0;
    return pid > 0 && process_wait(pid, 0, &code, &faulted, 8000) == 0 && code == 0 && !faulted;
}

void test_editor(void)
{
    title("Texteditor");
    char data[256], tmp[64];
    if (fs_disk_volume() < 0) {
        check("Editor-Tests brauchen /disk", 0);
        return;
    }
    drain_keys(tmp, sizeof(tmp));
    fs_unlink("/disk/ED1.TXT");
    fs_unlink("/disk/ED2.TXT");
    fs_unlink("/disk/ED3.TXT");

    /* 1. Neue Datei: tippen, Pfeil hoch, Ende, Suchen (ohne Beachtung der Schreibweise), speichern, beenden */
    feed("Hallo\nWelt");
    keyboard_deliver(KEY_UP);
    keyboard_deliver(KEY_END);
    feed("!");
    keyboard_deliver(6);             /* Strg+F */
    feed("welt\n");
    keyboard_deliver(KEY_LEFT);      /* Markierung aufheben, Cursor an den Anfang des Funds */
    feed("x");
    keyboard_deliver(19);            /* Strg+S */
    keyboard_deliver(17);            /* Strg+Q */
    int pid;
    int ok1 = editor_wait(editor_run("/disk/ED1.TXT", &pid));
    check("Tippen, Pfeiltasten, Suchen, Speichern", ok1 && slurp("/disk/ED1.TXT", data, sizeof(data)) > 0 &&
                                                    strcmp(data, "Hallo!\nxWelt\n") == 0);

    /* 2. Aenderung ohne Speichern: erstes Strg+Q warnt, das zweite beendet ohne zu speichern */
    feed("zzz");
    keyboard_deliver(17);
    keyboard_deliver(17);
    int ok2 = editor_wait(editor_run("/disk/ED1.TXT", 0));
    check("Beenden mit ungespeicherten Aenderungen fragt nach (Datei bleibt unveraendert)",
          ok2 && slurp("/disk/ED1.TXT", data, sizeof(data)) > 0 && strcmp(data, "Hallo!\nxWelt\n") == 0);

    /* 3. Umlaute und Backspace (ein ganzes UTF-8-Zeichen) */
    feed("Gr\xC3\xBC\xC3\x9F\xE2\x82\xAC\b");
    keyboard_deliver(19);
    keyboard_deliver(17);
    int ok3 = editor_wait(editor_run("/disk/ED2.TXT", 0));
    check("Umlaute tippen, Backspace loescht ein ganzes Zeichen", ok3 && slurp("/disk/ED2.TXT", data, sizeof(data)) > 0 &&
                                                              strcmp(data, "Gr\xC3\xBC\xC3\x9F\n") == 0);

    /* 4. Strg+K schneidet die Zeile aus (Zwischenablage), Strg+V fuegt sie woanders ein */
    feed("eins\nzwei");
    keyboard_deliver(KEY_UP);
    keyboard_deliver(11);            /* Strg+K */
    keyboard_deliver(KEY_END);
    feed("\n");
    int pid4 = editor_run("/disk/ED3.TXT", 0);
    thread_sleep_ms(1500);           /* erst muss der Editor Strg+K verarbeitet haben */
    uint32_t clen;
    const char *clip = console_clipboard(&clen);
    int clip_ok = clen == 5 && memcmp(clip, "eins\n", 5) == 0;
    keyboard_deliver(0x16);          /* Strg+V */
    keyboard_deliver(19);
    keyboard_deliver(17);
    int ok4 = editor_wait(pid4);
    check("Strg+K schneidet eine Zeile aus, Strg+V fuegt sie ein", ok4 && clip_ok &&
          slurp("/disk/ED3.TXT", data, sizeof(data)) > 0 && strcmp(data, "zwei\neins\n\n") == 0);

    /* 5. Windows-Zeilenenden bleiben erhalten */
    write_text("/disk/ED3.TXT", "a\r\nb\r\n");
    keyboard_deliver(KEY_END);
    feed("x");
    keyboard_deliver(19);
    keyboard_deliver(17);
    int ok5 = editor_wait(editor_run("/disk/ED3.TXT", 0));
    check("Windows-Zeilenenden (CRLF) bleiben erhalten", ok5 && slurp("/disk/ED3.TXT", data, sizeof(data)) > 0 &&
                                                         strcmp(data, "ax\r\nb\r\n") == 0);

    /* 6. Maus: Klick setzt den Cursor, rechte Taste fuegt ein; die Konsole markiert/blaettert waehrenddessen nicht */
    write_text("/disk/ED3.TXT", "abc\ndef\n");
    int pid6 = editor_run("/disk/ED3.TXT", 0);
    thread_sleep_ms(1500);           /* Editor laeuft und hat die Maus uebernommen */
    int cw = 8 * (int)console_scale(), ch = 16 * (int)console_scale();
    mouse_goto((4 + 2) * cw + 2, 2 * ch + 2); /* Textzeile 2 (Bildschirmzeile 2), Spalte 2 hinter der Zeilennummer */
    mouse_report(1, 0, 0, 0);
    mouse_report(0, 0, 0, 0);
    int no_console_sel = !console_has_selection();
    mouse_report(0, 0, 0, 3);        /* Rad: darf nicht im Konsolenverlauf blaettern */
    thread_sleep_ms(200);
    int no_console_scroll = console_view_offset() == 0;
    feed("Y");
    thread_sleep_ms(300);
    console_clipboard_set("Q", 1);
    mouse_report(2, 0, 0, 0);        /* rechte Taste: einfuegen */
    mouse_report(0, 0, 0, 0);
    thread_sleep_ms(300);
    keyboard_deliver(19);
    keyboard_deliver(17);
    int ok6 = editor_wait(pid6);
    check("Maus: Klick setzt den Cursor, rechte Taste fuegt ein", ok6 && slurp("/disk/ED3.TXT", data, sizeof(data)) > 0 &&
                                                                  strcmp(data, "abc\ndeYQf\n") == 0);
    check("Waehrend der Editor laeuft, markiert/blaettert die Konsole nicht", no_console_sel && no_console_scroll);
    console_select_press(1, 1); /* nach dem Beenden gehoert die Maus wieder der Konsole */
    console_select_move(3 * cw, 1);
    check("Nach dem Beenden gehoert die Maus wieder der Konsole", console_has_selection());
    console_select_clear();

    fs_unlink("/disk/ED1.TXT");
    fs_unlink("/disk/ED2.TXT");
    fs_unlink("/disk/ED3.TXT");
    drain_keys(tmp, sizeof(tmp));
    console_clear();
}

static int file_is(const char *path, const char *want)
{
    static char buf[2048];
    int64_t n = slurp(path, buf, sizeof(buf));
    return n >= 0 && strcmp(buf, want) == 0;
}

void test_script(void)
{
    title("Shell-Skripte");
    if (fs_disk_volume() < 0) {
        check("Skript-Tests brauchen /disk", 0);
        return;
    }
    { /* FPU/SSE: mehr Programme als CPUs rechnen mit double/float und geben staendig die CPU ab: so wechseln sich
         * zwei Programme auf derselben CPU ab, und ohne Sichern der Register landen die Werte des einen beim anderen */
        enum { N = 8 };
        int pid[N], ok = 1;
        char cmd[N][16];
        for (int i = 0; i < N; i++) {
            ksnprintf(cmd[i], sizeof(cmd[i]), "fputest %d", 3 + 4 * i);
            pid[i] = process_spawn("/bin/fputest", cmd[i], 0);
        }
        for (int i = 0; i < N; i++) {
            int code = -1, faulted = 0;
            ok &= pid[i] > 0 && process_wait(pid[i], 0, &code, &faulted, 20000) == 0 && !faulted && code == 0;
        }
        check("FPU/SSE-Register ueberstehen Threadwechsel (8 Programme, mehr als CPUs)", ok);
    }
    int rc = run_sh("sh /etc/tests/shell.sh eins zwei drei");
    const char *want =
        "gross\nfuenf\ni=3\nw=a\nw=b\nw=c\nn=2\nsumme=20\nrechnen: 2\nargs=3\nfak5=120\nloop=1\nloop=3\nret=7\n"
        "status=127\nzeilen=2 2 10\nvorgabe=std\nlaenge=1\np=eins 3 eins zwei drei\nnach_shift=zwei\nglob=SB.TXT\n"
        "quote='5' $x\nnegiert\nread=b\nende\n";
    static char got[2048];
    slurp("/disk/SR.TXT", got, sizeof(got));
    int same = strcmp(got, want) == 0;
    if (!same) { /* bei Abweichung zeigen, was herauskam */
        kprintf("  erwartet:\n%s  erhalten:\n%s", want, got);
    }
    check("if/elif/else, while, until, for, Funktionen, Rekursion, $(...), $((...))", rc == 0 && same);
    check("Fehlerausgabe umleiten (2>, 2>>, >&2)", file_has("/disk/SE.TXT", "fehler") &&
                                                 file_has("/disk/SE.TXT", "gibtsnicht: Befehl nicht gefunden"));
    fs_unlink("/disk/SR.TXT");
    fs_unlink("/disk/SE.TXT");
    fs_unlink("/disk/SB.TXT");

    /* Syntaxfehler werden mit Zeile gemeldet, unvollstaendige Skripte erkannt */
    write_text("/disk/SF.SH", "echo a\nif true; then\necho b\n");
    int rc_incomplete = run_sh("sh /disk/SF.SH");
    write_text("/disk/SF.SH", "echo a\nfi\n");
    int rc_syntax = run_sh("sh /disk/SF.SH");
    fs_unlink("/disk/SF.SH");
    check("Unvollstaendiges Skript und Syntaxfehler: Exit-Code 2", rc_incomplete == 2 && rc_syntax == 2);
}

void test_tools(void)
{
    title("Werkzeuge");
    if (fs_disk_volume() < 0) {
        check("Werkzeug-Tests brauchen /disk", 0);
        return;
    }
    int rc = run_sh("sh /etc/tests/tools.sh");
    check("tools.sh laeuft durch", rc == 0);
    check("sort", file_is("/disk/T1.TXT", "a\na\nb\nc\n"));
    check("sort -r | uniq", file_is("/disk/T2.TXT", "c\nb\na\n"));
    check("uniq -c", file_is("/disk/T3.TXT", "      2 a\n      1 b\n      1 c\n"));
    check("sort -n", file_is("/disk/T4.TXT", "9\n10\n100\n"));
    check("diff (geaendert, hinzugefuegt, Exit-Code 1)", file_is("/disk/T5.TXT", "2c2\n< zwei\n---\n> ZWEI\n3a4\n> vier\nrc=1\n"));
    check("diff gleicher Dateien: keine Ausgabe, Exit-Code 0", file_is("/disk/T6.TXT", "rc=0\n"));
    check("find -name", file_is("/disk/T7.TXT", "FT/A.TXT\nFT/SUB/B.TXT\n"));
    check("find -type d", file_is("/disk/T8.TXT", "FT\nFT/SUB\n"));
    check("du -s -b", file_is("/disk/T9.TXT", "5        FT\n"));
    check("hexdump", file_has("/disk/TA.TXT", "00000000  41 42 43") && file_has("/disk/TA.TXT", "|ABC|") &&
                     file_has("/disk/TA.TXT", "00000003\n"));
    check("df zeigt /disk", file_has("/disk/TB.TXT", "/disk") && file_has("/disk/TB.TXT", "FAT32"));
    check("tree", file_has("/disk/TC.TXT", "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ") && file_has("/disk/TC.TXT", "1 Verzeichnisse, 2 Dateien"));
    check("cal (Februar 2026 beginnt am Sonntag, KW 5)", file_has("/disk/TD.TXT", "Februar 2026") &&
                                                       file_has("/disk/TD.TXT", " 5                     1 \n"));
    check("uptime", file_has("/disk/TE.TXT", "laeuft seit"));
    uint64_t log_size = 0;
    check("dmesg: Kernel-Log vom Start an", file_has("/disk/TF.TXT", "Kernel gestartet") &&
                                            dir_has("/disk", "TF.TXT", &log_size) && log_size > 4096);

    static const char *const files[] = {"U1", "T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "T9", "TA", "TB", "TC",
                                        "TD", "TE", "TF", "D1", "D2", 0};
    char path[32];
    for (int i = 0; files[i]; i++) {
        ksnprintf(path, sizeof(path), "/disk/%s.TXT", files[i]);
        fs_unlink(path);
    }
    fs_unlink("/disk/FT/SUB/B.TXT");
    fs_unlink("/disk/FT/SUB");
    fs_unlink("/disk/FT/A.TXT");
    fs_unlink("/disk/FT");
}

void test_graphics(void)
{
    title("Grafik fuer Programme");
    char tmp[64];
    drain_keys(tmp, sizeof(tmp));

    /* Kernel-Ebene: Konsole pausiert, Blit landet im Framebuffer, Freigeben stellt die Konsole wieder her */
    console_clear();
    for (const char *c = "Konsole"; *c; c++)
        console_putc(*c);
    uint32_t before = console_debug_fb_pixel(8 * 7 * console_scale() + 3, 8);
    int acq = console_gfx_acquire(4242) == 0;
    int busy = console_gfx_acquire(4343) == -2;
    for (const char *c = "XXXXXXXXXX"; *c; c++) /* Ausgabe waehrend des Grafikmodus: nur ins Abbild */
        console_putc(*c);
    int hidden = console_debug_fb_pixel(8 * 7 * console_scale() + 3, 8) == before;
    static uint32_t red[16];
    for (int i = 0; i < 16; i++)
        red[i] = 0xFF0000;
    console_gfx_blit(red, 4, 100, 100, 4, 4);
    int blit = console_debug_fb_pixel(101, 101) == 0xFF0000;
    console_gfx_release(4242);
    int restored = console_debug_fb_pixel(101, 101) == console_read_pixel(101, 101) && !console_gfx_owner(4242);
    console_clear();
    title("Grafik fuer Programme");
    check("Bildschirm uebernehmen (ein zweites Programm wird abgewiesen)", acq && busy);
    check("Konsolenausgabe waehrend des Grafikmodus erscheint nicht", hidden);
    check("Bild kopieren (Blit) in den Framebuffer", blit);
    check("Freigeben stellt die Konsole wieder her", restored);

    if (fs_disk_volume() < 0)
        return;
    /* paint: Bild speichern (Strg+S) und beenden (Esc) */
    fs_unlink("/disk/GT.BMP");
    int pid = process_spawn("/bin/paint", "paint /disk/GT.BMP", 0);
    thread_sleep_ms(1500);
    int during = pid > 0 && !console_gfx_owner(0);
    keyboard_deliver(19);
    thread_sleep_ms(500);
    keyboard_deliver(0x1B);
    int code = 0, faulted = 0;
    int ended = pid > 0 && process_wait(pid, 0, &code, &faulted, 8000) == 0 && code == 0 && !faulted;
    FsStat st;
    uint64_t want = 54 + (((uint64_t)console_width_px() * 3 + 3) & ~3ULL) * (console_height_px() - 52 - 18);
    check("paint: speichert ein BMP und beendet sich", during && ended && fs_stat("/disk/GT.BMP", &st) == 0 && st.size == want);

    /* view zeigt das Bild und endet mit Esc */
    pid = process_spawn("/bin/view", "view /disk/GT.BMP", 0);
    thread_sleep_ms(1500);
    keyboard_deliver(0x1B);
    check("view: Bild anzeigen und beenden", pid > 0 && process_wait(pid, 0, &code, &faulted, 8000) == 0 && code == 0 && !faulted);
    fs_unlink("/disk/GT.BMP");

    /* gltest: kleines OpenGL mit der CPU - Abschneiden an nah/fern/Schutzstreifen, Rueckseiten weglassen, Mischen */
    pid = process_spawn("/bin/gltest", "gltest", 0);
    check("gltest (3D: Abschneiden an der nahen Ebene, Rueckseiten, Mischen)",
          pid > 0 && process_wait(pid, 0, &code, &faulted, 30000) == 0 && code == 0 && !faulted);
    if (pid > 0 && code != 0)
        kprintf("  gltest: Pruefung %d fehlgeschlagen\n", code);

    /* desktop: im Terminal-Fenster laeuft eine Shell. Das Terminal ist ein eigenes Programm (desktop -> term -> sh):
     * erst tippen, wenn dessen Shell laeuft (Start dauert unterschiedlich lange) */
    fs_unlink("/disk/DT.TXT");
    pid = process_spawn("/bin/desktop", "desktop", 0);
    for (int t = 0, up = 0; t < 100 && !up; t++) {
        thread_sleep_ms(100);
        ProcInfo pi;
        for (unsigned i = 0; process_info(i, &pi) == 0 && !up; i++)
            up = pi.state == 0 && strcmp(pi.name, "sh") == 0 && pi.pid > (unsigned)pid;
    }
    thread_sleep_ms(500);
    feed("echo hallo desktop > /disk/DT.TXT\n");
    thread_sleep_ms(2500);
    static char data[64];
    int shell_ok = slurp("/disk/DT.TXT", data, sizeof(data)) > 0 && strcmp(data, "hallo desktop\n") == 0;
    process_kill_pid((uint32_t)pid);
    int killed = pid > 0 && process_wait(pid, 0, &code, &faulted, 8000) == 0;
    thread_sleep_ms(300);
    check("desktop: Terminal-Fenster fuehrt Befehle aus", shell_ok);
    check("desktop beendet: Konsole ist wieder da", killed && console_debug_fb_pixel(0, 0) == console_read_pixel(0, 0));
    fs_unlink("/disk/DT.TXT");
    drain_keys(tmp, sizeof(tmp));
}
