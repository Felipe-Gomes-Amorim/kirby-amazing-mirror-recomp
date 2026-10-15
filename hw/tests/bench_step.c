/* bench_step.c - custo de hw_step por quadro (sem janela, sem SDL).
 * Compile com -O2; -DHW_STEP_CAP=128 reproduz o passo fixo antigo para comparar.
 * Cenas: A = PPU ligada, audio desligado; B = PPU + PCM (FIFO/DMA/Timer0) + PSG 1-4. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../hw.h"

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

static void ppu_scene(void) {
    for (int i = 0; i < 16; i++) { gba_write16(0x05000000 + i * 32 + 2, (uint16_t)(i * 1000 + 7)); gba_write16(0x05000200 + i * 32 + 2, 0x1F); }
    for (int r = 0; r < 8 * 4; r++) gba_write32(0x06000000 + 32 + r * 4, 0x11111111);
    for (int i = 0; i < 32 * 32; i++) gba_write16(0x06004000 + i * 2, (uint16_t)(1 | (i & 15) << 12));
    for (int t = 0; t < 16; t++) for (int r = 0; r < 8; r++) gba_write32(0x06010000 + t * 32 + r * 4, 0x11111111);
    for (int i = 0; i < 32; i++) {                       /* 32 sprites 8x8 espalhados */
        gba_write16(0x07000000 + i * 8, (uint16_t)(i * 5));
        gba_write16(0x07000000 + i * 8 + 2, (uint16_t)(i * 7));
        gba_write16(0x07000000 + i * 8 + 4, 0);
    }
    for (int i = 32; i < 128; i++) gba_write16(0x07000000 + i * 8, 0x0200);
    gba_write16(0x04000008, 0x0800);
    gba_write16(0x04000000, 0x1000 | 0x0100 | 0x0040);
}
static void audio_scene(void) {
    for (int i = 0; i < 0x8000; i++) ewram[i] = (uint8_t)(int8_t)(((i / 16) & 1) ? -60 : 60);
    hw_write(0x04000084, 2, 0x0080);
    hw_write(0x04000082, 2, 0xA90E | 2);
    hw_write(0x04000080, 2, 0xFF77);
    hw_write(0x040000BC, 4, 0x02000000); hw_write(0x040000C0, 4, 0x040000A0); hw_write(0x040000C4, 4, 0xB6000000u);
    hw_write(0x04000100, 2, (uint16_t)(0x10000 - 924)); hw_write(0x04000102, 2, 0x0080);
    hw_write(0x04000062, 2, 0xF080); hw_write(0x04000064, 2, 1750 | 0x8000);
    hw_write(0x04000068, 2, 0xF080); hw_write(0x0400006C, 2, 1200 | 0x8000);
    hw_write(0x04000070, 2, 0x80); hw_write(0x04000072, 2, 0x2000); hw_write(0x04000074, 2, 1900 | 0x8000);
    hw_write(0x04000078, 2, 0xF000); hw_write(0x0400007C, 2, 0x21 | 0x8000);
}

static double bench(int with_audio, int frames) {
    hw_reset();
    ppu_scene();
    if (with_audio == 1) audio_scene();
    if (with_audio == 2) hw_write(0x04000000, 2, 0x0080);    /* forced blank: so o custo do passo/timers */
    int16_t tmp[2 * 4096];
    clock_t t0 = clock();
    for (int f = 0; f < frames; f++) {
        hw_step(280896);
        hw_audio_read(tmp, 4096);
        hw_frame_ready();
    }
    return 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / frames;
}

int main(void) {
    int frames = 600;
    bench(0, 30);                                        /* aquecimento */
    double a = bench(0, frames), b = bench(1, frames), c = bench(2, frames);
#ifdef HW_STEP_CAP
    printf("passo fixo %d: ", HW_STEP_CAP);
#else
    printf("passo por evento: ");
#endif
    printf("A (so PPU) %.3f | B (PPU+PCM+PSG) %.3f | C (forced blank, sem render) %.3f  ms/quadro (orcamento 16.7)\n", a, b, c);
    return 0;
}
