"""Harness de teste diferencial: C recompilado x oraculo (Unicorn/QEMU).

Para cada caso, os dois lados partem do mesmo estado (r0-r14, NZCV, EWRAM,
IWRAM, ROM) e o harness compara o estado ANTES DE CADA INSTRUCAO (pc, r0-r14,
N Z C V), o estado final (+ pc/modo de saida) e a memoria. Falhas de memoria
precisam ocorrer na mesma instrucao nos dois lados.

Limites conhecidos do oraculo: Unicorn nao tem ARM7TDMI; usamos ARM926 (ARMv5TEJ).
Acessos nao alinhados em STR/STRH/LDRH/LDRSH divergem do ARM7 e os casos sao
descartados (Cpu.skip), nao comparados.
"""
import ctypes as C
import hashlib
import os
import random
import struct
import subprocess
import sys

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_ALL,
                     UC_PROT_READ, UC_PROT_EXEC, UC_HOOK_MEM_FETCH_UNMAPPED,
                     UC_HOOK_MEM_FETCH_PROT)
from unicorn import arm_const as AC

from .emit import emit_unit

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, 'recomp', 'runtime')
ROM_BASE, EWRAM, IWRAM, SENTINEL = 0x08000000, 0x02000000, 0x03000000, 0x09000000
EWRAM_SIZE, IWRAM_SIZE = 0x40000, 0x8000
MAX_STEPS = 1000
GPRS = [getattr(AC, 'UC_ARM_REG_R%d' % i) for i in range(15)]


class CpuStruct(C.Structure):
    _fields_ = [('r', C.c_uint32 * 16), ('n', C.c_uint32), ('z', C.c_uint32),
                ('c', C.c_uint32), ('v', C.c_uint32), ('thumb', C.c_uint32),
                ('pc', C.c_uint32), ('steps', C.c_uint32), ('status', C.c_uint32),
                ('fault_addr', C.c_uint32), ('skip', C.c_uint32),
                ('mode', C.c_uint32), ('psr_if', C.c_uint32), ('bank_sp', C.c_uint32 * 16),
                ('bank_lr', C.c_uint32 * 16), ('stop', C.c_uint32)]


class Rom:
    def __init__(self, data):
        self.data = bytes(data)

    def read16(self, addr):
        o = addr - ROM_BASE
        if 0 <= o <= len(self.data) - 2:
            return self.data[o] | (self.data[o + 1] << 8)
        return None


# ------------------------------------------------------------ build do C
def zig_cc():
    return [sys.executable, '-m', 'ziglang', 'cc']


def build_lib(c_source, outdir):
    """Compila runtime + C gerado numa DLL; o nome depende do conteudo."""
    os.makedirs(outdir, exist_ok=True)
    h = hashlib.sha1(c_source.encode())
    for name in sorted(os.listdir(RUNTIME)):   # runtime tambem entra no hash
        with open(os.path.join(RUNTIME, name), 'rb') as f:
            h.update(f.read())
    h = h.hexdigest()[:12]
    gen = os.path.join(outdir, 'gen_%s.c' % h)
    ext = '.dll' if os.name == 'nt' else '.so'
    out = os.path.join(outdir, 'lib_%s%s' % (h, ext))
    if not os.path.exists(out):
        with open(gen, 'w') as f:
            f.write(c_source)
        cmd = zig_cc() + ['-shared', '-O1', '-w', '-I', RUNTIME, '-o', out,
                          os.path.join(RUNTIME, 'gba_cpu.c'), gen]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError('falha ao compilar C gerado:\n' + r.stderr[-4000:])
    lib = C.CDLL(out)
    lib.rc_set_rom.argtypes = [C.c_char_p, C.c_uint32]
    lib.rc_ewram.restype = C.c_void_p
    lib.rc_iwram.restype = C.c_void_p
    lib.rc_trace_buf.restype = C.POINTER(C.c_uint32)
    lib.rc_trace_len.restype = C.c_uint32
    return lib


# ------------------------------------------------------------ lados
class Native:
    def __init__(self, lib, rom):
        self.lib, self.rom = lib, rom
        size = (len(rom.data) + 0xFFF) & ~0xFFF   # o oraculo mapeia por pagina
        self._rom_buf = C.create_string_buffer(rom.data, size)
        lib.rc_set_rom(C.cast(self._rom_buf, C.c_char_p), size)

    def run(self, fn, entry_thumb, st, entry):
        lib = self.lib
        C.memmove(lib.rc_ewram(), st['ewram'], EWRAM_SIZE)
        C.memmove(lib.rc_iwram(), st['iwram'], IWRAM_SIZE)
        lib.rc_reset()
        cpu = CpuStruct()
        for i in range(15):
            cpu.r[i] = st['r'][i]
        cpu.n, cpu.z, cpu.c, cpu.v = st['nzcv']
        cpu.thumb = int(entry_thumb)
        cpu.pc = entry
        getattr(lib, fn)(C.byref(cpu))
        n = lib.rc_trace_len()
        buf = lib.rc_trace_buf()
        recs = [tuple(buf[k * 20:(k + 1) * 20]) for k in range(n)]
        if cpu.status != 2:
            recs.append((cpu.pc,) + tuple(cpu.r[:15]) + (cpu.n, cpu.z, cpu.c, cpu.v))
        mem = None
        if cpu.status != 2:
            mem = (C.string_at(lib.rc_ewram(), EWRAM_SIZE), C.string_at(lib.rc_iwram(), IWRAM_SIZE))
        return dict(recs=recs, status=cpu.status, thumb=cpu.thumb, mem=mem, skip=bool(cpu.skip))


class Oracle:
    def __init__(self, rom):
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        try:
            uc.ctl_set_cpu_model(AC.UC_CPU_ARM_ARM926)
        except Exception:
            pass
        size = (len(rom.data) + 0xFFF) & ~0xFFF
        uc.mem_map(EWRAM, EWRAM_SIZE, UC_PROT_ALL)
        uc.mem_map(IWRAM, IWRAM_SIZE, UC_PROT_ALL)
        uc.mem_map(ROM_BASE, size, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_write(ROM_BASE, rom.data)
        uc.mem_map(SENTINEL, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
        self.uc, self.fnset, self.recs, self.thumb, self.lazy = uc, frozenset(), [], False, []
        uc.hook_add(UC_HOOK_CODE, self._hook)
        uc.hook_add(UC_HOOK_MEM_FETCH_UNMAPPED | UC_HOOK_MEM_FETCH_PROT, self._fetch_fault)

    def _snap(self, addr):
        uc = self.uc
        r = uc.reg_read_batch(GPRS)
        cp = uc.reg_read(AC.UC_ARM_REG_CPSR)
        return (addr,) + tuple(r) + ((cp >> 31) & 1, (cp >> 30) & 1, (cp >> 29) & 1, (cp >> 28) & 1)

    def _fetch_fault(self, uc, access, addr, size, value, _):
        # Mapeia uma pagina zerada so p/ o hook de codigo disparar com o endereco
        # exato do alvo; a instrucao nunca chega a executar (emu_stop no hook).
        page = addr & ~0xFFF
        if page not in self.lazy:
            uc.mem_map(page, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
            self.lazy.append(page)
        return True

    def _hook(self, uc, addr, size, _):
        wrong_mode = bool(uc.reg_read(AC.UC_ARM_REG_CPSR) & 0x20) != self.thumb
        if addr not in self.fnset or wrong_mode or len(self.recs) >= MAX_STEPS:
            self.recs.append(self._snap(addr))
            self.final = True
            uc.emu_stop()
        else:
            self.recs.append(self._snap(addr))

    def run(self, fnset, entry, thumb, st):
        uc = self.uc
        uc.mem_write(EWRAM, st['ewram'])
        uc.mem_write(IWRAM, st['iwram'])
        n, z, c, v = st['nzcv']
        # CPSR primeiro: trocar de modo depois trocaria o banco de r13/r14
        uc.reg_write(AC.UC_ARM_REG_CPSR,
                     0x1F | (0x20 if thumb else 0) | (n << 31) | (z << 30) | (c << 29) | (v << 28))
        for i in range(15):
            uc.reg_write(GPRS[i], st['r'][i])
        self.fnset, self.recs, self.final, self.thumb = fnset, [], False, bool(thumb)
        status = 0
        try:
            uc.emu_start(entry | int(thumb), 0xFFFFFFFE)
        except UcError as e:
            status = 2
            self.err = e
        if status == 0 and not self.final:
            # chegou ao endereco `until` sem executar: conta como saida em 0xFFFFFFFE
            self.recs.append(self._snap(0xFFFFFFFE))
            self.final = True
        t = bool(uc.reg_read(AC.UC_ARM_REG_CPSR) & 0x20)
        for page in self.lazy:
            uc.mem_unmap(page, 0x1000)
        self.lazy = []
        mem = None
        if status == 0:
            mem = (bytes(uc.mem_read(EWRAM, EWRAM_SIZE)), bytes(uc.mem_read(IWRAM, IWRAM_SIZE)))
        return dict(recs=self.recs, status=status, thumb=t, mem=mem)


# ------------------------------------------------------------ estados
class StateGen:
    def __init__(self, seed, rom_size, nimages=6):
        self.rng = random.Random(seed)
        self.rom_size = rom_size
        mk = lambda n: bytearray(self.rng.randbytes(n))
        self.images = []
        for _ in range(nimages):
            e, i = mk(EWRAM_SIZE), mk(IWRAM_SIZE)
            # sprinkle de ponteiros validos para tornar cadeias ldr->ldr uteis
            for img, base, size in ((e, EWRAM, EWRAM_SIZE), (i, IWRAM, IWRAM_SIZE)):
                for _ in range(size // 64):
                    o = self.rng.randrange(0, size - 4, 4)
                    struct.pack_into('<I', img, o, self.ptr())
            self.images.append((bytes(e), bytes(i)))

    def ptr(self):
        g = self.rng
        k = g.randrange(3)
        if k == 0:
            return EWRAM + g.randrange(0, EWRAM_SIZE - 0x200, 4) + 0x100
        if k == 1:
            return IWRAM + g.randrange(0, IWRAM_SIZE - 0x200, 4) + 0x100
        return ROM_BASE + g.randrange(0, max(4, self.rom_size - 0x200), 4)

    def value(self):
        g = self.rng
        k = g.randrange(20)
        if k < 7:
            return self.ptr()
        if k < 11:
            return g.randrange(0, 40)
        if k < 13:
            return g.choice([0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xFF, 0xFFFF, 0x8000])
        return g.getrandbits(32)

    def case(self, thumb):
        g = self.rng
        r = [self.value() for _ in range(13)]
        r.append(0x03007F00 - 4 * g.randrange(0, 64))             # sp
        r.append((SENTINEL | int(thumb)) if g.random() < 0.85 else self.value())  # lr
        e, i = g.choice(self.images)
        return dict(r=r, nzcv=tuple(g.getrandbits(1) for _ in range(4)), ewram=e, iwram=i)


# ------------------------------------------------------------ comparacao
def compare(native, oracle, label=''):
    """Retorna None se igual; senao string descrevendo a primeira divergencia."""
    nr, orr = native['recs'], oracle['recs']
    if native['status'] == 2 or oracle['status'] == 2:
        if native['status'] != oracle['status']:
            return '%s: falha de memoria so em um lado (nativo=%d oraculo=%d), %d/%d passos' % (
                label, native['status'], oracle['status'], len(nr), len(orr))
        # em falha, o ultimo registro e o estado antes da instrucao que falhou
    for k, (a, b) in enumerate(zip(nr, orr)):
        if a != b:
            names = ['pc'] + ['r%d' % i for i in range(15)] + ['N', 'Z', 'C', 'V']
            d = ['%s: nativo=%08x oraculo=%08x' % (names[j], a[j], b[j]) for j in range(20) if a[j] != b[j]]
            prev = ' (insn anterior em %08x)' % nr[k - 1][0] if k else ''
            return '%s: passo %d em %08x%s: %s' % (label, k, a[0], prev, ', '.join(d))
    if len(nr) != len(orr):
        return '%s: trace de tamanho diferente (nativo=%d oraculo=%d)' % (label, len(nr), len(orr))
    if native['status'] != 2:
        if native['status'] == 0 and native['thumb'] != oracle['thumb']:
            return '%s: modo de saida diferente (nativo thumb=%d oraculo=%d)' % (
                label, native['thumb'], oracle['thumb'])
        for nm, x, y in zip(('EWRAM', 'IWRAM'), native['mem'], oracle['mem']):
            if x != y:
                o = next(i for i in range(len(x)) if x[i] != y[i])
                return '%s: %s difere em +0x%x (nativo=%02x oraculo=%02x)' % (label, nm, o, x[o], y[o])
    return None


def diff_function(native, oracle, fn, ncases, seed):
    """Roda `ncases` estados em `fn`. Retorna (executados, descartados, falhas, passos)."""
    gen = StateGen(seed, len(native.rom.data))
    fnset = frozenset(fn.insns)
    ok = skipped = steps = 0
    fails = []
    for k in range(ncases):
        st = gen.case(fn.thumb)
        a = native.run(fn.name, fn.thumb, st, fn.entry)
        if a['skip']:
            skipped += 1
            continue
        b = oracle.run(fnset, fn.entry, fn.thumb, st)
        msg = compare(a, b, '%s caso %d' % (fn.name, k))
        if msg:
            fails.append(msg)
        else:
            ok += 1
            steps += len(a['recs'])
    return ok, skipped, fails, steps
