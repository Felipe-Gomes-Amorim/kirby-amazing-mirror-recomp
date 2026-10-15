/* hw.c - I/O, paleta, VRAM, OAM, PPU (scanline), DMA, timers, IRQ. Ver NOTES.md "Hardware GBA (hw/)".
 * Tempo: 1232 ciclos/linha (HBlank a partir de 960), 228 linhas, 280896 ciclos/frame.
 * DMA e instantaneo (nao consome ciclos). */
#include <string.h>
#include "hw.h"

#define CYC_LINE 1232
#define CYC_HBL  960
#define LINES    228

enum { DISPCNT = 0x00, DISPSTAT = 0x04, VCOUNT = 0x06, IE = 0x200, IF = 0x202, IME = 0x208 };

static uint8_t io[0x400];          /* valor escrito (cru); leituras dinamicas sobrepoem */
static uint8_t pal[0x400];
static uint8_t vram[0x18000];
static uint8_t oam[0x400];
static uint32_t fb[240 * 160];
static int frame_flag;
static uint16_t keyinput;
static uint16_t if_reg;
static uint32_t line_cyc;
static int vcount;
static uint8_t rd_ok[0x200];       /* por halfword: 1 = legivel; senao le 0 */

typedef struct { uint16_t cnt; uint32_t acc; } Timer;
static Timer tm[4];
typedef struct { uint32_t src, dst, cnt; } Dma;
static Dma dma[4];

static inline uint16_t R16(int o) { return (uint16_t)(io[o] | io[o + 1] << 8); }
static inline void W16(int o, uint16_t v) { io[o] = (uint8_t)v; io[o + 1] = (uint8_t)(v >> 8); }
static inline uint16_t V16(uint32_t o) { return (uint16_t)(vram[o] | vram[o + 1] << 8); }
static inline uint16_t P16(uint32_t o) { return (uint16_t)(pal[o] | pal[o + 1] << 8); }

static void mark(int lo, int hi) { for (int o = lo; o <= hi; o += 2) rd_ok[o >> 1] = 1; }

void hw_reset(void) {
    memset(io, 0, sizeof io); memset(pal, 0, sizeof pal); memset(vram, 0, sizeof vram);
    memset(oam, 0, sizeof oam); memset(fb, 0, sizeof fb); memset(tm, 0, sizeof tm);
    memset(dma, 0, sizeof dma); memset(rd_ok, 0, sizeof rd_ok);
    frame_flag = 0; keyinput = 0x03FF; if_reg = 0; line_cyc = 0; vcount = 0;
    W16(DISPCNT, 0x0080);   /* forced blank, como a BIOS deixa */
    mark(0x00, 0x00); mark(0x04, 0x0E); mark(0x48, 0x4A); mark(0x50, 0x52); mark(0x60, 0x9E);
    for (int n = 0; n < 4; n++) mark(0xBA + 12 * n, 0xBA + 12 * n);
    mark(0x100, 0x10E); mark(0x120, 0x12A); mark(0x130, 0x134); mark(0x140, 0x15A);
    mark(0x200, 0x204); mark(0x208, 0x208);
}

/* ---------------- IRQ ---------------- */
static void raise_if(uint16_t bits) { if_reg |= bits; }
uint16_t hw_irq_pending(void) { return (R16(IME) & 1) ? (uint16_t)(R16(IE) & if_reg & 0x3FFF) : 0; }
void hw_set_keys(uint16_t k) { keyinput = k & 0x03FF; }

/* ---------------- DMA ---------------- */
static uint32_t io32(int o) {
    return io[o] | io[o + 1] << 8 | io[o + 2] << 16 | (uint32_t)io[o + 3] << 24;
}
static void dma_run(int n) {
    uint16_t ctl = R16(0xBA + 12 * n);
    int w32 = ctl >> 10 & 1, step = w32 ? 4 : 2, dc = ctl >> 5 & 3, sc = ctl >> 7 & 3;
    Dma *d = &dma[n];
    for (uint32_t i = 0; i < d->cnt; i++) {
        if (w32) gba_write32(d->dst & ~3u, gba_read32(d->src & ~3u));
        else     gba_write16(d->dst & ~1u, gba_read16(d->src & ~1u));
        d->src += sc == 1 ? -step : sc == 2 ? 0 : step;
        d->dst += dc == 1 ? -step : dc == 2 ? 0 : step;
    }
    if (ctl & 0x4000) raise_if((uint16_t)(0x100 << n));
    uint32_t max = n == 3 ? 0x10000 : 0x4000;
    if ((ctl & 0x200) && (ctl >> 12 & 3)) {            /* repeat: continua armado */
        uint32_t c = R16(0xB8 + 12 * n); d->cnt = c ? c : max;
        if (dc == 3) d->dst = io32(0xB4 + 12 * n) & (n == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    } else {
        W16(0xBA + 12 * n, ctl & 0x7FFF);
    }
}
static void dma_trigger(int timing) {
    for (int n = 0; n < 4; n++) {
        uint16_t ctl = R16(0xBA + 12 * n);
        if ((ctl & 0x8000) && (ctl >> 12 & 3) == timing) dma_run(n);
    }
}
static void dma_enable(int n) {
    int b = 0xB0 + 12 * n;
    uint32_t c = R16(b + 8), max = n == 3 ? 0x10000 : 0x4000;
    dma[n].src = io32(b) & (n == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu);
    dma[n].dst = io32(b + 4) & (n == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    dma[n].cnt = c ? c : max;
    if ((R16(b + 10) >> 12 & 3) == 0) dma_run(n);      /* imediato */
}

/* ---------------- Timers ---------------- */
static void tm_ticks(int t, uint32_t n) {
    while (n) {
        uint32_t space = 0x10000u - tm[t].cnt;
        if (n < space) { tm[t].cnt = (uint16_t)(tm[t].cnt + n); return; }
        n -= space; tm[t].cnt = R16(0x100 + 4 * t);
        uint16_t ctl = R16(0x102 + 4 * t);
        if (ctl & 0x40) raise_if((uint16_t)(0x08 << t));
        if (t < 3) {
            uint16_t nx = R16(0x102 + 4 * (t + 1));
            if ((nx & 0x80) && (nx & 4)) tm_ticks(t + 1, 1);
        }
    }
}
static void timers_advance(uint32_t c) {
    static const uint32_t div[4] = { 1, 64, 256, 1024 };
    for (int t = 0; t < 4; t++) {
        uint16_t ctl = R16(0x102 + 4 * t);
        if (!(ctl & 0x80) || ((ctl & 4) && t > 0)) continue;
        uint32_t dv = div[ctl & 3];
        tm[t].acc += c;
        uint32_t n = tm[t].acc / dv; tm[t].acc %= dv;
        if (n) tm_ticks(t, n);
    }
}

/* ---------------- PPU ---------------- */
static inline uint32_t rgb(uint16_t c) {
    uint32_t r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

static void render_text_bg(int b, int y, int32_t *out) {
    uint16_t cnt = R16(0x08 + 2 * b);
    uint32_t cbase = (cnt >> 2 & 3) * 0x4000u, sbase = (cnt >> 8 & 31) * 0x800u;
    int c256 = cnt >> 7 & 1, size = cnt >> 14;
    int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    int hofs = R16(0x10 + 4 * b) & 0x1FF, vofs = R16(0x12 + 4 * b) & 0x1FF;
    int py = (y + vofs) & (h - 1);
    for (int x = 0; x < 240; x++) {
        int px = (x + hofs) & (w - 1);
        uint32_t sb = sbase + ((uint32_t)(py >> 8) * (w >> 8) + (px >> 8)) * 0x800u;
        uint16_t e = V16(sb + (((py & 255) >> 3) * 32 + ((px & 255) >> 3)) * 2);
        int ix = px & 7, iy = py & 7;
        if (e & 0x400) ix = 7 - ix;
        if (e & 0x800) iy = 7 - iy;
        uint32_t a; int idx;
        if (c256) { a = cbase + (e & 0x3FF) * 64u + iy * 8 + ix; idx = a < 0x18000 ? vram[a] : 0; }
        else {
            a = cbase + (e & 0x3FF) * 32u + iy * 4 + (ix >> 1);
            idx = a < 0x18000 ? ((ix & 1) ? vram[a] >> 4 : vram[a] & 15) : 0;
            if (idx) idx |= (e >> 12) << 4;
        }
        out[x] = idx ? P16(idx * 2) : -1;
    }
}

static void render_objs(int y, int32_t *col, uint8_t *prio) {
    static const uint8_t sz[3][4][2] = {
        { {8,8},{16,16},{32,32},{64,64} }, { {16,8},{32,8},{32,16},{64,32} }, { {8,16},{8,32},{16,32},{32,64} } };
    int one_d = R16(DISPCNT) >> 6 & 1;
    for (int i = 0; i < 128; i++) {
        uint16_t a0 = oam[i * 8] | oam[i * 8 + 1] << 8, a1 = oam[i * 8 + 2] | oam[i * 8 + 3] << 8,
                 a2 = oam[i * 8 + 4] | oam[i * 8 + 5] << 8;
        if (a0 & 0x300) continue;                 /* affine / desabilitado: nao suportado */
        int shape = a0 >> 14; if (shape == 3) continue;
        int w = sz[shape][a1 >> 14][0], h = sz[shape][a1 >> 14][1];
        int oy = (y - (a0 & 255)) & 255; if (oy >= h) continue;
        int ox = a1 & 0x1FF; if (ox >= 256) ox -= 512;
        int c256 = a0 >> 13 & 1, p = a2 >> 10 & 3, step = c256 ? 2 : 1;
        int ly = (a1 & 0x2000) ? h - 1 - oy : oy;
        for (int lx = 0; lx < w; lx++) {
            int x = ox + lx; if (x < 0 || x >= 240) continue;
            int sx = (a1 & 0x1000) ? w - 1 - lx : lx;
            int tx = sx >> 3, ty = ly >> 3, ix = sx & 7, iy = ly & 7;
            int tile = (a2 & 0x3FF) + (one_d ? (ty * (w >> 3) + tx) * step : ty * 32 + tx * step);
            uint32_t a; int c;
            if (c256) {
                a = 0x10000 + (tile & 0x3FF) * 32u + iy * 8 + ix;
                c = vram[a] ? P16(0x200 + vram[a] * 2) : -1;
            } else {
                a = 0x10000 + (tile & 0x3FF) * 32u + iy * 4 + (ix >> 1);
                int idx = (ix & 1) ? vram[a] >> 4 : vram[a] & 15;
                c = idx ? P16(0x200 + ((a2 >> 12) * 16 + idx) * 2) : -1;
            }
            if (c < 0) continue;
            if (col[x] < 0 || p < prio[x]) { col[x] = c; prio[x] = (uint8_t)p; }
        }
    }
}

static void render_line(int y) {
    uint32_t *row = &fb[y * 240];
    uint16_t dc = R16(DISPCNT);
    if (dc & 0x80) { for (int x = 0; x < 240; x++) row[x] = 0xFFFFFF; return; }
    int mode = dc & 7;
    int32_t bg[4][240]; int32_t ob[240]; uint8_t op[240];
    for (int b = 0; b < 4; b++) for (int x = 0; x < 240; x++) bg[b][x] = -1;
    for (int b = 0; b < 4; b++) {
        if (!(dc >> (8 + b) & 1)) continue;
        if (mode == 0 || (mode == 1 && b < 2)) render_text_bg(b, y, bg[b]);
        else if (b == 2 && mode == 3) { for (int x = 0; x < 240; x++) bg[2][x] = V16((y * 240 + x) * 2); }
        else if (b == 2 && mode == 4) {
            uint32_t base = (dc & 0x10) ? 0xA000 : 0;
            for (int x = 0; x < 240; x++) { int i = vram[base + y * 240 + x]; bg[2][x] = i ? P16(i * 2) : -1; }
        } else if (b == 2 && mode == 5 && y < 128) {
            uint32_t base = (dc & 0x10) ? 0xA000 : 0;
            for (int x = 0; x < 160; x++) bg[2][x] = V16(base + (y * 160 + x) * 2);
        }                                          /* mode 1/2 afim: nao suportado */
    }
    for (int x = 0; x < 240; x++) { ob[x] = -1; op[x] = 4; }
    if (dc & 0x1000) render_objs(y, ob, op);
    uint32_t back = rgb(P16(0));
    for (int x = 0; x < 240; x++) {
        uint32_t c = back;
        for (int p = 0; p < 4; p++) {
            if (ob[x] >= 0 && op[x] == p) { c = rgb((uint16_t)ob[x]); break; }
            int hit = 0;
            for (int b = 0; b < 4 && !hit; b++)
                if (bg[b][x] >= 0 && (R16(0x08 + 2 * b) & 3) == p) { c = rgb((uint16_t)bg[b][x]); hit = 1; }
            if (hit) break;
        }
        row[x] = c;
    }
}

/* ---------------- Tempo ---------------- */
static void enter_hblank(void) {
    if (vcount < 160) { render_line(vcount); dma_trigger(2); }
    if (R16(DISPSTAT) & 0x10) raise_if(0x02);
}
static void end_line(void) {
    line_cyc = 0;
    vcount = (vcount + 1) % LINES;
    uint16_t ds = R16(DISPSTAT);
    if (vcount == 160) {
        frame_flag = 1;
        if (ds & 0x08) raise_if(0x01);
        dma_trigger(1);
    }
    if (vcount == (ds >> 8) && (ds & 0x20)) raise_if(0x04);
}
void hw_step(uint32_t cycles) {
    while (cycles) {
        uint32_t to = line_cyc < CYC_HBL ? CYC_HBL - line_cyc : CYC_LINE - line_cyc;
        uint32_t c = cycles < to ? cycles : to;
        timers_advance(c); line_cyc += c; cycles -= c;
        if (line_cyc == CYC_HBL) enter_hblank();
        else if (line_cyc == CYC_LINE) end_line();
    }
}
const uint32_t *hw_frame_ready(void) {
    if (!frame_flag) return 0;
    frame_flag = 0; return fb;
}

/* ---------------- Barramento ---------------- */
static uint16_t io_rd16(uint32_t o) {
    o &= ~1u;
    if (o >= 0x400 || !rd_ok[o >> 1]) return 0;
    switch (o) {
    case DISPSTAT: {
        uint16_t v = R16(DISPSTAT) & 0xFFF8;
        if (vcount >= 160 && vcount <= 226) v |= 1;
        if (line_cyc >= CYC_HBL) v |= 2;
        if (vcount == (v >> 8)) v |= 4;
        return v; }
    case VCOUNT: return (uint16_t)vcount;
    case 0x130:  return keyinput;
    case IF:     return if_reg;
    case 0x100: case 0x104: case 0x108: case 0x10C: return tm[(o - 0x100) >> 2].cnt;
    case IME:    return R16(IME) & 1;
    }
    return R16(o);
}

static void io_wr16(uint32_t o, uint16_t v) {
    o &= ~1u;
    if (o >= 0x400) return;
    switch (o) {
    case DISPSTAT: W16(o, (uint16_t)((R16(o) & 7) | (v & 0xFFF8))); return;
    case VCOUNT: case 0x130: return;
    case IF: if_reg &= (uint16_t)~v; return;
    case IE: W16(o, v & 0x3FFF); return;
    case IME: W16(o, v & 1); return;
    }
    if (o >= 0x100 && o <= 0x10E) {
        int t = (o - 0x100) >> 2;
        if (!(o & 2)) { W16(o, v); return; }               /* reload */
        int was = R16(o) & 0x80;
        W16(o, v);
        if ((v & 0x80) && !was) { tm[t].cnt = R16(0x100 + 4 * t); tm[t].acc = 0; }
        return;
    }
    if (o >= 0xB0 && o <= 0xDE && ((o - 0xB0) % 12) == 10) {  /* DMAxCNT_H */
        int n = (o - 0xB0) / 12, was = R16(o) & 0x8000;
        W16(o, v);
        if ((v & 0x8000) && !was) dma_enable(n);
        return;
    }
    W16(o, v);
}

uint32_t hw_read(uint32_t addr, int size) {
    uint32_t o;
    switch (addr >> 24) {
    case 4:
        o = addr & 0xFFFFFF;
        if (o >= 0x400) return 0;
        if (size == 1) return (io_rd16(o) >> ((o & 1) * 8)) & 0xFF;
        if (size == 2) return io_rd16(o);
        return io_rd16(o) | (uint32_t)io_rd16(o + 2) << 16;
    case 5: o = addr & 0x3FF; break;
    case 7: o = addr & 0x3FF; break;
    case 6: o = addr & 0x1FFFF; if (o >= 0x18000) o -= 0x8000; break;
    default: return 0;
    }
    const uint8_t *m = (addr >> 24) == 5 ? pal : (addr >> 24) == 7 ? oam : vram;
    if (size == 2) o &= ~1u; else if (size == 4) o &= ~3u;
    uint32_t v = 0;
    for (int i = size - 1; i >= 0; i--) v = v << 8 | m[o + i];
    return v;
}

void hw_write(uint32_t addr, int size, uint32_t v) {
    uint32_t o;
    switch (addr >> 24) {
    case 4:
        o = addr & 0xFFFFFF;
        if (o >= 0x400) return;
        if (size == 4) { io_wr16(o & ~3u, (uint16_t)v); io_wr16((o & ~3u) + 2, (uint16_t)(v >> 16)); }
        else if (size == 2) io_wr16(o, (uint16_t)v);
        else if ((o & ~1u) == IF) io_wr16(IF, (uint16_t)((v & 0xFF) << ((o & 1) * 8)));   /* w1c so no byte */
        else if (o >= 0x300) io[o] = (uint8_t)v;                                           /* POSTFLG/HALTCNT */
        else {
            uint16_t h = R16(o & ~1u);
            h = (o & 1) ? (uint16_t)((h & 0xFF) | (v & 0xFF) << 8) : (uint16_t)((h & 0xFF00) | (v & 0xFF));
            io_wr16(o & ~1u, h);
        }
        return;
    case 5: o = addr & 0x3FF;
        if (size == 1) { pal[o & ~1u] = pal[o | 1] = (uint8_t)v; return; }
        break;
    case 6: o = addr & 0x1FFFF; if (o >= 0x18000) o -= 0x8000;
        if (size == 1) {
            uint32_t lim = (R16(DISPCNT) & 7) >= 3 ? 0x14000 : 0x10000;
            if (o < lim) vram[o & ~1u] = vram[o | 1] = (uint8_t)v;
            return;
        }
        break;
    case 7: o = addr & 0x3FF; if (size == 1) return; break;
    default: return;
    }
    uint8_t *m = (addr >> 24) == 5 ? pal : (addr >> 24) == 7 ? oam : vram;
    if (size == 2) o &= ~1u; else if (size == 4) o &= ~3u;
    for (int i = 0; i < size; i++) m[o + i] = (uint8_t)(v >> (8 * i));
}
