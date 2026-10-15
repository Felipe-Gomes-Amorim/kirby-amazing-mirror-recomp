/* font5x7.h - embedded 5x7 bitmap font shared by the progress window and the launcher UI.
 * font5x7_rows(ch) returns 7 row bytes (bit 4 = leftmost column, 5 bits used) or the '?' glyph for
 * characters outside the set. Lowercase letters use the uppercase glyphs. Characters: A-Z 0-9 space
 * . , : - _ / % ( ) ! ? & ' + = < >  plus [ ] * # | ~ (added for the launcher UI). No accents. */
#ifndef FONT5X7_H
#define FONT5X7_H
#include <stdint.h>

const uint8_t *font5x7_rows(int ch);

#endif
