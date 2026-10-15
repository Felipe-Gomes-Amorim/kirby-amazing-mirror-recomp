/* Teste do PPU contra resultado calculado a mao (independente de recomp/ e do decodificador de hw.c).
 * Cena A: modo 0, BG0 + sprites; o quadro esperado e montado pixel a pixel por codigo simples aqui.
 * Cena B: modo 1, BG2 afim (identidade, escala, offset, PD por linha). */
#include <stdio.h>
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

#define FRAME 280896u
static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (linha %d)\n", msg, __LINE__); fails++; } } while (0)

static uint16_t c15(int r, int g, int b) { return (uint16_t)(r | g << 5 | b << 10); }
static uint32_t x8(int r, int g, int b) {   /* 5 bits -> 8 bits: v*8 + v/4 */
    return (uint32_t)((r * 8 + r / 4) << 16 | (g * 8 + g / 4) << 8 | (b * 8 + b / 4));
}
static void ppm(const char *dir, const char *name, const uint32_t *f) {
    char path[600]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *o = fopen(path, "wb");
    if (!o) { printf("FAIL: nao abriu %s\n", path); fails++; return; }
    fprintf(o, "P6\n240 160\n255\n");
    for (int i = 0; i < 240 * 160; i++) { fputc(f[i] >> 16 & 255, o); fputc(f[i] >> 8 & 255, o); fputc(f[i] & 255, o); }
    fclose(o);
}

static uint32_t exp_fb[240 * 160];
static void put(int x, int y, uint32_t c) { if (x >= 0 && x < 240 && y >= 0 && y < 160) exp_fb[y * 240 + x] = c; }

/* padrao do tile 1: x==y -> 3 ; x<4 -> 1 ; senao 2 */
static int pat1(int x, int y) { return x == y ? 3 : x < 4 ? 1 : 2; }
static void draw1(int x0, int y0, int hf, int vf, const uint32_t col[4]) {
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        put(x0 + x, y0 + y, col[pat1(hf ? 7 - x : x, vf ? 7 - y : y)]);
}
static void tile4(uint32_t base, int (*f)(int, int)) {
    for (int y = 0; y < 8; y++) {
        uint32_t row = 0;
        for (int x = 0; x < 8; x++) row |= (uint32_t)f(x, y) << (x * 4);
        gba_write32(base + y * 4, row);
    }
}
static int pat2(int x, int y) { return ((x + y) & 1) ? 0 : 2; }   /* xadrez com transparencia */

static void scene_a(const char *dir) {
    hw_reset();
    const uint32_t BD = x8(0, 0, 8);
    gba_write16(0x05000000, c15(0, 0, 8));
    gba_write16(0x05000000 + 2 * 32 + 2, c15(31, 0, 0));    /* BG banco 2: 1=vermelho 2=verde 3=azul */
    gba_write16(0x05000000 + 2 * 32 + 4, c15(0, 31, 0));
    gba_write16(0x05000000 + 2 * 32 + 6, c15(0, 0, 31));
    gba_write16(0x05000200 + 2, c15(31, 31, 0));            /* OBJ banco 0: 1=amarelo 2=magenta 3=ciano */
    gba_write16(0x05000200 + 4, c15(31, 0, 31));
    gba_write16(0x05000200 + 6, c15(0, 31, 31));
    const uint32_t BGC[4] = { 0, 0xFF0000, 0x00FF00, 0x0000FF };
    const uint32_t OBC[4] = { 0, 0xFFFF00, 0xFF00FF, 0x00FFFF };
    tile4(0x06000000 + 32, pat1);             /* BG tile 1 */
    tile4(0x06010000 + 32, pat1);             /* OBJ tile 1 */
    tile4(0x06010000 + 64, pat2);             /* OBJ tile 2 */
    /* tilemap: screenblock 8 -> VRAM 0x4000. (tx,ty)->(entrada) */
    gba_write16(0x06004000 + (1 * 32 + 2) * 2, 1 | 2 << 12);                 /* normal  em px (16,8) */
    gba_write16(0x06004000 + (1 * 32 + 4) * 2, 1 | 2 << 12 | 0x400);        /* hflip   em px (32,8) */
    gba_write16(0x06004000 + (1 * 32 + 6) * 2, 1 | 2 << 12 | 0x800);        /* vflip   em px (48,8) */
    gba_write16(0x06004000 + (1 * 32 + 8) * 2, 1 | 2 << 12 | 0xC00);        /* h+v     em px (64,8) */
    gba_write16(0x04000008, 0x0800 | 1);                                    /* BG0CNT prio 1 */
    /* OAM: a0=y, a1=x|flip, a2=tile|prio<<10 */
    struct { int y, x, fl, tile, prio; } sp[] = {
        {  8, 16, 0x0000, 1, 1 },   /* mesmo prio do BG: sprite vence, cobre o tile em (16,8) */
        {  8, 32, 0x0000, 1, 2 },   /* prio pior que BG0: BG vence em (32,8) */
        { 30, 20, 0x0000, 1, 0 },
        { 30, 40, 0x1000, 1, 0 },   /* hflip */
        { 30, 60, 0x2000, 1, 0 },   /* vflip */
        { 50, 80, 0x0000, 2, 0 },   /* xadrez c/ transparencia sobre backdrop */
        { 100, 236, 0x0000, 1, 0 }, /* cortado na borda direita */
        { 100, 508 & 0x1FF, 0x0000, 1, 0 }, /* x=-4 (9 bits): cortado na borda esquerda */
    };
    for (unsigned i = 0; i < sizeof sp / sizeof sp[0]; i++) {
        gba_write16(0x07000000 + i * 8, sp[i].y);
        gba_write16(0x07000000 + i * 8 + 2, (sp[i].x & 0x1FF) | sp[i].fl);
        gba_write16(0x07000000 + i * 8 + 4, sp[i].tile | sp[i].prio << 10);
    }
    for (unsigned i = sizeof sp / sizeof sp[0]; i < 128; i++)           /* demais sprites desligados */
        gba_write16(0x07000000 + i * 8, 0x0200);
    gba_write16(0x04000000, 0x0100 | 0x1000 | 0x0040);                  /* modo 0, BG0, OBJ, 1D */

    for (int i = 0; i < 240 * 160; i++) exp_fb[i] = BD;
    draw1(16, 8, 0, 0, BGC); draw1(32, 8, 1, 0, BGC); draw1(48, 8, 0, 1, BGC); draw1(64, 8, 1, 1, BGC);
    draw1(16, 8, 0, 0, OBC);                  /* sprite 0 por cima do tile */
    /* sprite 1 (prio 2) atras do BG: nada novo em (32,8) */
    draw1(20, 30, 0, 0, OBC); draw1(40, 30, 1, 0, OBC); draw1(60, 30, 0, 1, OBC);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if (!((x + y) & 1)) put(80 + x, 50 + y, OBC[2]);
    draw1(236, 100, 0, 0, OBC);               /* put() corta em x>=240 */
    draw1(-4, 100, 0, 0, OBC);

    hw_step(FRAME);
    const uint32_t *f = hw_frame_ready();
    CHECK(f != 0, "A: frame pronto");
    if (!f) return;
    ppm(dir, "ppu_scene_a.ppm", f);
    int bad = 0;
    for (int i = 0; i < 240 * 160; i++)
        if (f[i] != exp_fb[i]) { if (bad < 5) printf("  dif (%d,%d): obtido %06X esperado %06X\n", i % 240, i / 240, f[i], exp_fb[i]); bad++; }
    CHECK(bad == 0, "A: quadro igual ao esperado");
    if (bad) printf("  %d pixels diferentes\n", bad);
    /* spot checks literais, calculados a mao */
    CHECK(f[8 * 240 + 16] == 0x00FFFF, "A: (16,8) = sprite idx3 ciano (diagonal)");
    CHECK(f[8 * 240 + 32 + 7] == 0x0000FF, "A: (39,8) = BG hflip idx3 azul (prio BG vence)");
    CHECK(f[8 * 240 + 33] == 0x00FF00, "A: (33,8) = BG hflip idx2 verde (x=1 -> x espelhado 6)");
    CHECK(f[50 * 240 + 81] == BD, "A: xadrez transparente deixa backdrop");
    CHECK(f[50 * 240 + 80] == 0xFF00FF, "A: xadrez opaco magenta");
    CHECK(f[100 * 240 + 236] == 0x00FFFF && f[100 * 240 + 239] == 0xFFFF00, "A: sprite cortado na borda direita");
    CHECK(f[100 * 240 + 0] == 0xFF00FF, "A: sprite cortado na borda esquerda (x local 4 -> idx2)");
}

static void scene_b(const char *dir) {
    for (int cfg = 0; cfg < 4; cfg++) {
        hw_reset();
        for (int i = 1; i <= 16; i++) gba_write16(0x05000000 + i * 2, c15(i, 31 - i, (i * 2) & 31));
        gba_write16(0x05000000, c15(0, 0, 8));
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {       /* 8bpp: tile1 idx=x+1, tile2 idx=y+1 */
            if (!(x & 1)) {   /* VRAM de BG: so halfword (byte write replica) */
                gba_write16(0x06000000 + 64 + y * 8 + x, (x + 1) | (x + 2) << 8);
                gba_write16(0x06000000 + 128 + y * 8 + x, (y + 1) | (y + 1) << 8);
            }
        }
        gba_write16(0x06004000, 1 | 2 << 8); /* screenblock 8, 1 byte/entrada: (0,0)=tile1 (1,0)=tile2 */
        gba_write16(0x0400000C, 0x0800 | 0x0000);   /* BG2CNT: char 0, screen 8, 128x128, sem wrap */
        gba_write16(0x04000020, cfg == 1 ? 0x200 : 0x100);   /* PA */
        gba_write16(0x04000022, 0);                           /* PB */
        gba_write16(0x04000024, 0);                           /* PC */
        gba_write16(0x04000026, cfg == 2 ? 0x200 : 0x100);   /* PD */
        gba_write32(0x04000028, cfg == 3 ? 4 * 256 : 0);     /* X */
        gba_write32(0x0400002C, 0);
        gba_write16(0x04000000, 0x0001 | 0x0400);            /* modo 1, BG2 */
        hw_step(FRAME);
        const uint32_t *f = hw_frame_ready();
        CHECK(f != 0, "B: frame pronto");
        if (!f) continue;
        if (cfg == 0) {
            ppm(dir, "ppu_scene_b.ppm", f);
            for (int x = 0; x < 8; x++) CHECK(f[3 * 240 + x] == x8(x + 1, 31 - (x + 1), ((x + 1) * 2) & 31), "B0: tile1 identidade");
            for (int y = 0; y < 8; y++) CHECK(f[y * 240 + 8] == x8(y + 1, 31 - (y + 1), ((y + 1) * 2) & 31), "B0: tile2 por linha");
            CHECK(f[0 * 240 + 16] == x8(0, 0, 8), "B0: tile0 transparente -> backdrop");
            CHECK(f[0 * 240 + 130] == x8(0, 0, 8), "B0: fora de 128 sem wrap -> backdrop");
        } else if (cfg == 1) {                                /* PA=2: screen x -> tex 2x */
            for (int x = 0; x < 4; x++) { int i = 2 * x + 1; CHECK(f[x] == x8(i, 31 - i, (i * 2) & 31), "B1: escala PA=2"); }
        } else if (cfg == 2) {                                /* PD=2: screen y -> tex 2y; tile2 em x 8..15 */
            for (int y = 0; y < 4; y++) { int i = 2 * y + 1; CHECK(f[y * 240 + 8] == x8(i, 31 - i, (i * 2) & 31), "B2: PD=2 acumula por linha"); }
        } else {                                              /* X=4: screen x -> tex x+4 */
            for (int x = 0; x < 4; x++) { int i = x + 5; CHECK(f[x] == x8(i, 31 - i, (i * 2) & 31), "B3: offset X=4"); }
            CHECK(f[4] == x8(1, 30, 2), "B3: x=4 -> tex 8 = tile2 linha 0 (idx 1)");
        }
    }
}


static const uint32_t *run_frame(void) { hw_step(FRAME); return hw_frame_ready(); }

/* BG0 (prio 0) vermelho solido, BG1 (prio 1) azul solido, backdrop verde. */
static void scene_c_setup(void) {
    hw_reset();
    gba_write16(0x05000000, c15(0, 31, 0));
    gba_write16(0x05000000 + 2, c15(31, 0, 0));          /* banco 0 idx 1 */
    gba_write16(0x05000000 + 34, c15(0, 0, 31));         /* banco 1 idx 1 */
    for (int r = 0; r < 8; r++) gba_write32(0x06000000 + 32 + r * 4, 0x11111111);
    for (int i = 0; i < 32 * 32; i++) {
        gba_write16(0x06004000 + i * 2, 1);              /* BG0: tile 1 banco 0 */
        gba_write16(0x06004800 + i * 2, 1 | 1 << 12);    /* BG1: tile 1 banco 1 */
    }
    gba_write16(0x04000008, 0x0800);                     /* BG0CNT prio 0 */
    gba_write16(0x0400000A, 0x0901);                     /* BG1CNT prio 1 */
}

static void scene_c(const char *dir) {
    const uint32_t RED = 0xFF0000, BLUE = 0x0000FF;
    const uint32_t *f;
    /* C1: janela 0 em x[40,80) y[20,60): dentro so BG1; fora BG0+BG1 */
    scene_c_setup();
    gba_write16(0x04000040, 40 << 8 | 80);
    gba_write16(0x04000044, 20 << 8 | 60);
    gba_write16(0x04000048, 0x0002);                      /* WININ win0: BG1 */
    gba_write16(0x0400004A, 0x0003);                      /* WINOUT: BG0+BG1 */
    gba_write16(0x04000000, 0x0003 << 8 | 0x2000);        /* modo 0, BG0+BG1, WIN0 */
    f = run_frame();
    CHECK(f != 0, "C1: frame");
    if (f) {
        ppm(dir, "ppu_window.ppm", f);
        CHECK(f[30 * 240 + 50] == BLUE, "C1: dentro da janela so BG1");
        CHECK(f[30 * 240 + 10] == RED, "C1: fora da janela BG0");
        CHECK(f[30 * 240 + 79] == BLUE && f[30 * 240 + 80] == RED, "C1: x2 exclusivo");
        CHECK(f[59 * 240 + 50] == BLUE && f[60 * 240 + 50] == RED, "C1: y2 exclusivo");
        CHECK(f[19 * 240 + 50] == RED && f[20 * 240 + 50] == BLUE, "C1: y1 inclusivo");
        CHECK(f[30 * 240 + 39] == RED && f[30 * 240 + 40] == BLUE, "C1: x1 inclusivo");
    }
    /* C2: alpha BG0 sobre BG1, EVA=8 EVB=8: (15,0,15) -> 0x7B007B */
    scene_c_setup();
    gba_write16(0x04000050, 0x0001 | 0x0040 | 0x0200);
    gba_write16(0x04000052, 0x0808);
    gba_write16(0x04000000, 0x0300);
    f = run_frame();
    CHECK(f && f[80 * 240 + 100] == 0x7B007B, "C2: alpha 8/16 + 8/16 = 0x7B007B");
    /* C3: brighten BG0 EVY=8: (31,15,15) -> FF7B7B ; C4: darken: (16,0,0) -> 840000 */
    scene_c_setup();
    gba_write16(0x04000050, 0x0001 | 0x0080);
    gba_write16(0x04000054, 8);
    gba_write16(0x04000000, 0x0300);
    f = run_frame();
    CHECK(f && f[80 * 240 + 100] == 0xFF7B7B, "C3: brighten EVY=8 = 0xFF7B7B");
    scene_c_setup();
    gba_write16(0x04000050, 0x0001 | 0x00C0);
    gba_write16(0x04000054, 8);
    gba_write16(0x04000000, 0x0300);
    f = run_frame();
    CHECK(f && f[80 * 240 + 100] == 0x840000, "C4: darken EVY=8 = 0x840000");
    /* C4b: alvo nao marcado => sem efeito */
    scene_c_setup();
    gba_write16(0x04000050, 0x0002 | 0x0040);              /* alvo 1 = BG1, topo e BG0 */
    gba_write16(0x04000052, 0x0808);
    gba_write16(0x04000000, 0x0300);
    f = run_frame();
    CHECK(f && f[80 * 240 + 100] == RED, "C4b: topo fora do alvo 1 nao mistura");

    /* C5: mosaico BG0 H=4 V=2; tile 2 com idx((x+y)&7)+1, cor i = (i,0,31-i) */
    hw_reset();
    for (int i = 1; i <= 8; i++) gba_write16(0x05000000 + i * 2, c15(i, 0, 31 - i));
    for (int y = 0; y < 8; y++) {
        uint32_t row = 0;
        for (int x = 0; x < 8; x++) row |= (uint32_t)(((x + y) & 7) + 1) << (x * 4);
        gba_write32(0x06000000 + 64 + y * 4, row);
    }
    gba_write16(0x06004000, 2);                           /* (0,0) = tile 2; resto tile 0 (transparente) */
    gba_write16(0x04000008, 0x0800 | 0x0040);             /* BG0CNT: mosaico */
    gba_write16(0x0400004C, 3 | 1 << 4);                  /* H=4 (3+1), V=2 (1+1) */
    gba_write16(0x04000000, 0x0100);
    f = run_frame();
    CHECK(f != 0, "C5: frame");
    if (f) {
        ppm(dir, "ppu_mosaic.ppm", f);
        int bad = 0;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int i = (((x - x % 4) + (y - y % 2)) & 7) + 1;
            if (f[y * 240 + x] != x8(i, 0, 31 - i)) bad++;
        }
        CHECK(bad == 0, "C5: mosaico H4 V2 igual ao esperado (64 pixels)");
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    scene_a(dir);
    scene_b(dir);
    scene_c(dir);
    CHECK(hw_get_framebuffer() != 0, "hw_get_framebuffer nao nulo");
    printf(fails ? "RESULTADO: %d falha(s)\n" : "RESULTADO: OK\n", fails);
    return fails != 0;
}
