#ifndef UTIL_H
#define UTIL_H

/* Kleine Helfer fuer die Werkzeuge (nach libc.h und malloc.h einbinden). */

#include "libc.h"
#include "malloc.h"

/* Liest alles aus fd in einen neuen Puffer (mit abschliessender 0). NULL bei Speichermangel. */
char *read_fd_all(int fd, u64 *len);
/* Datei (oder "-"/NULL = stdin) ganz lesen; Fehler werden gemeldet (prog: Programmname) */
char *read_file_all(const char *prog, const char *path, u64 *len);
/* Zerlegt den Puffer in Zeilen (ersetzt \n durch 0, entfernt \r). Ein abschliessendes \n ergibt keine leere Zeile. */
char **split_lines(char *buf, u64 len, int *count);
/* 1536 -> "1,5K", 5000000 -> "4,8M" */
void fmt_size(u64 b, char *out, int max);
/* Platzhalter: * ? [abc] [a-z] [!x] */
int glob_match(const char *p, const char *s);
/* Pfad aus Verzeichnis und Name */
void join_path(char *out, int max, const char *dir, const char *name);
/* Gepufferte Ausgabe auf stdout (am Ende out_flush aufrufen) */
void out_flush(void);
void out_write(const char *s, u64 n);
void out_str(const char *s);
void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
