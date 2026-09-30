#include "font.h"

/* Sucht das Glyph zu einem Unicode-Zeichen: ASCII kommt aus font8x16, alles Weitere aus der erzeugten Tabelle
 * (font_ext.c). Unbekannte Zeichen und C1-Steuerzeichen erscheinen als '?'. */
const unsigned char *font_glyph(unsigned cp)
{
    if (cp < 128)
        return &font8x16[cp * FONT_HEIGHT];
    for (int i = 0; i < font_ext_range_count; i++) {
        const FontRange *r = &font_ext_ranges[i];
        if (cp >= r->lo && cp < r->hi)
            return &font_ext_data[(r->offset + (cp - r->lo)) * FONT_HEIGHT];
    }
    return &font8x16['?' * FONT_HEIGHT];
}
