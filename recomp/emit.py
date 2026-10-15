"""Emissor de C: funcoes ARM/Thumb -> C sobre `Cpu` (runtime/gba_cpu.h).

Uma funcao guest = uma funcao C `fn_XXXXXXXX(Cpu*)`. Cada instrucao vira um
label + um hook de trace + codigo inline; desvios dentro da funcao viram `goto`.
Tudo que sai do conjunto (chamadas `bl`, retornos, saltos para fora) grava
`cpu->pc` e retorna ao chamador: o despachante (harness hoje, runtime depois)
decide o que fazer.
"""
from .decode import AL, Insn, Unsupported, decode

COND = {
    0: 'cpu->z', 1: '!cpu->z', 2: 'cpu->c', 3: '!cpu->c', 4: 'cpu->n', 5: '!cpu->n',
    6: 'cpu->v', 7: '!cpu->v', 8: '(cpu->c && !cpu->z)', 9: '(!cpu->c || cpu->z)',
    10: '(cpu->n == cpu->v)', 11: '(cpu->n != cpu->v)',
    12: '(!cpu->z && cpu->n == cpu->v)', 13: '(cpu->z || cpu->n != cpu->v)',
}
SHFN = {'lsl': 'sh_lsl', 'lsr': 'sh_lsr', 'asr': 'sh_asr', 'ror': 'sh_ror'}


def hx(v):
    return '0x%08Xu' % (v & 0xFFFFFFFF)


class Function:
    def __init__(self, entry, thumb, insns):
        self.entry, self.thumb, self.insns = entry, thumb, insns  # insns: {addr: Insn}

    @property
    def name(self):
        return 'fn_%08X' % self.entry


# ------------------------------------------------------------ discovery
def discover(read16, entry, thumb, max_insns=300, max_span=0x400):
    """Segue o fluxo a partir de `entry`. Levanta Unsupported se algo no caminho
    nao for suportado. Desvios incondicionais para longe (> max_span) viram saida
    (tail call); `bl` e tratado como chamada que retorna."""
    insns, todo = {}, [entry]
    while todo:
        a = todo.pop()
        if a in insns:
            continue
        i = decode(read16, a, thumb)
        insns[a] = i
        if len(insns) > max_insns:
            raise Unsupported('funcao grande demais')
        succ = []
        f = i.f
        if i.kind == 'b' and not f['link']:
            if entry <= f['target'] < entry + max_span:
                succ.append(f['target'])
            if i.cond != AL:
                succ.append(i.next)
        elif i.kind == 'bx' or _writes_pc(i):
            if i.cond != AL:
                succ.append(i.next)
        else:
            succ.append(i.next)
        todo.extend(succ)
    return Function(entry, thumb, insns)


def _writes_pc(i):
    f = i.f
    if i.kind == 'dp':
        return f['rd'] == 15 and not 8 <= f['op'] <= 11
    if i.kind in ('ls', 'lsh'):
        return f['load'] and f['rd'] == 15
    if i.kind == 'lsm':
        return f['load'] and 15 in f['regs']
    return False


# ------------------------------------------------------------ emissao
class Emitter:
    def __init__(self, fn, rt=False, known=None, read16=None):
        """rt=True: codigo para o runtime (sem trace; `bl` vira chamada real).
        known: enderecos de entrada das funcoes do programa (chamada direta).
        read16: leitor da ROM, p/ reconhecer stubs `bx rN` (_call_via_rN)."""
        self.fn = fn
        self.thumb = fn.thumb
        self.rt, self.known, self.read16 = rt, known or {}, read16

    def reg(self, n, i, extra=0):
        if n == 15:
            return hx(i.addr + (4 if i.thumb else 8) + extra)
        return 'cpu->r[%d]' % n

    # destino de salto dinamico (valor ja em `t`): fica dentro da funcao se casar
    def jump_dyn(self, expr, mask):
        if mask == '~3u':   # ARM: bits baixos ignorados no ARM7; o oraculo nao alinha
            return ['if (((%s) & 3u) || (%s) >= 0xFFFFF000u) cpu->skip = 1;' % (expr, expr),
                    'cpu->pc = (%s) & %s;' % (expr, mask), 'goto dispatch;']
        return ['if ((%s) >= 0xFFFFF000u) cpu->skip = 1;' % expr,
                'cpu->pc = (%s) & %s;' % (expr, mask), 'goto dispatch;']

    def branch(self, target):
        if target in self.fn.insns:
            return ['goto L_%08X;' % target]
        return ['cpu->pc = %s;' % hx(target), 'return;']

    def op2(self, i, op2, regshift):
        """Linhas que definem `b` (valor do operando 2) e `sc` (carry do shifter)."""
        kind = op2[0]
        if kind == 'imm':
            out = ['uint32_t sc = cpu->c;']
            if op2[2] is not None:
                out.append('sc = %d;' % op2[2])
            out.append('uint32_t b = %s;' % hx(op2[1]))
            return out
        _, rm, st, amt = op2
        src = self.reg(rm, i, 4 if regshift else 0)
        out = ['uint32_t sc = cpu->c;']
        if st is None:
            out.append('uint32_t b = %s;' % src)
        elif st == 'rrx':
            out.append('uint32_t b = sh_rrx(%s, &sc);' % src)
        elif isinstance(amt, tuple):
            out.append('uint32_t b = %s(%s, %s & 0xFF, &sc);' % (SHFN[st], src, self.reg(amt[1], i, 4)))
        else:
            out.append('uint32_t b = %s(%s, %du, &sc);' % (SHFN[st], src, amt))
        return out

    # ---- tipos
    def dp(self, i):
        f = i.f
        op, s, rd, rn = f['op'], f['s'], f['rd'], f['rn']
        out = ['{'] + self.op2(i, f['op2'], f['regshift'])
        if f['regshift'] and (f['rn'] == 15 or f['op2'][1] == 15):
            out.append('cpu->skip = 1; /* ARM7 le pc+12; o oraculo le pc+8 */')
        out.append('uint32_t a = %s;' % self.reg(rn, i, 4 if f['regshift'] else 0))
        out.append('uint32_t res;')
        logical = {0: 'a & b', 1: 'a ^ b', 8: 'a & b', 9: 'a ^ b', 12: 'a | b',
                   13: 'b', 14: 'a & ~b', 15: '~b'}
        arith = {2: ('a', '~b', '1u'), 3: ('b', '~a', '1u'), 4: ('a', 'b', '0u'),
                 5: ('a', 'b', 'cpu->c'), 6: ('a', '~b', 'cpu->c'), 7: ('b', '~a', 'cpu->c'),
                 10: ('a', '~b', '1u'), 11: ('a', 'b', '0u')}
        if op in logical:
            out.append('res = %s;' % logical[op])
            if s:
                out += ['SETNZ(res);', 'cpu->c = sc;']
        else:
            x, y, cin = arith[op]
            out += ['uint32_t x = %s, y = %s;' % (x, y),
                    'uint64_t t = (uint64_t)x + y + %s;' % cin,
                    'res = (uint32_t)t;']
            if s:
                out += ['SETNZ(res);', 'cpu->c = (uint32_t)(t >> 32);',
                        'cpu->v = ((x ^ res) & (y ^ res)) >> 31;']
        if not 8 <= op <= 11:
            if rd == 15:
                out += self.jump_dyn('res', '~1u' if i.thumb else '~3u')
            else:
                out.append('cpu->r[%d] = res;' % rd)
        out.append('}')
        return out

    def mul(self, i):
        f = i.f
        for n in (f['rd'], f['rm'], f['rs']):
            if n == 15:
                raise Unsupported('mul com pc')
        e = '%s * %s' % (self.reg(f['rm'], i), self.reg(f['rs'], i))
        if f['acc']:
            e += ' + %s' % self.reg(f['ra'], i)
        out = ['{', 'uint32_t res = %s;' % e, 'cpu->r[%d] = res;' % f['rd']]
        if f['s']:
            out.append('SETNZ(res);')   # C: indefinido em ARMv4; mantido
        out.append('}')
        return out

    def address(self, i):
        f = i.f
        out = ['uint32_t base = %s;' % (hx(f['base'][1]) if f['base'][0] == 'const'
                                          else self.reg(f['base'][1], i))]
        off = f['off']
        if off[0] == 'imm':
            out.append('uint32_t off = %s;' % hx(off[1]))
        else:
            _, rm, st, amt = off
            src = self.reg(rm, i)
            out.append('uint32_t sc = cpu->c;')
            if st is None:
                out.append('uint32_t off = %s;' % src)
            elif st == 'rrx':
                out.append('uint32_t off = sh_rrx(%s, &sc);' % src)
            else:
                out.append('uint32_t off = %s(%s, %du, &sc);' % (SHFN[st], src, amt))
        out.append('uint32_t sum = %s;' % ('base + off' if f['up'] else 'base - off'))
        out.append('uint32_t ea = %s;' % ('sum' if f['pre'] else 'base'))
        return out

    def ls(self, i):
        f = i.f
        out = ['{'] + self.address(i)
        wb = f['wb'] and f['rn'] != 15
        wbl = ['cpu->r[%d] = sum;' % f['rn']] if wb else []
        rd = f['rd']
        if f['load']:
            fn = 'rc_ld8' if f['byte'] else 'rc_ldr32'
            out += ['uint32_t val = %s(cpu, ea);' % fn, 'if (cpu->status) return;'] + wbl
            if rd == 15:
                out += self.jump_dyn('val', '~1u' if i.thumb else '~3u')
            else:
                out.append('cpu->r[%d] = val;' % rd)
        else:
            fn = 'rc_st8' if f['byte'] else 'rc_st32'
            if rd == 15:
                out.append('cpu->skip = 1;')  # ARM7 grava pc+12, oraculo grava pc+8
            out += ['%s(cpu, ea, %s);' % (fn, self.reg(rd, i, 4)), 'if (cpu->status) return;'] + wbl
        out.append('}')
        return out

    def lsh(self, i):
        f = i.f
        out = ['{'] + self.address(i)
        wb = f['wb'] and f['rn'] != 15
        wbl = ['cpu->r[%d] = sum;' % f['rn']] if wb else []
        rd = f['rd']
        if f['load']:
            fn = {'h': 'rc_ld16', 'sh': 'rc_ld16s', 'sb': 'rc_ld8s'}[f['kind_h']]
            out += ['uint32_t val = %s(cpu, ea);' % fn, 'if (cpu->status) return;'] + wbl
            if rd == 15:
                out += self.jump_dyn('val', '~1u' if i.thumb else '~3u')
            else:
                out.append('cpu->r[%d] = val;' % rd)
        else:
            if rd == 15:
                out.append('cpu->skip = 1;')
            out += ['rc_st16(cpu, ea, %s);' % self.reg(rd, i, 4), 'if (cpu->status) return;'] + wbl
        out.append('}')
        return out

    def lsm(self, i):
        f = i.f
        regs, rn = f['regs'], f['rn']
        n = len(regs)
        out = ['{', 'uint32_t base = %s;' % self.reg(rn, i)]
        if f['up']:
            start = 'base + 4u' if f['pre'] else 'base'
            newb = 'base + %du' % (4 * n)
        else:
            start = 'base - %du' % (4 * n) if f['pre'] else 'base - %du' % (4 * n - 4)
            newb = 'base - %du' % (4 * n)
        out.append('uint32_t ad = %s;' % start)
        wbl = ['cpu->r[%d] = %s;' % (rn, newb)] if f['wb'] else []
        if f['load']:
            for r in regs:     # le tudo antes de escrever: falha nao deixa estado parcial
                out.append('uint32_t v%d = rc_ld32(cpu, ad); ad += 4u;' % r)
            out.append('if (cpu->status) return;')
            out += wbl         # o valor carregado vence se rn estiver na lista
            for r in regs:
                if r != 15:
                    out.append('cpu->r[%d] = v%d;' % (r, r))
            if 15 in regs:
                if i.thumb:   # ARMv5 (oraculo) troca para ARM se bit0=0; ARM7 nao
                    out.append('if (!(v15 & 1u)) cpu->skip = 1;')
                out += self.jump_dyn('v15', '~1u' if i.thumb else '~3u')
        else:
            if 15 in regs:
                out.append('cpu->skip = 1;')
            for r in regs:
                out.append('rc_st32(cpu, ad, %s); ad += 4u;' % self.reg(r, i, 4))
            out.append('if (cpu->status) return;')
            out += wbl
        out.append('}')
        return out

    def bx(self, i):
        rm = self.reg(i.f['rm'], i)
        return ['{', 'uint32_t t = %s;' % rm,
                'cpu->thumb = t & 1u;',
                'if ((t & 3u) == 2u || t >= 0xFFFFF000u) cpu->skip = 1;',
                'cpu->pc = t & ((t & 1u) ? ~1u : ~3u);',
                'if (cpu->thumb == %du) goto dispatch;' % int(self.thumb),
                'return;', '}']

    def stub_reg(self, target):
        """Se `target` e um veneer Thumb `bx rN` (ex.: _call_via_rN), devolve N."""
        if self.read16 is None:
            return None
        try:
            v = decode(self.read16, target, True)
        except Unsupported:
            return None
        return v.f['rm'] if v.kind == 'bx' and v.cond == AL else None

    def call(self, i):
        t = i.f['target']
        ret = i.next | (1 if i.thumb else 0)
        out = ['cpu->r[14] = %s;' % hx(ret)]
        n = self.stub_reg(t) if i.thumb else None
        if n is not None and n != 15:
            out += ['{', 'uint32_t t = cpu->r[%d];' % n, 'cpu->thumb = t & 1u;',
                    'cpu->pc = t & ((t & 1u) ? ~1u : ~3u);', '}']
        else:
            tgt = t & ~1
            callee_thumb = 1 if i.thumb else 0   # `bl` Thumb->Thumb; ARM `bl` -> ARM
            out += ['cpu->pc = %s;' % hx(tgt), 'cpu->thumb = %du;' % callee_thumb]
            if tgt in self.known:
                out.append('fn_%08X(cpu);' % tgt)
            else:
                out.append('rt_call(cpu);')
        out += ['if (cpu->pc != %s) rt_run_until(cpu, %s);' % (hx(i.next), hx(i.next)),
                'if (cpu->stop) return;']
        return out

    def swi(self, i):
        return ['if (rt_swi(cpu, %du, %s)) return;' % (i.f['imm'] & 0xFF if i.thumb else
                                                         (i.f['imm'] >> 16) & 0xFF, hx(i.next))]

    def mrs(self, i):
        return ['cpu->r[%d] = rc_mrs(cpu);' % i.f['rd']]

    def msr(self, i):
        op = i.f['op']
        v = hx(op[1]) if op[0] == 'imm' else self.reg(op[1], i)
        return ['rc_msr(cpu, %du, %s);' % (i.f['mask'], v)]

    def b(self, i):
        f = i.f
        if self.rt and f['link']:
            return self.call(i)
        out = []
        if f['link']:
            out.append('cpu->r[14] = %s;' % hx(i.next | (1 if i.thumb else 0)))
        return out + self.branch(f['target'])

    def insn(self, i):
        body = getattr(self, {'lsh': 'lsh'}.get(i.kind, i.kind))(i)
        lines = ['L_%08X:' % i.addr]
        if not self.rt:
            lines.append('    if (rc_trace(cpu, %s)) return;' % hx(i.addr))
        if i.cond != AL:
            lines.append('    if (!(%s)) goto N_%08X;' % (COND[i.cond], i.addr))
        lines += ['    ' + l for l in body]
        if i.cond != AL:
            lines.append('N_%08X:;' % i.addr)
        # cai para a proxima instrucao; se ela nao pertence a funcao, sai
        terminal = i.cond == AL and (i.kind == 'bx' or (i.kind == 'b' and not i.f['link'])
                                     or _writes_pc(i))
        if not terminal and i.next not in self.fn.insns:
            lines.append('    cpu->pc = %s; return;' % hx(i.next))
        return lines

    def emit(self):
        fn = self.fn
        out = ['RC_API void %s(Cpu *cpu) {' % fn.name, '    goto dispatch;']
        for a in sorted(fn.insns):
            out += self.insn(fn.insns[a])
        out.append('dispatch:')
        out.append('    switch (cpu->pc) {')
        for a in sorted(fn.insns):
            out.append('    case %s: goto L_%08X;' % (hx(a), a))
        out.append('    default: return;')
        out.append('    }')
        out.append('}')
        return '\n'.join(out)


def emit_function(fn, **kw):
    return Emitter(fn, **kw).emit()


def emit_unit(funcs):
    head = '#include "gba_cpu.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n\n'
    return head + '\n\n'.join(emit_function(f) for f in funcs) + '\n'


def emit_program(funcs, read16, nshards=8):
    """Programa completo p/ o runtime: (header de prototipos, [shards .c], tabela .c)."""
    funcs = sorted(funcs, key=lambda f: f.entry)
    known = {f.entry: f for f in funcs}
    protos = ['#include "gba_cpu.h"'] + ['RC_API void %s(Cpu *cpu);' % f.name for f in funcs]
    head = '#include "rc_protos.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n\n'
    # shard estavel por endereco (adicionar overlays nao muda os shards da ROM);
    # o ultimo shard guarda so os overlays (codigo em RAM)
    shards = [[] for _ in range(nshards + 1)]
    for f in funcs:
        k = nshards if f.entry < 0x08000000 else (f.entry >> 2) % nshards
        shards[k].append(emit_function(f, rt=True, known=known, read16=read16))
    table = ['#include "rc_protos.h"', 'const RcFunc rc_table[] = {']
    for f in funcs:
        hi = max(i.next for i in f.insns.values())
        sig = 0
        if f.entry < 0x08000000:   # overlay em RAM: assinatura dos 4 primeiros bytes
            sig = (read16(f.entry) or 0) | ((read16(f.entry + 2) or 0) << 16)
        table.append('    {%s, %s, %d, %s, %s},' % (hx(f.entry), hx(hi), int(f.thumb), f.name, hx(sig)))
    table += ['};', 'const unsigned rc_table_n = %d;' % len(funcs)]
    return ('\n'.join(protos) + '\n', [head + '\n\n'.join(s) + '\n' for s in shards],
            '\n'.join(table) + '\n')
