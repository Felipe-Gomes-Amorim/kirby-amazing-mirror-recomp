/* Interpretador ARM/Thumb minimo (ARMv4T): fallback para codigo sem funcao recompilada
 * (RAM desconhecida, ROM fora do mapa). Mesma semantica do emissor (emit.py/decode.py):
 * PC le +8/+4 (+12 com shift por registrador), LDR rotaciona, STR/STRH alinham, LDM/STM,
 * BL/BX, MUL/MULL, MRS/MSR, SWI -> HLE.
 * Executa ate o pc cair numa funcao recompilada (fora de [skip_lo,skip_hi)), num retorno
 * esperado por um frame C externo (rt_rets) ou RT_IRQ_RET. Chamadas (bl) para funcoes
 * recompiladas sao feitas como chamadas C normais e a interpretacao continua depois. */
#include <stdio.h>
#include <stdlib.h>
#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_rt.h"

const RcFunc *rt_lookup(uint32_t a, uint32_t thumb);
extern uint32_t rt_loops;

static uint32_t sk_lo, sk_hi;
uint64_t rt_interp_steps, rt_interp_entries;

static uint32_t ror(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

static int cond_ok(const Cpu *c, uint32_t cc) {
    switch (cc) {
    case 0: return c->z;
    case 1: return !c->z;
    case 2: return c->c;
    case 3: return !c->c;
    case 4: return c->n;
    case 5: return !c->n;
    case 6: return c->v;
    case 7: return !c->v;
    case 8: return c->c && !c->z;
    case 9: return !c->c || c->z;
    case 10: return c->n == c->v;
    case 11: return c->n != c->v;
    case 12: return !c->z && c->n == c->v;
    case 13: return c->z || c->n != c->v;
    case 14: return 1;
    }
    return 0;
}

/* devolve 1 se o pc deve sair do interpretador (funcao conhecida ou retorno externo) */
static int should_exit(const Cpu *c) {
    uint32_t pc = c->pc;
    for (uint32_t k = 0; k < rt_depth && k < 4096; k++)
        if (rt_rets[k & 4095u] == pc) return 1;
    if (pc == RT_IRQ_RET) return 1;
    const RcFunc *f = rt_lookup(pc, c->thumb);
    if (f && !(pc >= sk_lo && pc < sk_hi)) return 1;
    return 0;
}

/* pc ja esta em c->pc; true = continuar interpretando */
static int jumped(Cpu *c, uint32_t from) {
    if (c->pc <= from && (++rt_loops & 0x3FFu) == 0) { /* laco: tempo + IRQ, como o compilado */
        uint32_t keep = c->pc;
        rt_tick_loop(c, from);
        c->pc = keep;
    }
    return !c->stop && !should_exit(c);
}

/* o compilado soma 24 ciclos por `bl` direto (nao por veneer `bx rN`): o interpretado idem,
 * para a temporizacao (IRQ/audio) ser a mesma */
static void tick_call(Cpu *c, uint32_t target, int thumb) {
    if (thumb) {
        uint32_t h = gba_read16(target);
        if ((h & 0xFF87u) == 0x4700u) return;   /* veneer: bx rN */
    }
    uint32_t keep = c->pc;
    rt_tick(c);
    c->pc = keep;
}

/* chamada de funcao recompilada a partir do codigo interpretado; 1 = continua em `ret` */
static int call_compiled(Cpu *c, uint32_t target, uint32_t ret_pc) {
    c->pc = target;
    if (c->stop) return 0;
    rt_rets[rt_depth++ & 4095u] = ret_pc;
    rt_call(c);
    rt_depth--;
    if (c->pc != ret_pc && !rt_run_until(c, ret_pc)) return 0;
    return !c->stop;
}

/* ---- ALU ---- */
static void setnz(Cpu *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }

/* devolve 1 se escreveu pc */
static int alu(Cpu *c, uint32_t op, int s, uint32_t rd, uint32_t a, uint32_t b, uint32_t sc, int thumb) {
    uint32_t res, x = 0, y = 0, cin = 0;
    int arith = 0;
    switch (op) {
    case 0: case 8: res = a & b; break;
    case 1: case 9: res = a ^ b; break;
    case 12: res = a | b; break;
    case 13: res = b; break;
    case 14: res = a & ~b; break;
    case 15: res = ~b; break;
    case 2: case 10: x = a; y = ~b; cin = 1; arith = 1; res = 0; break;
    case 3: x = b; y = ~a; cin = 1; arith = 1; res = 0; break;
    case 4: case 11: x = a; y = b; cin = 0; arith = 1; res = 0; break;
    case 5: x = a; y = b; cin = c->c; arith = 1; res = 0; break;
    case 6: x = a; y = ~b; cin = c->c; arith = 1; res = 0; break;
    default: x = b; y = ~a; cin = c->c; arith = 1; res = 0; break; /* 7: RSC */
    }
    if (arith) {
        uint64_t t = (uint64_t)x + y + cin;
        res = (uint32_t)t;
        if (s) {
            setnz(c, res);
            c->c = (uint32_t)(t >> 32);
            c->v = ((x ^ res) & (y ^ res)) >> 31;
        }
    } else if (s) {
        setnz(c, res);
        c->c = sc;
    }
    if (op >= 8 && op <= 11) return 0;
    if (rd == 15) {
        c->pc = res & (thumb ? ~1u : ~3u);
        return 1;
    }
    c->r[rd] = res;
    return 0;
}

static uint32_t shift_reg(int st, uint32_t v, uint32_t n, uint32_t *sc) {
    switch (st) {
    case 0: return sh_lsl(v, n, sc);
    case 1: return sh_lsr(v, n, sc);
    case 2: return sh_asr(v, n, sc);
    default: return sh_ror(v, n, sc);
    }
}
/* shift por imediato de 5 bits (0 tem significado especial) */
static uint32_t shift_imm(int st, uint32_t v, uint32_t amt, uint32_t *sc) {
    if (amt == 0) {
        if (st == 0) return v;
        if (st == 3) return sh_rrx(v, sc);
        amt = 32;
    }
    return shift_reg(st, v, amt, sc);
}

/* ---- memoria ---- */
static int ldst(Cpu *c, int load, int kind, uint32_t rd, uint32_t ea, uint32_t pcval, int thumb) {
    /* kind: 0 word, 1 byte, 2 half, 3 sbyte, 4 shalf */
    if (load) {
        uint32_t v;
        switch (kind) {
        case 0: v = rc_ldr32(c, ea); break;
        case 1: v = rc_ld8(c, ea); break;
        case 2: v = rc_ld16(c, ea); break;
        case 3: v = rc_ld8s(c, ea); break;
        default: v = rc_ld16s(c, ea); break;
        }
        if (rd == 15) {
            c->pc = v & (thumb ? ~1u : ~3u);
            return 1;
        }
        c->r[rd] = v;
    } else {
        uint32_t v = rd == 15 ? pcval : c->r[rd];
        if (kind == 0) rc_st32(c, ea, v);
        else if (kind == 1) rc_st8(c, ea, v);
        else rc_st16(c, ea, v);
    }
    return 0;
}

static int block(Cpu *c, int load, uint32_t rn, uint32_t mask, int pre, int up, int wb, uint32_t pcval, int thumb) {
    int n = __builtin_popcount(mask);
    uint32_t base = c->r[rn];
    uint32_t ad = up ? (pre ? base + 4 : base) : (pre ? base - 4u * n : base - 4u * n + 4);
    uint32_t nb = up ? base + 4u * n : base - 4u * n;
    int wrote_pc = 0;
    uint32_t vals[16];
    if (load) {
        for (int r = 0; r < 16; r++)
            if (mask >> r & 1) { vals[r] = rc_ld32(c, ad); ad += 4; }
        if (wb) c->r[rn] = nb;
        for (int r = 0; r < 15; r++)
            if (mask >> r & 1) c->r[r] = vals[r];
        if (mask >> 15 & 1) { c->pc = vals[15] & (thumb ? ~1u : ~3u); wrote_pc = 1; }
    } else {
        for (int r = 0; r < 16; r++)
            if (mask >> r & 1) { rc_st32(c, ad, r == 15 ? pcval : c->r[r]); ad += 4; }
        if (wb) c->r[rn] = nb;
    }
    return wrote_pc;
}

static void do_msr(Cpu *c, uint32_t mask, uint32_t v) { rc_msr(c, mask, v); }

/* executa uma instrucao ARM em c->pc */
static void step_arm(Cpu *c) {
    uint32_t pc = c->pc, w = gba_read32(pc), next = pc + 4;
    c->pc = next;
    uint32_t cc = w >> 28;
    if (cc == 15 || !cond_ok(c, cc)) return;
    uint32_t top = (w >> 25) & 7;
    uint32_t *r = c->r;
    #define RV(n, extra) ((n) == 15 ? pc + 8 + (extra) : r[n])
    if (top == 0 && (w & 0x0FFFFFF0) == 0x012FFF10) { /* BX */
        uint32_t t = RV(w & 15, 0);
        c->thumb = t & 1;
        c->pc = t & ((t & 1) ? ~1u : ~3u);
        return;
    }
    if (top == 0 && (w & 0x0FC000F0) == 0x00000090) { /* MUL/MLA */
        uint32_t rd = (w >> 16) & 15, rn = (w >> 12) & 15, rs = (w >> 8) & 15, rm = w & 15;
        uint32_t res = r[rm] * r[rs] + ((w >> 21 & 1) ? r[rn] : 0);
        r[rd] = res;
        if (w >> 20 & 1) setnz(c, res);
        return;
    }
    if (top == 0 && (w & 0x0F8000F0) == 0x00800090) { /* UMULL/SMULL/UMLAL/SMLAL */
        uint32_t hi = (w >> 16) & 15, lo = (w >> 12) & 15, rs = (w >> 8) & 15, rm = w & 15;
        uint64_t p = (w >> 22 & 1) ? (uint64_t)((int64_t)(int32_t)r[rm] * (int64_t)(int32_t)r[rs])
                                   : (uint64_t)r[rm] * (uint64_t)r[rs];
        if (w >> 21 & 1) p += ((uint64_t)r[hi] << 32) | r[lo];
        r[lo] = (uint32_t)p;
        r[hi] = (uint32_t)(p >> 32);
        if (w >> 20 & 1) { c->n = (uint32_t)(p >> 63); c->z = p == 0; }
        return;
    }
    if (top == 0 && (w & 0x90) == 0x90 && (w & 0x60)) { /* LDRH/STRH/LDRSB/LDRSH */
        int sh = (w >> 5) & 3, load = w >> 20 & 1, p = w >> 24 & 1, u = w >> 23 & 1, im = w >> 22 & 1;
        int wbit = w >> 21 & 1;
        uint32_t rn = (w >> 16) & 15, rd = (w >> 12) & 15;
        uint32_t off = im ? (((w >> 4) & 0xF0) | (w & 15)) : r[w & 15];
        uint32_t base = RV(rn, 0);
        uint32_t sum = u ? base + off : base - off, ea = p ? sum : base;
        int kind = sh == 1 ? 2 : sh == 2 ? 3 : 4;
        if (!load) kind = 2;
        ldst(c, load, kind, rd, ea, pc + 12, 0);
        if (((wbit && p) || !p) && rn != 15 && !(load && rd == rn)) r[rn] = sum;
        return;
    }
    if (top == 0 || top == 1) { /* data processing (e MRS/MSR) */
        uint32_t opc = (w >> 21) & 15, s = w >> 20 & 1, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
        int imm = top == 1;
        if (opc >= 8 && opc <= 11 && !s) { /* PSR */
            if (!imm && (w & 0x0FBF0FFF) == 0x010F0000) { r[rd] = rc_mrs(c); return; }
            if (!imm && (w & 0x0FB0FFF0) == 0x0120F000) { do_msr(c, (w >> 16) & 15, r[w & 15]); return; }
            if (imm && (w & 0x0FB0F000) == 0x0320F000) {
                uint32_t rot = ((w >> 8) & 15) * 2, v = w & 0xFF;
                do_msr(c, (w >> 16) & 15, rot ? ror(v, rot) : v);
                return;
            }
            return; /* nao suportado: ignora */
        }
        uint32_t sc = c->c, b, extra = 0;
        if (imm) {
            uint32_t rot = ((w >> 8) & 15) * 2, v = w & 0xFF;
            b = rot ? ror(v, rot) : v;
            if (rot) sc = b >> 31;
        } else {
            uint32_t rm = w & 15;
            int st = (w >> 5) & 3;
            if (w & 0x10) {
                extra = 4;
                uint32_t amt = RV((w >> 8) & 15, 4) & 0xFF;
                b = shift_reg(st, RV(rm, 4), amt, &sc);
            } else {
                b = shift_imm(st, RV(rm, 0), (w >> 7) & 31, &sc);
            }
        }
        if (alu(c, opc, (int)s, rd, RV(rn, extra), b, sc, 0)) { /* escreveu pc */ }
        return;
    }
    if (top == 2 || top == 3) { /* LDR/STR */
        int p = w >> 24 & 1, u = w >> 23 & 1, byte = w >> 22 & 1, wbit = w >> 21 & 1, load = w >> 20 & 1;
        uint32_t rn = (w >> 16) & 15, rd = (w >> 12) & 15, off;
        if (top == 2) off = w & 0xFFF;
        else {
            uint32_t sc = c->c;
            off = shift_imm((w >> 5) & 3, r[w & 15], (w >> 7) & 31, &sc);
        }
        uint32_t base = RV(rn, 0);
        uint32_t sum = u ? base + off : base - off, ea = p ? sum : base;
        ldst(c, load, byte ? 1 : 0, rd, ea, pc + 12, 0);
        if (((wbit && p) || !p) && rn != 15 && !(load && rd == rn)) r[rn] = sum;
        return;
    }
    if (top == 4) { /* LDM/STM */
        block(c, w >> 20 & 1, (w >> 16) & 15, w & 0xFFFF, w >> 24 & 1, w >> 23 & 1, w >> 21 & 1, pc + 12, 0);
        return;
    }
    if (top == 5) { /* B/BL */
        int32_t off = (int32_t)(w << 8) >> 6;
        uint32_t target = pc + 8 + (uint32_t)off;
        if (w >> 24 & 1) {
            r[14] = next;
            tick_call(c, target, 0);
            const RcFunc *f = rt_lookup(target, 0);
            if (f && !(target >= sk_lo && target < sk_hi)) {
                if (!call_compiled(c, target, next)) return;
                c->pc = next;
                return;
            }
        }
        c->pc = target;
        return;
    }
    if (top == 7 && (w & (1u << 24))) { /* SWI */
        if (rt_swi(c, (w >> 16) & 0xFF, next)) return;
        return;
    }
    /* coprocessador / indefinido: ignora */
}

static void step_thumb(Cpu *c) {
    uint32_t pc = c->pc, hw = gba_read16(pc), next = pc + 2;
    c->pc = next;
    uint32_t *r = c->r;
    uint32_t pcv = pc + 4;
    #define RT(n) ((n) == 15 ? pcv : r[n])
    uint32_t rd0 = hw & 7, rs3 = (hw >> 3) & 7, rd8 = (hw >> 8) & 7;
    switch (hw >> 13) {
    case 0: {
        uint32_t op = (hw >> 11) & 3;
        if (op == 3) {
            uint32_t o = (hw >> 6) & 7, b = (hw & 0x400) ? o : r[o];
            alu(c, (hw & 0x200) ? 2 : 4, 1, rd0, r[rs3], b, c->c, 1);
        } else {
            uint32_t sc = c->c;
            uint32_t v = shift_imm((int)op, r[rs3], (hw >> 6) & 31, &sc);
            alu(c, 13, 1, rd0, 0, v, sc, 1);
        }
        return;
    }
    case 1: {
        static const uint8_t opc[4] = {13, 10, 4, 2};
        alu(c, opc[(hw >> 11) & 3], 1, rd8, r[rd8], hw & 0xFF, c->c, 1);
        return;
    }
    }
    if ((hw >> 10) == 0x10) { /* ALU */
        uint32_t op = (hw >> 6) & 15, rd = rd0, rs = rs3, sc = c->c;
        if (op == 2 || op == 3 || op == 4 || op == 7) {
            static const int st[8] = {0, 0, 0, 1, 2, 0, 0, 3};
            uint32_t v = shift_reg(st[op], r[rd], r[rs] & 0xFF, &sc);
            alu(c, 13, 1, rd, 0, v, sc, 1);
        } else if (op == 9) {
            alu(c, 3, 1, rd, r[rs], 0, c->c, 1);
        } else if (op == 13) {
            uint32_t res = r[rd] * r[rs];
            r[rd] = res;
            setnz(c, res);
        } else {
            alu(c, op, 1, rd, r[rd], r[rs], c->c, 1);
        }
        return;
    }
    if ((hw >> 10) == 0x11) { /* hi regs / BX */
        uint32_t op = (hw >> 8) & 3, rs = rs3 | ((hw >> 3) & 8), rd = rd0 | ((hw >> 4) & 8);
        if (op == 3) {
            uint32_t t = RT(rs);
            c->thumb = t & 1;
            c->pc = t & ((t & 1) ? ~1u : ~3u);
            return;
        }
        static const uint8_t opc[3] = {4, 10, 13};
        alu(c, opc[op], op == 1, rd, RT(rd), RT(rs), c->c, 1);
        return;
    }
    if ((hw >> 11) == 9) { /* LDR rd,[pc,#imm] */
        ldst(c, 1, 0, rd8, ((pc + 4) & ~3u) + ((hw & 0xFF) << 2), 0, 1);
        return;
    }
    if ((hw >> 12) == 5) { /* reg offset */
        uint32_t op = (hw >> 9) & 7, ea = r[rs3] + r[(hw >> 6) & 7];
        static const uint8_t load[8] = {0, 0, 0, 1, 1, 1, 1, 1};
        static const uint8_t kind[8] = {0, 2, 1, 3, 0, 2, 1, 4};
        ldst(c, load[op], kind[op], rd0, ea, 0, 1);
        return;
    }
    if ((hw >> 13) == 3) { /* imm offset word/byte */
        int byte = hw >> 12 & 1;
        uint32_t off = ((hw >> 6) & 31) * (byte ? 1 : 4);
        ldst(c, hw >> 11 & 1, byte, rd0, r[rs3] + off, 0, 1);
        return;
    }
    if ((hw >> 12) == 8) { /* halfword imm */
        ldst(c, hw >> 11 & 1, 2, rd0, r[rs3] + ((hw >> 6) & 31) * 2, 0, 1);
        return;
    }
    if ((hw >> 12) == 9) { /* sp-relative */
        ldst(c, hw >> 11 & 1, 0, rd8, r[13] + ((hw & 0xFF) << 2), 0, 1);
        return;
    }
    if ((hw >> 12) == 10) { /* add rd, pc/sp, #imm */
        uint32_t imm = (hw & 0xFF) << 2;
        r[rd8] = (hw & 0x800) ? r[13] + imm : ((pc + 4) & ~3u) + imm;
        return;
    }
    if ((hw >> 8) == 0xB0) { /* add sp, #+/-imm7*4 */
        uint32_t imm = (hw & 0x7F) << 2;
        r[13] = (hw & 0x80) ? r[13] - imm : r[13] + imm;
        return;
    }
    if ((hw >> 12) == 0xB && ((hw >> 9) & 3) == 2) { /* PUSH/POP */
        int load = hw >> 11 & 1;
        uint32_t mask = hw & 0xFF;
        if (hw & 0x100) mask |= load ? 0x8000u : 0x4000u;
        block(c, load, 13, mask, !load, load, 1, 0, 1);
        return;
    }
    if ((hw >> 12) == 0xC) { /* LDMIA/STMIA */
        block(c, hw >> 11 & 1, rd8, hw & 0xFF, 0, 1, 1, 0, 1);
        return;
    }
    if ((hw >> 12) == 0xD) { /* B<cond> / SWI */
        uint32_t cc = (hw >> 8) & 15;
        if (cc == 15) { rt_swi(c, hw & 0xFF, next); return; }
        if (cc == 14 || !cond_ok(c, cc)) return;
        c->pc = pc + 4 + (uint32_t)((int32_t)(int8_t)(hw & 0xFF) * 2);
        return;
    }
    if ((hw >> 11) == 0x1C) { /* B */
        int32_t off = (int32_t)((hw & 0x7FF) << 21) >> 20;
        c->pc = pc + 4 + (uint32_t)off;
        return;
    }
    if ((hw >> 11) == 0x1E) { /* BL (par de halfwords) */
        uint32_t hw2 = gba_read16(pc + 2);
        int32_t hi = (int32_t)((hw & 0x7FF) << 21) >> 9;   /* sext(11) << 12 */
        uint32_t target = pc + 4 + (uint32_t)hi + ((hw2 & 0x7FF) << 1);
        uint32_t ret = pc + 4;
        r[14] = ret | 1;
        c->pc = ret;
        tick_call(c, target, 1);
        const RcFunc *f = rt_lookup(target, 1);
        if (f && !(target >= sk_lo && target < sk_hi)) {
            if (!call_compiled(c, target, ret)) return;
            c->pc = ret;
            return;
        }
        c->pc = target;
        return;
    }
    /* formato nao suportado em ARMv4T/subconjunto: ignora */
}

/* Interpreta a partir de c->pc. Retorna 0 = saiu (pc conhecido), 1 = limite de passos. */
int rt_interp_run(Cpu *c, uint32_t skip_lo, uint32_t skip_hi, uint64_t max_steps) {
    uint32_t save_lo = sk_lo, save_hi = sk_hi;
    sk_lo = skip_lo;
    sk_hi = skip_hi;
    rt_interp_entries++;
    int rc = 0;
    for (uint64_t n = 0; ; n++) {
        if (n >= max_steps) { rc = 1; break; }
        uint32_t from = c->pc;
        if (c->thumb) step_thumb(c); else step_arm(c);
        rt_interp_steps++;
        if (c->stop) break;
        uint32_t seq = c->pc;
        if (seq == from + 2 || seq == from + 4) {
            /* sequencial (ou condicao falsa): so confere se caiu em retorno esperado */
            if (seq == RT_IRQ_RET) break;
            continue;
        }
        if (!jumped(c, from)) break;
    }
    sk_lo = save_lo;
    sk_hi = save_hi;
    return rc;
}
