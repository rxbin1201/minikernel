#ifndef FDOBJ_H
#define FDOBJ_H

#include <stdint.h>
#include "fs/fs.h"

/* Objekte hinter den Datei-Deskriptoren eines Prozesses. Ein Objekt kann von mehreren Deskriptoren (dup, dup2) und
 * mehreren Prozessen (fork, geerbte stdin/stdout) geteilt werden; es wird mit einem Zaehler verwaltet. Geteilte
 * Dateiobjekte teilen sich auch die Position (wie unter Unix). */

typedef enum { FD_CONSOLE, FD_FILE, FD_PIPE_R, FD_PIPE_W, FD_UDP, FD_TCP } FdKind;

struct Pipe;
struct UdpSock;
struct TcpConn;

typedef struct FdObj {
    FdKind          kind;
    int             refs;
    FsFile          file;  /* FD_FILE */
    struct Pipe    *pipe;  /* FD_PIPE_R / FD_PIPE_W */
    struct UdpSock *udp;   /* FD_UDP (read = naechstes Datagramm, Absender geht verloren; sonst SYS_RECVFROM) */
    struct TcpConn *tcp;   /* FD_TCP (read/write: Datenstrom; close baut die Verbindung ab) */
} FdObj;

FdObj *fdobj_new_udp(struct UdpSock *s);
FdObj *fdobj_new_tcp(struct TcpConn *c);

FdObj *fdobj_new_console(void);
FdObj *fdobj_new_file(const FsFile *file);             /* kopiert den geoeffneten FsFile */
int    fdobj_new_pipe(FdObj **read_end, FdObj **write_end); /* 0 = ok */

FdObj *fdobj_ref(FdObj *o);                            /* Zaehler erhoehen, liefert o */
void   fdobj_unref(FdObj *o);                          /* Zaehler senken; bei 0 freigeben (schliesst Datei bzw. Pipe-Ende) */

/* Blockierende Aufrufe: warten in Timer-Tick-Schritten und geben ERR_INTR (-4) zurueck, wenn der Prozess per Ctrl-C/kill
 * beendet werden soll. Lesen liefert 0 am Ende (Pipe ohne Schreiber, Dateiende), Schreiben in eine Pipe ohne Leser ERR_PIPE. */
int64_t fdobj_read(FdObj *o, void *buf, uint64_t len);
int64_t fdobj_write(FdObj *o, const void *buf, uint64_t len);
int64_t fdobj_seek(FdObj *o, int64_t offset, int whence); /* nur Dateien; sonst ERR_SPIPE */
/* Pipe-Leseende und TCP: Zahl der wartenden Bytes, 0 = noch nichts, -1 = Ende (kein Schreiber mehr bzw. geschlossen). Pipe-Schreibende: freier
 * Platz, -1 = kein Leser mehr. Dateien: 1, Konsole: 0 */
int64_t fdobj_available(FdObj *o);

#endif
