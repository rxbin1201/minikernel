#include "libc.h"

/* wget: laedt eine Datei per HTTP herunter (TCP ueber den Netzwerk-Stack des Kernels).
 *   wget http://example.com/              -> index.html im aktuellen Verzeichnis
 *   wget -O bild.bmp http://host:8000/a.bmp
 *   wget -O - http://example.com/          (auf die Standardausgabe)
 *   wget -O /dev/null http://host/gross    (nichts speichern: misst nur die Geschwindigkeit des Netzes)
 *   wget -S ...                            (zeigt zusaetzlich die Kopfzeilen der Antwort)
 *   wget -q ...                            (ohne Meldungen; Fehler kommen trotzdem)
 * Folgt bis zu 5 Weiterleitungen, versteht Content-Length und "chunked". Nur http:// (kein TLS fuer https://). */

#define BUF_SIZE 65536

static char  rbuf[BUF_SIZE];
static int   rpos, rlen, sock = -1;
static int   tty, quiet, show_headers, mfd = 1; /* Meldungen auf mfd: stdout, bei -O - stderr */
static s64   rerr;

/* ---------- Gepuffertes Lesen vom Socket ---------- */

static int fill(void)
{
    rpos = 0;
    s64 n = sys_read(sock, rbuf, sizeof(rbuf));
    rlen = n > 0 ? (int)n : 0;
    if (n < 0)
        rerr = n;
    return rlen;
}

static int rd_byte(void)
{
    if (rpos == rlen && !fill())
        return -1;
    return (unsigned char)rbuf[rpos++];
}

/* Eine Zeile ohne \r\n; Laenge oder -1 am Ende */
static int rd_line(char *out, int max)
{
    int n = 0, c;
    while ((c = rd_byte()) >= 0 && c != '\n')
        if (c != '\r' && n < max - 1)
            out[n++] = (char)c;
    out[n] = 0;
    return c < 0 && n == 0 ? -1 : n;
}

/* ---------- Hilfen ---------- */

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Kopfzeile "Name: Wert" -> Wert, wenn der Name passt (ohne Gross/Klein) */
static const char *header(const char *line, const char *name)
{
    int i = 0;
    for (; name[i]; i++)
        if (lower(line[i]) != lower(name[i]))
            return 0;
    if (line[i] != ':')
        return 0;
    line += i + 1;
    while (*line == ' ' || *line == '\t')
        line++;
    return line;
}

static int contains_ci(const char *s, const char *w)
{
    int n = (int)strlen(w);
    for (; *s; s++) {
        int i = 0;
        while (i < n && lower(s[i]) == lower(w[i]))
            i++;
        if (i == n)
            return 1;
    }
    return 0;
}

static void size_str(char *out, int max, u64 b)
{
    if (b < 1024)
        snprintf(out, max, "%llu B", b);
    else if (b < 1024 * 1024)
        snprintf(out, max, "%llu.%llu KB", b / 1024, b % 1024 * 10 / 1024);
    else
        snprintf(out, max, "%llu.%llu MB", b / (1024 * 1024), b % (1024 * 1024) * 10 / (1024 * 1024));
}

static void fail(const char *msg, s64 e)
{
    if (e)
        fprintf(2, "%swget: %s: %s%s\n", tty ? C_RED : "", msg, net_strerror(e), tty ? C_RESET : "");
    else
        fprintf(2, "%swget: %s%s\n", tty ? C_RED : "", msg, tty ? C_RESET : "");
    sys_exit(1);
}

/* ---------- URL ---------- */

typedef struct {
    char     host[128];
    unsigned port;
    char     path[512];
} Url;

static int parse_url(const char *s, Url *u)
{
    if (!strncmp(s, "https://", 8)) {
        fprintf(2, "wget: https wird nicht unterstuetzt (dafuer braeuchte es TLS-Verschluesselung); nur http://\n");
        return -1;
    }
    if (!strncmp(s, "http://", 7))
        s += 7;
    int n = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && n < (int)sizeof(u->host) - 1)
        u->host[n++] = *s++;
    u->host[n] = 0;
    u->port = 80;
    if (*s == ':') {
        u->port = (unsigned)atoi(s + 1);
        while (*s && *s != '/' && *s != '?')
            s++;
    }
    if (!n || !u->port || u->port > 65535) {
        fprintf(2, "wget: ungueltige Adresse\n");
        return -1;
    }
    snprintf(u->path, sizeof(u->path), "%s%s", *s == '/' ? "" : "/", s);
    return 0;
}

/* Name der Zieldatei aus dem Pfad (letzter Teil ohne ?...), sonst index.html */
static void file_name(const Url *u, char *out, int max)
{
    char p[512];
    strcpy(p, u->path);
    char *q = strchr(p, '?');
    if (q)
        *q = 0;
    char *slash = strrchr(p, '/');
    const char *name = slash ? slash + 1 : p;
    snprintf(out, max, "%s", *name ? name : "index.html");
}

/* ---------- Herunterladen ---------- */

static int    out_fd = -1;
static char   out_name[256];
static int    to_stdout, out_named, discard;
static u64    got, total, t_start, t_last;

static void progress(int final)
{
    if (!tty || quiet)
        return;
    u64 now = (u64)sys_time_us();
    if (!final && now - t_last < 200000)
        return;
    t_last = now;
    char a[24], b[24], r[24];
    size_str(a, sizeof(a), got);
    u64 us = now - t_start ? now - t_start : 1;
    size_str(r, sizeof(r), got * 1000000 / us);
    if (total) {
        size_str(b, sizeof(b), total);
        fprintf(mfd, "\r  %s / %s (%llu%%)  %s/s   ", a, b, got * 100 / total, r);
    } else {
        fprintf(mfd, "\r  %s  %s/s   ", a, r);
    }
    if (final)
        fprintf(mfd, "\n");
}

static void put(const char *p, int n)
{
    if (discard) { /* -O /dev/null: nur zaehlen (Geschwindigkeit des Netzes ohne die der Platte) */
        got += (u64)n;
        progress(0);
        return;
    }
    if (out_fd < 0) {
        out_fd = to_stdout ? 1 : (int)sys_open(out_name, O_WRONLY | O_CREAT | O_TRUNC);
        if (out_fd < 0) {
            fprintf(2, "wget: %s kann nicht angelegt werden: %s\n", out_name,
                    out_fd == ERR_ROFS ? "nur lesbares Dateisystem (z.B. nach /disk wechseln)" :
                    out_fd == ERR_NOENT ? "Verzeichnis gibt es nicht" : out_fd == ERR_NOSPC ? "Platte voll" :
                    out_fd == -21 ? "ist ein Verzeichnis" : "Fehler");
            sys_exit(1);
        }
    }
    if (write_all(out_fd, p, (size_t)n) < 0) {
        if (to_stdout) /* der Leser hat aufgehoert (z.B. head): still beenden */
            sys_exit(1);
        fprintf(2, "\nwget: Schreiben in %s fehlgeschlagen (Platte voll?)\n", out_name);
        sys_exit(1);
    }
    got += (u64)n;
    progress(0);
}

/* n Bytes (oder bis zum Ende, n = ~0) uebernehmen; 0 = vollstaendig */
static int copy_body(u64 n)
{
    while (n) {
        if (rpos == rlen && !fill())
            return n == ~0ULL ? 0 : -1;
        u64 k = (u64)(rlen - rpos);
        if (k > n)
            k = n;
        put(rbuf + rpos, (int)k);
        rpos += (int)k;
        if (n != ~0ULL)
            n -= k;
    }
    return 0;
}

static int copy_chunked(void)
{
    char line[128];
    for (;;) {
        if (rd_line(line, sizeof(line)) < 0)
            return -1;
        u64 size = 0;
        int digits = 0;
        for (const char *p = line; *p; p++, digits++) {
            int c = lower(*p);
            if (c >= '0' && c <= '9')
                size = size * 16 + (u64)(c - '0');
            else if (c >= 'a' && c <= 'f')
                size = size * 16 + (u64)(c - 'a' + 10);
            else
                break;
        }
        if (!digits)
            return -1;
        if (!size) { /* letzter Abschnitt: noch die Schlusszeilen */
            while (rd_line(line, sizeof(line)) > 0)
                ;
            return 0;
        }
        if (copy_body(size) != 0)
            return -1;
        if (rd_line(line, sizeof(line)) < 0)
            return -1;
    }
}

/* Eine Anfrage; 0 = fertig, 1 = Weiterleitung (u ist dann das neue Ziel) */
static int fetch(Url *u)
{
    unsigned char ips[4][4];
    s64 n = sys_resolve(u->host, ips, 4);
    if (n < 0)
        fail(u->host, n);
    if (!quiet)
        fprintf(mfd, "Verbinde mit %s (%u.%u.%u.%u):%u ... ", u->host, ips[0][0], ips[0][1], ips[0][2], ips[0][3], u->port);
    sock = (int)sys_tcp_connect(ips[0], u->port, 10000);
    if (sock < 0) {
        if (!quiet)
            fprintf(mfd, "\n");
        fail("Verbindung fehlgeschlagen", sock);
    }
    if (!quiet)
        fprintf(mfd, "verbunden.\n");
    char req[800];
    int len;
    if (u->port != 80) {
        len = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: MiniKernel-wget/1.0\r\nAccept: */*\r\n"
                       "Connection: close\r\n\r\n",
                       u->path, u->host, u->port);
    } else {
        len = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: MiniKernel-wget/1.0\r\nAccept: */*\r\n"
                       "Connection: close\r\n\r\n",
                       u->path, u->host);
    }
    s64 w = sys_write(sock, req, (u64)len);
    if (w != len)
        fail("Senden der Anfrage fehlgeschlagen", w < 0 ? w : ERR_IO);

    rpos = rlen = 0;
    char line[1024], status[128], location[600] = "";
    if (rd_line(line, sizeof(line)) < 0)
        fail("keine Antwort vom Server", rerr);
    int code = 0;
    const char *sp = strchr(line, ' ');
    if (strncmp(line, "HTTP/", 5) || !sp)
        fail("keine HTTP-Antwort", 0);
    code = atoi(sp + 1);
    snprintf(status, sizeof(status), "%s", sp + 1);
    if (show_headers)
        fprintf(2, "  %s\n", line);
    int chunked = 0, have_len = 0;
    total = 0;
    while (rd_line(line, sizeof(line)) > 0) {
        if (show_headers)
            fprintf(2, "  %s\n", line);
        const char *v;
        if ((v = header(line, "Content-Length"))) {
            total = 0;
            for (; *v >= '0' && *v <= '9'; v++)
                total = total * 10 + (u64)(*v - '0');
            have_len = 1;
        } else if ((v = header(line, "Transfer-Encoding")) && contains_ci(v, "chunked")) {
            chunked = 1;
        } else if ((v = header(line, "Location"))) {
            snprintf(location, sizeof(location), "%s", v);
        }
    }
    if (!quiet)
        fprintf(mfd, "HTTP-Antwort: %s\n", status);

    if (code >= 300 && code < 400 && location[0]) {
        sys_close(sock);
        if (location[0] == '/') {
            snprintf(u->path, sizeof(u->path), "%s", location);
        } else {
            Url nu;
            if (parse_url(location, &nu) != 0)
                sys_exit(1);
            *u = nu;
        }
        if (!quiet)
            fprintf(mfd, "Weiterleitung nach %s\n", location);
        return 1;
    }
    if (code < 200 || code >= 300) {
        sys_close(sock);
        fprintf(2, "%swget: Server meldet %s%s\n", tty ? C_RED : "", status, tty ? C_RESET : "");
        sys_exit(1);
    }

    if (!out_named)
        file_name(u, out_name, sizeof(out_name));
    if (!quiet) {
        char sz[24];
        size_str(sz, sizeof(sz), total);
        fprintf(mfd, "Speichere %s %s%s%s%s\n", have_len ? sz : "(Groesse unbekannt)", to_stdout ? "auf die Standardausgabe" : "nach ",
                tty ? C_WHITE : "", to_stdout ? "" : out_name, tty ? C_RESET : "");
    }
    t_start = t_last = (u64)sys_time_us();
    got = 0;
    if (have_len && total == 0)
        put("", 0);
    int r = chunked ? copy_chunked() : copy_body(have_len ? total : ~0ULL);
    progress(1);
    sys_close(sock);
    if (out_fd > 1)
        sys_close(out_fd);
    if (r != 0 || rerr) {
        fprintf(2, "%swget: Verbindung vorzeitig beendet (%llu Bytes erhalten)%s%s%s\n", tty ? C_RED : "", got,
                rerr ? ": " : "", rerr ? net_strerror(rerr) : "", tty ? C_RESET : "");
        sys_exit(1);
    }
    if (!quiet) {
        u64 us = (u64)sys_time_us() - t_start;
        char sz[24];
        size_str(sz, sizeof(sz), got);
        fprintf(mfd, "Fertig: %s in %llu.%llu s\n", sz, us / 1000000, us / 100000 % 10);
    }
    return 0;
}

void _start(int argc, char **argv)
{
    const char *url = 0;
    tty = sys_isatty(1) != 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "-"))
                to_stdout = 1;
            discard = !strcmp(argv[i], "/dev/null");
            snprintf(out_name, sizeof(out_name), "%s", argv[i]);
            out_named = 1;
        } else if (!strcmp(argv[i], "-q")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "-S")) {
            show_headers = 1;
        } else if (!url && argv[i][0] != '-') {
            url = argv[i];
        } else {
            url = 0;
            break;
        }
    }
    if (!url) {
        fprintf(2, "Aufruf: wget [-q] [-O datei|-] [-S] http://host[:port]/pfad\n");
        sys_exit(2);
    }
    if (to_stdout) { /* Meldungen und Fortschritt nicht zwischen die Daten */
        mfd = 2;
        tty = sys_isatty(2) != 0;
    }
    Url u;
    if (parse_url(url, &u) != 0)
        sys_exit(1);
    for (int hops = 0; fetch(&u); hops++)
        if (hops == 5)
            fail("zu viele Weiterleitungen", 0);
    sys_exit(0);
}
