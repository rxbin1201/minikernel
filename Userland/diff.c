#include "util.h"

/* diff [-u] alt neu: zeigt die Unterschiede zweier Textdateien zeilenweise.
 *   ohne -u: klassisch ("3c3", "< alt", "---", "> neu"); mit -u: vereinheitlicht ("-alt", "+neu", 3 Zeilen Umfeld)
 * Exit-Code 0 = gleich, 1 = verschieden, 2 = Fehler. Rechnet ueber die laengste gemeinsame Teilfolge (bis etwa
 * 4000 x 4000 Zeilen). */
static char **A, **B;
static int    na, nb, tty;

typedef struct {
    char op;   /* ' ' gleich, '-' nur in A, '+' nur in B */
    int  a, b; /* Zeilenindex in A bzw. B */
} Edit;

static Edit *ed;
static int   ned;

static void color(const char *c)
{
    if (tty)
        out_str(c);
}

static void print_classic(void)
{
    for (int i = 0; i < ned;) {
        if (ed[i].op == ' ') {
            i++;
            continue;
        }
        int j = i, dels = 0, adds = 0, a0 = -1, b0 = -1;
        while (j < ned && ed[j].op != ' ') {
            if (ed[j].op == '-') {
                if (a0 < 0) a0 = ed[j].a;
                dels++;
            } else {
                if (b0 < 0) b0 = ed[j].b;
                adds++;
            }
            j++;
        }
        /* Position vor dem Block (fuer 'a' und 'd') */
        int apos = i > 0 ? ed[i - 1].a + 1 : 0, bpos = i > 0 ? ed[i - 1].b + 1 : 0;
        char ra[32], rb[32];
        if (dels > 1) snprintf(ra, sizeof(ra), "%d,%d", a0 + 1, a0 + dels);
        else if (dels == 1) snprintf(ra, sizeof(ra), "%d", a0 + 1);
        else snprintf(ra, sizeof(ra), "%d", apos);
        if (adds > 1) snprintf(rb, sizeof(rb), "%d,%d", b0 + 1, b0 + adds);
        else if (adds == 1) snprintf(rb, sizeof(rb), "%d", b0 + 1);
        else snprintf(rb, sizeof(rb), "%d", bpos);
        color(C_CYAN);
        out_printf("%s%c%s\n", ra, dels && adds ? 'c' : dels ? 'd' : 'a', rb);
        color(C_RESET);
        for (int k = i; k < j; k++) {
            if (ed[k].op == '-') {
                color(C_RED);
                out_printf("< %s\n", A[ed[k].a]);
                color(C_RESET);
            }
        }
        if (dels && adds)
            out_str("---\n");
        for (int k = i; k < j; k++) {
            if (ed[k].op == '+') {
                color(C_GREEN);
                out_printf("> %s\n", B[ed[k].b]);
                color(C_RESET);
            }
        }
        i = j;
    }
}

static void print_unified(const char *fa, const char *fb)
{
    color(C_BOLD);
    out_printf("--- %s\n+++ %s\n", fa, fb);
    color(C_RESET);
    const int ctx = 3;
    for (int i = 0; i < ned;) {
        if (ed[i].op == ' ') {
            i++;
            continue;
        }
        int s = i - ctx < 0 ? 0 : i - ctx, e = i;
        for (;;) { /* Block erweitern, solange die naechste Aenderung nahe genug ist */
            while (e < ned && ed[e].op != ' ')
                e++;
            int gap = e;
            while (gap < ned && ed[gap].op == ' ' && gap - e < 2 * ctx)
                gap++;
            if (gap < ned && ed[gap].op != ' ' && gap - e < 2 * ctx) {
                e = gap;
                continue;
            }
            e = e + ctx > ned ? ned : e + ctx;
            break;
        }
        int a0 = -1, b0 = -1, ca = 0, cb = 0;
        for (int k = s; k < e; k++) {
            if (ed[k].op != '+') {
                if (a0 < 0) a0 = ed[k].a;
                ca++;
            }
            if (ed[k].op != '-') {
                if (b0 < 0) b0 = ed[k].b;
                cb++;
            }
        }
        color(C_CYAN);
        out_printf("@@ -%d,%d +%d,%d @@\n", a0 < 0 ? 0 : a0 + 1, ca, b0 < 0 ? 0 : b0 + 1, cb);
        color(C_RESET);
        for (int k = s; k < e; k++) {
            if (ed[k].op == '-') {
                color(C_RED);
                out_printf("-%s\n", A[ed[k].a]);
                color(C_RESET);
            } else if (ed[k].op == '+') {
                color(C_GREEN);
                out_printf("+%s\n", B[ed[k].b]);
                color(C_RESET);
            } else {
                out_printf(" %s\n", A[ed[k].a]);
            }
        }
        i = e;
    }
}

void _start(int argc, char **argv)
{
    int unified = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-u") == 0) {
        unified = 1;
        i++;
    }
    if (argc - i != 2) {
        fprintf(2, "Aufruf: diff [-u] alt neu\n");
        sys_exit(2);
    }
    tty = sys_isatty(1) != 0;
    u64 la, lb;
    char *ba = read_file_all("diff", argv[i], &la), *bb = ba ? read_file_all("diff", argv[i + 1], &lb) : 0;
    if (!ba || !bb)
        sys_exit(2);
    A = split_lines(ba, la, &na);
    B = split_lines(bb, lb, &nb);

    /* gleicher Anfang und gleiches Ende brauchen keine Tabelle */
    int pre = 0;
    while (pre < na && pre < nb && strcmp(A[pre], B[pre]) == 0)
        pre++;
    int suf = 0;
    while (suf < na - pre && suf < nb - pre && strcmp(A[na - 1 - suf], B[nb - 1 - suf]) == 0)
        suf++;
    int m = na - pre - suf, n = nb - pre - suf;
    if ((u64)(m + 1) * (u64)(n + 1) > 16000000ULL) {
        fprintf(2, "diff: Dateien zu gross (%d x %d verschiedene Zeilen)\n", m, n);
        sys_exit(2);
    }
    /* L[i][j] = Laenge der laengsten gemeinsamen Teilfolge von A[pre+i..] und B[pre+j..] */
    unsigned *L = u_malloc(sizeof(unsigned) * (u64)(m + 1) * (u64)(n + 1));
    ed = u_malloc(sizeof(Edit) * (u64)(na + nb + 1));
    if (!L || !ed) {
        fprintf(2, "diff: kein Speicher\n");
        sys_exit(2);
    }
#define LL(x, y) L[(u64)(x) * (u64)(n + 1) + (u64)(y)]
    for (int x = m; x >= 0; x--) {
        for (int y = n; y >= 0; y--) {
            if (x == m || y == n)
                LL(x, y) = 0;
            else if (strcmp(A[pre + x], B[pre + y]) == 0)
                LL(x, y) = LL(x + 1, y + 1) + 1;
            else
                LL(x, y) = LL(x + 1, y) > LL(x, y + 1) ? LL(x + 1, y) : LL(x, y + 1);
        }
    }
    for (int k = 0; k < pre; k++)
        ed[ned++] = (Edit){' ', k, k};
    int x = 0, y = 0;
    while (x < m || y < n) {
        if (x < m && y < n && strcmp(A[pre + x], B[pre + y]) == 0) {
            ed[ned++] = (Edit){' ', pre + x, pre + y};
            x++;
            y++;
        } else if (y < n && (x == m || LL(x, y + 1) >= LL(x + 1, y))) {
            ed[ned++] = (Edit){'+', pre + x, pre + y};
            y++;
        } else {
            ed[ned++] = (Edit){'-', pre + x, pre + y};
            x++;
        }
    }
    for (int k = 0; k < suf; k++)
        ed[ned++] = (Edit){' ', na - suf + k, nb - suf + k};

    int differs = m || n;
    if (differs) {
        if (unified)
            print_unified(argv[i], argv[i + 1]);
        else
            print_classic();
    }
    out_flush();
    sys_exit(differs ? 1 : 0);
}
