/* demo_audio.c - toca por 2 s, via SDL2: (1) onda quadrada PCM (FIFO A + DMA1 + Timer0) e
 * (2) um pulso PSG (canal 1). Sem placa de som: sai com mensagem clara (nao crasha).
 * Uso: demo_audio.exe [segundos]   (padrao 2). Nao usa recomp/. */
#define SDL_MAIN_HANDLED
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include "../hw.h"
#include "../audio_sdl.h"

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

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 2.0;
    if (secs <= 0) secs = 2.0;
    if (audio_open() != 0) { fprintf(stderr, "demo_audio: sem saida de audio; encerrando sem tocar.\n"); return 2; }

    hw_reset();
    for (int i = 0; i < 0x8000; i++) ewram[i] = (uint8_t)(int8_t)(((i / 16) & 1) ? -60 : 60);  /* quadrada 567 Hz */
    hw_write(0x04000084, 2, 0x0080);                  /* master on */
    hw_write(0x04000082, 2, 0xA90E);                  /* PCM A->R, B->L, 100% */
    hw_write(0x040000BC, 4, 0x02000000);              /* DMA1SAD */
    hw_write(0x040000C0, 4, 0x040000A0);              /* DMA1DAD = FIFO_A */
    hw_write(0x040000C4, 4, 0xB6000000u);             /* DMA especial */
    hw_write(0x04000100, 2, (uint16_t)(0x10000 - 924));
    hw_write(0x04000102, 2, 0x0080);                  /* Timer0 on */

    int psg_started = 0;
    uint32_t total_frames = (uint32_t)(secs * 59.7275);
    int16_t tmp[2 * 4096];
    for (uint32_t f = 0; f < total_frames; f++) {
        if (f == total_frames / 2 && !psg_started) {  /* 2a metade: troca para o pulso PSG (440 Hz) */
            hw_write(0x040000C6, 2, 0);               /* para o DMA1 */
            hw_write(0x04000082, 2, 0x0002);          /* so PSG a 100% */
            hw_write(0x04000080, 2, 0x1177);          /* canal 1 em L e R */
            hw_write(0x04000062, 2, 0xF080);          /* duty 50%, vol 15 */
            hw_write(0x04000064, 2, 1750 | 0x8000);   /* ~440 Hz */
            psg_started = 1;
        }
        hw_step(280896);
        int n = hw_audio_read(tmp, 4096);
        audio_push(tmp, n);
        SDL_Delay(16);                                 /* ~1 quadro de tempo real */
    }
    SDL_Delay(100);
    printf("demo_audio: %.1f s tocados (underruns=%d, descartados=%d)\n", secs, audio_underruns(), audio_dropped());
    audio_close();
    return 0;
}
