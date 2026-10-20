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
import re
import os
import socket
import subprocess
import tempfile
import time
import unittest

from tests.netharness import Cluster, LT, exchange_prefix_ok
from tests.test_link_lockstep import EXE, ROM, KEYS_HOST, KEYS_JOIN, free_port, stats


def run_pair(tmp, tag, frames, host_args=(), join_args=(), env_join=None, env_host=None, wait_s=None, join_frames=None):
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
                            + peek + (['--frames', str(join_frames)] if join_frames else []) + list(join_args), stdout=outs['j'], stderr=subprocess.STDOUT, text=True, env=ej)
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


@unittest.skipUnless(os.environ.get('RECOMP_SLOW') and EXE and os.path.exists(ROM),
                     'RECOMP_SLOW=1 + rt_boot + baserom.gba')
class UdpTest(unittest.TestCase):
    """Transporte UDP (confiabilidade por redundancia): mesmo resultado do TCP, bem menos paradas com perda real."""
    SIM = '10,20,5,3,2'   # ida 10 ms, jitter 20, 5% de perda, 3% fora de ordem, 2% duplicados

    def run_k2(self, tmp, tag, transport, sim):
        args = ['--link-delay', '2', '--link-transport', transport]
        jargs = ['--link-transport', transport]
        if sim:
            args += ['--link-sim', sim]
            jargs += ['--link-sim', sim]
        hr, ho, jr, jo = run_pair(tmp, tag, 3800, args, jargs)
        self.assertEqual((hr, jr), (0, 0), ho[-600:] + jo[-600:])
        with open(os.path.join(tmp, tag + '.h.json')) as f:
            return stats(ho), ho, jo, json.load(f)

    def test_same_hash_as_tcp_and_fewer_stalls_under_loss(self):
        with tempfile.TemporaryDirectory() as tmp:
            clean, ho, jo, _ = self.run_k2(tmp, 'c', 'tcp', None)
            udp, uho, ujo, ust = self.run_k2(tmp, 'u', 'udp', self.SIM)
            tcp, tho, tjo, tst = self.run_k2(tmp, 't', 'tcp', self.SIM)
            self.assertEqual(clean[0], udp[0], 'UDP under loss/reorder/duplication must give the same exchange hash as TCP without loss')
            self.assertEqual(clean[0], tcp[0])
            for out in (uho, ujo):
                self.assertEqual(len(re.findall(r'02038580=00000000 03002554=0000(?:8393|8303)', out)), 2, out[-500:])
            self.assertNotIn('DESYNC', uho + ujo)
            self.assertEqual(ust['transport'], 'udp')
            self.assertEqual(tst['transport'], 'tcp')
            # paradas do host: UDP bem melhor que TCP nas mesmas condicoes
            self.assertLess(ust['stall_gt100'] + ust['stall_gt250'], (tst['stall_gt100'] + tst['stall_gt250']) / 4.0,
                            (ust['stall_gt33'], ust['stall_gt100'], ust['stall_gt250'], tst['stall_gt33'], tst['stall_gt100'], tst['stall_gt250']))
            self.assertLess(ust['stall_gt33'], tst['stall_gt33'] / 2.0)
            self.assertGreater(ust['udp_dup_rx'] + ust['udp_retx'], 0)
            print('\nUDP stalls >33/>100/>250: %d/%d/%d  TCP: %d/%d/%d' % (ust['stall_gt33'], ust['stall_gt100'], ust['stall_gt250'],
                                                                          tst['stall_gt33'], tst['stall_gt100'], tst['stall_gt250']))

    def test_udp_reference_hash_and_clean_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            hr, ho, jr, jo = run_pair(tmp, 'z', 3200, ['--link-delay', '0', '--link-transport', 'udp'], ['--link-transport', 'udp'])
            self.assertEqual((hr, jr), (0, 0))
            self.assertEqual(stats(ho)[0], '3f70b1ec58b17557')
            t0 = time.time()
            hr, ho, jr, jo = run_pair(tmp, 'y', 900, ['--link-transport', 'udp'], ['--link-transport', 'udp'], join_frames=1800)
            self.assertEqual((hr, jr), (0, 0))
            self.assertIn('host disconnected', jo)   # BYE: sem esperar os 8 s de timeout
            self.assertNotIn('no datagram from the peer', jo)

    def test_udp_hello_checks_and_tcp_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            for var, text in (('KIRBY_LINK_TEST_BADROM', 'different ROM'), ('KIRBY_LINK_TEST_BADVER', 'protocol version')):
                hr, ho, jr, jo = run_pair(tmp, 's', 60, ['--link-transport', 'udp'], ['--link-transport', 'udp'], env_join={var: '1'}, wait_s=3)
                self.assertIn(text, ho)
                self.assertIn('rejected', jo)
                self.assertIn('running without cable', jo)
            # auto: o host escuta TCP+UDP; o filho que nao fala UDP (host so TCP) cai para TCP em 3 s
            hr, ho, jr, jo = run_pair(tmp, 'f', 120, ['--link-transport', 'tcp'], ['--link-transport', 'auto'], wait_s=30)
            self.assertEqual((hr, jr), (0, 0), jo[-400:])
            self.assertIn('falling back to tcp', jo)
            self.assertIn('(lockstep)', jo)
            hr, ho, jr, jo = run_pair(tmp, 'g', 120, ['--link-transport', 'auto'], ['--link-transport', 'auto'], wait_s=30)
            self.assertEqual((hr, jr), (0, 0))
            self.assertIn('player 2 connected (udp)', ho)

    def test_coop_over_udp_with_loss_matches_tcp_hash(self):
        """Co-op (2 jogadores, K=2) com 5% de perda, jitter 20 ms e reordenacao: chega ao co-op sem erro e o hash do host
        e o mesmo do TCP sem perda."""
        def run(extra):
            with Cluster(2, COOP + 1, extra=['--link-delay', '2'] + extra) as c:
                return c.run(timeout=400)
        tcp = run(['--link-transport', 'tcp'])
        udp = run(['--link-transport', 'udp', '--link-sim', self.SIM])
        for r in tcp + udp:
            self.assertEqual(r.rc, 0, r.err[-300:])
            self.assertTrue(reached_coop(r, 2), (r.idx, r.state(COOP)))
        self.assertEqual(tcp[0].hash[0], udp[0].hash[0])
        self.assertTrue(exchange_prefix_ok(udp[0], udp[1])[0])

    def test_four_players_over_udp_reach_coop(self):
        with tempfile.TemporaryDirectory() as tmp:
            c = Cluster(4, COOP + 1, extra=['--link-delay', '1', '--link-transport', 'udp', '--link-sim', '3,3,2,2,1'])
            with c:
                res = c.run(timeout=400)
            for r in res:
                self.assertEqual(r.rc, 0, r.err[-300:])
                self.assertTrue(reached_coop(r, 4), (r.idx, r.state(COOP)))
            for r in res[1:]:   # o log de cada filho e prefixo do log do host
                self.assertTrue(exchange_prefix_ok(res[0], r)[0], r.idx)


if __name__ == '__main__':
    unittest.main()
