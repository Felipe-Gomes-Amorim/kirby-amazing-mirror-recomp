/* Teste do audio PCM (FIFO A/B + DMA1/DMA2 especial + Timer0). Independente de recomp/.
 * Onda quadrada conhecida em EWRAM: 16 amostras +V, 16 amostras -V (periodo 32 amostras).
 * Timer0 reload -924 => 16777216/924 = 18157,3 Hz => onda de 567,4 Hz. Conta cruzamentos. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (linha %d)\n", msg, __LINE__); fails++; } } while (0)
#define SEC 16777216u
#define FRAME 280896u

static int16_t out[48000 * 2 + 4096];
static int nout;

static void fill_square(uint32_t off, int amp) {
    for (uint32_t i = 0; i < 0x8000; i++) ewram[off + i] = (uint8_t)(int8_t)(((i / 16) & 1) ? -amp : amp);
}

static void run_seconds(void) {
    nout = 0;
    for (uint32_t c = 0; c < SEC; c += FRAME) {
        uint32_t n = SEC - c < FRAME ? SEC - c : FRAME;
        hw_step(n);
        int got = hw_audio_read(out + nout * 2, 48000 + 2048 - nout);
        nout += got;
    }
}

static void common_setup(void) {
    hw_reset();
    gba_write16(0x04000084, 0x0080);                 /* master enable */
    gba_write16(0x04000100, (uint16_t)(0x10000 - 924));
    gba_write16(0x04000102, 0x0080);                 /* Timer0 on, sem IRQ */
}

int main(void) {
    CHECK(hw_audio_rate() == 48000, "taxa de saida 48000");

    /* --- A (direita), amplitude 100, vol 100% --- */
    fill_square(0, 100);
    common_setup();
    gba_write16(0x04000082, 0xA90E);
    gba_write32(0x040000BC, 0x02000000);             /* DMA1SAD */
    gba_write32(0x040000C0, 0x040000A0);             /* DMA1DAD = FIFO_A */
    gba_write32(0x040000C4, 0xB6000000u);            /* CNT_L=0, CNT_H=0xB600 */
    CHECK((hw_read(0x040000C6, 2) & 0x8000) != 0, "DMA1 especial fica armado (nao dispara na hora)");
    run_seconds();
    CHECK(nout >= 47990 && nout <= 48010, "48000 quadros de audio em 1 s");
    int cross = 0, mx = -32768, mn = 32767, lmax = 0;
    for (int i = 1; i < nout; i++) {
        int r0 = out[(i - 1) * 2 + 1], r1 = out[i * 2 + 1];
        if (r0 <= 0 && r1 > 0) cross++;
        if (r1 > mx) mx = r1;
        if (r1 < mn) mn = r1;
        if (abs(out[i * 2]) > lmax) lmax = abs(out[i * 2]);
    }
    printf("A: %d quadros, %d periodos (esperado ~567,4), max=%d min=%d, |L|max=%d\n", nout, cross, mx, mn, lmax);
    CHECK(cross >= 566 && cross <= 569, "frequencia da onda ~567 Hz");
    CHECK(mx == 100 * 256 && mn == -100 * 256, "amplitude A = 100 * 256");
    CHECK(lmax == 0, "esquerda silenciosa (B sem DMA)");

    /* --- B (esquerda), amplitude 40, vol 50% (bit3=0): 40*256>>1 = 5120 --- */
    fill_square(0x10000, 40);
    common_setup();
    gba_write16(0x04000082, 0xA906);                 /* B vol 50% */
    gba_write32(0x040000C8, 0x02010000);             /* DMA2SAD */
    gba_write32(0x040000CC, 0x040000A4);             /* DMA2DAD = FIFO_B */
    gba_write32(0x040000D0, 0xB6000000u);
    run_seconds();
    int lmx = -32768, lmn = 32767, rabs = 0;
    for (int i = 0; i < nout; i++) {
        if (out[i * 2] > lmx) lmx = out[i * 2];
        if (out[i * 2] < lmn) lmn = out[i * 2];
        if (abs(out[i * 2 + 1]) > rabs) rabs = abs(out[i * 2 + 1]);
    }
    printf("B: L max=%d min=%d, |R|max=%d\n", lmx, lmn, rabs);
    CHECK(lmx == 5120 && lmn == -5120, "B na esquerda com volume 50%");
    CHECK(rabs == 0, "direita silenciosa (A sem DMA)");

    /* --- master disable => silencio e FIFO nao consome --- */
    gba_write16(0x04000084, 0x0000);
    hw_audio_read(out, 4096);                         /* drena */
    hw_step(FRAME);
    int n2 = hw_audio_read(out, 4096), nz = 0;
    for (int i = 0; i < n2 * 2; i++) if (out[i]) nz++;
    CHECK(n2 > 0 && nz == 0, "master off = silencio");

    printf(fails ? "RESULTADO: %d falha(s)\n" : "RESULTADO: OK\n", fails);
    return fails != 0;
}
