#include "libc.h"

/* keymap              zeigt das aktuelle Tastaturlayout
 * keymap de|uk|us     stellt es sofort um
 * keymap -p de        ... und speichert es in \cmdline.txt des Boot-Volumes (kbd=de), gilt dann auch nach dem Neustart
 * Optionen: -d PFAD   Boot-Volume selbst angeben (z.B. /mnt/usb0p1) */
void _start(int argc, char **argv)
{
    const char *name = NULL, *dir = NULL;
    int persist = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0)
            persist = 1;
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            dir = argv[++i], persist = 1;
        else if (argv[i][0] != '-')
            name = argv[i];
        else {
            fprintf(2, "keymap: unbekannte Option '%s'\n", argv[i]);
            sys_exit(2);
        }
    }

    int tty = sys_isatty(1) != 0;
    char cur[16];
    if (!name) {
        sys_keymap(NULL, cur);
        printf("Tastaturlayout: %s%s%s   (verfuegbar: us de uk; aendern mit 'keymap de', dauerhaft mit 'keymap -p de')\n",
               tty ? C_GREEN : "", cur, tty ? C_RESET : "");
        sys_exit(0);
    }
    if (sys_keymap(name, cur) != 0) {
        fprintf(2, "keymap: unbekanntes Layout '%s' (verfuegbar: us de uk)\n", name);
        sys_exit(2);
    }
    printf("Tastaturlayout jetzt: %s\n", cur);

    if (persist) {
        char dirs[4][40];
        int n;
        if (dir) {
            snprintf(dirs[0], 40, "%s", dir);
            n = 1;
        } else {
            n = find_boot_volumes(dirs, 4);
        }
        if (!n) {
            fprintf(2, "keymap: kein Boot-Volume gefunden (enthaelt \\kernel.elf und \\EFI). Mit -d PFAD angeben.\n");
            sys_exit(1);
        }
        char key_value[16];
        snprintf(key_value, sizeof(key_value), "%s", cur);
        for (int i = 0; i < n; i++) {
            int r = boot_cmdline_set(dirs[i], "kbd=", strcmp(cur, "us") == 0 ? NULL : key_value);
            if (r == 0)
                printf("%s/cmdline.txt aktualisiert (gilt auch nach dem Neustart).\n", dirs[i]);
            else
                fprintf(2, "keymap: %s/cmdline.txt konnte nicht geschrieben werden (Fehler %d)\n", dirs[i], r);
        }
    }
    sys_exit(0);
}
