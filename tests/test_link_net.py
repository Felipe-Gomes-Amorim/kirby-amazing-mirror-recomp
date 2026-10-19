"""Link em rede (Nasus, P2): atraso de entrada K, rede simulada, medicao, dessincronia e seguranca do HELLO.

Duas instancias reais do jogo em 127.0.0.1 (`--link host` / `--link join`), roteiro SUB-GAMES > Speed Eaters.
- K=2 (quadros de folga do host): sem "Communication Error" (`0x02038580 == 0`, gLink sincronizado `0x03002554 == 0x8393`).
- O fluxo de trocas depende so de K, nunca da rede: o hash do host e o mesmo com e sem latencia/jitter/perda
  simulados (`--link-sim`), e o log do filho e prefixo do log do host.
- `--link-status` grava o JSON de medicao (RTT, quadros em espera, bytes/quadro, desync).
- Dessincronia injetada (KIRBY_LINK_FAULT) e detectada nos dois lados e registrada no JSON.
- O host rejeita ROM diferente, versao de protocolo diferente e lixo no socket.
Lento: so com RECOMP_SLOW=1."""
import json
import os
import socket
import subprocess
import tempfile
import time
import unittest

from tests.netharness import Cluster, LT
from tests.test_link_lockstep import EXE, ROM, KEYS_HOST, KEYS_JOIN, free_port, stats


def run_pair(tmp, tag, frames, host_args=(), join_args=(), env_join=None, env_host=None, wait_s=None):
    port = free_port()
    ini = os.path.join(tmp, 'empty.ini')
    open(ini, 'w').close()
    base = [EXE, ROM, '--bench', '--sav', 'none', '--ini', ini, '--link-nopace', '--link-port', str(port),
            '--frames', str(frames)]
    peek = ['--snap', '3000,%d' % (frames - 10), '--peek', '02038580,03002554']
    eh = dict(os.environ, KIRBY_SIO_LOG=os.path.join(tmp, tag + '.h.log'), **(env_host or {}))
    ej = dict(os.environ, KIRBY_SIO_LOG=os.path.join(tmp, tag + '.j.log'), **(env_join or {}))
    if wait_s:
        eh['KIRBY_LINK_WAIT_S'] = ej['KIRBY_LINK_WAIT_S'] = str(wait_s)
    outs = {}
    for who in 'hj':
        outs[who] = open(os.path.join(tmp, tag + '.%s.out' % who), 'w+')
    host = subprocess.Popen(base + ['--link', 'host', '--keys', KEYS_HOST, '--link-status', os.path.join(tmp, tag + '.h.json')]
                            + peek + list(host_args), stdout=outs['h'], stderr=subprocess.STDOUT, text=True, env=eh)
    time.sleep(1)
    join = subprocess.Popen(base + ['--link', 'join', '--keys', KEYS_JOIN, '--link-status', os.path.join(tmp, tag + '.j.json')]
                            + peek + list(join_args), stdout=outs['j'], stderr=subprocess.STDOUT, text=True, env=ej)
    try:
        join.wait(timeout=400)
        host.wait(timeout=400)
    finally:
        for p in (host, join):
            if p.poll() is None:
                p.kill()
    res = {}
    for who in 'hj':
        outs[who].seek(0)
        res[who] = outs[who].read()
        outs[who].close()
    return host.returncode, res['h'], join.returncode, res['j']


def log_lines(tmp, tag, who):
    with open(os.path.join(tmp, '%s.%s.log' % (tag, who)), 'rb') as f:
        return f.read().splitlines()


@unittest.skipUnless(os.environ.get('RECOMP_SLOW') and EXE and os.path.exists(ROM),
                     'RECOMP_SLOW=1 + rt_boot + baserom.gba')
class NetTest(unittest.TestCase):
    def check_game_ok(self, out):
        # sem erro (0x02038580 = 0) e gLink sincronizado (0x03002554 = 0x8393), no quadro 3000 e no fim
        import re
        self.assertEqual(len(re.findall(r'02038580=00000000 03002554=0000(?:8393|8303)', out)), 2, out[-800:])

    def test_delay_k2_with_simulated_network(self):
        with tempfile.TemporaryDirectory() as tmp:
            hr, ho, jr, jo = run_pair(tmp, 'a', 3800, ['--link-delay', '2'], [])
            self.assertEqual((hr, jr), (0, 0), ho[-800:] + jo[-800:])
            ha, ja = stats(ho), stats(jo)
            self.check_game_ok(ho)
            self.check_game_ok(jo)
            self.assertGreater(ha[2], 500)
            self.assertGreater(ha[3], 500)
            hr, ho2, jr, jo2 = run_pair(tmp, 'b', 3800, ['--link-delay', '2', '--link-sim', '25,10,3'], ['--link-sim', '25,10,3'])
            self.assertEqual((hr, jr), (0, 0), ho2[-800:] + jo2[-800:])
            hb = stats(ho2)
            self.assertEqual(ha[0], hb[0], 'same K, same keys: the exchange stream must not depend on the network')
            self.check_game_ok(ho2)
            self.check_game_ok(jo2)
            self.assertNotIn('DESYNC', ho2 + jo2)
            self.assertIn('RTT', ho2)
            # o filho fica atras do host: o log dele e prefixo do log do host
            h, j = log_lines(tmp, 'b', 'h'), log_lines(tmp, 'b', 'j')
            self.assertEqual(h[:len(j)], j)
            # medicao
            st = json.load(open(os.path.join(tmp, 'b.h.json')))
            self.assertEqual(st['delay'], 2)
            self.assertEqual(st['role'], 'host')
            self.assertGreater(st['rtt_avg_ms'][1], 30)   # 25 ms de ida + jitter
            self.assertFalse(st['desync'])
            self.assertGreater(st['tx_bytes'], 0)

    def test_desync_detected(self):
        with tempfile.TemporaryDirectory() as tmp:
            hr, ho, jr, jo = run_pair(tmp, 'd', 1500, [], [], env_join={'KIRBY_LINK_FAULT': '1200'})
            self.assertEqual((hr, jr), (0, 0))
            self.assertIn('DESYNC', ho)
            self.assertIn('DESYNC', jo)
            self.assertTrue(json.load(open(os.path.join(tmp, 'd.h.json')))['desync'])
            self.assertTrue(json.load(open(os.path.join(tmp, 'd.j.json')))['desync'])

    def test_hello_is_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            for var, text in (('KIRBY_LINK_TEST_BADROM', 'different ROM'), ('KIRBY_LINK_TEST_BADVER', 'protocol version')):
                hr, ho, jr, jo = run_pair(tmp, 's', 60, [], [], env_join={var: '1'}, wait_s=3)
                self.assertIn(text, ho)
                self.assertIn('rejected', jo)
                self.assertIn('running without cable', jo)
            # lixo no socket do host: recusado, o host segue esperando o jogador de verdade
            port = free_port()
            ini = os.path.join(tmp, 'e.ini')
            open(ini, 'w').close()
            host = subprocess.Popen([EXE, ROM, '--bench', '--sav', 'none', '--ini', ini, '--link', 'host', '--link-port', str(port),
                                     '--frames', '30'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    env=dict(os.environ, KIRBY_LINK_WAIT_S='4'))
            time.sleep(1)
            s = socket.create_connection(('127.0.0.1', port))
            s.sendall(b'GET / HTTP/1.0\r\n\r\n')
            time.sleep(0.5)
            s.close()
            out = host.communicate(timeout=60)[0]
            self.assertIn('rejected a connection', out)


COOP = LT.COOP_FRAMES


def reached_coop(r, players):
    st = r.state(COOP)   # (sessao, jogadores, status gLink, link ligado, erro)
    return st is not None and st[0] == 2 and st[1] == players and st[3] == 1 and st[4] == 0


@unittest.skipUnless(os.environ.get('RECOMP_SLOW') and EXE and os.path.exists(ROM),
                     'RECOMP_SLOW=1 + rt_boot + baserom.gba')
class MaxDelayTest(unittest.TestCase):
    """K maximo por numero de jogadores: 2 -> 2; 3 e 4 -> 1 (KatAM: a cadeia do lobby passa por 2 saltos)."""

    def cluster(self, n, delay, tmp, proxies=None, env=None):
        status = os.path.join(tmp, 'host.json')
        return Cluster(n, COOP + 1, extra=['--link-delay', str(delay)], per_instance_extra={0: ['--link-status', status]},
                       proxies=proxies, env=env), status

    def test_requested_above_max_is_reduced_and_reaches_coop(self):
        for n, ask, want in ((2, 3, 2), (3, 2, 1), (4, 2, 1)):
            with self.subTest(players=n, asked=ask), tempfile.TemporaryDirectory() as tmp:
                c, status = self.cluster(n, ask, tmp)
                with c:
                    res = c.run()
                for r in res:
                    self.assertEqual(r.rc, 0, r.err[-300:])
                    self.assertTrue(reached_coop(r, n), (r.idx, r.state(COOP)))
                msg = '[link] K=%d is above %d for %d players, using %d' % (ask, want, n, want)
                self.assertIn(msg, '\n'.join(res[0].link_lines()))
                with open(status) as f:
                    st = json.load(f)
                self.assertEqual((st['delay'], st['delay_max'], st['players']), (want, want, n))

    def test_auto_respects_the_max_with_latency(self):
        # RTT ~70 ms pediria K=4 -> limitado ao maximo do numero de jogadores
        for n, want in ((2, 2), (3, 1)):
            with self.subTest(players=n), tempfile.TemporaryDirectory() as tmp:
                c, status = self.cluster(n, 'auto', tmp, proxies={1: dict(latency_ms=35)})
                with c:
                    res = c.run(timeout=400)
                for r in res:
                    self.assertEqual(r.rc, 0, r.err[-300:])
                    self.assertTrue(reached_coop(r, n), (r.idx, r.state(COOP)))
                self.assertRegex('\n'.join(res[0].link_lines()), r'input delay K=%d frame\(s\) \(auto\)' % want)
                with open(status) as f:
                    self.assertEqual(json.load(f)['delay_max'], want)

    def test_reference_hashes_two_players(self):
        """Os hashes de referencia (roteiro SUB-GAMES) nao mudam: K=0 e K=2."""
        with tempfile.TemporaryDirectory() as tmp:
            for tag, k, frames, want in (('r0', 0, 3200, '3f70b1ec58b17557'), ('r2', 2, 3800, '57fb962b2ca52a64')):
                hr, ho, jr, jo = run_pair(tmp, tag, frames, ['--link-delay', str(k)], [])
                self.assertEqual((hr, jr), (0, 0))
                self.assertEqual(stats(ho)[0], want, 'K=%d' % k)


if __name__ == '__main__':
    unittest.main()
