#ifndef FONT_H
#define FONT_H

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/* 128 Zeichen (ASCII) * 16 Zeilen; pro Zeile ein Byte, Bit 7 = linkes Pixel */
extern const unsigned char font8x16[128 * 16];

/* Zeichen oberhalb von ASCII: Bereiche [lo, hi) mit dem Index des ersten Glyphs in font_ext_data (font_ext.c, erzeugt) */
typedef struct {
    unsigned lo, hi, offset;
} FontRange;
extern const FontRange font_ext_ranges[];
extern const int font_ext_range_count;
extern const unsigned char font_ext_data[];

/* Glyph (16 Bytes) zu einem Unicode-Zeichen; Unbekanntes liefert das Glyph von '?' */
const unsigned char *font_glyph(unsigned codepoint);

#endif
