/* demo_window.c - abre a janela SDL2 e mostra uma cena do PPU (BG de tiles + sprite).
 * Setas movem o sprite; Z/X trocam a cor do fundo; Esc fecha. Uso: demo_window.exe [quadros]
 * (com [quadros] > 0 sai sozinho depois desse numero de quadros). Nao usa recomp/. */
#include <stdio.h>
#include <stdlib.h>
#define SDL_MAIN_HANDLED   /* mantem o main normal (sem SDL2main) */
#include <SDL.h>           /* so para SDL_Delay */
#include "../hw.h"
#include "../window_sdl.h"

static uint8_t ewram[0x40000];
static int is_hw(uint32_t a) { return (a >> 24) >= 4 && (a >> 24) <= 7; }
uint32_t gba_read8(uint32_t a)  { return is_hw(a) ? hw_read(a, 1) : ewram[a & 0x3FFFF]; }
uint32_t gba_read16(uint32_t a) { return is_hw(a) ? hw_read(a, 2) : (uint32_t)(ewram[a & 0x3FFFE] | ewram[(a & 0x3FFFE) + 1] << 8); }
uint32_t gba_read32(uint32_t a) { return is_hw(a) ? hw_read(a, 4) : gba_read16(a) | gba_read16(a + 2) << 16; }
void gba_write8(uint32_t a, uint32_t v)  { if (is_hw(a)) hw_write(a, 1, v); else ewram[a & 0x3FFFF] = (uint8_t)v; }
void gba_write16(uint32_t a, uint32_t v) {
    if (is_hw(a)) hw_write(a, 2, v);
    else { ewram[a & 0x3FFFE] = (uint8_t)v; ewram[(a & 0x3FFFE) + 1] = (uint8_t)(v >> 8); }
}
void gba_write32(uint32_t a, uint32_t v) {
    if (is_hw(a)) hw_write(a, 4, v); else { gba_write16(a, v & 0xFFFF); gba_write16(a + 2, v >> 16); }
}

static uint16_t c15(int r, int g, int b) { return (uint16_t)(r | g << 5 | b << 10); }

static int pat(int t, int x, int y) {
    return t == 0 ? 1 : t == 1 ? (((x + y) & 1) ? 1 : 2) : t == 2 ? ((x == y || x == 7 - y) ? 2 : 1) : ((x & 4) ? 2 : 1);
}

static void setup(void) {
    hw_reset();
    for (int i = 0; i < 16; i++) {
        gba_write16(0x05000000 + i * 32 + 2, c15((i * 2) & 31, 31 - i * 2, (i * 5) & 31));
        gba_write16(0x05000000 + i * 32 + 4, c15(31, 31, 31));
        gba_write16(0x05000200 + i * 32 + 2, c15(31, 0, 0));
        gba_write16(0x05000200 + i * 32 + 4, c15(31, 31, 0));
    }
    gba_write16(0x05000000, c15(2, 2, 8));
    for (int t = 0; t < 4; t++)
        for (int r = 0; r < 8; r++) {
            uint32_t row = 0;
            for (int c = 0; c < 8; c++) row |= (uint32_t)pat(t, c, r) << (c * 4);
            gba_write32(0x06000000 + t * 32 + r * 4, row);
        }
    for (int i = 0; i < 32 * 32; i++) {
        int x = i % 32, y = i / 32;
        gba_write16(0x06004000 + i * 2, (uint16_t)(((x + y) & 3) | (((x / 2 + y) & 15) << 12)));
    }
    for (int t = 0; t < 16; t++)
        for (int r = 0; r < 8; r++) gba_write32(0x06010000 + t * 32 + r * 4, 0x21212121);
    gba_write16(0x04000008, 0x0800);
    gba_write16(0x04000000, 0x1000 | 0x0100 | 0x0040);
}

int main(int argc, char **argv) {
    int limit = argc > 1 ? atoi(argv[1]) : 0;
    setup();
    if (win_open("Kirby recomp - hw demo", 3) != 0) return 1;
    int px = 100, py = 60, hscroll = 0;
    for (int frame = 0; win_poll() && (!limit || frame < limit); frame++) {
        uint16_t k = win_keys();
        hw_set_keys(k);
        if (!(k & 0x10)) px++;       /* direita */
        if (!(k & 0x20)) px--;
        if (!(k & 0x40)) py--;
        if (!(k & 0x80)) py++;
        if (!(k & 0x01)) gba_write16(0x05000000, c15((frame >> 1) & 31, 2, 8));
        hscroll++;
        gba_write16(0x04000010, (uint16_t)(hscroll >> 1));       /* BG0HOFS: scroll continuo */
        gba_write16(0x07000000, (uint16_t)(py & 255) | 2 << 14);
        gba_write16(0x07000002, (uint16_t)(px & 511) | 2 << 14);
        gba_write16(0x07000004, 0);
        hw_step(280896);
        const uint32_t *f = hw_frame_ready();
        win_present(f ? f : hw_get_framebuffer());
        SDL_Delay(16);
    }
    win_close();
    return 0;
}
