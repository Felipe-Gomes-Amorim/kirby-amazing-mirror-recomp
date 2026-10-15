/* HLE da BIOS: so as chamadas que o Kirby usa (ver NOTES.md, "Censo de SWI").
 * Cada uma implementa o efeito arquitetural (registradores/memoria), nao o ciclo.
 * Convencao: rt_swi retorna 0 = continua na proxima instrucao, !=0 = sair da funcao
 * C (cpu->pc ja aponta onde continuar, ou cpu->stop != 0). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_rt.h"

int rt_opt_stop_at_vblank_wait;
void (*rt_frame_hook)(uint64_t);
int rt_swi_count[256];

#define BIOS_IF 0x03007FF8u
#define IRQ_VECTOR 0x03007FFCu

/* ------------------------------------------------------------ matematica */
static void bios_div(Cpu *c) {
    int32_t num = (int32_t)c->r[0], den = (int32_t)c->r[1];
    int32_t q, r;
    if (den == 0) { /* o hw trava; devolve algo previsivel */
        q = num < 0 ? -1 : 1;
        r = num;
    } else if (den == -1) {
        q = (int32_t)(0u - (uint32_t)num);
        r = 0;
    } else {
        q = num / den;
        r = num % den;
    }
    c->r[0] = (uint32_t)q;
    c->r[1] = (uint32_t)r;
    c->r[3] = (uint32_t)(q < 0 ? -(int64_t)q : q);
}

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* Algoritmo da BIOS (polinomio em ponto fixo); escrito de memoria, ainda NAO
 * comparado com a BIOS real - ver NOTES.md. */
static int32_t arctan(int32_t i) {
    int32_t a = -((i * i) >> 14);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}
static int32_t arctan2(int32_t x, int32_t y) {
    if (!y) return x >= 0 ? 0 : 0x8000;
    if (!x) return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) return arctan((y << 14) / x);
        } else if (-x >= y) {
            return arctan((y << 14) / x) + 0x8000;
        }
        return 0x4000 - arctan((x << 14) / y);
    }
    if (x <= 0) {
        if (-x > -y) return arctan((y << 14) / x) + 0x8000;
    } else if (x >= -y) {
        return arctan((y << 14) / x) + 0x10000;
    }
    return 0xC000 - arctan((x << 14) / y);
}

/* ------------------------------------------------------------ copia/fill */
static void cpu_set(Cpu *c) {
    uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2];
    uint32_t n = ctl & 0x1FFFFF;
    int fill = (ctl >> 24) & 1, w32 = (ctl >> 26) & 1;
    if (w32) {
        src &= ~3u; dst &= ~3u;
        uint32_t v = fill ? gba_read32(src) : 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!fill) v = gba_read32(src + 4 * i);
            gba_write32(dst + 4 * i, v);
        }
    } else {
        src &= ~1u; dst &= ~1u;
        uint32_t v = fill ? gba_read16(src) : 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!fill) v = gba_read16(src + 2 * i);
            gba_write16(dst + 2 * i, v);
        }
    }
}

static void cpu_fast_set(Cpu *c) {
    uint32_t src = c->r[0] & ~3u, dst = c->r[1] & ~3u, ctl = c->r[2];
    uint32_t n = ((ctl & 0x1FFFFFu) + 7) & ~7u; /* multiplo de 8 palavras */
    int fill = (ctl >> 24) & 1;
    uint32_t v = fill ? gba_read32(src) : 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!fill) v = gba_read32(src + 4 * i);
        gba_write32(dst + 4 * i, v);
    }
}

/* ------------------------------------------------------------ descompressao */
static void flush_out(uint32_t dst, const uint8_t *out, uint32_t n, int vram) {
    if (vram) {
        for (uint32_t i = 0; i < n; i += 2)
            gba_write16(dst + i, out[i] | ((uint32_t)(i + 1 < n ? out[i + 1] : 0) << 8));
    } else {
        for (uint32_t i = 0; i < n; i++) gba_write8(dst + i, out[i]);
    }
}

static void lz77(Cpu *c, int vram) {
    uint32_t src = c->r[0], dst = c->r[1];
    uint32_t hdr = gba_read32(src);
    uint32_t size = hdr >> 8;
    uint8_t *out = calloc(size + 2, 1);
    uint32_t o = 0;
    src += 4;
    while (o < size) {
        uint32_t flags = gba_read8(src++);
        for (int k = 0; k < 8 && o < size; k++, flags <<= 1) {
            if (flags & 0x80) {
                uint32_t b1 = gba_read8(src++), b2 = gba_read8(src++);
                uint32_t len = (b1 >> 4) + 3, disp = (((b1 & 15) << 8) | b2) + 1;
                for (uint32_t j = 0; j < len && o < size; j++, o++)
                    out[o] = o >= disp ? out[o - disp] : 0;
            } else {
                out[o++] = (uint8_t)gba_read8(src++);
            }
        }
    }
    flush_out(dst, out, size, vram);
    free(out);
}

static void rl(Cpu *c, int vram) {
    uint32_t src = c->r[0], dst = c->r[1];
    uint32_t size = gba_read32(src) >> 8;
    uint8_t *out = calloc(size + 2, 1);
    uint32_t o = 0;
    src += 4;
    while (o < size) {
        uint32_t f = gba_read8(src++);
        if (f & 0x80) {
            uint32_t len = (f & 0x7F) + 3, v = gba_read8(src++);
            for (uint32_t j = 0; j < len && o < size; j++) out[o++] = (uint8_t)v;
        } else {
            uint32_t len = (f & 0x7F) + 1;
            for (uint32_t j = 0; j < len && o < size; j++) out[o++] = (uint8_t)gba_read8(src++);
        }
    }
    flush_out(dst, out, size, vram);
    free(out);
}

/* ------------------------------------------------------------ reset / IRQ */
static void clear_range32(uint32_t a, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 4) gba_write32(a + i, 0);
}

static void ram_reset(uint32_t flags) {
    if (flags & 0x01) clear_range32(0x02000000, 0x40000);
    if (flags & 0x02) clear_range32(0x03000000, 0x8000 - 0x200); /* poupa a pilha/flags */
    if (flags & 0x04) clear_range32(0x05000000, 0x400);
    if (flags & 0x08) clear_range32(0x06000000, 0x18000);
    if (flags & 0x10) clear_range32(0x07000000, 0x400);
    /* registradores de I/O: faixas aproximadas (a BIOS real poe valores iniciais
     * especificos em alguns; zero basta p/ o jogo reinicializar tudo depois) */
    if (flags & 0x20) { clear_range32(0x04000120, 0x10); clear_range32(0x04000134, 0x28); }
    if (flags & 0x40) clear_range32(0x04000060, 0x48);
    if (flags & 0x80) { clear_range32(0x04000000, 0x60); clear_range32(0x040000B0, 0x50); }
}

/* Entrega uma IRQ como o stub da BIOS: salva o contexto, entra em modo IRQ com
 * r0 = 0x04000000 e lr = RT_IRQ_RET, chama o handler em [0x03007FFC] e restaura. */
uint64_t rt_irq_delivered, rt_irq_biosif_by_handler, rt_irq_biosif_by_runtime,
    rt_irq_ack_by_runtime;
static const uint32_t *last_frame;
uint64_t rt_frames_done, rt_frames_limit;
static uint64_t rt_frames_counted;
const uint32_t *rt_last_frame(void) { return last_frame; }

static int in_irq;   /* >0 enquanto o handler de IRQ executa: o relogio do hw fica congelado */

static void irq_deliver(Cpu *c, uint16_t pend) {
    uint32_t sr[13], sflags[4] = {c->n, c->z, c->c, c->v};
    uint32_t smode = c->mode, sthumb = c->thumb, spsr_if = c->psr_if, spc = c->pc;
    memcpy(sr, c->r, sizeof sr);
    rt_set_mode(c, 0x12);
    c->psr_if |= 0x80;
    c->r[14] = RT_IRQ_RET;
    c->r[0] = 0x04000000;
    uint32_t vec = gba_read32(IRQ_VECTOR);
    c->thumb = vec & 1;
    c->pc = vec & ~1u;
    rt_irq_delivered++;
    in_irq++;
    { extern int rt_trace_psr; if (rt_trace_psr) fprintf(stderr, "[irq] deliver pend=%04X vec=%08X from mode %02X pc=%08X\n", pend, vec, smode, spc); }
    int ok_ret = rt_run_until(c, RT_IRQ_RET);
    in_irq--;
    if (!ok_ret) rt_abort(c, "IRQ handler returned to an outer frame");
    { extern int rt_trace_psr; if (rt_trace_psr) fprintf(stderr, "[irq] handler returned pc=%08X mode=%02X\n", c->pc, c->mode); }
    if (c->stop) return;
    /* a BIOS nao reconhece IF; o handler do jogo normalmente faz. Se nao fez, o
     * runtime garante (senao a IRQ dispara de novo para sempre). */
    uint32_t iflag = gba_read16(0x04000202);
    if (iflag & pend) {
        gba_write16(0x04000202, iflag & pend); /* w1c */
        rt_irq_ack_by_runtime++;
    }
    if (gba_read16(BIOS_IF) & pend) {
        rt_irq_biosif_by_handler++;
    } else {
        gba_write16(BIOS_IF, gba_read16(BIOS_IF) | pend);
        rt_irq_biosif_by_runtime++;
    }
    rt_set_mode(c, smode);
    memcpy(c->r, sr, sizeof sr);
    c->n = sflags[0]; c->z = sflags[1]; c->c = sflags[2]; c->v = sflags[3];
    c->thumb = sthumb;
    c->psr_if = spsr_if;
    c->pc = spc;
}

/* chamado em pontos seguros (limites de chamada) e dentro das esperas da BIOS */
void rt_irq_service(Cpu *c) {
    if (c->psr_if & 0x80) return;
    uint16_t pend = hw_irq_pending();
    if (pend) irq_deliver(c, pend);
}

static void grab_frame(void) {
    const uint32_t *fb = hw_frame_ready();
    if (fb) {
        last_frame = fb;
        rt_frames_done++;
    }
}

extern double rt_hwstep_us, rt_now_us(void);
extern uint64_t rt_hwstep_calls, rt_hwstep_cycles;
extern int rt_measure;

static uint32_t timed_step_irq(uint32_t cycles) {
    uint32_t used;
    if (rt_measure) {   /* so mede com --bench/--window: QueryPerformanceCounter custa */
        double t0 = rt_now_us();
        used = hw_step_irq(cycles);
        rt_hwstep_us += rt_now_us() - t0;
    } else {
        used = hw_step_irq(cycles);
    }
    rt_hwstep_calls++;
    rt_hwstep_cycles += used;
    return used;
}

/* Avanca `n` ciclos, mas PARA assim que um IRQ habilitado fica pendente e o entrega na hora
 * (latencia de IRQ ~0: m4aSoundVSync roda sempre em VBlank+0, como no hardware). */
static void run_cycles(Cpu *c, uint32_t n) {
    uint32_t left = n;
    while (left) {
        uint32_t used = timed_step_irq(left);
        if (used == 0) {   /* IRQ ja pendente e nao entregavel (I=1): segue sem parar */
            hw_step(left);
            rt_hwstep_cycles += left;
            used = left;
        }
        left -= used > left ? left : used;
        grab_frame();
        rt_irq_service(c);
        if (c->stop) break;
    }
}

void rt_tick(Cpu *c) {
    static uint32_t acc;
    if (in_irq) return;   /* relogio congelado durante o handler */
    acc += 24; /* estimativa grosseira de ciclos por chamada de funcao */
    if (acc < 1232) return;
    uint32_t n = acc;
    acc = 0;
    run_cycles(c, n);
}

uint32_t rt_loops, rt_loop_site;

int64_t rt_test_budget;   /* >0: testes limitam laços (cada tick = 1024 voltas) */

void rt_tick_loop(Cpu *c, uint32_t site) {
    rt_loop_site = site;
    if (rt_test_budget > 0 && --rt_test_budget == 0) {
        c->stop = RT_STOP_ERROR;
        return;
    }
    if (in_irq) return;   /* relogio congelado durante o handler */
    run_cycles(c, 8192); /* ~1024 voltas de um laco curto */
}

static int vblank_intr_wait(Cpu *c, uint32_t next) {
    gba_write16(BIOS_IF, gba_read16(BIOS_IF) & ~1u);
    gba_write16(0x04000208, 1); /* IME = 1 */
    c->pc = next;
    if (rt_opt_stop_at_vblank_wait) {
        c->stop = RT_STOP_VBLANK_WAIT;
        return 1;
    }
    /* sem IE/DISPSTAT de VBlank o jogo travaria no hw real tambem */
    for (unsigned guard = 0; !(gba_read16(BIOS_IF) & 1); guard++) {
        if (guard > 228 * 3) rt_abort(c, "VBlankIntrWait: no VBlank IRQ within 3 frames");
        run_cycles(c, 1232);
        if (c->stop) return 1;
    }
    c->pc = next;
    rt_frames_counted++;
    if (rt_frame_hook) rt_frame_hook(rt_frames_counted);
    if (c->stop) return 1;
    if (rt_frames_limit && rt_frames_counted >= rt_frames_limit) {
        c->stop = RT_STOP_FRAMES;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ SWI */
int rt_swi(Cpu *c, uint32_t num, uint32_t next) {
    rt_swi_count[num & 0xFF]++;
    switch (num) {
    case 0x00: { /* SoftReset */
        uint8_t flag = (uint8_t)gba_read8(0x03007FFA);
        clear_range32(0x03007E00, 0x200);
        rt_set_mode(c, 0x13); c->r[13] = 0x03007FE0;
        rt_set_mode(c, 0x12); c->r[13] = 0x03007FA0;
        rt_set_mode(c, 0x1F); c->r[13] = 0x03007F00;
        memset(c->r, 0, 13 * sizeof(uint32_t));
        c->r[14] = 0;
        c->thumb = 0;
        c->pc = flag ? 0x02000000 : 0x08000000;
        return 1;
    }
    case 0x01: ram_reset(c->r[0]); return 0;
    case 0x03: /* Stop */
        c->stop = RT_STOP_STOPCALL;
        c->pc = next;
        return 1;
    case 0x05: return vblank_intr_wait(c, next);
    case 0x06: bios_div(c); return 0;
    case 0x08: c->r[0] = isqrt32(c->r[0]); return 0;
    case 0x0A: c->r[0] = (uint32_t)arctan2((int32_t)c->r[0], (int32_t)c->r[1]) & 0xFFFFu; return 0;
    case 0x0B: cpu_set(c); return 0;
    case 0x0C: cpu_fast_set(c); return 0;
    case 0x11: lz77(c, 0); return 0;
    case 0x12: lz77(c, 1); return 0;
    case 0x14: rl(c, 0); return 0;
    case 0x15: rl(c, 1); return 0;
    case 0x19: { /* SoundBiasChange: sem rampa; vai direto ao valor final */
        uint32_t bias = hw_read(0x04000088, 2) & ~0x3FFu;
        hw_write(0x04000088, 2, bias | (c->r[0] ? 0x200u : 0u));
        return 0;
    }
    }
    char msg[64];
    snprintf(msg, sizeof msg, "SWI 0x%02X not implemented in the BIOS HLE", num);
    c->pc = next;
    rt_abort(c, msg);
    return 1;
}
