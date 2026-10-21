/* ui_font.h - baked pixel fonts of the launcher (data in ui_font_data.c, made by tools/gen_ui_font.py). */
#ifndef UI_FONT_H
#define UI_FONT_H

enum { UF_LBL, UF_S11, UF_S12, UF_B13, UF_S15, UF_S18, UF_T22, UF_HERO, UF_COUNT };

typedef struct {
    unsigned short off;          /* offset of the bitmap in the font's bits[] (rows padded to bytes, LSB = left pixel) */
    unsigned char w, h;          /* bitmap size (0 = blank glyph) */
    signed char dx, dy;          /* bitmap top-left relative to the pen position on the baseline */
    unsigned char adv;           /* pen advance */
} UiGlyph;

typedef struct {
    int px, asc, desc;           /* design size, ascent and descent in pixels */
    const UiGlyph *glyphs;       /* ASCII 32..126 then Latin-1 160..255 */
    const unsigned char *bits;
} UiFont;

extern const UiFont ui_fonts[UF_COUNT];
#define UI_GLYPHS 191            /* 95 + 96 */

/* index of code point cp in the glyph table, or the index of '?' when the font has no such glyph */
static inline int ui_glyph_index(int cp) {
    if (cp >= 32 && cp <= 126) return cp - 32;
    if (cp >= 160 && cp <= 255) return cp - 160 + 95;
    return '?' - 32;
}

#endif
