"""Save atomico (recomp/runtime/rt_save.c).

- debounce/limites/falha: `rt_boot.exe ROM --selftest-save DIR` (relogio falso)
- integridade sob kill: um processo grava a SRAM inteira sem parar (cada geracao = um byte
  uniforme), e o teste o MATA (TerminateProcess = kill -9) em instantes aleatorios; o .sav tem que
  ter sempre 32768 bytes uniformes (save antigo ou novo, nunca parcial).
Pula se o executavel (build/recomp/program/rt_boot.exe ou $KIRBY_RT_BOOT) ou a ROM nao existem."""
import os
import random
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, 'baserom.gba')


def find_exe():
    cands = [os.environ.get('KIRBY_RT_BOOT')]
    for sub in (('build', 'recomp', 'program'), ('build', 'cold', 'io', 'interp'), ('build', 'cold', 'os48')):
        cands.append(os.path.join(ROOT, *sub, 'rt_boot.exe' if os.name == 'nt' else 'rt_boot'))
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


EXE = find_exe()


@unittest.skipUnless(EXE and os.path.exists(ROM), 'needs rt_boot and baserom.gba (build first)')
class SaveTest(unittest.TestCase):
    def test_debounce_heavy_writes_and_failure_recovery(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run([EXE, ROM, '--selftest-save', d, '--sav', 'none'], capture_output=True,
                               text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stderr[-1500:])
            self.assertIn('[selftest-save] OK', r.stderr)

    def test_kill_in_the_middle_never_leaves_a_partial_save(self):
        rnd = random.Random(7)
        with tempfile.TemporaryDirectory() as d:
            sav = os.path.join(d, 'k.sav')
            with open(sav, 'wb') as f:
                f.write(b'\xAA' * 32768)
            seen = set()
            for i in range(40):
                p = subprocess.Popen([EXE, ROM, '--selftest-save-writer', sav, '--sav', 'none'],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(rnd.uniform(0.15, 0.6))   # tempo p/ iniciar + varias geracoes
                p.kill()
                p.wait()
                with open(sav, 'rb') as f:
                    data = f.read()
                self.assertEqual(len(data), 32768, 'iteration %d: size %d' % (i, len(data)))
                self.assertEqual(len(set(data)), 1, 'iteration %d: PARTIAL save (%d distinct bytes)'
                                 % (i, len(set(data))))
                seen.add(data[0])
            self.assertGreater(len(seen), 3, 'the writer should have produced several generations: %s' % seen)


if __name__ == '__main__':
    unittest.main()
