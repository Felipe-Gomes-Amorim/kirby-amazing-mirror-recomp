"""Testes do recompilador. Rodar: .venv/Scripts/python -m unittest discover -s tests -t .

- decode: encodings de instrucoes ARM/Thumb (vetores sinteticos do manual da ISA)
- fuzz diferencial por instrucao (ARM e Thumb) contra o Unicorn
- bios HLE (C, tests/bios_test.c)
- boot: crt0 -> primeira VBlankIntrWait  (lento: RECOMP_SLOW=1 e baserom.gba + mapa)
"""
import os
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from recomp import suites                      # noqa: E402
from recomp.decode import decode_arm, decode_thumb  # noqa: E402
from recomp.harness import RUNTIME, zig_cc     # noqa: E402

ROM = os.path.join(ROOT, 'baserom.gba')
MAP = os.path.join(ROOT, 'build', 'analysis', 'functions.csv')


class DecodeTest(unittest.TestCase):
    def test_arm_basic(self):
        i = decode_arm(0xE3A00012, 0x08001000)          # mov r0,#0x12
        self.assertEqual((i.kind, i.f['op'], i.f['rd'], i.f['op2']), ('dp', 13, 0, ('imm', 0x12, None)))
        i = decode_arm(0xE129F000, 0x08001004)          # msr cpsr_fc, r0
        self.assertEqual((i.kind, i.f['mask']), ('msr', 9))
        i = decode_arm(0xE12FFF11, 0x08001008)          # bx r1
        self.assertEqual((i.kind, i.f['rm']), ('bx', 1))
        i = decode_arm(0xEAFFFFF2, 0x0800100C)          # b 0x08000FDC
        self.assertEqual((i.kind, i.f['target']), ('b', 0x08000FDC))
        i = decode_arm(0xE59FD028, 0x08001010)          # ldr sp,[pc,#0x28]
        self.assertEqual((i.kind, i.f['rd'], i.f['base']), ('ls', 13, ('const', 0x08001010 + 8)))

    def test_thumb(self):
        i = decode_thumb(0xB500, 0x08002000)            # push {lr}
        self.assertEqual((i.kind, i.f['load'], i.f['regs']), ('lsm', False, [14]))
        i = decode_thumb(0xBD10, 0)                     # pop {r4,pc}
        self.assertEqual((i.kind, i.f['load'], i.f['regs']), ('lsm', True, [4, 15]))
        i = decode_thumb(0xF7FE, 0x08152A06, 0xFDC1)    # bl
        self.assertEqual(i.kind, 'b')
        self.assertTrue(i.f['link'])
        self.assertEqual(i.size, 4)
        i = decode_thumb(0xDF05, 0)                     # swi 5
        self.assertEqual((i.kind, i.f['imm']), ('swi', 5))


class FuzzTest(unittest.TestCase):
    def _run(self, thumb):
        ok, skipped, fails, steps = suites.fuzz(thumb, n_insns=200, cases=4, seed=3, verbose=False)
        self.assertEqual(fails, [], '\n'.join(fails[:10]))
        self.assertGreater(ok, 300)

    def test_fuzz_thumb(self):
        self._run(True)

    def test_fuzz_arm(self):
        self._run(False)


class BiosTest(unittest.TestCase):
    def test_bios_hle(self):
        out = os.path.join(ROOT, 'build', 'recomp', 'bios_test.exe')
        os.makedirs(os.path.dirname(out), exist_ok=True)
        srcs = [os.path.join(ROOT, 'tests', 'bios_test.c')] + [
            os.path.join(RUNTIME, n) for n in ('gba_bus.c', 'gba_rt.c', 'gba_bios.c', 'rt_interp.c')]
        r = subprocess.run(zig_cc() + ['-O1', '-w', '-I', RUNTIME, '-o', out] + srcs,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run([out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)


@unittest.skipUnless(os.environ.get('RECOMP_SLOW') and os.path.exists(ROM) and os.path.exists(MAP),
                     'RECOMP_SLOW=1 + baserom.gba + build/analysis/functions.csv')
class BootTest(unittest.TestCase):
    def test_boot_to_first_vblank_wait(self):
        from recomp import program
        rc, out, err, overlays = program.iterate(
            ROM, MAP, verbose=False, boot_args=['--timeout', '120', '--frames', '600'])
        self.assertEqual(rc, 0, err[-2000:])
        self.assertIn('RT_STOP frames', out)
        self.assertIn('nao_resolvidos=0', out)
        self.assertRegex(out, r'IRQ entregues=60[01]')

    def test_interpreter_selftest_and_forced_fallback(self):
        import subprocess as sp
        exe = os.path.join(ROOT, 'build', 'recomp', 'program', 'rt_boot.exe')
        self.assertTrue(os.path.exists(exe), 'rode test_boot_to_first_vblank_wait antes')
        r = sp.run([exe, ROM, '--selftest-interp', '--sav', 'none', '--timeout', '100'],
                   capture_output=True, text=True, timeout=150)
        self.assertEqual(r.returncode, 0, r.stderr[-1500:])
        self.assertIn('0 divergencias', r.stderr)
        self.assertIn('corpos SRAM em 3 enderecos da pilha: OK', r.stderr)


if __name__ == '__main__':
    unittest.main()
