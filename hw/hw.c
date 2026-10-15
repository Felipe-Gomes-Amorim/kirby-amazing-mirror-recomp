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
static int32_t aff_x[2], aff_y[2];   /* ponto de referencia interno BG2/BG3 (24.8) */
static uint8_t rd_ok[0x200];       /* por halfword: 1 = legivel; senao le 0 */

typedef struct { uint16_t cnt; uint32_t acc; } Timer;
static Timer tm[4];
typedef struct { uint32_t src, dst, cnt; } Dma;
static Dma dma[4];
#define AUDIO_RATE 48000
#define ARING 16384
typedef struct { int8_t buf[32]; int rd, cnt; int8_t cur; } Fifo;
static Fifo fifo[2];
static int16_t aring[ARING * 2];
static uint32_t a_w, a_r;
static uint64_t a_acc;

typedef struct { int on, len, vol, envt, pos, shadow, swt, swen; int32_t t; } Psg;
static Psg ps[4];            /* 0=pulso+sweep 1=pulso 2=onda 3=ruido */
static uint32_t lfsr, seq_acc;
static int seq_step;
static uint8_t wave[2][16];  /* WAVE_RAM: 2 bancos de 16 bytes (32 amostras de 4 bit) */

static inline uint16_t R16(int o) { return (uint16_t)(io[o] | io[o + 1] << 8); }
static inline void W16(int o, uint16_t v) { io[o] = (uint8_t)v; io[o + 1] = (uint8_t)(v >> 8); }
static inline uint16_t V16(uint32_t o) { return (uint16_t)(vram[o] | vram[o + 1] << 8); }
static inline uint16_t P16(uint32_t o) { return (uint16_t)(pal[o] | pal[o + 1] << 8); }

static void mark(int lo, int hi) { for (int o = lo; o <= hi; o += 2) rd_ok[o >> 1] = 1; }

void hw_reset(void) {
    memset(io, 0, sizeof io); memset(pal, 0, sizeof pal); memset(vram, 0, sizeof vram);
    memset(oam, 0, sizeof oam); memset(fb, 0, sizeof fb); memset(tm, 0, sizeof tm);
    memset(dma, 0, sizeof dma); memset(rd_ok, 0, sizeof rd_ok);
    memset(aff_x, 0, sizeof aff_x); memset(aff_y, 0, sizeof aff_y);
    memset(fifo, 0, sizeof fifo); a_w = a_r = 0; a_acc = 0;
    memset(ps, 0, sizeof ps); memset(wave, 0, sizeof wave); lfsr = 0x7FFF; seq_acc = 0; seq_step = 0;
    frame_flag = 0; keyinput = 0x03FF; if_reg = 0; line_cyc = 0; vcount = 0;
    W16(DISPCNT, 0x0080);   /* forced blank, como a BIOS deixa */
    mark(0x00, 0x00); mark(0x04, 0x0E); mark(0x48, 0x4A); mark(0x50, 0x52); mark(0x60, 0x9E);
    for (int n = 0; n < 4; n++) mark(0xBA + 12 * n, 0xBA + 12 * n);
    mark(0x100, 0x10E); mark(0x120, 0x12A); mark(0x130, 0x134); mark(0x140, 0x15A);
    mark(0x200, 0x204); mark(0x208, 0x208);
}

static uint32_t io32(int o);
/* ---------------- IRQ ---------------- */
static int32_t sx28(uint32_t v) { v &= 0x0FFFFFFF; return (int32_t)(v << 4) >> 4; }
static void aff_latch(int i) {
    aff_x[i] = sx28(io32(0x28 + 0x10 * i));
    aff_y[i] = sx28(io32(0x2C + 0x10 * i));
}
static void raise_if(uint16_t bits) { if_reg |= bits; }
uint16_t hw_irq_pending(void) { return (R16(IME) & 1) ? (uint16_t)(R16(IE) & if_reg & 0x3FFF) : 0; }
void hw_set_keys(uint16_t k) { keyinput = k & 0x03FF; }

/* ---------------- DMA ---------------- */
static uint32_t io32(int o)
{
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

/* ---------------- Audio: FIFO A/B (Direct Sound) ---------------- */

static void fifo_reset(int c) { memset(&fifo[c], 0, sizeof fifo[c]); }
static void fifo_push(int c, uint32_t v, int size) {
    Fifo *f = &fifo[c];
    for (int i = 0; i < size; i++) {
        if (f->cnt == 32) return;                         /* cheia: descarta */
        f->buf[(f->rd + f->cnt) & 31] = (int8_t)(v >> (8 * i));
        f->cnt++;
    }
}
/* FIFO pediu dados: DMA especial (timing 3) cujo destino e o registrador da FIFO */
static void fifo_request(int c) {
    uint32_t addr = c ? 0x040000A4u : 0x040000A0u;
    for (int n = 1; n <= 2; n++) {
        uint16_t ctl = R16(0xBA + 12 * n);
        if (!(ctl & 0x8000) || (ctl >> 12 & 3) != 3 || (dma[n].dst & ~3u) != addr) continue;
        for (int i = 0; i < 4; i++) { gba_write32(addr, gba_read32(dma[n].src & ~3u)); dma[n].src += 4; }
        if (ctl & 0x4000) raise_if((uint16_t)(0x100 << n));
        return;
    }
}
static void audio_timer_overflow(int t) {
    if (!(R16(0x84) & 0x80)) return;
    uint16_t h = R16(0x82);
    for (int c = 0; c < 2; c++) {
        if ((h >> (10 + 4 * c) & 1) != t) continue;
        Fifo *f = &fifo[c];
        if (f->cnt) { f->cur = f->buf[f->rd]; f->rd = (f->rd + 1) & 31; f->cnt--; }
        if (f->cnt <= 16) fifo_request(c);
    }
}
/* ---- PSG (canais 1-4) ---- */
static const uint8_t duty_pat[4] = { 0x01, 0x81, 0x87, 0x7E };   /* MSB primeiro */
static const int len_reg_x[4] = { 0x64, 0x6C, 0x74, 0x7C };
static const int env_reg[4] = { 0x62, 0x68, 0, 0x78 };

static int ps_freq(int c) { return R16(len_reg_x[c]) & 0x7FF; }
static int ps_period(int c) {                         /* ciclos de CPU entre passos */
    switch (c) {
    case 0: case 1: return (2048 - ps_freq(c)) * 16;
    case 2: return (2048 - ps_freq(2)) * 8;
    default: {
        uint16_t r = R16(0x7C); int d = r & 7, sh = r >> 4 & 15;
        return (d ? d * 64 : 32) << sh;
    }
    }
}
static void psg_off_all(void) { memset(ps, 0, sizeof ps); }
static void psg_trigger(int c) {
    Psg *p = &ps[c];
    int dac = c == 2 ? (R16(0x70) & 0x80) != 0 : (R16(env_reg[c]) & 0xF800) != 0;
    p->on = dac;
    if (!p->len) p->len = c == 2 ? 256 : 64;
    if (c != 2) { uint16_t e = R16(env_reg[c]); p->vol = e >> 12; p->envt = e >> 8 & 7; }
    p->t = ps_period(c);
    if (c == 2) p->pos = 0;
    if (c == 3) lfsr = 0x7FFF;
    if (c == 0) {
        uint16_t sw = R16(0x60); int sh = sw & 7, tm_ = sw >> 4 & 7;
        p->shadow = ps_freq(0); p->swt = tm_ ? tm_ : 8; p->swen = tm_ || sh;
        if (sh) {
            int n = p->shadow + ((sw & 8) ? -(p->shadow >> sh) : (p->shadow >> sh));
            if (n > 2047) p->on = 0;
        }
    }
}
static void psg_seq_tick(int step) {
    if (!(step & 1)) {                                           /* length: 256 Hz */
        for (int c = 0; c < 4; c++)
            if (ps[c].on && (R16(len_reg_x[c]) & 0x4000) && ps[c].len > 0 && --ps[c].len == 0) ps[c].on = 0;
    }
    if (step == 2 || step == 6) {                                /* sweep: 128 Hz */
        Psg *p = &ps[0];
        uint16_t sw = R16(0x60); int sh = sw & 7, tm_ = sw >> 4 & 7;
        if (p->on && p->swen && --p->swt <= 0) {
            p->swt = tm_ ? tm_ : 8;
            if (tm_) {
                int n = p->shadow + ((sw & 8) ? -(p->shadow >> sh) : (p->shadow >> sh));
                if (n > 2047) p->on = 0;
                else if (sh) {
                    p->shadow = n;
                    W16(0x64, (uint16_t)((R16(0x64) & ~0x7FF) | n));
                    int n2 = n + ((sw & 8) ? -(n >> sh) : (n >> sh));
                    if (n2 > 2047) p->on = 0;
                }
            }
        }
    }
    if (step == 7) {                                             /* envelope: 64 Hz */
        for (int c = 0; c < 4; c++) {
            if (c == 2 || !ps[c].on) continue;
            uint16_t e = R16(env_reg[c]); int t = e >> 8 & 7;
            if (!t) continue;
            if (--ps[c].envt <= 0) {
                ps[c].envt = t;
                if (e & 0x800) { if (ps[c].vol < 15) ps[c].vol++; }
                else if (ps[c].vol > 0) ps[c].vol--;
            }
        }
    }
}
static void psg_advance(uint32_t cycles) {
    for (int c = 0; c < 4; c++) {
        Psg *p = &ps[c];
        if (!p->on) continue;
        int32_t rem = (int32_t)cycles;
        while (rem > 0) {
            int32_t per = ps_period(c);
            if (p->t > rem) { p->t -= rem; break; }
            rem -= p->t; p->t = per;
            if (c < 2) p->pos = (p->pos + 1) & 7;
            else if (c == 2) p->pos = (p->pos + 1) & ((R16(0x70) & 0x20) ? 63 : 31);
            else {
                uint32_t b = (lfsr ^ (lfsr >> 1)) & 1;
                lfsr = (lfsr >> 1) | (b << 14);
                if (R16(0x7C) & 8) lfsr = (lfsr & ~0x40u) | (b << 6);
            }
        }
    }
    seq_acc += cycles;
    while (seq_acc >= 32768) { seq_acc -= 32768; psg_seq_tick(seq_step); seq_step = (seq_step + 1) & 7; }
}
/* soma os canais PSG em L/R (escala: nivel*vol_mestre*razao*32; PCM 8 bit = 256/unidade) */
static void psg_mix(int *L, int *R) {
    uint16_t cl = R16(0x80), ch = R16(0x82);
    int ratio = (ch & 3) == 0 ? 1 : (ch & 3) == 1 ? 2 : 4;
    int sl = 0, sr = 0;
    for (int c = 0; c < 4; c++) {
        Psg *p = &ps[c];
        if (!p->on) continue;
        int v;
        if (c < 2) { uint8_t pat = duty_pat[R16(c ? 0x68 : 0x62) >> 6 & 3]; v = ((pat >> (7 - p->pos)) & 1) ? p->vol : -p->vol; }
        else if (c == 2) {
            int bank = (R16(0x70) & 0x20) ? (p->pos >> 5) : (R16(0x70) >> 6 & 1);
            int i = p->pos & 31, b = wave[bank][i >> 1];
            int smp = (i & 1) ? b & 15 : b >> 4;
            uint16_t h = R16(0x72);
            int vc = h >> 13 & 3;
            int mul = (h & 0x8000) ? 3 : vc == 1 ? 4 : vc == 2 ? 2 : vc == 3 ? 1 : 0;
            v = (smp * 2 - 15) * mul / 4;
        } else v = (lfsr & 1) ? -p->vol : p->vol;
        if (cl >> (12 + c) & 1) sl += v;
        if (cl >> (8 + c) & 1) sr += v;
    }
    *L += sl * ((cl >> 4 & 7) + 1) * ratio * 32;
    *R += sr * ((cl & 7) + 1) * ratio * 32;
}

static int16_t clip16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static void audio_gen(uint32_t cycles) {
    if (R16(0x84) & 0x80) psg_advance(cycles);
    a_acc += (uint64_t)cycles * AUDIO_RATE;
    while (a_acc >= 16777216ull) {
        a_acc -= 16777216ull;
        int L = 0, R = 0;
        if (R16(0x84) & 0x80) {
            uint16_t h = R16(0x82);
            int a = (fifo[0].cur * 256) >> ((h & 4) ? 0 : 1), b = (fifo[1].cur * 256) >> ((h & 8) ? 0 : 1);
            if (h >> 8 & 1) R += a;
            if (h >> 9 & 1) L += a;
            if (h >> 12 & 1) R += b;
            if (h >> 13 & 1) L += b;
            psg_mix(&L, &R);
        }
        if (a_w - a_r >= ARING) a_r = a_w - ARING + 1;        /* sobrescreve o mais antigo */
        aring[(a_w % ARING) * 2] = clip16(L);
        aring[(a_w % ARING) * 2 + 1] = clip16(R);
        a_w++;
    }
}
int hw_audio_read(int16_t *lr, int frames) {
    int n = 0;
    while (n < frames && a_r != a_w) {
        lr[n * 2] = aring[(a_r % ARING) * 2];
        lr[n * 2 + 1] = aring[(a_r % ARING) * 2 + 1];
        a_r++; n++;
    }
    return n;
}
int hw_audio_rate(void) { return AUDIO_RATE; }

/* ---------------- Timers ---------------- */
static void tm_ticks(int t, uint32_t n) {
    while (n) {
        uint32_t space = 0x10000u - tm[t].cnt;
        if (n < space) { tm[t].cnt = (uint16_t)(tm[t].cnt + n); return; }
        n -= space; tm[t].cnt = R16(0x100 + 4 * t);
        uint16_t ctl = R16(0x102 + 4 * t);
        if (ctl & 0x40) raise_if((uint16_t)(0x08 << t));
        if (t < 2) audio_timer_overflow(t);
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

static void render_affine_bg(int b, int32_t *out) {
    int base = 0x20 + 0x10 * (b - 2);
    int pa = (int16_t)R16(base), pc = (int16_t)R16(base + 4);
    uint16_t cnt = R16(0x08 + 2 * b);
    uint32_t cbase = (cnt >> 2 & 3) * 0x4000u, sbase = (cnt >> 8 & 31) * 0x800u;
    int wrap = cnt >> 13 & 1, dim = 128 << (cnt >> 14);
    int32_t x = aff_x[b - 2], y = aff_y[b - 2];
    for (int sx = 0; sx < 240; sx++, x += pa, y += pc) {
        int tx = x >> 8, ty = y >> 8;
        out[sx] = -1;
        if (tx < 0 || tx >= dim || ty < 0 || ty >= dim) {
            if (!wrap) continue;
            tx &= dim - 1; ty &= dim - 1;
        }
        uint32_t t = vram[sbase + (uint32_t)(ty >> 3) * (dim >> 3) + (tx >> 3)];
        uint32_t a = cbase + t * 64u + (ty & 7) * 8 + (tx & 7);
        int idx = a < 0x18000 ? vram[a] : 0;
        if (idx) out[sx] = P16(idx * 2);
    }
}

static void render_objs(int y, int32_t *col, uint8_t *prio, uint8_t *semi, uint8_t *objwin) {
    static const uint8_t sz[3][4][2] = {
        { {8,8},{16,16},{32,32},{64,64} }, { {16,8},{32,8},{32,16},{64,32} }, { {8,16},{8,32},{16,32},{32,64} } };
    int one_d = R16(DISPCNT) >> 6 & 1;
    uint16_t mos = R16(0x4C);
    int mh = (mos >> 8 & 15) + 1, mv = (mos >> 12 & 15) + 1;
    for (int i = 0; i < 128; i++) {
        uint16_t a0 = oam[i * 8] | oam[i * 8 + 1] << 8, a1 = oam[i * 8 + 2] | oam[i * 8 + 3] << 8,
                 a2 = oam[i * 8 + 4] | oam[i * 8 + 5] << 8;
        if (a0 & 0x300) continue;                 /* affine / desabilitado: nao suportado */
        int shape = a0 >> 14; if (shape == 3) continue;
        int mode = a0 >> 10 & 3; if (mode == 3) continue;
        int w = sz[shape][a1 >> 14][0], h = sz[shape][a1 >> 14][1];
        int yy = (a0 & 0x1000) ? y - y % mv : y;
        int oy = (yy - (a0 & 255)) & 255; if (oy >= h) continue;
        int ox = a1 & 0x1FF; if (ox >= 256) ox -= 512;
        int c256 = a0 >> 13 & 1, p = a2 >> 10 & 3, step = c256 ? 2 : 1;
        int ly = (a1 & 0x2000) ? h - 1 - oy : oy;
        for (int lx = 0; lx < w; lx++) {
            int x = ox + lx; if (x < 0 || x >= 240) continue;
            int lxm = lx;
            if (a0 & 0x1000) { lxm = (x - x % mh) - ox; if (lxm < 0) lxm = 0; if (lxm >= w) lxm = w - 1; }
            int sx = (a1 & 0x1000) ? w - 1 - lxm : lxm;
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
            if (mode == 2) { objwin[x] = 1; continue; }     /* sprite de janela: nao desenha */
            if (col[x] < 0 || p < prio[x]) { col[x] = c; prio[x] = (uint8_t)p; semi[x] = (uint8_t)(mode == 1); }
        }
    }
}

static int in_window(int n, int x, int y) {
    uint16_t hh = R16(0x40 + 2 * n), vv = R16(0x44 + 2 * n);
    int x1 = hh >> 8, x2 = hh & 255, y1 = vv >> 8, y2 = vv & 255;
    if (x2 > 240) x2 = 240;
    if (y2 > 160) y2 = 160;
    int ix = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
    int iy = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
    return ix && iy;
}

/* canais de 5 bits; a/b em BGR555 */
static uint16_t mix15(uint16_t a, uint16_t b, int eva, int evb) {
    uint16_t o = 0;
    for (int s = 0; s <= 10; s += 5) {
        int v = (((a >> s) & 31) * eva + ((b >> s) & 31) * evb) >> 4;
        o |= (uint16_t)((v > 31 ? 31 : v) << s);
    }
    return o;
}
static uint16_t fade15(uint16_t a, int evy, int up) {
    uint16_t o = 0;
    for (int s = 0; s <= 10; s += 5) {
        int c = (a >> s) & 31;
        c = up ? c + (((31 - c) * evy) >> 4) : c - ((c * evy) >> 4);
        o |= (uint16_t)(c << s);
    }
    return o;
}

static void render_line(int y) {
    static int32_t cache[4][240];     /* ultima linha renderizada de cada BG (mosaico vertical) */
    uint32_t *row = &fb[y * 240];
    uint16_t dc = R16(DISPCNT);
    if (dc & 0x80) { for (int x = 0; x < 240; x++) row[x] = 0xFFFFFF; return; }
    int mode = dc & 7;
    uint16_t mos = R16(0x4C);
    int bh = (mos & 15) + 1, bv = (mos >> 4 & 15) + 1;
    int32_t bg[4][240], ob[240]; uint8_t op[240], semi[240], objwin[240], wf[240];
    for (int b = 0; b < 4; b++) for (int x = 0; x < 240; x++) bg[b][x] = -1;
    for (int b = 0; b < 4; b++) {
        if (!(dc >> (8 + b) & 1)) continue;
        int mosaic = R16(0x08 + 2 * b) >> 6 & 1;
        if (mosaic && bv > 1 && (y % bv) != 0) { memcpy(bg[b], cache[b], sizeof bg[b]); }
        else {
            if (mode == 0 || (mode == 1 && b < 2)) render_text_bg(b, y, bg[b]);
            else if ((mode == 1 && b == 2) || (mode == 2 && b >= 2)) render_affine_bg(b, bg[b]);
            else if (b == 2 && mode == 3) { for (int x = 0; x < 240; x++) bg[2][x] = V16((y * 240 + x) * 2); }
            else if (b == 2 && mode == 4) {
                uint32_t base = (dc & 0x10) ? 0xA000 : 0;
                for (int x = 0; x < 240; x++) { int i = vram[base + y * 240 + x]; bg[2][x] = i ? P16(i * 2) : -1; }
            } else if (b == 2 && mode == 5 && y < 128) {
                uint32_t base = (dc & 0x10) ? 0xA000 : 0;
                for (int x = 0; x < 160; x++) bg[2][x] = V16(base + (y * 160 + x) * 2);
            }
            memcpy(cache[b], bg[b], sizeof bg[b]);
        }
        if (mosaic && bh > 1) {
            int32_t t[240];
            for (int x = 0; x < 240; x++) t[x] = bg[b][x - x % bh];
            memcpy(bg[b], t, sizeof t);
        }
    }
    for (int x = 0; x < 240; x++) { ob[x] = -1; op[x] = 4; semi[x] = 0; objwin[x] = 0; }
    if (dc & 0x1000) render_objs(y, ob, op, semi, objwin);

    if (!(dc & 0xE000)) memset(wf, 0x3F, sizeof wf);
    else {
        uint16_t wi = R16(0x48), wo = R16(0x4A);
        for (int x = 0; x < 240; x++) {
            uint8_t f = wo & 0x3F;
            if ((dc & 0x8000) && objwin[x]) f = (wo >> 8) & 0x3F;
            if ((dc & 0x4000) && in_window(1, x, y)) f = (wi >> 8) & 0x3F;
            if ((dc & 0x2000) && in_window(0, x, y)) f = wi & 0x3F;
            wf[x] = f;
        }
    }

    uint16_t bld = R16(0x50), alpha = R16(0x52);
    int bmode = bld >> 6 & 3, t1 = bld & 0x3F, t2 = bld >> 8 & 0x3F;
    int eva = alpha & 31, evb = alpha >> 8 & 31, evy = R16(0x54) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    uint16_t back = P16(0);
    for (int x = 0; x < 240; x++) {
        uint8_t f = wf[x];
        int lay[2] = { 5, 5 }; uint16_t col[2] = { back, back }; int n = 0;
        for (int p = 0; p < 4 && n < 2; p++) {
            if ((f & 0x10) && ob[x] >= 0 && op[x] == p) { lay[n] = 4; col[n] = (uint16_t)ob[x]; n++; if (n == 2) break; }
            for (int b = 0; b < 4 && n < 2; b++)
                if ((f >> b & 1) && bg[b][x] >= 0 && (R16(0x08 + 2 * b) & 3) == p) { lay[n] = b; col[n] = (uint16_t)bg[b][x]; n++; }
        }
        uint16_t c = col[0];
        int top_semi = lay[0] == 4 && semi[x];
        if (top_semi && (t2 >> lay[1] & 1)) c = mix15(col[0], col[1], eva, evb);
        else if (f & 0x20) {
            if (bmode == 1 && (t1 >> lay[0] & 1) && (t2 >> lay[1] & 1)) c = mix15(col[0], col[1], eva, evb);
            else if (bmode == 2 && (t1 >> lay[0] & 1)) c = fade15(col[0], evy, 1);
            else if (bmode == 3 && (t1 >> lay[0] & 1)) c = fade15(col[0], evy, 0);
        }
        row[x] = rgb(c);
    }
}

/* ---------------- Tempo ---------------- */
static void enter_hblank(void) {
    if (vcount < 160) {
        render_line(vcount);
        for (int i = 0; i < 2; i++) {
            aff_x[i] += (int16_t)R16(0x22 + 0x10 * i);
            aff_y[i] += (int16_t)R16(0x26 + 0x10 * i);
        }
        dma_trigger(2);
    }
    if (R16(DISPSTAT) & 0x10) raise_if(0x02);
}
static void end_line(void) {
    line_cyc = 0;
    vcount = (vcount + 1) % LINES;
    if (vcount == 0) { aff_latch(0); aff_latch(1); }
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
#ifdef HW_STEP_CAP
        if (c > HW_STEP_CAP) c = HW_STEP_CAP;       /* so para medir a versao antiga (passo fixo) */
#else
        if (R16(0x84) & 0x80) {                      /* audio ligado: no maximo ate a proxima amostra de saida */
            uint32_t to_s = (uint32_t)((16777216ull - a_acc + AUDIO_RATE - 1) / AUDIO_RATE);
            if (to_s == 0) to_s = 1;
            if (c > to_s) c = to_s;
        }
#endif
        timers_advance(c); audio_gen(c); line_cyc += c; cycles -= c;
        if (line_cyc == CYC_HBL) enter_hblank();
        else if (line_cyc == CYC_LINE) end_line();
    }
}
const uint32_t *hw_get_framebuffer(void) { return fb; }
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
    case 0x84:   return (uint16_t)((R16(0x84) & 0x80) | ps[0].on | ps[1].on << 1 | ps[2].on << 2 | ps[3].on << 3);
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
    case 0x82:
        if (v & 0x0800) fifo_reset(0);
        if (v & 0x8000) fifo_reset(1);
        W16(o, v & 0x770F); return;
    case 0x84:
        if (!(v & 0x80)) { psg_off_all(); memset(&io[0x60], 0, 0x22); }   /* master off zera os registradores PSG */
        W16(o, (uint16_t)(v & 0x80)); return;
    case 0x62: W16(o, v); ps[0].len = 64 - (v & 63); return;
    case 0x68: W16(o, v); ps[1].len = 64 - (v & 63); return;
    case 0x72: W16(o, v); ps[2].len = 256 - (v & 255); return;
    case 0x78: W16(o, v); ps[3].len = 64 - (v & 63); return;
    case 0x64: case 0x6C: case 0x74: case 0x7C:
        W16(o, v & 0x7FFF);
        if (v & 0x8000) psg_trigger(o == 0x64 ? 0 : o == 0x6C ? 1 : o == 0x74 ? 2 : 3);
        return;
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
    if (o >= 0x28 && o <= 0x3E && (o & 0xF) >= 8 && (o & 0xF) <= 0xE) aff_latch((o >> 4) - 2);
}

uint32_t hw_read(uint32_t addr, int size) {
    uint32_t o;
    switch (addr >> 24) {
    case 4:
        o = addr & 0xFFFFFF;
        if (o >= 0x400) return 0;
        if (o >= 0x90 && o < 0xA0) {
            uint32_t v = 0; int bk = !(R16(0x70) >> 6 & 1);
            for (int i = size - 1; i >= 0; i--) v = v << 8 | wave[bk][((o & ~(uint32_t)(size - 1)) + i) & 15];
            return v;
        }
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
        if (o >= 0xA0 && o < 0xA8) { fifo_push(o >= 0xA4, v, size); return; }
        if (o >= 0x90 && o < 0xA0) {
            int bk = !(R16(0x70) >> 6 & 1);
            for (int i = 0; i < size; i++) wave[bk][((o & ~(uint32_t)(size - 1)) + i) & 15] = (uint8_t)(v >> (8 * i));
            return;
        }
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
