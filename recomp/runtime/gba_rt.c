/* Runtime do codigo recompilado: acesso a memoria sobre o barramento, PSR/modos,
 * despacho de chamadas (rt_call / rt_run_until) e diagnostico de falhas. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_rt.h"

static uint32_t ror32(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

/* ---- acesso a memoria (o barramento nao rotaciona nem estende sinal) ---- */
uint32_t rc_ldr32(Cpu *c, uint32_t a) { (void)c; return ror32(gba_read32(a), (a & 3) * 8); }
uint32_t rc_ld32(Cpu *c, uint32_t a) { (void)c; return gba_read32(a); }
uint32_t rc_ld16(Cpu *c, uint32_t a) { (void)c; return ror32(gba_read16(a), (a & 1) * 8); }
uint32_t rc_ld16s(Cpu *c, uint32_t a) {
    (void)c;
    if (a & 1) return (uint32_t)(int32_t)(int8_t)gba_read8(a); /* ARM7: LDRSH impar = LDRSB */
    return (uint32_t)(int32_t)(int16_t)gba_read16(a);
}
uint32_t rc_ld8(Cpu *c, uint32_t a) { (void)c; return gba_read8(a); }
uint32_t rc_ld8s(Cpu *c, uint32_t a) { (void)c; return (uint32_t)(int32_t)(int8_t)gba_read8(a); }
void rc_st32(Cpu *c, uint32_t a, uint32_t v) { (void)c; gba_write32(a, v); }
void rc_st16(Cpu *c, uint32_t a, uint32_t v) { (void)c; gba_write16(a, v); }
void rc_st8(Cpu *c, uint32_t a, uint32_t v) { (void)c; gba_write8(a, v); }
int rc_trace(Cpu *c, uint32_t addr) { (void)c; (void)addr; return 0; }

/* ---- modos / CPSR ---- */
static int bank_idx(uint32_t mode) { return (mode == 0x1F ? 0x10 : (int)mode) & 15; }

void rt_set_mode(Cpu *c, uint32_t nm) {
    nm &= 0x1F;
    int o = bank_idx(c->mode), n = bank_idx(nm);
    if (o != n) {
        c->bank_sp[o] = c->r[13];
        c->bank_lr[o] = c->r[14];
        c->r[13] = c->bank_sp[n];
        c->r[14] = c->bank_lr[n];
    }
    c->mode = nm;
}

void rc_msr(Cpu *c, uint32_t mask, uint32_t v) {
    if (mask & 1) {
        rt_set_mode(c, v);
        c->psr_if = v & 0xC0;
    }
    if (mask & 8) {
        c->n = v >> 31;
        c->z = (v >> 30) & 1;
        c->c = (v >> 29) & 1;
        c->v = (v >> 28) & 1;
    }
}

uint32_t rc_mrs(Cpu *c) {
    return (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) | c->psr_if |
           (c->thumb << 5) | c->mode;
}

/* ---- despacho ---- */
#define HIST 16
static uint32_t hist[HIST];
static unsigned hist_n;
uint64_t rt_ncalls;

static const RcFunc *lookup(uint32_t a) {
    unsigned lo = 0, hi = rc_table_n;
    while (lo < hi) { /* primeiro indice com .lo > a */
        unsigned mid = (lo + hi) / 2;
        if (rc_table[mid].lo <= a) lo = mid + 1;
        else hi = mid;
    }
    for (unsigned k = 1; k <= 16 && k <= lo; k++) { /* funcoes podem se intercalar */
        const RcFunc *f = &rc_table[lo - k];
        if (a >= f->lo && a < f->hi) return f;
    }
    return 0;
}

void rt_dump(Cpu *c, const char *why) {
    fprintf(stderr, "\n[rt] PARADA: %s\n", why);
    fprintf(stderr, "[rt] pc=%08X lr=%08X sp=%08X %s modo=%02X NZCV=%u%u%u%u calls=%llu\n", c->pc,
            c->r[14], c->r[13], c->thumb ? "thumb" : "arm", c->mode, c->n, c->z, c->c, c->v,
            (unsigned long long)rt_ncalls);
    for (int i = 0; i < 13; i += 4)
        fprintf(stderr, "[rt] r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X\n", i, c->r[i], i + 1,
                c->r[i + 1], i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    fprintf(stderr, "[rt] ultimas chamadas (antigas -> recentes):");
    for (unsigned k = 0; k < HIST; k++) {
        uint32_t a = hist[(hist_n + k) % HIST];
        if (a) fprintf(stderr, " %08X", a);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

const char *rt_dump_path;

void rt_abort(Cpu *c, const char *why) {
    rt_dump(c, why);
    if (rt_dump_path) {
        FILE *f = fopen(rt_dump_path, "wb");
        if (f) {
            fwrite(gba_iwram(), 1, 0x8000, f);
            fwrite(gba_ewram(), 1, 0x40000, f);
            fclose(f);
        }
    }
    c->stop = RT_STOP_ERROR;
    exit(2);
}

void rt_call(Cpu *c) {
    uint32_t a = c->pc;
    const RcFunc *f = lookup(a);
    if (!f) rt_abort(c, "endereco sem funcao recompilada (candidato a interpretador/mapa)");
    if (f->thumb != c->thumb) rt_abort(c, "modo ARM/Thumb incompativel com a funcao");
    if (f->sig && gba_read32(f->lo) != f->sig)
        rt_abort(c, "codigo em RAM (overlay) mudou desde a recompilacao");
    hist[hist_n++ % HIST] = a;
    rt_ncalls++;
    f->fn(c);
    if (c->pc == a && !c->stop) rt_abort(c, "funcao saiu sem avancar o pc (alvo fora do conjunto)");
}

void rt_run_until(Cpu *c, uint32_t ret) {
    while (c->pc != ret && !c->stop) rt_call(c);
}

void rt_cpu_reset(Cpu *c) {
    memset(c, 0, sizeof *c);
    c->mode = 0x13; /* SVC */
    c->psr_if = 0xC0;
}
