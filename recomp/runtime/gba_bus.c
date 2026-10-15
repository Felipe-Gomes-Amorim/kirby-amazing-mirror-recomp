/* Barramento do runtime: ROM/EWRAM/IWRAM/SRAM aqui; 0x04-0x07 vai para hw_*.
 * Se hw/ ainda nao existe, as definicoes FRACAS abaixo (stub minimo: I/O como RAM,
 * VCOUNT que avanca por leitura, DMA imediato) sao usadas; hw/ as sobrescreve. */
#include <stdio.h>
#include <string.h>
#include "gba_bus.h"

#define WEAK __attribute__((weak))

static uint8_t ewram[0x40000];
static uint8_t iwram[0x8000];
static uint8_t sram[0x8000];
static const uint8_t *rom;
static uint32_t rom_size;

void gba_load_rom(const uint8_t *p, uint32_t n) { rom = p; rom_size = n; }
static void sram_init(void) __attribute__((constructor));
static void sram_init(void) { memset(sram, 0xFF, sizeof sram); }
uint32_t gba_rom_size(void) { return rom_size; }
uint8_t *gba_ewram(void) { return ewram; }
void gba_sram_set(const uint8_t *p) { memcpy(sram, p, sizeof sram); }
void gba_sram_get(uint8_t *p) { memcpy(p, sram, sizeof sram); }
uint8_t *gba_iwram(void) { return iwram; }

WEAK void gba_bus_unmapped(uint32_t addr, int size, int is_write) {
    static int n;
    if (n++ < 8)
        fprintf(stderr, "[bus] %s%d em endereco sem regiao 0x%08X\n", is_write ? "write" : "read",
                size * 8, addr);
}

static uint32_t rom_read(uint32_t a, int size) {
    uint32_t off = a & 0x01FFFFFFu, v = 0;
    for (int i = 0; i < size; i++) {
        uint32_t o = off + (uint32_t)i;
        uint32_t b = o < rom_size ? rom[o] : (((o >> 1) >> ((o & 1) * 8)) & 0xFF); /* open bus */
        v |= b << (8 * i);
    }
    return v;
}

uint32_t gba_read8(uint32_t a) {
    switch (a >> 24) {
    case 0: return 0;
    case 2: return ewram[a & 0x3FFFF];
    case 3: return iwram[a & 0x7FFF];
    case 4: case 5: case 6: case 7: return hw_read(a, 1);
    case 8: case 9: case 10: case 11: case 12: case 13: return rom_read(a, 1);
    case 14: case 15: return sram[a & 0x7FFF];
    }
    gba_bus_unmapped(a, 1, 0);
    return 0;
}
uint32_t gba_read16(uint32_t a) {
    a &= ~1u;
    switch (a >> 24) {
    case 0: return 0;
    case 2: { uint32_t o = a & 0x3FFFF; return ewram[o] | ((uint32_t)ewram[o + 1] << 8); }
    case 3: { uint32_t o = a & 0x7FFF; return iwram[o] | ((uint32_t)iwram[o + 1] << 8); }
    case 4: case 5: case 6: case 7: return hw_read(a, 2);
    case 8: case 9: case 10: case 11: case 12: case 13: return rom_read(a, 2);
    case 14: case 15: return sram[a & 0x7FFF] * 0x0101u;
    }
    gba_bus_unmapped(a, 2, 0);
    return 0;
}
uint32_t gba_read32(uint32_t a) {
    a &= ~3u;
    switch (a >> 24) {
    case 0: return 0;
    case 2: { uint32_t v; memcpy(&v, ewram + (a & 0x3FFFF), 4); return v; }
    case 3: { uint32_t v; memcpy(&v, iwram + (a & 0x7FFF), 4); return v; }
    case 4: case 5: case 6: case 7: return hw_read(a, 4);
    case 8: case 9: case 10: case 11: case 12: case 13: return rom_read(a, 4);
    case 14: case 15: return sram[a & 0x7FFF] * 0x01010101u;
    }
    gba_bus_unmapped(a, 4, 0);
    return 0;
}
void gba_write8(uint32_t a, uint32_t v) {
    switch (a >> 24) {
    case 2: ewram[a & 0x3FFFF] = (uint8_t)v; return;
    case 3: iwram[a & 0x7FFF] = (uint8_t)v; return;
    case 4: case 5: case 6: case 7: hw_write(a, 1, v & 0xFF); return;
    case 8: case 9: case 10: case 11: case 12: case 13: return; /* ROM: ignora */
    case 14: case 15: sram[a & 0x7FFF] = (uint8_t)v; return;
    case 0: return;
    }
    gba_bus_unmapped(a, 1, 1);
}
void gba_write16(uint32_t a, uint32_t v) {
    a &= ~1u;
    switch (a >> 24) {
    case 2: { uint32_t o = a & 0x3FFFF; ewram[o] = (uint8_t)v; ewram[o + 1] = (uint8_t)(v >> 8); return; }
    case 3: { uint32_t o = a & 0x7FFF; iwram[o] = (uint8_t)v; iwram[o + 1] = (uint8_t)(v >> 8); return; }
    case 4: case 5: case 6: case 7: hw_write(a, 2, v & 0xFFFF); return;
    case 8: case 9: case 10: case 11: case 12: case 13: return;
    case 14: case 15: sram[a & 0x7FFF] = (uint8_t)v; return; /* HW: so 8 bits (baixo) */
    case 0: return;
    }
    gba_bus_unmapped(a, 2, 1);
}
void gba_write32(uint32_t a, uint32_t v) {
    a &= ~3u;
    switch (a >> 24) {
    case 2: memcpy(ewram + (a & 0x3FFFF), &v, 4); return;
    case 3: memcpy(iwram + (a & 0x7FFF), &v, 4); return;
    case 4: case 5: case 6: case 7: hw_write(a, 4, v); return;
    case 8: case 9: case 10: case 11: case 12: case 13: return;
    case 14: case 15: sram[a & 0x7FFF] = (uint8_t)v; return;
    case 0: return;
    }
    gba_bus_unmapped(a, 4, 1);
}

/* ---------------------------------------------------------------- hw stub */
static uint8_t io[0x400], pal[0x400], vram[0x18000], oam[0x400];
static uint32_t vcount;

static uint32_t io_get(uint32_t off, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)io[(off + (uint32_t)i) & 0x3FF] << (8 * i);
    return v;
}
static void io_put(uint32_t off, int size, uint32_t v) {
    for (int i = 0; i < size; i++) io[(off + (uint32_t)i) & 0x3FF] = (uint8_t)(v >> (8 * i));
}

static void do_dma(int ch) {
    uint32_t b = 0xB0 + (uint32_t)ch * 12;
    uint32_t src = io_get(b, 4), dst = io_get(b + 4, 4);
    uint32_t n = io_get(b + 8, 2), ctl = io_get(b + 10, 2);
    if (n == 0) n = ch == 3 ? 0x10000 : 0x4000;
    int w32 = (ctl >> 10) & 1;
    int step = w32 ? 4 : 2;
    int dctl = (ctl >> 5) & 3, sctl = (ctl >> 7) & 3;
    src &= ch == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu;
    dst &= ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        if (w32) gba_write32(dst, gba_read32(src));
        else gba_write16(dst, gba_read16(src));
        src += (uint32_t)((sctl == 0 || sctl == 3) ? step : sctl == 1 ? -step : 0);
        dst += (uint32_t)((dctl == 0 || dctl == 3) ? step : dctl == 1 ? -step : 0);
    }
    if (!((ctl >> 9) & 1)) io_put(b + 10, 2, ctl & ~0x8000u); /* sem repeat: desliga */
}

WEAK void hw_reset(void) { memset(io, 0, sizeof io); vcount = 0; }
WEAK void hw_step(uint32_t cycles) { vcount = (vcount + cycles / 1232) % 228; }
WEAK uint16_t hw_irq_pending(void) { return 0; }
WEAK void hw_set_keys(uint16_t k) { (void)k; }
WEAK const uint32_t *hw_frame_ready(void) { return 0; }
WEAK const uint32_t *hw_get_framebuffer(void) { return 0; }
WEAK int hw_audio_read(int16_t *lr, int frames) { (void)lr; (void)frames; return 0; }
WEAK int hw_audio_rate(void) { return 48000; }

WEAK uint32_t hw_read(uint32_t a, int size) {
    uint32_t v = 0;
    switch (a >> 24) {
    case 4: {
        uint32_t off = a & 0x3FF;
        if (off == 6) { vcount = (vcount + 1) % 228; return vcount; }
        if (off == 4) return (vcount >= 160 && vcount < 227 ? 1u : 0u) | (io_get(4, 2) & 0xFFF8u);
        if (off == 0x130) return 0x3FF;
        return io_get(off, size);
    }
    case 5:
        for (int i = 0; i < size; i++) v |= (uint32_t)pal[(a + (uint32_t)i) & 0x3FF] << (8 * i);
        return v;
    case 6: {
        uint32_t o = a & 0x1FFFF;
        if (o >= 0x18000) o -= 0x8000;
        for (int i = 0; i < size; i++) v |= (uint32_t)vram[(o + (uint32_t)i) % 0x18000] << (8 * i);
        return v;
    }
    case 7:
        for (int i = 0; i < size; i++) v |= (uint32_t)oam[(a + (uint32_t)i) & 0x3FF] << (8 * i);
        return v;
    }
    return 0;
}

WEAK void hw_write(uint32_t a, int size, uint32_t v) {
    switch (a >> 24) {
    case 4: {
        uint32_t off = a & 0x3FF;
        uint32_t before[4];
        for (int c = 0; c < 4; c++) before[c] = io_get(0xBA + (uint32_t)c * 12, 2) & 0x8000;
        if (off == 0x202) { /* IF: escrever 1 limpa */
            io_put(0x202, 2, io_get(0x202, 2) & ~v);
            return;
        }
        io_put(off, size, v);
        for (int c = 0; c < 4; c++) {
            uint32_t ctl = io_get(0xBA + (uint32_t)c * 12, 2);
            if ((ctl & 0x8000) && !before[c] && ((ctl >> 12) & 3) == 0) do_dma(c);
        }
        return;
    }
    case 5:
        for (int i = 0; i < size; i++) pal[(a + (uint32_t)i) & 0x3FF] = (uint8_t)(v >> (8 * i));
        return;
    case 6: {
        uint32_t o = a & 0x1FFFF;
        if (o >= 0x18000) o -= 0x8000;
        for (int i = 0; i < size; i++) vram[(o + (uint32_t)i) % 0x18000] = (uint8_t)(v >> (8 * i));
        return;
    }
    case 7:
        for (int i = 0; i < size; i++) oam[(a + (uint32_t)i) & 0x3FF] = (uint8_t)(v >> (8 * i));
        return;
    }
}
