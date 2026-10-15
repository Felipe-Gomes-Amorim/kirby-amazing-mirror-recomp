#!/usr/bin/env python3
"""Analise estatica de uma ROM de GBA.

Etapas:
  1. le o header (titulo, codigo do jogo, checksum, entry point)
  2. descida recursiva a partir do entry point, seguindo B/BL/BX e rastreando
     o modo (ARM x Thumb); resolve `ldr rX,=addr ; bx rX` e tabelas de switch
  3. varre a ROM atras de ponteiros para prologos de funcao (heuristica)
  4. classifica cada byte: ARM / Thumb / literal pool / tabela de salto / dados
  5. agrupa instrucoes em funcoes e grava relatorios em build/analysis/

Uso: python tools/analyze_rom.py [rom.gba] [-o build/analysis]
"""
import argparse
import csv
import json
import re
import struct
import sys
from array import array
from collections import Counter, defaultdict, deque
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs

ROM_BASE = 0x08000000
CC = ("eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al")
REG_ALIAS = {"ip": "r12", "fp": "r11", "sb": "r9", "sl": "r10", "r13": "sp", "r14": "lr", "r15": "pc"}
# instrucoes cujo primeiro operando e o registrador de destino
WRITES = {
    "mov", "movs", "mvn", "mvns", "add", "adds", "sub", "subs", "rsb", "rsbs", "adc", "adcs", "sbc", "sbcs",
    "and", "ands", "orr", "orrs", "eor", "eors", "bic", "bics", "lsl", "lsls", "lsr", "lsrs", "asr", "asrs",
    "ror", "rors", "neg", "negs", "mul", "muls", "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "mla", "mlas",
}
LDM_PLAIN = {"ldm", "ldmia", "ldmib", "ldmda", "ldmdb", "ldmfd", "ldmfa", "ldmed", "ldmea", "pop"}
RETURN_REGS = {"lr"}


def reg(name):
    name = name.strip().lower()
    return REG_ALIAS.get(name, name)


def split_ops(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse_imm(s):
    s = s.strip().lstrip("#")
    try:
        return int(s, 0)
    except ValueError:
        return None


def split_branch(m):
    """'bne' -> ('b', True); 'bl' -> ('bl', False); outro -> (None, False)."""
    if m in ("b", "bl", "bx", "blx"):
        return m, False
    for base in ("blx", "bl", "bx", "b"):
        if m.startswith(base) and m[len(base):] in CC:
            return base, True
    return None, False


class Insn:
    __slots__ = ("addr", "size", "thumb", "mnem", "ops", "kind", "target", "succ", "calls", "cond")

    def __init__(self, addr, size, thumb, mnem, ops):
        self.addr, self.size, self.thumb, self.mnem, self.ops = addr, size, thumb, mnem, ops
        self.kind = "seq"   # seq | jump | call | ret | ind
        self.target = None  # alvo direto de B/BL
        self.succ = []      # sucessores dentro da mesma funcao
        self.calls = []     # alvos de chamada
        self.cond = False


class Analyzer:
    def __init__(self, rom):
        self.rom = rom
        self.n = len(rom)
        self.cs = {False: Cs(CS_ARCH_ARM, CS_MODE_ARM), True: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
        self.insns = {}            # addr -> Insn
        self.funcs = {}            # addr -> (thumb, source)
        self.pool = set()          # enderecos de words lidos por ldr pc-relativo
        self.tables = []           # dicts: site, base, targets, thumb
        self.unresolved = []       # (addr, mnemonic) saltos/chamadas indiretos sem alvo
        self.ram_targets = Counter()
        self.adr_seeds = []        # alvos de `add rX,pc,#imm` (ARM)
        self.bad = []              # enderecos que nao decodificaram
        self.mode_conflicts = 0

    # ---- acesso a ROM ----
    def off(self, addr):
        o = addr - ROM_BASE
        return o if 0 <= o < self.n else None

    def word(self, addr):
        o = self.off(addr)
        if o is None or o + 4 > self.n:
            return None
        return struct.unpack_from("<I", self.rom, o)[0]

    def decode(self, addr, thumb):
        o = self.off(addr)
        if o is None:
            return None
        for i in self.cs[thumb].disasm(bytes(self.rom[o:o + 4]), addr, 1):
            return i
        return None

    def add_func(self, addr, thumb, src):
        if self.off(addr) is None:
            self.ram_targets[addr] += 1
            return False
        if addr not in self.funcs:
            self.funcs[addr] = (thumb, src)
        return True

    # ---- descida recursiva ----
    def run(self, seeds):
        work = deque(seeds)
        while work:
            addr, thumb, src = work.popleft()
            if self.off(addr) is None:
                continue
            self.walk(addr, thumb, work)
            while self.adr_seeds:  # `add rX,pc,#imm` em ARM: ponteiro de codigo (ex.: handler de IRQ)
                t = self.adr_seeds.pop()
                if self.add_func(t, False, "adr"):
                    work.append((t, False, "adr"))

    def walk(self, addr, thumb, work):
        regs = {}
        last_cmp = None
        mov_lr_pc = False
        step = 2 if thumb else 4
        while True:
            if addr in self.insns:
                if self.insns[addr].thumb != thumb:
                    self.mode_conflicts += 1
                return
            if self.off(addr) is None or addr % step:
                return
            d = self.decode(addr, thumb)
            if d is None:
                self.bad.append(addr)
                return
            ins = Insn(addr, d.size, thumb, d.mnemonic, split_ops(d.op_str))
            self.insns[addr] = ins
            nxt = addr + d.size
            stop = self.classify(ins, regs, last_cmp, mov_lr_pc, work)
            m, ops = ins.mnem, ins.ops
            if m == "cmp" and len(ops) == 2:
                imm = parse_imm(ops[1])
                last_cmp = imm if imm is not None else last_cmp
            mov_lr_pc = (m == "mov" and ops == ["lr", "pc"])
            if not stop:
                ins.succ.append(nxt)
            if ins.kind == "jump" and ins.target is not None:
                ins.succ.append(ins.target)
            if stop:
                return
            addr = nxt

    def classify(self, ins, regs, last_cmp, mov_lr_pc, work):
        """Preenche kind/target/succ. Retorna True se o fluxo linear termina aqui."""
        m, ops, thumb, addr = ins.mnem, ins.ops, ins.thumb, ins.addr
        base, cond = split_branch(m)
        ins.cond = cond

        if base in ("b", "bl", "blx") and ops and ops[0].startswith("#"):
            tgt = parse_imm(ops[0])
            ins.target = tgt
            if base == "b":
                ins.kind = "jump"
                work.append((tgt, thumb, "branch"))
                return not cond
            flip = base == "blx"
            tmode = (not thumb) if flip else thumb
            ins.kind = "call"
            ins.calls.append(tgt)
            if self.add_func(tgt, tmode, "bl"):
                work.append((tgt, tmode, "bl"))
            regs.clear()
            return False

        # ---- indirect: bx rX / blx rX / mov pc,rX / ldr pc,... / add pc,... ----
        indirect = None  # 'bx' | 'mov' | 'ldr' | 'add' | 'blx'
        if base == "bx" and ops:
            indirect = "bx"
        elif base == "blx" and ops:
            indirect = "blx"
        elif ops and reg(ops[0]) == "pc":
            core = m
            for suf in CC:
                if len(core) > 2 and core.endswith(suf) and core[:-2] in WRITES:
                    core, ins.cond = core[:-2], True
                    break
            if core in ("mov", "movs"):
                indirect = "mov"
            elif core.startswith("ldr"):
                indirect = "ldr"
            elif core in ("add", "adds"):
                indirect = "add"
            elif core in ("sub", "subs"):
                ins.kind = "ret"
                return not ins.cond
        elif m.startswith(("ldm", "pop")) and re.search(r"\bpc\b", ins.ops[-1] if ops else ""):
            ins.kind = "ret"
            return m in LDM_PLAIN  # condicional continua no fluxo linear
        if indirect == "ldr" and len(ops) > 1 and ops[1].startswith("[sp"):
            ins.kind = "ret"       # ldr pc, [sp], #4
            return not ins.cond
        if indirect is None:
            self.track(ins, regs, last_cmp)
            return False

        src = reg(ops[-1]) if indirect in ("bx", "blx", "mov") else None
        if src in RETURN_REGS and indirect != "blx":
            ins.kind = "ret"
            return not ins.cond
        is_call = indirect == "blx" or mov_lr_pc
        ins.kind = "call" if is_call else "ind"
        val = regs.get(src) if src else None

        # switch com tabela: add rd,rn,rm ; ldr rd,[rd] ; mov pc,rd / bx rd
        if val and val[0] == "e" and not is_call:
            self.add_table(ins, val[1], last_cmp, work)
            return True
        if indirect == "add" and thumb is False and ops[:2] and reg(ops[1]) == "pc":
            self.add_arm_branch_table(ins, last_cmp, work)
            return True
        if val and val[0] == "c":
            v = val[1]
            tthumb = bool(v & 1) if indirect in ("bx", "blx") else thumb
            tgt = v & ~1 if tthumb else v & ~3
            if self.add_func(tgt, tthumb, "bx-const"):
                work.append((tgt, tthumb, "bx-const"))
                ins.target = tgt
                if is_call:
                    ins.calls.append(tgt)
                else:
                    ins.kind = "jump"
            if is_call:
                regs.clear()
            return not (is_call or ins.cond)
        self.unresolved.append((addr, m + " " + " ".join(ops)))
        if is_call:
            regs.clear()
        return not (is_call or ins.cond)

    def track(self, ins, regs, last_cmp):
        m, ops, addr = ins.mnem, ins.ops, ins.addr
        if m in ("pop",) or m.startswith(("ldm", "stm")):
            regs.clear()
            return
        if not ops:
            return
        rd = reg(ops[0])
        if m == "ldr" and len(ops) >= 2 and ops[1].startswith("[pc"):
            mm = re.match(r"\[pc(?:,\s*#(-?(?:0x)?[0-9a-fA-F]+))?\]", ops[1])
            imm = parse_imm(mm.group(1)) if mm and mm.group(1) else 0
            if mm and imm is not None:
                pc = (addr + 4) & ~3 if ins.thumb else addr + 8
                lit = pc + imm
                self.pool.add(lit)
                w = self.word(lit)
                regs.pop(rd, None)
                if w is not None:
                    regs[rd] = ("c", w)
                return
        if m in ("mov", "movs") and len(ops) == 2:
            if ops[1].startswith("#"):
                imm = parse_imm(ops[1])
                regs.pop(rd, None)
                if imm is not None:
                    regs[rd] = ("c", imm & 0xFFFFFFFF)
            else:
                v = regs.get(reg(ops[1]))
                regs.pop(rd, None)
                if v:
                    regs[rd] = v
            return
        if m == "add" and not ins.thumb and len(ops) == 3 and reg(ops[1]) == "pc" and ops[2].startswith("#"):
            imm = parse_imm(ops[2])
            if imm is not None and rd != "pc":
                tgt = (addr + 8 + imm) & 0xFFFFFFFF
                if self.off(tgt) is not None:
                    self.adr_seeds.append(tgt)
                    regs[rd] = ("c", tgt)
                    return
        if m in ("add", "adds") and len(ops) in (2, 3) and not ops[-1].startswith("#"):
            a, b = (reg(ops[0]), reg(ops[1])) if len(ops) == 2 else (reg(ops[1]), reg(ops[2]))
            for x in (a, b):
                v = regs.get(x)
                if v and v[0] == "c" and ROM_BASE <= v[1] < ROM_BASE + self.n:
                    regs[rd] = ("t", v[1])
                    return
            regs.pop(rd, None)
            return
        if m == "ldr" and len(ops) >= 2:
            mm = re.match(r"\[(\w+)(?:,\s*#0(?:x0)?)?\]$", ops[1])
            if mm:
                v = regs.get(reg(mm.group(1)))
                regs.pop(rd, None)
                if v and v[0] == "t":
                    regs[rd] = ("e", v[1])
                return
        if m in WRITES or m.startswith("ldr"):
            regs.pop(rd, None)

    # ---- tabelas de salto ----
    def add_table(self, ins, base, last_cmp, work):
        limit = 64 if last_cmp is None else last_cmp + 1
        targets, a = [], base
        while len(targets) < limit:
            w = self.word(a)
            if w is None:
                break
            t = w & ~1 if ins.thumb else w & ~3
            ok = self.off(t) is not None and abs(t - ins.addr) < 0x4000 and (w & 1 == 0 or ins.thumb)
            if not ok:
                break
            targets.append(t)
            a += 4
        if not targets:
            self.unresolved.append((ins.addr, ins.mnem + " <tabela vazia>"))
            return
        self.tables.append({"site": ins.addr, "base": base, "targets": targets, "thumb": ins.thumb})
        ins.kind = "ind"
        ins.succ.extend(targets)
        for t in targets:
            work.append((t, ins.thumb, "jumptable"))

    def add_arm_branch_table(self, ins, last_cmp, work):
        limit = 64 if last_cmp is None else last_cmp + 1
        targets, a = [], ins.addr + 8
        while len(targets) < limit:
            d = self.decode(a, False)
            if not d or split_branch(d.mnemonic)[0] != "b" or not d.op_str.startswith("#"):
                break
            targets.append(parse_imm(d.op_str))
            a += 4
        if targets:
            self.tables.append({"site": ins.addr, "base": ins.addr + 8, "targets": targets, "thumb": False,
                                "branch_table": True})
            ins.succ.extend(targets)
            ins.kind = "ind"
            for t in targets:
                work.append((t, False, "jumptable"))
            work.append((ins.addr + 8, False, "jumptable"))
        else:
            self.unresolved.append((ins.addr, "add pc,..."))

    # ---- heuristica: ponteiros para prologos ----
    def pointer_seeds(self, lo, hi):
        words = array("I")
        words.frombytes(bytes(self.rom[: self.n - self.n % 4]))
        end = ROM_BASE + self.n
        seeds = {}
        for i, w in enumerate(words):
            if not (ROM_BASE <= w < end) or not (lo <= w < hi) or w in self.insns:
                continue
            o = w - ROM_BASE
            if w & 1:
                if o + 1 < self.n and self.rom[o] == 0xB5:  # push {..,lr}: byte alto do halfword em w-1
                    seeds[w & ~1] = True
            elif w % 4 == 0 and o + 4 <= self.n:
                v = struct.unpack_from("<I", self.rom, o)[0]
                if (v & 0xFFFF0000) == 0xE92D0000 and v & 0x4000:  # stmdb sp!,{..,lr}
                    seeds[w] = False
        return [(w, is_thumb) for w, is_thumb in seeds.items()]

    # ---- funcoes ----
    def build_functions(self):
        res = []
        callers = defaultdict(set)
        for ins in self.insns.values():
            for t in ins.calls:
                callers[t].add(ins.addr)
        for f, (thumb, src) in sorted(self.funcs.items()):
            if f not in self.insns:
                continue
            seen, stack, calls = set(), [f], set()
            while stack:
                a = stack.pop()
                if a in seen or a not in self.insns or (a != f and a in self.funcs):
                    continue
                seen.add(a)
                ins = self.insns[a]
                calls.update(ins.calls)
                stack.extend(ins.succ)
            if not seen:
                continue
            nbytes = sum(self.insns[a].size for a in seen)
            lo, hi = min(seen), max(seen) + self.insns[max(seen)].size
            has_tbl = any(t["site"] in seen for t in self.tables)
            res.append({
                "addr": f, "mode": "thumb" if thumb else "arm", "insns": len(seen), "bytes": nbytes,
                "lo": lo, "hi": hi, "callers": len(callers.get(f, ())), "callees": len(calls),
                "jumptable": has_tbl, "source": src,
            })
        return res

    def coverage(self):
        cov = bytearray(self.n)  # 0 dados, 1 ARM, 2 Thumb, 3 tabela, 4 pool
        for ins in self.insns.values():
            o = ins.addr - ROM_BASE
            cov[o:o + ins.size] = bytes([2 if ins.thumb else 1]) * ins.size
        for t in self.tables:
            if t.get("branch_table"):
                continue
            o = t["base"] - ROM_BASE
            for k in range(len(t["targets"]) * 4):
                if o + k < self.n and cov[o + k] == 0:
                    cov[o + k] = 3
        for a in self.pool:
            o = a - ROM_BASE
            if 0 <= o and o + 4 <= self.n:
                for k in range(4):
                    if cov[o + k] == 0:
                        cov[o + k] = 4
        return cov


# ---- header ----
def parse_header(rom):
    h = {}
    w = struct.unpack_from("<I", rom, 0)[0]
    h["entry_instruction"] = f"0x{w:08X}"
    if (w >> 24) & 0xFF == 0xEA:
        imm = w & 0xFFFFFF
        if imm & 0x800000:
            imm -= 1 << 24
        h["entry_branch_target"] = ROM_BASE + 8 + (imm << 2)
    else:
        h["entry_branch_target"] = None
    h["logo_ok"] = bytes(rom[4:12]) == bytes.fromhex("24FFAE51699AA221")
    txt = lambda b: bytes(b).split(b"\0")[0].decode("ascii", "replace")
    h["title"] = txt(rom[0xA0:0xAC])
    h["game_code"] = txt(rom[0xAC:0xB0])
    h["maker_code"] = txt(rom[0xB0:0xB2])
    h["fixed_96"] = rom[0xB2] == 0x96
    h["unit_code"] = rom[0xB3]
    h["device_type"] = rom[0xB4]
    h["version"] = rom[0xBC]
    chk = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF
    h["complement_check_stored"] = rom[0xBD]
    h["complement_check_computed"] = chk
    h["header_checksum_ok"] = chk == rom[0xBD]
    h["rom_size"] = len(rom)
    return h


def runs(cov):
    names = {0: "data", 1: "arm", 2: "thumb", 3: "jumptable", 4: "literal_pool"}
    out, start = [], 0
    for i in range(1, len(cov) + 1):
        if i == len(cov) or cov[i] != cov[start]:
            out.append((ROM_BASE + start, ROM_BASE + i, names[cov[start]]))
            start = i
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", nargs="?", help="caminho da ROM (padrao: baserom.gba ou o unico *.gba da pasta)")
    ap.add_argument("-o", "--out", default="build/analysis")
    ap.add_argument("--no-ptr-scan", action="store_true", help="nao usar a heuristica de ponteiros")
    args = ap.parse_args()

    rom_path = Path(args.rom) if args.rom else None
    if rom_path is None:
        cands = [Path("baserom.gba")] if Path("baserom.gba").exists() else sorted(Path(".").glob("*.gba"))
        if len(cands) != 1:
            sys.exit("ROM nao encontrada: passe o caminho como argumento")
        rom_path = cands[0]
    rom = rom_path.read_bytes()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    hdr = parse_header(rom)
    print(f"ROM: {rom_path.name}  ({len(rom) / 1048576:.1f} MiB)")
    print(f"  titulo={hdr['title']!r} codigo={hdr['game_code']} fabricante={hdr['maker_code']} versao={hdr['version']}")
    print(f"  logo ok={hdr['logo_ok']}  0x96 fixo={hdr['fixed_96']}  checksum do header ok={hdr['header_checksum_ok']}")
    entry = hdr["entry_branch_target"]
    if entry is None:
        sys.exit("primeira instrucao nao e um B: entry point desconhecido")
    print(f"  entry point: 0x{ROM_BASE:08X} -> B 0x{entry:08X} (ARM)")

    a = Analyzer(rom)
    a.add_func(ROM_BASE, False, "entry")
    a.add_func(entry, False, "entry")
    a.run([(ROM_BASE, False, "entry")])
    n1 = len(a.insns)
    lo = min(a.insns)
    hi = max(a.insns)
    print(f"  passo 1 (sem heuristica): {n1} instrucoes, {len(a.funcs)} funcoes, "
          f"codigo entre 0x{lo:08X} e 0x{hi:08X}")

    if not args.no_ptr_scan:
        seeds = a.pointer_seeds(max(ROM_BASE, lo - 0x10000), hi + 0x10000)
        for addr, thumb in seeds:
            if a.add_func(addr, thumb, "ptr-scan"):
                a.run([(addr, thumb, "ptr-scan")])
        print(f"  passo 2 (ponteiros p/ prologos): +{len(a.insns) - n1} instrucoes "
              f"de {len(seeds)} candidatos")

    funcs = a.build_functions()
    cov = a.coverage()
    cnt = Counter(cov)
    n_arm = sum(1 for i in a.insns.values() if not i.thumb)
    n_thumb = len(a.insns) - n_arm

    # ---- relatorios ----
    (out / "header.json").write_text(json.dumps(hdr, indent=2), encoding="utf-8")
    with open(out / "functions.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["addr", "mode", "insns", "bytes", "lo", "hi", "callers", "callees", "jumptable", "source"])
        for r in funcs:
            w.writerow([f"0x{r['addr']:08X}", r["mode"], r["insns"], r["bytes"], f"0x{r['lo']:08X}",
                        f"0x{r['hi']:08X}", r["callers"], r["callees"], int(r["jumptable"]), r["source"]])
    with open(out / "jumptables.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["site", "table", "entries", "mode", "targets"])
        for t in a.tables:
            w.writerow([f"0x{t['site']:08X}", f"0x{t['base']:08X}", len(t["targets"]),
                        "thumb" if t["thumb"] else "arm", " ".join(f"0x{x:08X}" for x in t["targets"])])
    with open(out / "regions.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start", "end", "size", "kind"])
        for s, e, k in runs(cov):
            w.writerow([f"0x{s:08X}", f"0x{e:08X}", e - s, k])
    with open(out / "unresolved.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["addr", "instruction"])
        for ad, txt in a.unresolved:
            w.writerow([f"0x{ad:08X}", txt])

    mb = lambda c: f"{c / 1024:9.1f} KiB ({100 * c / len(rom):4.1f}%)"
    print("\nResumo")
    print(f"  instrucoes: {n_arm} ARM, {n_thumb} Thumb")
    print(f"  bytes ARM          {mb(cnt[1])}")
    print(f"  bytes Thumb        {mb(cnt[2])}")
    print(f"  bytes lit. pool    {mb(cnt[4])}")
    print(f"  bytes tab. salto   {mb(cnt[3])}")
    print(f"  bytes dados/outros {mb(cnt[0])}")
    by_src = Counter(r["source"] for r in funcs)
    by_mode = Counter(r["mode"] for r in funcs)
    print(f"  funcoes: {len(funcs)}  ({dict(by_mode)})  por origem: {dict(by_src)}")
    print(f"  tabelas de salto: {len(a.tables)} ({sum(len(t['targets']) for t in a.tables)} entradas)")
    print(f"  saltos/chamadas indiretos nao resolvidos: {len(a.unresolved)}")
    print(f"  chamadas para RAM (codigo copiado p/ IWRAM/EWRAM?): {sum(a.ram_targets.values())} "
          f"em {len(a.ram_targets)} enderecos")
    print(f"  falhas de decodificacao: {len(a.bad)}  conflitos ARM/Thumb: {a.mode_conflicts}")
    print(f"\nArquivos em {out}/: header.json functions.csv jumptables.csv regions.csv unresolved.csv")


if __name__ == "__main__":
    main()
