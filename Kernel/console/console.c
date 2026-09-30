#include "console/console.h"
#include "arch/x86_64/cpu.h"
#include "console/font.h"
#include "mm/heap.h"
#include "lib/kprintf.h"
#include "core/sched.h"
#include "lib/string.h"
#include "lib/utf8.h"

/* Der Framebuffer ist uncached bzw. write-combining gemappt: Lesen daraus ist sehr langsam, viele kleine Schreibzugriffe
 * sind es auch. Deshalb gibt es (nach console_enable_shadow) ein Abbild des Bildschirms im normalen RAM: gezeichnet
 * wird dort und in den Framebuffer, gescrollt wird im RAM, und der Framebuffer wird danach in einem Rutsch
 * beschrieben. Aus dem Framebuffer wird nie mehr gelesen.
 *
 * Zusaetzlich merkt sich die Konsole den Text (Zeichen + Farben je Zelle): fuer den Verlauf. Aus dem Bildschirm
 * herausgescrollte Zeilen landen in einem Ringpuffer; mit Shift+Bild hoch/runter blaettert man darin (die Ausgabe
 * springt bei neuem Text von selbst zurueck zum Ende). Bei sehr hohen Aufloesungen wird die Schrift verdoppelt. */

#define MAX_COLS 640
#define MAX_ROWS 200
#define HIST_BYTES (4u << 20) /* Platz fuer den Verlauf */
#define HIST_MAX_LINES 5000

typedef struct {
    uint16_t ch;     /* Unicode-Zeichen (BMP), 0 = leer */
    uint8_t  fg, bg; /* Index in color_tab */
} Cell;

static volatile uint32_t *fb;
static uint32_t *shadow;        /* gleiche Zeilenlaenge (pitch) wie der Framebuffer, oder NULL */
static volatile int gfx_mode;   /* ein Programm zeichnet selbst: die Konsole schreibt nur noch ins Abbild */
static uint32_t     gfx_owner;  /* PID dieses Programms */
static uint32_t pitch;          /* Pixel pro Zeile */
static uint32_t width_px, height_px;
static uint32_t cols, rows;     /* Zeichen pro Zeile / Anzahl Zeilen */
static uint32_t scale = 1;      /* Schriftvergroesserung: Zelle = (8 * scale) x (16 * scale) Pixel */
static uint32_t cw = FONT_WIDTH, chh = FONT_HEIGHT;
static uint32_t cx, cy;
#define DEF_FG 0x00C0C0C0u
#define DEF_BG 0x00000000u
static uint32_t fg = DEF_FG, bg = DEF_BG;
static uint32_t def_fg = DEF_FG, def_bg = DEF_BG;

/* ANSI-Escape-Folgen: ESC [ Zahlen ; ... Endzeichen. Unterstuetzt werden m (Farben), J (2J = Bildschirm loeschen),
 * H (Cursor setzen) und K (Zeile ab Cursor loeschen). Alles andere wird verschluckt. */
static const uint32_t palette[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};
static int esc_state;            /* 0 = normal, 1 = nach ESC, 2 = in der Folge */
static uint32_t u8_cp;           /* UTF-8: bisher gelesene Bits des Zeichens */
static int u8_need;              /* noch fehlende Folgebytes */
static int esc_params[8], esc_np;
static int sgr_fg = -1, sgr_bg = -1, sgr_bold, sgr_rev;

/* ---------- Mauszeiger ---------- */

/* Der Zeiger wird nur in den Framebuffer gezeichnet, nie ins Abbild: "Zeiger weg" heisst, die Pixel aus dem Abbild
 * zurueckzukopieren. Zeichnet die Konsole darunter, wird der Zeiger danach neu aufgesetzt. */
#define CUR_W 12
#define CUR_H 19
static const char *const cursor_sprite[CUR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ",
    "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ",
    "X    X..X   ", "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};
static int32_t  cur_x, cur_y;
static int      cur_wanted, cur_drawn;
static uint32_t cur_s = 1;
static void   (*tick_hook)(void);

static void cursor_hide(void)
{
    if (!cur_drawn || !shadow || gfx_mode)
        return;
    for (uint32_t y = 0; y < CUR_H * cur_s; y++) {
        int32_t py = cur_y + (int32_t)y;
        if (py < 0 || (uint32_t)py >= height_px)
            continue;
        for (uint32_t x = 0; x < CUR_W * cur_s; x++) {
            int32_t px = cur_x + (int32_t)x;
            if (px < 0 || (uint32_t)px >= width_px)
                continue;
            uint64_t i = (uint64_t)py * pitch + (uint32_t)px;
            fb[i] = shadow[i];
        }
    }
    cur_drawn = 0;
}

static void cursor_show(void)
{
    if (!cur_wanted || !shadow || gfx_mode)
        return;
    cur_s = scale;
    for (uint32_t y = 0; y < CUR_H * cur_s; y++) {
        int32_t py = cur_y + (int32_t)y;
        if (py < 0 || (uint32_t)py >= height_px)
            continue;
        const char *row = cursor_sprite[y / cur_s];
        for (uint32_t x = 0; x < CUR_W * cur_s; x++) {
            char c = row[x / cur_s];
            int32_t px = cur_x + (int32_t)x;
            if (c == ' ' || px < 0 || (uint32_t)px >= width_px)
                continue;
            fb[(uint64_t)py * pitch + (uint32_t)px] = c == 'X' ? 0x000000u : 0xFFFFFFu;
        }
    }
    cur_drawn = 1;
}

/* Setzt den Zeiger (Pixelposition der Spitze) bzw. blendet ihn aus */
void console_cursor_set(int x, int y, int visible)
{
    uint64_t f = irq_save();
    cursor_hide();
    cur_x = x;
    cur_y = y;
    cur_wanted = visible;
    cursor_show();
    irq_restore(f);
}

/* ---------- Grafikmodus: ein Programm zeichnet den ganzen Bildschirm ---------- */

int console_gfx_acquire(uint32_t pid)
{
    if (!fb || !shadow)
        return -1;
    if (gfx_mode && gfx_owner != pid)
        return -2; /* belegt */
    uint64_t f = irq_save();
    cursor_hide();
    gfx_mode = 1;
    gfx_owner = pid;
    irq_restore(f);
    return 0;
}

void console_gfx_release(uint32_t pid)
{
    if (!gfx_mode || gfx_owner != pid)
        return;
    uint64_t f = irq_save();
    gfx_mode = 0;
    gfx_owner = 0;
    /* Konsole wiederherstellen: das Abbild enthaelt alles, was inzwischen ausgegeben wurde */
    memmove((void *)fb, shadow, (uint64_t)height_px * pitch * sizeof(uint32_t));
    cursor_show();
    irq_restore(f);
}

int console_gfx_owner(uint32_t pid)
{
    return gfx_mode && gfx_owner == pid;
}

/* Kopiert ein Rechteck aus einem Puffer (32 Bit je Pixel, Zeilenlaenge src_pitch Pixel) in den Framebuffer */
void console_gfx_blit(const uint32_t *src, uint32_t src_pitch, int x, int y, int w, int h)
{
    if (!gfx_mode)
        return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > (int)width_px ? (int)width_px : x + w, y1 = y + h > (int)height_px ? (int)height_px : y + h;
    for (int yy = y0; yy < y1; yy++) {
        const uint32_t *s = src + (uint64_t)(yy - y) * src_pitch + (x0 - x);
        volatile uint32_t *d = fb + (uint64_t)yy * pitch + x0;
        memcpy((void *)d, s, (uint64_t)(x1 - x0) * 4);
    }
}

void console_set_tick(void (*hook)(void))
{
    tick_hook = hook;
}

uint32_t console_debug_fb_pixel(uint32_t x, uint32_t y)
{
    return fb[(uint64_t)y * pitch + x];
}

/* ---------- Farbtabelle und Textzellen ---------- */

static uint32_t color_tab[256];
static int      ncolors;
static uint8_t  cur_fg_i, cur_bg_i;
static int      colors_dirty = 1;

static uint8_t color_index(uint32_t rgb)
{
    if (!ncolors) {
        for (int i = 0; i < 16; i++)
            color_tab[i] = palette[i];
        ncolors = 16;
    }
    for (int i = 0; i < ncolors; i++)
        if (color_tab[i] == rgb)
            return (uint8_t)i;
    if (ncolors < 256) {
        color_tab[ncolors] = rgb;
        return (uint8_t)ncolors++;
    }
    return 0;
}

static void update_color_idx(void)
{
    cur_fg_i = color_index(fg);
    cur_bg_i = color_index(bg);
    colors_dirty = 0;
}

static Cell cells[MAX_COLS * MAX_ROWS]; /* der aktuelle Bildschirm als Text */

/* Verlauf: Ringpuffer aus Zeilen zu je `cols` Zellen */
static Cell    *hist;
static uint32_t hist_cap, hist_count, hist_head;
static uint64_t view_off;                /* 0 = Ende (live), sonst Zeilen zurueckgeblaettert */
static uint64_t hist_dropped;            /* Zeilen, die ganz verloren sind (aus dem Verlauf gefallen): Nummer der aeltesten = hist_dropped */

/* Markierung mit der Maus: von (a_id, a_col) bis (b_id, b_col), beide Enden eingeschlossen. id = Zeilennummer wie oben. */
static int      sel_active, sel_dragging;
static uint64_t sel_a_id, sel_b_id;
static uint32_t sel_a_col, sel_b_col;
static char     clip[16384];              /* Zwischenablage (UTF-8) */
static uint32_t clip_len;

/* Liegt die Zelle in der Markierung? */
static int in_sel(uint64_t id, uint32_t col)
{
    if (!sel_active)
        return 0;
    uint64_t i1 = sel_a_id, i2 = sel_b_id;
    uint32_t c1 = sel_a_col, c2 = sel_b_col;
    if (i1 > i2 || (i1 == i2 && c1 > c2)) {
        uint64_t ti = i1; i1 = i2; i2 = ti;
        uint32_t tc = c1; c1 = c2; c2 = tc;
    }
    if (id < i1 || id > i2)
        return 0;
    if (id == i1 && col < c1)
        return 0;
    if (id == i2 && col > c2)
        return 0;
    return 1;
}

/* Nummer der Bildschirmzeile row im gerade sichtbaren Bild */
static uint64_t row_id(uint32_t row)
{
    return hist_dropped + hist_count - view_off + row;
}
static volatile int64_t view_req;        /* angeforderte Blaetter-Schritte (Tastatur-Interrupt/USB-Thread -> Konsolen-Thread) */

static void sgr_apply(void)
{
    uint32_t f = sgr_fg < 0 ? def_fg : palette[sgr_bold && sgr_fg < 8 ? sgr_fg + 8 : sgr_fg];
    uint32_t b = sgr_bg < 0 ? def_bg : palette[sgr_bg];
    fg = sgr_rev ? b : f;
    bg = sgr_rev ? f : b;
    colors_dirty = 1;
}

/* Zeichnet ein Zeichen mit den angegebenen Farben in Framebuffer und Abbild */
static void paint(uint32_t col, uint32_t row, uint32_t cp, uint32_t fgc, uint32_t bgc)
{
    const unsigned char *g = font_glyph(cp >= 0x80 && cp < 0xA0 ? '?' : cp); /* C1-Steuerzeichen: '?' */
    uint64_t base = (uint64_t)row * chh * pitch + (uint64_t)col * cw;
    for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
        for (uint32_t sy = 0; sy < scale; sy++) {
            uint64_t line = base + (uint64_t)(y * scale + sy) * pitch;
            volatile uint32_t *dst = gfx_mode ? 0 : fb + line; /* Grafikmodus: nur ins Abbild */
            uint32_t *sdst = shadow ? shadow + line : 0;
            for (uint32_t x = 0; x < FONT_WIDTH; x++) {
                uint32_t px = (g[y] & (0x80 >> x)) ? fgc : bgc;
                for (uint32_t sx = 0; sx < scale; sx++) {
                    if (dst)
                        dst[x * scale + sx] = px;
                    if (sdst)
                        sdst[x * scale + sx] = px;
                }
            }
        }
    }
}

static void draw_glyph(uint32_t col, uint32_t row, uint32_t cp)
{
    if (col >= cols || row >= rows)
        return; /* ausserhalb des Textfelds: paint() wuerde hinter Framebuffer und Abbild schreiben */
    if (colors_dirty)
        update_color_idx();
    if (view_off)
        ; /* im Verlauf geblaettert: nur den Text merken, der Bildschirm zeigt gerade Aelteres */
    else if (sel_active && in_sel(row_id(row), col))
        paint(col, row, cp, bg, fg); /* markiert: Farben vertauscht */
    else
        paint(col, row, cp, fg, bg);
    if (col < cols && row < rows) {
        Cell *c = &cells[row * cols + col];
        c->ch = cp > 0xFFFF ? 0xFFFD : (uint16_t)cp;
        c->fg = cur_fg_i;
        c->bg = cur_bg_i;
    }
    if (cur_drawn) /* der Zeiger liegt vielleicht ueber dieser Zelle */
        cursor_show();
}

/* Fuellt `count` Textzeilen ab `first_row` mit der Hintergrundfarbe (Framebuffer, Abbild und Text) */
static void clear_rows(uint32_t first_row, uint32_t count)
{
    uint64_t start = (uint64_t)first_row * chh * pitch;
    uint64_t n = view_off ? 0 : (uint64_t)count * chh * pitch; /* im Verlauf: nur die Zellen */
    for (uint64_t i = 0; i < n; i++) {
        if (!gfx_mode)
            fb[start + i] = bg;
        if (shadow)
            shadow[start + i] = bg;
    }
    if (colors_dirty)
        update_color_idx();
    for (uint32_t r = first_row; r < first_row + count && r < rows; r++)
        for (uint32_t c = 0; c < cols; c++) {
            Cell *cell = &cells[r * cols + c];
            cell->ch = 0;
            cell->fg = cur_fg_i;
            cell->bg = cur_bg_i;
        }
    if (cur_drawn)
        cursor_show();
}

static void render_all(void);

/* Legt die obersten k Textzeilen in den Verlauf */
static void history_push(uint32_t k)
{
    if (!hist) {
        hist_dropped += k;
        return;
    }
    for (uint32_t r = 0; r < k; r++) {
        uint32_t slot;
        if (hist_count == hist_cap) {
            slot = hist_head;
            hist_head = (hist_head + 1) % hist_cap;
            hist_dropped++;
        } else {
            slot = (hist_head + hist_count) % hist_cap;
            hist_count++;
        }
        memcpy(hist + (uint64_t)slot * cols, cells + (uint64_t)r * cols, cols * sizeof(Cell));
    }
}

/* Scrollt um ein Viertel des Bildschirms statt um eine Zeile: das Umkopieren des ganzen Bildschirms kostet
 * bei jedem Mal viel, so passiert es nur alle rows/4 Zeilen. */
static void scroll(void)
{
    uint32_t k = rows / 4 ? rows / 4 : 1;
    history_push(k);
    memmove(cells, cells + (uint64_t)k * cols, (uint64_t)(rows - k) * cols * sizeof(Cell));

    if (view_off) { /* im Verlauf geblaettert: dieselben Zeilen weiter zeigen (sie ruecken im Verlauf nach hinten) */
        if (colors_dirty)
            update_color_idx();
        for (uint32_t r = rows - k; r < rows; r++)
            for (uint32_t c = 0; c < cols; c++) {
                Cell *cell = &cells[r * cols + c];
                cell->ch = 0;
                cell->fg = cur_fg_i;
                cell->bg = cur_bg_i;
            }
        view_off += k;
        if (view_off > hist_count)
            view_off = hist_count;
        cy = rows - k;
        render_all();
        return;
    }

    uint64_t keep_bytes = (uint64_t)(rows - k) * chh * pitch * sizeof(uint32_t);
    uint64_t shift = (uint64_t)k * chh * pitch;

    if (shadow) {
        memmove(shadow, shadow + shift, keep_bytes);
        for (uint64_t i = keep_bytes / 4; i < (uint64_t)rows * chh * pitch; i++)
            shadow[i] = bg;
        /* Ganzes Abbild in den Framebuffer schreiben (64-Bit-Zugriffe, nur schreiben) */
        if (!gfx_mode)
            memmove((void *)fb, shadow, (uint64_t)rows * chh * pitch * sizeof(uint32_t));
        if (colors_dirty)
            update_color_idx();
        for (uint32_t r = rows - k; r < rows; r++)
            for (uint32_t c = 0; c < cols; c++) {
                Cell *cell = &cells[r * cols + c];
                cell->ch = 0;
                cell->fg = cur_fg_i;
                cell->bg = cur_bg_i;
            }
    } else {
        memmove((void *)fb, (const void *)(fb + shift), keep_bytes);
        clear_rows(rows - k, k);
    }
    cy = rows - k;
    if (cur_drawn) /* das Umkopieren aus dem Abbild hat den Zeiger uebermalt */
        cursor_show();
}

/* Berechnet Zellgroesse und Textfeld neu (nach Aufloesungs- oder Schriftaenderung) */
static void set_layout(uint32_t s)
{
    scale = s < 1 ? 1 : s > 4 ? 4 : s;
    cw = FONT_WIDTH * scale;
    chh = FONT_HEIGHT * scale;
    cols = width_px / cw;
    rows = height_px / chh;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;
    cx = cy = 0;
    view_off = 0;
    hist_count = hist_head = 0;
    hist_dropped = 0;
    sel_active = sel_dragging = 0;
    if (hist) { /* Zeilenlaenge hat sich geaendert: Verlauf neu anlegen */
        kfree(hist);
        hist = 0;
        hist_cap = 0;
    }
    if (shadow) { /* Heap ist bereit */
        hist_cap = HIST_BYTES / (cols * (uint32_t)sizeof(Cell));
        if (hist_cap > HIST_MAX_LINES)
            hist_cap = HIST_MAX_LINES;
        hist = kmalloc((uint64_t)hist_cap * cols * sizeof(Cell));
        if (!hist)
            hist_cap = 0;
    }
}

void console_init(const BootFramebuffer *info)
{
    fb        = (volatile uint32_t *)info->base;
    pitch     = info->pixels_per_line;
    width_px  = info->width;
    height_px = info->height;
    set_layout(width_px >= 2400 && height_px >= 1300 ? 2 : 1); /* 4K und aehnlich: doppelte Schrift */
    console_clear();
}

/* Legt das RAM-Abbild an (braucht den Heap). Der aktuelle Bildschirminhalt wird einmal aus dem Framebuffer uebernommen. */
void console_enable_shadow(void)
{
    if (shadow || !fb)
        return;
    uint64_t bytes = (uint64_t)pitch * height_px * sizeof(uint32_t);
    shadow = kmalloc(bytes);
    if (!shadow)
        return;
    /* Wortweise (64 Bit) kopieren; memmove wuerde hier rueckwaerts byteweise laufen (Ziel liegt hinter der Quelle) */
    const volatile uint64_t *src = (const volatile uint64_t *)fb;
    uint64_t *dst = (uint64_t *)shadow;
    for (uint64_t i = 0; i < bytes / 8; i++)
        dst[i] = src[i];

    /* Verlauf anlegen (die bisherigen Textzellen bleiben) */
    hist_cap = HIST_BYTES / (cols * (uint32_t)sizeof(Cell));
    if (hist_cap > HIST_MAX_LINES)
        hist_cap = HIST_MAX_LINES;
    hist = kmalloc((uint64_t)hist_cap * cols * sizeof(Cell));
    if (!hist)
        hist_cap = 0;
    hist_count = hist_head = 0;
    hist_dropped = 0;
    sel_active = sel_dragging = 0;
}

void console_set_scale(uint32_t s)
{
    if (!fb || s == scale)
        return;
    uint64_t f = irq_save();
    set_layout(s);
    console_clear();
    irq_restore(f);
}

uint32_t console_scale(void)
{
    return scale;
}

void console_clear(void)
{
    if (!fb)
        return;
    uint64_t f = irq_save();
    view_off = 0;
    sel_active = sel_dragging = 0;
    clear_rows(0, rows);
    cx = cy = 0;
    irq_restore(f);
}

void console_set_color(uint32_t new_fg, uint32_t new_bg)
{
    fg = def_fg = new_fg;
    bg = def_bg = new_bg;
    sgr_fg = sgr_bg = -1;
    sgr_bold = sgr_rev = 0;
    colors_dirty = 1;
}

uint32_t console_cols(void)
{
    return cols;
}

void console_get_cursor(uint32_t *col, uint32_t *row)
{
    *col = cx;
    *row = cy;
}

uint32_t console_rows(void)
{
    return rows;
}

uint32_t console_width_px(void)  { return width_px; }
uint32_t console_height_px(void) { return height_px; }

uint32_t console_read_pixel(uint32_t x, uint32_t y)
{
    return shadow ? shadow[(uint64_t)y * pitch + x] : fb[(uint64_t)y * pitch + x];
}

/* ---------- Verlauf (Blaettern) ---------- */

static const Cell *line_cells(uint64_t idx) /* idx: 0 = aelteste Verlaufszeile, dahinter der Bildschirm */
{
    if (idx < hist_count)
        return hist + (uint64_t)((hist_head + idx) % hist_cap) * cols;
    return cells + (idx - hist_count) * cols;
}

/* Zeichnet alles aus den Textzellen neu (Verlauf oder Bildschirm, je nach view_off) */
static void render_all(void)
{
    uint64_t start = hist_count - view_off;
    for (uint32_t r = 0; r < rows; r++) {
        const Cell *src = line_cells(start + r);
        uint64_t id = hist_dropped + start + r;
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t f = color_tab[src[c].fg], b = color_tab[src[c].bg];
            if (sel_active && in_sel(id, c)) {
                uint32_t t = f; f = b; b = t;
            }
            paint(c, r, src[c].ch ? src[c].ch : ' ', f, b);
        }
    }
    if (view_off) { /* Hinweis oben rechts */
        char msg[80];
        int n = ksnprintf(msg, sizeof(msg), " Verlauf -%lu Zeilen  (Shift+Bild runter / Ende = zurueck) ", (unsigned long)view_off);
        if ((uint32_t)n + 1 > cols)
            n = ksnprintf(msg, sizeof(msg), " -%lu ", (unsigned long)view_off);
        for (int i = 0; i < n && (uint32_t)n <= cols; i++)
            paint(cols - (uint32_t)n + (uint32_t)i, 0, (unsigned char)msg[i], 0x000000, palette[11]);
    }
    if (cur_drawn)
        cursor_show();
}

void console_view_live(void)
{
    if (!fb || !view_off)
        return;
    view_off = 0;
    render_all();
}

void console_view_scroll(int64_t delta)
{
    if (!fb || !hist_cap)
        return;
    int64_t target = (int64_t)view_off + delta;
    if (target < 0)
        target = 0;
    if (target > (int64_t)hist_count)
        target = (int64_t)hist_count;
    if ((uint64_t)target == view_off)
        return;
    view_off = (uint64_t)target;
    render_all();
}

static volatile int live_req;

void console_live_request(void)
{
    if (view_off)
        live_req = 1;
}

void console_scroll_request(int lines)
{
    __atomic_fetch_add(&view_req, lines, __ATOMIC_RELAXED);
}

static void console_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t r = __atomic_exchange_n(&view_req, 0, __ATOMIC_RELAXED);
        if (r || live_req) {
            uint64_t f = irq_save();
            if (live_req) {
                live_req = 0;
                console_view_live();
            }
            if (r)
                console_view_scroll(r);
            irq_restore(f);
        }
        if (tick_hook)
            tick_hook();
        thread_sleep_ms(10);
    }
}

void console_start_thread(void)
{
    thread_create("console", console_thread, 0);
}

/* Pixel -> Zelle im sichtbaren Bild */
static void pixel_cell(int x, int y, uint64_t *id, uint32_t *col)
{
    int c = x < 0 ? 0 : x / (int)cw, r = y < 0 ? 0 : y / (int)chh;
    if (c >= (int)cols)
        c = (int)cols - 1;
    if (r >= (int)rows)
        r = (int)rows - 1;
    *col = (uint32_t)c;
    *id = row_id((uint32_t)r);
}

static void selection_changed(void)
{
    render_all();
}

void console_select_press(int x, int y)
{
    if (!fb)
        return;
    uint64_t f = irq_save();
    int had = sel_active;
    pixel_cell(x, y, &sel_a_id, &sel_a_col);
    sel_b_id = sel_a_id;
    sel_b_col = sel_a_col;
    sel_active = 0; /* ein einfacher Klick markiert nichts (und hebt eine alte Markierung auf) */
    sel_dragging = 1;
    if (had)
        selection_changed();
    irq_restore(f);
}

void console_select_move(int x, int y)
{
    if (!fb || !sel_dragging)
        return;
    uint64_t f = irq_save();
    uint64_t id;
    uint32_t col;
    pixel_cell(x, y, &id, &col);
    int active = id != sel_a_id || col != sel_a_col;
    if (id != sel_b_id || col != sel_b_col || active != sel_active) {
        sel_b_id = id;
        sel_b_col = col;
        sel_active = active;
        selection_changed();
    }
    irq_restore(f);
}

/* Markiert in der Zeile unter (x, y) die Zellen from..to und zeichnet neu */
static void select_cells(uint64_t id, uint32_t from, uint32_t to)
{
    sel_a_id = sel_b_id = id;
    sel_a_col = from;
    sel_b_col = to;
    sel_active = 1;
    sel_dragging = 0;
    selection_changed();
}

static int is_word_char(uint16_t ch)
{
    return ch && ch != ' ' && ch != '\t';
}

/* Doppelklick: das Wort unter dem Zeiger (alles zwischen Leerzeichen, also auch Pfade wie /mnt/usb0/a.txt) */
void console_select_word(int x, int y)
{
    if (!fb)
        return;
    uint64_t f = irq_save();
    uint64_t id;
    uint32_t col;
    pixel_cell(x, y, &id, &col);
    if (id >= hist_dropped && id - hist_dropped < hist_count + rows) {
        const Cell *line = line_cells(id - hist_dropped);
        if (is_word_char(line[col].ch)) {
            uint32_t a = col, b = col;
            while (a > 0 && is_word_char(line[a - 1].ch))
                a--;
            while (b + 1 < cols && is_word_char(line[b + 1].ch))
                b++;
            select_cells(id, a, b);
        }
    }
    irq_restore(f);
}

/* Dreifachklick: die ganze Zeile */
void console_select_line(int x, int y)
{
    if (!fb)
        return;
    uint64_t f = irq_save();
    uint64_t id;
    uint32_t col;
    pixel_cell(x, y, &id, &col);
    select_cells(id, 0, cols - 1);
    irq_restore(f);
}

void console_select_release(void)
{
    sel_dragging = 0;
}

int console_has_selection(void)
{
    return sel_active;
}

void console_select_clear(void)
{
    if (!sel_active && !sel_dragging)
        return;
    uint64_t f = irq_save();
    sel_active = sel_dragging = 0;
    selection_changed();
    irq_restore(f);
}

/* Kopiert die Markierung als UTF-8-Text in die Zwischenablage (Zeilen mit \n getrennt, Leerzeichen am Zeilenende
 * entfernt) und hebt die Markierung auf. Liefert die Laenge (0 = nichts markiert). */
uint32_t console_copy_selection(void)
{
    if (!sel_active)
        return 0;
    uint64_t f = irq_save();
    uint64_t i1 = sel_a_id, i2 = sel_b_id;
    uint32_t c1 = sel_a_col, c2 = sel_b_col;
    if (i1 > i2 || (i1 == i2 && c1 > c2)) {
        uint64_t ti = i1; i1 = i2; i2 = ti;
        uint32_t tc = c1; c1 = c2; c2 = tc;
    }
    uint32_t n = 0;
    uint64_t total = hist_count + rows;
    for (uint64_t id = i1; id <= i2; id++) {
        if (id < hist_dropped || id - hist_dropped >= total)
            continue; /* Zeile gibt es nicht mehr */
        const Cell *line = line_cells(id - hist_dropped);
        uint32_t from = id == i1 ? c1 : 0, to = id == i2 ? c2 : cols - 1;
        uint32_t last = from; /* hinter dem letzten sichtbaren Zeichen */
        for (uint32_t c = from; c <= to; c++)
            if (line[c].ch && line[c].ch != ' ')
                last = c + 1;
        for (uint32_t c = from; c < last && n + 5 < sizeof(clip); c++)
            n += (uint32_t)utf8_encode(line[c].ch ? line[c].ch : ' ', clip + n);
        if (id != i2 && n + 1 < sizeof(clip))
            clip[n++] = '\n';
    }
    clip[n] = 0;
    clip_len = n;
    sel_active = sel_dragging = 0;
    selection_changed();
    irq_restore(f);
    return n;
}

const char *console_clipboard(uint32_t *len)
{
    if (len)
        *len = clip_len;
    return clip;
}

void console_clipboard_set(const char *text, uint32_t len)
{
    if (len > sizeof(clip) - 1)
        len = sizeof(clip) - 1;
    memcpy(clip, text, len);
    clip[len] = 0;
    clip_len = len;
}

uint32_t console_history_lines(void) { return hist_count; }
uint64_t console_view_offset(void)   { return view_off; }

/* Zeichen an einer Zelle des aktuell sichtbaren Bildes (fuer Selbsttests) */
uint32_t console_debug_char(uint32_t col, uint32_t row)
{
    if (col >= cols || row >= rows)
        return 0;
    return line_cells(hist_count - view_off + row)[col].ch;
}

/* ---------- Ausgabe ---------- */

static void esc_finish(char final)
{
    int n = esc_np + 1;
    switch (final) {
    case 'm':
        for (int i = 0; i < n; i++) {
            int p = esc_params[i];
            if (p == 0) {
                sgr_fg = sgr_bg = -1;
                sgr_bold = sgr_rev = 0;
            } else if (p == 1) {
                sgr_bold = 1;
            } else if (p == 22) {
                sgr_bold = 0;
            } else if (p == 7) {
                sgr_rev = 1;
            } else if (p == 27) {
                sgr_rev = 0;
            } else if (p >= 30 && p <= 37) {
                sgr_fg = p - 30;
            } else if (p == 39) {
                sgr_fg = -1;
            } else if (p >= 40 && p <= 47) {
                sgr_bg = p - 40;
            } else if (p == 49) {
                sgr_bg = -1;
            } else if (p >= 90 && p <= 97) {
                sgr_fg = p - 90 + 8;
            } else if (p >= 100 && p <= 107) {
                sgr_bg = p - 100 + 8;
            }
        }
        sgr_apply();
        break;
    case 'J':
        if (esc_params[0] == 2)
            console_clear();
        break;
    case 'H': {
        uint32_t r = esc_params[0] > 0 ? (uint32_t)esc_params[0] - 1 : 0;
        uint32_t c = n > 1 && esc_params[1] > 0 ? (uint32_t)esc_params[1] - 1 : 0;
        cy = r < rows ? r : rows - 1;
        cx = c < cols ? c : cols - 1;
        break;
    }
    case 'K':
        for (uint32_t x = cx; x < cols; x++)
            draw_glyph(x, cy, ' ');
        break;
    default:
        break;
    }
}

static void esc_feed(char c)
{
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_np = 0;
            for (int i = 0; i < 8; i++)
                esc_params[i] = 0;
        } else {
            esc_state = 0;
        }
        return;
    }
    if (c >= '0' && c <= '9') {
        if (esc_params[esc_np] < 10000)
            esc_params[esc_np] = esc_params[esc_np] * 10 + (c - '0');
    } else if (c == ';') {
        if (esc_np < 7)
            esc_np++;
    } else {
        esc_state = 0;
        if (c >= '@' && c <= '~')
            esc_finish(c);
    }
}

static void putc_locked(char c);

/* Mit gesperrten Interrupts: sonst kann ein anderer Thread (z.B. eine kprintf-Meldung des Netz-Threads) mitten in
 * einem Zeichen scrollen, und dieses Zeichen landet an einer veralteten Position hinter dem Bildschirm. */
void console_putc(char c)
{
    if (!fb)
        return;
    uint64_t f = irq_save();
    putc_locked(c);
    irq_restore(f);
}

static void putc_locked(char c)
{
    if (esc_state) {
        esc_feed(c);
        return;
    }
    if (c == 0x1B) {
        esc_state = 1;
        u8_need = 0;
        return;
    }

    /* UTF-8: mehrere Bytes ergeben ein Zeichen (eine Zelle). Ungueltiges wird zu U+FFFD ('?'). */
    uint8_t u = (uint8_t)c;
    if (u8_need) {
        if ((u & 0xC0) == 0x80) {
            u8_cp = (u8_cp << 6) | (u & 0x3F);
            if (--u8_need == 0) {
                draw_glyph(cx, cy, u8_cp);
                cx++;
                goto advance;
            }
            return;
        }
        u8_need = 0; /* abgebrochene Folge */
        draw_glyph(cx, cy, 0xFFFD);
        cx++;
        if (cx >= cols) {
            cx = 0;
            cy++;
        }
        if (cy >= rows)
            scroll();
    }
    if (u >= 0x80) {
        if ((u & 0xE0) == 0xC0) {
            u8_cp = u & 0x1F;
            u8_need = 1;
            return;
        }
        if ((u & 0xF0) == 0xE0) {
            u8_cp = u & 0x0F;
            u8_need = 2;
            return;
        }
        if ((u & 0xF8) == 0xF0) {
            u8_cp = u & 0x07;
            u8_need = 3;
            return;
        }
        draw_glyph(cx, cy, 0xFFFD);
        cx++;
        goto advance;
    }

    switch (c) {
    case '\n':
        cx = 0;
        cy++;
        break;
    case '\r':
        cx = 0;
        break;
    case '\t':
        cx = (cx + 8) & ~7u;
        break;
    case '\b':
        if (cx)
            cx--;
        break;
    default:
        draw_glyph(cx, cy, (unsigned char)c);
        cx++;
    }

advance:
    if (cx >= cols) {
        cx = 0;
        cy++;
    }
    if (cy >= rows)
        scroll();
}
