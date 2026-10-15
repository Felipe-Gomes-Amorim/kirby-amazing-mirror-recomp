"""Decoder ARMv4T (ARM + Thumb) para uma IR comum.

Cobre o subconjunto que o recompilador minimo sabe emitir. Tudo o que fica de
fora levanta `Unsupported` (nunca decodifica errado em silencio).

Operandos:
  reg:  int 0..15
  op2 / offset: ('imm', valor, carry|None)            carry so p/ DP imediato ARM
                ('reg', rm, shift, amt)               shift in lsl/lsr/asr/ror/rrx/None
                                                      amt: int (imediato) ou ('reg', rs)
"""
from dataclasses import dataclass, field

AL = 14
DP_NAMES = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc',
            'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn']
SHIFTS = ['lsl', 'lsr', 'asr', 'ror']


class Unsupported(Exception):
    pass


@dataclass
class Insn:
    addr: int
    size: int
    thumb: bool
    cond: int
    kind: str          # dp mul ls lsh lsm b bx
    f: dict = field(default_factory=dict)
    raw: int = 0

    @property
    def next(self):
        return self.addr + self.size


def _imm_shift(stype, amt):
    """Decodifica shift por imediato (campo de 5 bits) -> (tipo, quantidade)."""
    t = SHIFTS[stype]
    if amt == 0:
        if t == 'lsl':
            return None, 0
        if t == 'ror':
            return 'rrx', 1
        return t, 32          # lsr/asr #0 == #32
    return t, amt


def _sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def decode_arm(w, addr):
    cond = w >> 28
    if cond == 15:
        raise Unsupported('cond=NV')
    ins = lambda kind, **f: Insn(addr, 4, False, cond, kind, f, w)
    top = (w >> 25) & 7
    if top == 0:
        if (w & 0x0FFFFFF0) == 0x012FFF10:
            return ins('bx', rm=w & 15)
        if (w & 0x0FC000F0) == 0x00000090:
            return ins('mul', rd=(w >> 16) & 15, rm=w & 15, rs=(w >> 8) & 15,
                       ra=(w >> 12) & 15, acc=bool(w & (1 << 21)), s=bool(w & (1 << 20)),
                       odd=not (w & (1 << 21)) and ((w >> 12) & 15) != 0)
        if (w & 0x90) == 0x90 and (w & 0x60) != 0:
            return _arm_lsh(w, ins, addr)
        if (w & 0x0F8000F0) == 0x00800090:
            hi, lo = (w >> 16) & 15, (w >> 12) & 15
            rm, rs = w & 15, (w >> 8) & 15
            return ins('mull', rdhi=hi, rdlo=lo, rm=rm, rs=rs, signed=bool(w & (1 << 22)),
                       acc=bool(w & (1 << 21)), s=bool(w & (1 << 20)),
                       odd=15 in (hi, lo, rm, rs) or hi == lo or hi == rm or lo == rm)
        if (w & 0x90) == 0x90:
            raise Unsupported('swp')
        return _arm_dp(w, ins, imm=False)
    if top == 1:
        return _arm_dp(w, ins, imm=True)
    if top in (2, 3):
        if top == 3 and (w & 0x10):
            raise Unsupported('undefined')
        p, u, b, wb, l = (bool(w & (1 << n)) for n in (24, 23, 22, 21, 20))
        if top == 2:
            off = ('imm', w & 0xFFF, None)
        else:
            st, amt = _imm_shift((w >> 5) & 3, (w >> 7) & 31)
            off = ('reg', w & 15, st, amt)
        rn = (w >> 16) & 15
        base = ('const', addr + 8) if rn == 15 else ('reg', rn)
        if rn == 15 and (wb or not p):
            raise Unsupported('pc base writeback')
        return ins('ls', load=l, byte=b, rd=(w >> 12) & 15, base=base, rn=rn,
                   off=off, pre=p, up=u, wb=(wb and p) or not p)
    if top == 4:
        if w & (1 << 22):
            raise Unsupported('ldm/stm user-bank/spsr')
        regs = [i for i in range(16) if w & (1 << i)]
        if not regs:
            raise Unsupported('empty rlist')
        if (w >> 16) & 15 == 15:
            raise Unsupported('ldm pc base')
        rn = (w >> 16) & 15
        return ins('lsm', load=bool(w & (1 << 20)), rn=rn, regs=regs,
                   pre=bool(w & (1 << 24)), up=bool(w & (1 << 23)), wb=bool(w & (1 << 21)),
                   odd=bool(w & (1 << 21)) and rn in regs)  # QEMU: UNDEF
    if top == 5:
        off = _sext(w & 0xFFFFFF, 24) << 2
        return ins('b', link=bool(w & (1 << 24)), target=(addr + 8 + off) & 0xFFFFFFFF)
    if top == 7 and (w & (1 << 24)):
        return ins('swi', imm=w & 0xFFFFFF)
    raise Unsupported('coproc')


def _arm_dp(w, ins, imm):
    opc = (w >> 21) & 15
    s = bool(w & (1 << 20))
    if 8 <= opc <= 11 and not s:
        return _arm_psr(w, ins, imm)
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    if rd == 15 and s and not (8 <= opc <= 11):
        raise Unsupported('s-form pc dest (exception return)')
    regshift = False
    if imm:
        rot = ((w >> 8) & 15) * 2
        v = w & 0xFF
        v = ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v
        op2 = ('imm', v, (v >> 31) if rot else None)
    else:
        rm = w & 15
        if w & 0x10:
            if w & 0x80:
                raise Unsupported('undefined')
            op2 = ('reg', rm, SHIFTS[(w >> 5) & 3], ('reg', (w >> 8) & 15))
            regshift = True
        else:
            st, amt = _imm_shift((w >> 5) & 3, (w >> 7) & 31)
            op2 = ('reg', rm, st, amt)
    # campos SBZ != 0: o ARM7 ignora, mas o oraculo (QEMU) trata como indefinido
    odd = (opc in (13, 15) and rn != 0) or (8 <= opc <= 11 and rd != 0)
    odd = odd or (regshift and op2[3][1] == 15)   # Rs = pc: imprevisivel
    return ins('dp', op=opc, s=s, rd=rd, rn=rn, op2=op2, regshift=regshift, odd=odd)


def _arm_psr(w, ins, imm):
    if w & (1 << 22):
        raise Unsupported('spsr')
    if not imm and (w & 0x0FBF0FFF) == 0x010F0000:
        return ins('mrs', rd=(w >> 12) & 15)
    if not imm and (w & 0x0FB0FFF0) == 0x0120F000:
        return ins('msr', mask=(w >> 16) & 15, op=('reg', w & 15))
    if imm and (w & 0x0FB0F000) == 0x0320F000:
        rot = ((w >> 8) & 15) * 2
        v = w & 0xFF
        v = ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v
        return ins('msr', mask=(w >> 16) & 15, op=('imm', v))
    raise Unsupported('psr transfer')


def _arm_lsh(w, ins, addr):
    sh = (w >> 5) & 3
    load = bool(w & (1 << 20))
    p, u, imm, wb = (bool(w & (1 << n)) for n in (24, 23, 22, 21))
    if not load and sh != 1:
        raise Unsupported('ldrd/strd')
    if not p and wb:
        raise Unsupported('undefined')
    rn = (w >> 16) & 15
    if rn == 15 and (wb or not p):
        raise Unsupported('pc base writeback')
    if imm:
        off = ('imm', ((w >> 4) & 0xF0) | (w & 0xF), None)
    else:
        if (w >> 8) & 15:
            raise Unsupported('undefined')
        off = ('reg', w & 15, None, 0)
    base = ('const', addr + 8) if rn == 15 else ('reg', rn)
    return ins('lsh', load=load, kind_h={1: 'h', 2: 'sb', 3: 'sh'}[sh], rd=(w >> 12) & 15,
               base=base, rn=rn, off=off, pre=p, up=u, wb=(wb and p) or not p)


# ---------------------------------------------------------------- Thumb
def decode_thumb(hw, addr, hw2=None):
    """hw2: halfword seguinte (so usado pelo par BL)."""
    ins = lambda kind, **f: Insn(addr, 2, True, AL, kind, f, hw)
    r = lambda lo: (hw >> lo) & 7
    top = hw >> 13
    if top == 0:
        op = (hw >> 11) & 3
        if op == 3:
            imm = bool(hw & 0x400)
            sub = bool(hw & 0x200)
            o = (hw >> 6) & 7
            op2 = ('imm', o, None) if imm else ('reg', o, None, 0)
            return ins('dp', op=2 if sub else 4, s=True, rd=r(0), rn=r(3), op2=op2, regshift=False)
        st, amt = _imm_shift(op, (hw >> 6) & 31)
        return ins('dp', op=13, s=True, rd=r(0), rn=0, op2=('reg', r(3), st, amt), regshift=False)
    if top == 1:
        op = (hw >> 11) & 3
        rd = r(8)
        opc = [13, 10, 4, 2][op]      # mov cmp add sub
        return ins('dp', op=opc, s=True, rd=rd, rn=rd, op2=('imm', hw & 0xFF, None), regshift=False)
    if hw >> 10 == 0b010000:
        op = (hw >> 6) & 15
        rd, rs = r(0), r(3)
        if op in (2, 3, 4, 7):
            st = {2: 'lsl', 3: 'lsr', 4: 'asr', 7: 'ror'}[op]
            return ins('dp', op=13, s=True, rd=rd, rn=0, op2=('reg', rd, st, ('reg', rs)), regshift=True)
        if op == 9:
            return ins('dp', op=3, s=True, rd=rd, rn=rs, op2=('imm', 0, None), regshift=False)
        if op == 13:
            return ins('mul', rd=rd, rm=rd, rs=rs, ra=0, acc=False, s=True)
        opc = {0: 0, 1: 1, 5: 5, 6: 6, 8: 8, 10: 10, 11: 11, 12: 12, 14: 14, 15: 15}[op]
        return ins('dp', op=opc, s=True, rd=rd, rn=rd, op2=('reg', rs, None, 0), regshift=False)
    if hw >> 10 == 0b010001:
        op = (hw >> 8) & 3
        rs = r(3) | ((hw >> 3) & 8)
        rd = r(0) | ((hw >> 4) & 8)
        if op == 3:
            if hw & 0x80:
                raise Unsupported('blx')
            return ins('bx', rm=rs, odd=bool(hw & 7))
        opc = [4, 10, 13][op]
        return ins('dp', op=opc, s=(op == 1), rd=rd, rn=rd, op2=('reg', rs, None, 0),
                   regshift=False, hi=True)
    if hw >> 11 == 0b01001:
        return ins('ls', load=True, byte=False, rd=r(8), rn=15,
                   base=('const', ((addr + 4) & ~3) + ((hw & 0xFF) << 2)),
                   off=('imm', 0, None), pre=True, up=True, wb=False)
    if hw >> 12 == 0b0101:
        op = (hw >> 9) & 7
        rd, rb, ro = r(0), r(3), r(6)
        off = ('reg', ro, None, 0)
        common = dict(rd=rd, rn=rb, base=('reg', rb), off=off, pre=True, up=True, wb=False)
        if op in (0, 2, 4, 6):
            return ins('ls', load=op >= 4, byte=op in (2, 6), **common)
        k = {1: 'h', 3: 'sb', 5: 'h', 7: 'sh'}[op]
        return ins('lsh', load=op >= 3, kind_h=k, **common)
    if top == 3:
        byte, load = bool(hw & 0x1000), bool(hw & 0x800)
        imm = ((hw >> 6) & 31) * (1 if byte else 4)
        return ins('ls', load=load, byte=byte, rd=r(0), rn=r(3), base=('reg', r(3)),
                   off=('imm', imm, None), pre=True, up=True, wb=False)
    if hw >> 12 == 0b1000:
        return ins('lsh', load=bool(hw & 0x800), kind_h='h', rd=r(0), rn=r(3), base=('reg', r(3)),
                   off=('imm', ((hw >> 6) & 31) * 2, None), pre=True, up=True, wb=False)
    if hw >> 12 == 0b1001:
        return ins('ls', load=bool(hw & 0x800), byte=False, rd=r(8), rn=13, base=('reg', 13),
                   off=('imm', (hw & 0xFF) << 2, None), pre=True, up=True, wb=False)
    if hw >> 12 == 0b1010:
        imm = (hw & 0xFF) << 2
        if hw & 0x800:
            return ins('dp', op=4, s=False, rd=r(8), rn=13, op2=('imm', imm, None), regshift=False)
        return ins('dp', op=13, s=False, rd=r(8), rn=0,
                   op2=('imm', ((addr + 4) & ~3) + imm, None), regshift=False)
    if hw >> 8 == 0b10110000:
        return ins('dp', op=2 if hw & 0x80 else 4, s=False, rd=13, rn=13,
                   op2=('imm', (hw & 0x7F) << 2, None), regshift=False)
    if hw >> 12 == 0b1011 and (hw >> 9) & 3 == 0b10:
        load = bool(hw & 0x800)
        regs = [i for i in range(8) if hw & (1 << i)]
        if hw & 0x100:
            regs.append(15 if load else 14)
        if not regs:
            raise Unsupported('empty rlist')
        return ins('lsm', load=load, rn=13, regs=regs, pre=not load, up=load, wb=True)
    if hw >> 12 == 0b1100:
        regs = [i for i in range(8) if hw & (1 << i)]
        if not regs:
            raise Unsupported('empty rlist')
        return ins('lsm', load=bool(hw & 0x800), rn=r(8), regs=regs, pre=False, up=True, wb=True)
    if hw >> 12 == 0b1101:
        c = (hw >> 8) & 15
        if c == 15:
            return ins('swi', imm=hw & 0xFF)
        if c == 14:
            raise Unsupported('undefined')
        i = ins('b', link=False, target=(addr + 4 + (_sext(hw & 0xFF, 8) << 1)) & 0xFFFFFFFF)
        i.cond = c
        return i
    if hw >> 11 == 0b11100:
        return ins('b', link=False, target=(addr + 4 + (_sext(hw & 0x7FF, 11) << 1)) & 0xFFFFFFFF)
    if hw >> 11 == 0b11110:
        if hw2 is None or hw2 >> 11 != 0b11111:
            raise Unsupported('bl prefix sem sufixo')
        off = (_sext(hw & 0x7FF, 11) << 12) + ((hw2 & 0x7FF) << 1)
        i = Insn(addr, 4, True, AL, 'b', dict(link=True, target=(addr + 4 + off) & 0xFFFFFFFF),
                 (hw << 16) | hw2)
        return i
    raise Unsupported('thumb 0x%04x' % hw)


def decode(rom_read16, addr, thumb):
    """rom_read16(addr) -> int|None. Decodifica uma instrucao em addr."""
    if thumb:
        hw = rom_read16(addr)
        if hw is None:
            raise Unsupported('fora da ROM')
        return decode_thumb(hw, addr, rom_read16(addr + 2))
    lo, hi = rom_read16(addr), rom_read16(addr + 2)
    if lo is None or hi is None:
        raise Unsupported('fora da ROM')
    return decode_arm(lo | (hi << 16), addr)
