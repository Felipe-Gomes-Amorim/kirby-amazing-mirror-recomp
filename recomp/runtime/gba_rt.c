/* Runtime do codigo recompilado: acesso a memoria sobre o barramento, PSR/modos,
 * despacho de chamadas (rt_call / rt_run_until) e diagnostico de falhas. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
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

#define HISTN 16
#define HISTM1 15
static uint32_t hist[HISTN];
static unsigned hist_n;
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

int rt_trace_psr;
void rc_msr(Cpu *c, uint32_t mask, uint32_t v) {
    if (rt_trace_psr && rt_trace_psr-- > 0)
        fprintf(stderr, "[psr] msr mask=%u v=%08X (modo %02X sp=%08X lr=%08X) call=%08X\n", mask, v, c->mode, c->r[13], c->r[14], hist[(hist_n + HISTM1) % HISTN]);
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
#define HIST HISTN
uint64_t rt_ncalls;

extern int rt_skip_overlays;
const RcFunc *rt_lookup(uint32_t a, uint32_t thumb);
static const RcFunc *lookup(uint32_t a, uint32_t thumb) {
    unsigned lo = 0, hi = rc_table_n;
    while (lo < hi) { /* primeiro indice com .lo > a */
        unsigned mid = (lo + hi) / 2;
        if (rc_table[mid].lo <= a) lo = mid + 1;
        else hi = mid;
    }
    for (unsigned k = 1; k <= 16 && k <= lo; k++) { /* funcoes podem se intercalar */
        const RcFunc *f = &rc_table[lo - k];
        if (a < f->lo || a >= f->hi || f->thumb != thumb) continue;
        if (f->lo < 0x08000000u && rt_skip_overlays) continue;
        if (f->sig) { /* overlay em RAM: so vale se o codigo na RAM for o recompilado */
            uint32_t h = 0x811C9DC5u;
            for (uint32_t p = f->lo; p < f->hi; p += 2) {
                uint32_t w = gba_read16(p);
                h = (h ^ (w & 0xFF)) * 0x01000193u;
                h = (h ^ (w >> 8)) * 0x01000193u;
            }
            if (!h) h = 1;
            if (h != f->sig) continue;
        }
        return f;
    }
    return 0;
}

const char *rt_dump_label = "PARADA";   /* "AVISO" para contextos informativos */

void rt_dump(Cpu *c, const char *why) {
    fprintf(stderr, "\n[rt] %s: %s\n", rt_dump_label, why);
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

jmp_buf *rt_abort_jmp;   /* testes: aborta por longjmp em vez de exit */
int rt_quiet;
uint64_t rt_abort_count;

void rt_abort(Cpu *c, const char *why) {
    if (rt_abort_jmp) {
        rt_abort_count++;
        longjmp(*rt_abort_jmp, 1);
    }
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

const char *rt_unresolved_path;
uint64_t rt_unresolved_count;
int rt_stop_on_unresolved;

const RcFunc *rt_lookup(uint32_t a, uint32_t thumb) { return lookup(a, thumb); }

/* Endereco sem funcao recompilada: registra 1x (com os 32 primeiros bytes do corpo, semente
 * de analise) em unresolved.log; --stop-on-unresolved aborta (modo de construcao). Fora disso o
 * chamador cai no interpretador (rt_interp.c). */
static void body_hex(char *out, uint32_t a, unsigned n) {
    static const char *hx = "0123456789abcdef";
    for (unsigned i = 0; i < n; i++) {
        uint32_t b = gba_read8(a + i);
        out[2 * i] = hx[b >> 4];
        out[2 * i + 1] = hx[b & 15];
    }
    out[2 * n] = 0;
}

static uint32_t body_hash(uint32_t a, unsigned n) { /* FNV-1a 32 */
    uint32_t h = 0x811C9DC5u;
    for (unsigned i = 0; i < n; i++) h = (h ^ gba_read8(a + i)) * 0x01000193u;
    return h;
}

uint64_t rt_outside_hits;

static void log_outside(Cpu *c, uint32_t a, const RcFunc *f) {
    static uint32_t seen[512];
    unsigned h = (a >> 1) & 511;
    for (unsigned k = 0; k < 512 && seen[(h + k) & 511]; k++)
        if (seen[(h + k) & 511] == a) return;
    for (unsigned k = 0; k < 512; k++)
        if (!seen[(h + k) & 511]) { seen[(h + k) & 511] = a; break; }
    if (rt_unresolved_path) {
        FILE *fp = fopen(rt_unresolved_path, "a");
        if (fp) {
            char hex[65];
            body_hex(hex, a, 32);
            fprintf(fp, "%08X,%s,lr=%08X,sp=%08X,body=%s,kind=outside-set,fn=%08X..%08X\n", a,
                    c->thumb ? "thumb" : "arm", c->r[14], c->r[13], hex, f->lo, f->hi);
            fclose(fp);
        }
    }
}

static void log_unresolved(Cpu *c, uint32_t a) {
    static uint32_t seen[4096];
    unsigned h = (a >> 1) & 4095;
    for (unsigned k = 0; k < 4096 && seen[(h + k) & 4095]; k++)
        if (seen[(h + k) & 4095] == a) { rt_unresolved_count++; return; }
    for (unsigned k = 0; k < 4096; k++)
        if (!seen[(h + k) & 4095]) { seen[(h + k) & 4095] = a; break; }
    rt_unresolved_count++;
    if (rt_unresolved_path) {
        FILE *f = fopen(rt_unresolved_path, "a");
        if (f) {
            char hex[65];
            body_hex(hex, a, 32);
            fprintf(f, "%08X,%s,lr=%08X,sp=%08X,body=%s\n", a, c->thumb ? "thumb" : "arm", c->r[14],
                    c->r[13], hex);
            fclose(f);
        }
    }
}

/* Rotinas da biblioteca SRAM_V113 que o jogo copia para a PILHA (o SRAM nao pode ser acessado
 * com codigo na ROM) e executa de la; o `sp` varia a cada chamada. Os corpos sao posicao-
 * independentes: se os bytes em pc batem com o corpo ROM, executa a funcao nativa da ROM
 * (mesmos registradores; retorna por `bx r0/r1` normalmente). */
static const struct { uint32_t rom, size; } sram_bodies[] = {
    {0x08159158u, 0x22}, /* ReadSram   (copiada por ReadSramFast  0x0815917C) */
    {0x08159220u, 0x30}, /* VerifySram (copiada por VerifySramFast 0x08159250) */
};
uint64_t rt_sram_alias_hits;
int rt_skip_overlays;   /* testes: ignora funcoes em RAM da tabela (forca o interpretador) */
int rt_no_alias;   /* testes: forca o fallback em vez do alias SRAM */

static uint32_t sram_alias(uint32_t a) {
    for (unsigned k = 0; k < sizeof sram_bodies / sizeof sram_bodies[0]; k++) {
        uint32_t i;
        for (i = 0; i < sram_bodies[k].size; i++)
            if (gba_read8(a + i) != gba_read8(sram_bodies[k].rom + i)) break;
        if (i == sram_bodies[k].size) return sram_bodies[k].rom;
    }
    return 0;
}

void rt_call(Cpu *c) {
    uint32_t a = c->pc;
    if (a == 0x0815917Cu && c->thumb && !rt_no_alias) a = c->pc = 0x08159158u; /* ReadSramFast -> ReadSram */
    const RcFunc *f = lookup(a, c->thumb);
    if (!f && c->thumb && !rt_no_alias && a >= 0x02000000u && a < 0x04000000u) {
        uint32_t rom_fn = sram_alias(a);
        if (rom_fn) {
            rt_sram_alias_hits++;
            a = c->pc = rom_fn;
            f = lookup(a, c->thumb);
        }
    }
    if (!f) {
        log_unresolved(c, a);
        int in_ram = a >= 0x02000000u && a < 0x04000000u;
        int in_rom = a >= 0x08000000u && a < 0x08000000u + gba_rom_size();
        if (rt_stop_on_unresolved || !(in_ram || in_rom)) {
            /* o que sobra de abort: 64 bytes no pc + hash p/ a Poppy/Riven decidirem
             * (overlay novo? alias como o SRAM?) */
            char hex[129];
            if (rt_quiet) rt_abort(c, "sem funcao");
            body_hex(hex, a, 64);
            fprintf(stderr, "\n[rt] SEM FUNCAO em %08X (%s): 64 bytes=%s\n", a, c->thumb ? "thumb" : "arm", hex);
            fprintf(stderr, "[rt] hash do corpo (FNV-1a, 64 bytes)=%08X; 32 bytes=%08X\n", body_hash(a, 64),
                    body_hash(a, 32));
            rt_abort(c, "endereco sem funcao recompilada (RAM/overlay ou ROM sem saida)");
        }
        /* fallback: interpreta ate cair em funcao conhecida / retorno esperado */
        extern int rt_interp_run(Cpu *, uint32_t, uint32_t, uint64_t);
        static int warned;
        if (rt_interp_run(c, 0, 0, 50000000ull) && !warned++ && !rt_quiet)
            fprintf(stderr, "[rt] interpretador: 50M passos sem sair (pc=%08X); continua\n", c->pc);
        return;
    }
    hist[hist_n++ % HIST] = a;
    if (rt_trace_psr && rt_irq_delivered) {
        static int n;
        if (n++ < 80)
            fprintf(stderr, "[call] %08X %s lr=%08X modo=%02X\n", a, c->thumb ? "T" : "A", c->r[14],
                    c->mode);
    }
    rt_ncalls++;
    f->fn(c);
    if (c->pc == a && !c->stop) {
        /* alvo de salto dinamico (tabela de saltos truncada etc.) fora do conjunto de instrucoes
         * da funcao: loga 1x e interpreta (como RAM desconhecida); o interpretador nao sai da
         * regiao da funcao ate o fluxo voltar a uma funcao/retorno conhecido. */
        log_outside(c, a, f);
        rt_outside_hits++;
        extern int rt_interp_run(Cpu *, uint32_t, uint32_t, uint64_t);
        if (rt_stop_on_unresolved)
            rt_abort(c, "funcao saiu sem avancar o pc (alvo fora do conjunto)");
        rt_interp_run(c, f->lo, f->hi, 50000000ull);
    }
}

uint32_t rt_rets[4096];
uint32_t rt_depth;

int rt_run_until(Cpu *c, uint32_t ret) {
    unsigned base = rt_depth;
    rt_rets[rt_depth++ & 4095u] = ret;
    for (;;) {
        if (c->pc == ret || c->stop) {
            rt_depth = base;
            return 1;
        }
        for (unsigned k = 0; k < base; k++) {
            if (rt_rets[k & 4095u] == c->pc) { /* retorno de um frame externo: desenrola */
                rt_depth = base;
                return 0;
            }
        }
        rt_call(c);
    }
}

void rt_cpu_reset(Cpu *c) {
    memset(c, 0, sizeof *c);
    c->mode = 0x13; /* SVC */
    c->psr_if = 0xC0;
}
