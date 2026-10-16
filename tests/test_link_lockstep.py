"""Link por quadro em LOCKSTEP (Nasus): duas instancias reais (`--link host` / `--link join`) do jogo.

- O fluxo de trocas e deterministico: duas execucoes com o mesmo roteiro de teclas dao o mesmo hash
  (`LINK exchange hash`), e host e join veem o mesmo fluxo (mesmo hash e mesmo log de halfwords).
- Aceite: as duas instancias passam pelo menu SUB-GAMES > Speed Eaters > MULTI PLAYER, trocam pacotes gLink
  VALIDOS (soma dos 12 halfwords = 0xFFF1) dos DOIS jogadores e chegam a partida.
- O join termina limpo quando o host sai antes.

Lento (~1 min): so com RECOMP_SLOW=1. Precisa de baserom.gba e rt_boot (build/recomp/program, $KIRBY_RT_BOOT
ou build/cold/os48). O gancho para o roteiro do lobby do co-op (Poppy) e a constante KEYS_COOP."""
import os
import re
import socket
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, 'baserom.gba')


def find_exe():
    cands = [os.environ.get('KIRBY_RT_BOOT')]
    for sub in (('build', 'recomp', 'program'), ('build', 'cold', 'os48')):
        cands.append(os.path.join(ROOT, *sub, 'rt_boot.exe' if os.name == 'nt' else 'rt_boot'))
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


EXE = find_exe()

# titulo > file select > game select > SUB-GAMES > Speed Eaters > MULTI PLAYER > "OK?" (host = P1)
KEYS_MENU = ('A@700+8,A@800+8,A@1060+8,DOWN@1200+4,A@1260+8,A@1500+8,DOWN@1650+4,A@1700+8,A@2500+8')
# O host comeca a partida com A (P1 e quem decide)
KEYS_HOST = KEYS_MENU + ',A@3400+8'
KEYS_JOIN = KEYS_MENU
# gancho: troque por o roteiro do lobby do co-op quando a Poppy entregar (NOTES, secao 12)
KEYS_COOP = None


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run_pair(frames, keys_host, keys_join, tmp, tag, join_frames=None):
    port = free_port()
    ini = os.path.join(tmp, 'empty.ini')
    open(ini, 'w').close()
    base = [EXE, ROM, '--bench', '--sav', 'none', '--ini', ini, '--link-nopace', '--link-port', str(port)]
    env_h = dict(os.environ, KIRBY_SIO_LOG=os.path.join(tmp, tag + '.h.log'))
    env_j = dict(os.environ, KIRBY_SIO_LOG=os.path.join(tmp, tag + '.j.log'))
    host = subprocess.Popen(base + ['--frames', str(frames), '--link', 'host', '--keys', keys_host],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env_h)
    join = subprocess.Popen(base + ['--frames', str(join_frames or frames), '--link', 'join', '--keys', keys_join],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env_j)
    try:
        jo = join.communicate(timeout=300)[0]
        ho = host.communicate(timeout=300)[0]
    finally:
        for p in (host, join):
            if p.poll() is None:
                p.kill()
    return host.returncode, ho, join.returncode, jo


def stats(out):
    h = re.search(r'LINK exchange hash: (\w+) over (\d+) exchanges', out)
    v = re.search(r'P1 valid=(\d+) invalid=(\d+) sync=\d+ \| P2 valid=(\d+) invalid=(\d+)', out)
    assert h and v, out[-1500:]
    return h.group(1), int(h.group(2)), int(v.group(1)), int(v.group(3))


@unittest.skipUnless(os.environ.get('RECOMP_SLOW') and EXE and os.path.exists(ROM),
                     'RECOMP_SLOW=1 + rt_boot + baserom.gba')
class LockstepTest(unittest.TestCase):
    def test_deterministic_and_valid_packets(self):
        with tempfile.TemporaryDirectory() as tmp:
            res = []
            for tag in ('a', 'b'):
                hr, ho, jr, jo = run_pair(3700, KEYS_HOST, KEYS_JOIN, tmp, tag)
                self.assertEqual(hr, 0, ho[-1500:])
                self.assertEqual(jr, 0, jo[-1500:])
                res.append((stats(ho), stats(jo)))
                with open(os.path.join(tmp, tag + '.h.log'), 'rb') as f, open(os.path.join(tmp, tag + '.j.log'), 'rb') as g:
                    self.assertEqual(f.read(), g.read(), 'host and join must see the same halfword stream')
            (ha, ja), (hb, jb) = res
            self.assertEqual(ha, hb, 'two runs with the same keys must produce the same exchange hash')
            self.assertEqual(ja, jb)
            self.assertEqual(ha[0], ja[0], 'host and join: same hash')
            # pacotes gLink validos dos DOIS jogadores, vistos pelas duas instancias
            for _, n, p1, p2 in (ha, ja):
                self.assertGreater(n, 5000)
                self.assertGreater(p1, 500)
                self.assertGreater(p2, 500)

    def test_join_exits_cleanly_when_host_leaves(self):
        with tempfile.TemporaryDirectory() as tmp:
            hr, ho, jr, jo = run_pair(900, 'A@700+8', 'A@700+8', tmp, 'x', join_frames=1800)
            self.assertEqual(hr, 0, ho[-800:])
            self.assertEqual(jr, 0, jo[-800:])
            self.assertIn('host disconnected', jo)


if __name__ == '__main__':
    unittest.main()
