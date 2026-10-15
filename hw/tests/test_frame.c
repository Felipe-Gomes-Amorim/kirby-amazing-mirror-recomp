/* Teste independente de hw/ (nao usa recomp/). Build/execucao: hw/tests/run.ps1.
 * Escreve paleta/VRAM/OAM, usa DMA3, avanca frames e grava <dir>/frame_*.ppm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define LINE 1232u
#define FRAME 280896u

static void ppm(const char *dir, const char *name, const uint32_t *f) {
    char path[600]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *o = fopen(path, "wb");
    if (!o) { printf("FAIL: nao abriu %s\n", path); fails++; return; }
    fprintf(o, "P6\n240 160\n255\n");
    for (int i = 0; i < 240 * 160; i++) { fputc(f[i] >> 16 & 255, o); fputc(f[i] >> 8 & 255, o); fputc(f[i] & 255, o); }
    fclose(o);
    printf("gravou %s\n", path);
}
static uint16_t c15(int r, int g, int b) { return (uint16_t)(r | g << 5 | b << 10); }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    hw_reset();

    /* --- espelhos e byte writes --- */
    gba_write8(0x05000001, 0x7F);
    CHECK(hw_read(0x05000000, 2) == 0x7F7F, "pal: byte write replica no halfword");
    CHECK(hw_read(0x05000400, 2) == 0x7F7F, "pal: espelho a cada 1K");
    gba_write16(0x06010000, 0x1234);
    CHECK(hw_read(0x06018000, 2) == 0x1234, "VRAM: espelho 0x18000 -> 0x10000");
    gba_write8(0x07000000, 0x55);
    CHECK(hw_read(0x07000000, 1) == 0, "OAM ignora escrita de byte");
    gba_write32(0x04000200, 0xFFFFFFFF);
    CHECK(hw_read(0x04000200, 2) == 0x3FFF, "IE mascara 14 bits");
    CHECK(hw_read(0x04000130, 2) == 0x03FF, "KEYINPUT solto");
    hw_set_keys(0x03FE);
    CHECK(hw_read(0x04000130, 2) == 0x03FE, "KEYINPUT A pressionado");

    /* --- cena: BG0 4bpp, tilemap via DMA3, sprite --- */
    hw_reset();
    for (int i = 0; i < 16; i++) {
        gba_write16(0x05000000 + i * 32 + 2, c15((i * 2) & 31, 31 - i * 2, (i * 5) & 31));
        gba_write16(0x05000000 + i * 32 + 4, c15(31, 31, 31));
        gba_write16(0x05000200 + i * 32 + 2, c15(31, 0, 0));
    }
    gba_write16(0x05000000, c15(2, 2, 8));
    for (int t = 0; t < 4; t++)
        for (int r = 0; r < 8; r++) {
            uint32_t row = 0;
            for (int c = 0; c < 8; c++) {
                int px = t == 0 ? 1 : t == 1 ? (((c + r) & 1) ? 1 : 2) : t == 2 ? ((c == r || c == 7 - r) ? 2 : 1) : ((c & 4) ? 2 : 1);
                row |= (uint32_t)px << (c * 4);
            }
            gba_write32(0x06000000 + t * 32 + r * 4, row);
        }
    for (int i = 0; i < 32 * 20; i++) {
        int x = i % 32, y = i / 32;
        uint16_t e = (uint16_t)(((x + y) & 3) | (((x / 2 + y) & 15) << 12));
        ewram[i * 2] = (uint8_t)e; ewram[i * 2 + 1] = (uint8_t)(e >> 8);
    }
    gba_write32(0x040000D4, 0x02000000);
    gba_write32(0x040000D8, 0x06004000);
    gba_write32(0x040000DC, 0x80000000u | (32 * 20));
    CHECK(hw_read(0x06004000, 2) == (uint32_t)(ewram[0] | ewram[1] << 8), "DMA3 imediato copiou tilemap");
    CHECK(hw_read(0x06004000 + 2 * 639, 2) == (uint32_t)(ewram[2 * 639] | ewram[2 * 639 + 1] << 8), "DMA3 copiou ate o fim");
    CHECK(!(hw_read(0x040000DE, 2) & 0x8000), "DMA3 desarma apos imediato");
    gba_write16(0x04000008, 0x0800);
    for (int t = 0; t < 16; t++)
        for (int r = 0; r < 8; r++) gba_write32(0x06010000 + t * 32 + r * 4, 0x11111111);
    gba_write16(0x07000000, 10);
    gba_write16(0x07000002, 100 | 2 << 14);
    gba_write16(0x07000004, 0);
    gba_write16(0x04000000, 0x1000 | 0x0100 | 0x0040);

    /* --- timing --- */
    gba_write16(0x04000004, 0x0008 | 0x0020 | (50 << 8));   /* VBlank IRQ, VCount IRQ, LYC=50 */
    gba_write16(0x04000200, 0x0005);                         /* IE: VBlank + VCount */
    gba_write16(0x04000208, 1);
    CHECK(hw_frame_ready() == 0, "frame nao pronto antes do VBlank");
    CHECK(hw_read(0x04000006, 2) == 0, "VCOUNT inicia em 0");
    hw_step(959);
    CHECK((hw_read(0x04000004, 2) & 2) == 0, "sem HBlank antes de 960");
    hw_step(1);
    CHECK((hw_read(0x04000004, 2) & 2) != 0, "HBlank em 960");
    hw_step(LINE - 960);
    CHECK(hw_read(0x04000006, 2) == 1, "VCOUNT=1 apos 1232 ciclos");
    hw_step(49 * LINE);
    CHECK(hw_read(0x04000006, 2) == 50, "VCOUNT=50");
    CHECK((hw_read(0x04000004, 2) & 4) != 0, "flag VCount match");
    CHECK((hw_irq_pending() & 4) != 0, "IRQ VCount pendente");
    gba_write16(0x04000202, 4);
    CHECK((hw_irq_pending() & 4) == 0, "IF w1c limpa VCount");
    hw_step(109 * LINE + LINE - 1);
    CHECK(hw_frame_ready() == 0, "frame nao pronto 1 ciclo antes da linha 160");
    CHECK((hw_read(0x04000004, 2) & 1) == 0, "VBlank flag ainda 0");
    hw_step(1);
    CHECK(hw_read(0x04000006, 2) == 160, "VCOUNT=160");
    CHECK((hw_read(0x04000004, 2) & 1) != 0, "VBlank flag em 160");
    CHECK((hw_irq_pending() & 1) != 0, "IRQ VBlank pendente");
    const uint32_t *f = hw_frame_ready();
    CHECK(f != 0, "frame pronto em VBlank");
    CHECK(hw_frame_ready() == 0, "frame_ready consome");
    if (f) {
        ppm(dir, "frame_bg_obj.ppm", f);
        CHECK(f[0] != f[8] || f[1] != f[9], "BG renderizou algo variado");
        CHECK(f[20 * 240 + 110] == 0xFF0000, "sprite vermelho em (110,20)");
        CHECK(f[100 * 240 + 3] != 0, "BG nao-preto");
    }
    hw_step(68 * LINE);
    CHECK(hw_read(0x04000006, 2) == 0, "VCOUNT volta a 0 apos 228 linhas");

    /* --- timer: reload 0xFFF0, 1 ciclo/tick, IRQ --- */
    gba_write16(0x04000202, 0xFFFF);
    gba_write16(0x04000100, 0xFFF0);
    gba_write16(0x04000102, 0x00C0);   /* enable + IRQ */
    CHECK((hw_irq_pending() & 8) == 0, "timer IRQ nao antes do overflow");
    hw_step(16);
    gba_write16(0x04000200, 0x0008);
    CHECK((hw_irq_pending() & 8) != 0, "timer0 overflow gera IF bit3");
    gba_write16(0x04000202, 8);

    /* --- DMA HBlank repeat: 1 halfword por linha visivel para um registrador de scroll --- */
    ewram[0x100] = 5; ewram[0x101] = 0;
    gba_write32(0x040000B0, 0x02000100);
    gba_write32(0x040000B4, 0x06000000);
    gba_write32(0x040000B8, 0xA2000000u | 1);   /* enable, HBlank(2), repeat, 16-bit, 1 unidade */
    hw_step(2 * LINE);
    CHECK(hw_read(0x040000BA, 2) & 0x8000, "DMA0 HBlank repeat segue armado");
    gba_write16(0x040000BA, 0);

    /* --- modo 3: gradiente direto na VRAM --- */
    hw_reset();
    for (int y = 0; y < 160; y++)
        for (int x = 0; x < 240; x++) gba_write16(0x06000000 + (y * 240 + x) * 2, c15(x * 31 / 239, y * 31 / 159, 16));
    gba_write16(0x04000000, 0x0403);
    hw_step(FRAME);
    f = hw_frame_ready();
    CHECK(f != 0, "mode 3: frame pronto");
    if (f) {
        ppm(dir, "frame_mode3.ppm", f);
        CHECK(f[0] != f[239], "mode 3: gradiente horizontal");
        CHECK(f[0] != f[159 * 240], "mode 3: gradiente vertical");
    }

    printf(fails ? "RESULTADO: %d falha(s)\n" : "RESULTADO: OK\n", fails);
    return fails != 0;
}
