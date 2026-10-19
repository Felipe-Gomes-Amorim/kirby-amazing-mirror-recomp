"""Testes de REDE do link (Poppy): instancias reais do jogo em 127.0.0.1 + proxy TCP com latencia/jitter/perda/corte/
corrupcao (tests/netharness.py). Independentes do `--link-sim` da Nasus (que fica em tests/test_link_net.py): aqui a
rede e de verdade (sockets) e o co-op usa o roteiro de teclas da secao 13 do NOTES (START GAME > MULTI PLAYER).

    .venv\\Scripts\\python.exe -m unittest tests.test_link_network -v                 # rapidos (~35 s)
    set RECOMP_SLOW=1 & .venv\\Scripts\\python.exe -m unittest tests.test_link_network -v   # tudo (~15 min)

Resultados medidos que estes testes fixam: NOTES.md, secao 14 ('P1d'). Nao use `rt_boot.exe` de outra pasta enquanto a
Nasus reconstroi o executavel: o relink mata os processos em execucao (`run_retry` repete se o rc for 1 sem relatorio).
"""
import os
import socket
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netharness as H                      # noqa: E402
from netharness import Cluster, LT, exchange_prefix_ok   # noqa: E402

SLOW = bool(os.environ.get('RECOMP_SLOW'))
need_exe = unittest.skipUnless(H.AVAILABLE, 'precisa de rt_boot.exe e baserom.gba')
need_slow = unittest.skipUnless(H.AVAILABLE and SLOW, 'RECOMP_SLOW=1 + rt_boot.exe + baserom.gba')

COOP = LT.COOP_FRAMES


def run_retry(make, tries=3, **kw):
    """Roda um Cluster; repete se um processo sai com rc=1 SEM relatorio (== morto de fora, ver docstring do modulo)."""
    last = None
    for _ in range(tries):
        with make() as c:
            last = c.run(**kw)
        if not any(r.rc == 1 and r.hash is None for r in last):
            break
    return last


def reached_coop(r, frame, players):
    st = r.state(frame)
    return bool(st) and st[0] == 2 and st[1] == players and st[3] == 1 and st[4] == 0


def link_lines(r, *needles):
    return [l for l in r.link_lines() if any(n in l for n in needles)]


class Base(unittest.TestCase):
    def assert_consistent(self, res):
        """Todos terminaram, tem relatorio, e o fluxo de trocas de cada filho e PREFIXO do host (com K quadros de folga o
        host sai antes de o filho consumir os ultimos; sem K os hashes sao iguais)."""
        for r in res:
            self.assertIsNotNone(r.hash, (r.idx, r.rc, r.out[-200:], r.err[-200:]))
        for r in res[1:]:
            ok, where = exchange_prefix_ok(res[0], r)
            self.assertTrue(ok, 'fluxo de trocas do jogador %d diverge do host na troca %s' % (r.idx + 1, where))

    def assert_coop(self, res, frame, players):
        for r in res:
            self.assertEqual(r.rc, 0, (r.idx, r.err[-300:]))
            self.assertTrue(reached_coop(r, frame, players), (r.idx, r.state(frame)))
        self.assert_consistent(res)


# ---------------------------------------------------------------------------------------------------------------
# 1. Base: 2 e 4 jogadores
# ---------------------------------------------------------------------------------------------------------------
@need_exe
class TestBaseline(Base):
    def test_two_players_coop(self):
        res = run_retry(lambda: Cluster(2, COOP + 1))
        self.assert_coop(res, COOP, 2)
        self.assertEqual(res[0].hash, res[1].hash)             # K=0 (auto em loopback): hash identico
        for r in res:
            v = r.valid()
            self.assertGreater(v[1][0], 1000)
            self.assertGreater(v[2][0], 1000)
            self.assertLessEqual(v[1][1] + v[2][1], 4)         # so aquecimento

    def test_four_players_coop(self):
        """host + 3 joins com o MESMO roteiro de 2 jogadores (OK? so no host; A@2000 em todos)."""
        res = run_retry(lambda: Cluster(4, COOP + 1))
        self.assert_coop(res, COOP, 4)
        self.assertEqual(len({r.hash for r in res}), 1)
        for r in res:
            for slot in (1, 2, 3, 4):
                self.assertGreater(r.valid()[slot][0], 1000, (r.idx, r.valid()))
        self.assertEqual(res[0].state(COOP)[2], 0x8F9F)        # mestre + 4 conectados
        for r in res[1:]:
            self.assertEqual(r.state(COOP)[2] & 0xFF0F, 0x8F0F)


# ---------------------------------------------------------------------------------------------------------------
# 2. K quadros de folga (`--link-delay`)
# ---------------------------------------------------------------------------------------------------------------
@need_slow
class TestDelayK(Base):
    FRAMES = 4501

    def run_k(self, k, **kw):
        return run_retry(lambda: Cluster(2, self.FRAMES, snaps=[4500], extra=['--link-delay', str(k)], **kw))

    def test_k_0_1_2_reach_coop(self):
        for k in (0, 1, 2):
            with self.subTest(K=k):
                self.assert_coop(self.run_k(k), 4500, 2)

    def test_k_3_and_4_break_the_game_glink(self):
        """MEDIDO (e documentado pela Nasus): K>=3 faz o atraso FILHO->HOST passar de ~3,5 quadros e o gLink do KatAM
        desiste: o co-op nunca comeca (0x0203AD10 != 2, link desligado). O runtime avisa no log."""
        for k in (3, 4):
            with self.subTest(K=k):
                res = self.run_k(k)
                for r in res:
                    self.assertEqual(r.rc, 0)
                    self.assertNotEqual(r.state(4500)[0], 2, 'K=%d nao deveria chegar ao co-op' % k)
                self.assertTrue(link_lines(res[0], 'above 2'), res[0].link_lines()[:4])

    def test_three_and_four_players_tolerate_only_k1(self):
        """ACHADO: o limite K<=2 vale SO para 2 jogadores. Com 3 ou 4 o gLink do KatAM desiste ja em K=2 (mesmo em
        loopback, sem rede): 0x0203AD10 != 2 e link desligado; K=1 funciona. O `auto` do runtime (limite 2) precisa
        usar 1 quando players >= 3."""
        for n in (3, 4):
            with self.subTest(players=n, K=1):
                res = run_retry(lambda: Cluster(n, COOP + 1, extra=['--link-delay', '1']))
                self.assert_coop(res, COOP, n)
            with self.subTest(players=n, K=2):
                res = run_retry(lambda: Cluster(n, COOP + 1, extra=['--link-delay', '2']))
                for r in res:
                    self.assertEqual(r.rc, 0)
                    self.assertNotEqual(r.state(COOP)[0], 2, '%d jogadores com K=2 nao deveria chegar ao co-op' % n)

    def test_host_hash_does_not_depend_on_the_network(self):
        """K=2: o hash do host e o mesmo sem proxy e com latencia + jitter (o fluxo depende so de K)."""
        a = run_retry(lambda: Cluster(2, COOP + 1, extra=['--link-delay', '2']))
        b = run_retry(lambda: Cluster(2, COOP + 1, extra=['--link-delay', '2'],
                                      proxies={1: dict(latency_ms=8, jitter_ms=8, seed=7)}), timeout=300)
        self.assertEqual(a[0].hash, b[0].hash)
        self.assert_coop(b, COOP, 2)


# ---------------------------------------------------------------------------------------------------------------
# 3. Latencia e jitter (TCP real atraves do proxy)
# ---------------------------------------------------------------------------------------------------------------
@need_slow
class TestLatency(Base):
    """MEDIDO (K=2, --link-nopace): nenhum ping causa erro; so a velocidade cai: periodo do quadro ~ (RTT+4 ms)/(K+1).
    Tempo real (16,7 ms/quadro) ate RTT ~45 ms. Co-op completo ate RTT 250 ms."""
    FRAMES = 300

    def measure(self, one_way, jitter=0, k=2):
        t = time.time()
        res = run_retry(lambda: Cluster(2, self.FRAMES, keys=['A@700+8'] * 2, snaps=[], peek=False,
                                        extra=['--link-delay', str(k)],
                                        proxies={1: dict(latency_ms=one_way, jitter_ms=jitter)}), timeout=300)
        return res, (time.time() - t) / self.FRAMES * 1000

    def test_rtt_20_60_120_250_table(self):
        table = []
        for one_way in (10, 30, 60, 125):
            res, mspf = self.measure(one_way)
            rtt = 2 * one_way
            table.append((rtt, round(mspf, 1)))
            for r in res:
                self.assertEqual(r.rc, 0, (rtt, r.err[-200:]))
            self.assert_consistent(res)
            model = (rtt + 4) / 3.0
            self.assertGreater(mspf, model * 0.5, (rtt, mspf))
            self.assertLess(mspf, model * 1.6 + 15, (rtt, mspf))
        print('\n[latencia K=2] RTT ms -> ms/quadro:', table)

    def test_jitter(self):
        res, mspf = self.measure(30, 30)
        for r in res:
            self.assertEqual(r.rc, 0)
        self.assert_consistent(res)
        self.assertLess(mspf, (2 * 60 + 4) / 3.0 * 1.6 + 20)

    def test_auto_delay_follows_rtt(self):
        for one_way, expect in ((0, 'K=0'), (30, 'K=2')):
            with self.subTest(one_way=one_way):
                res = run_retry(lambda: Cluster(2, 200, keys=['A@700+8'] * 2, snaps=[], peek=False,
                                                proxies={1: dict(latency_ms=one_way)} if one_way else {}))
                self.assertTrue(link_lines(res[0], 'input delay %s' % expect, '(auto)'), res[0].link_lines()[:5])

    def test_coop_with_20ms_and_250ms_rtt(self):
        for one_way in (10, 125):
            with self.subTest(one_way=one_way):
                res = run_retry(lambda: Cluster(2, COOP + 1, proxies={1: dict(latency_ms=one_way)}), timeout=600)
                self.assert_coop(res, COOP, 2)

    def test_four_players_with_20ms_rtt(self):
        """4 jogadores com 20 ms de RTT em cada join: K=1 (K=2 nao serve com 3-4 jogadores, ver TestDelayK)."""
        res = run_retry(lambda: Cluster(4, COOP + 1, extra=['--link-delay', '1'],
                                        proxies={i: dict(latency_ms=10) for i in (1, 2, 3)}), timeout=600)
        self.assert_coop(res, COOP, 4)


# ---------------------------------------------------------------------------------------------------------------
# 4. Perda, desconexao, travamento
# ---------------------------------------------------------------------------------------------------------------
def drive(c, action, delay_s=9.0, wait=120):
    """Inicia o cluster, espera `delay_s` (co-op rodando) e aplica `action(c)`; devolve os resultados."""
    c.start()
    time.sleep(delay_s)
    action(c)
    return c.results(c.wait(wait))


@need_slow
class TestDisconnect(Base):
    FRAMES = 9000
    ENV = {'KIRBY_LINK_TIMEOUT_MS': '3000'}      # esperas de 3 s em vez de 8 s: testes mais curtos

    def scenario(self, action, delay_s=9.0, wait=120, **kw):
        t = time.time()
        with Cluster(2, self.FRAMES, snaps=[], peek=False, proxies={1: {}}, env=self.ENV,
                     extra=['--link-delay', '2'], **kw) as c:
            res = drive(c, action, delay_s, wait)
        return res, time.time() - t

    def test_join_killed_host_keeps_running(self):
        res, dt = self.scenario(lambda c: c.procs[1][0].kill())
        self.assertEqual(res[0].rc, 0)
        self.assertTrue(link_lines(res[0], 'player 2 disconnected'), res[0].link_lines())
        self.assertLess(dt, 100)

    def test_host_killed_join_continues_without_cable(self):
        res, dt = self.scenario(lambda c: c.procs[0][0].kill())
        self.assertEqual(res[1].rc, 0)
        self.assertTrue(link_lines(res[1], 'host disconnected'), res[1].link_lines())
        self.assertLess(dt, 100)

    def test_connection_cut(self):
        res, dt = self.scenario(lambda c: c.proxies[1].cut())
        self.assertEqual([r.rc for r in res], [0, 0])
        self.assertLess(dt, 100)

    def test_blackhole_does_not_hang(self):
        """O proxy para de repassar SEM fechar (cabo desligado no meio, firewall): as esperas tem timeout
        (`KIRBY_LINK_TIMEOUT_MS`); os dois processos terminam sozinhos."""
        res, dt = self.scenario(lambda c: c.proxies[1].blackhole(), wait=150)
        self.assertEqual([r.rc for r in res], [0, 0])
        self.assertTrue(link_lines(res[0], 'player 2 disconnected'), res[0].link_lines())
        self.assertLess(dt, 140)

    def test_lost_message_ends_session_but_not_process(self):
        """MEDIDO: 1 mensagem perdida (so possivel no proxy; TCP nao perde) derruba a sessao com mensagem clara
        (`frame order error` no filho, `player 2 disconnected` no host) e os dois seguem sem cabo, sem travar."""
        with Cluster(2, 3001, snaps=[3000], proxies={1: dict(loss_pct=0.3, seed=5)}, extra=['--link-delay', '2'],
                     env=self.ENV) as c:
            res = c.run(200)
            dropped = c.proxies[1].dropped
        self.assertGreaterEqual(dropped, 1)
        self.assertEqual([r.rc for r in res], [0, 0])
        self.assertTrue(link_lines(res[0], 'disconnected') or link_lines(res[1], 'order error', 'disconnected'))

    def test_short_stall_is_survived(self):
        """Rede congelada por 3 s e volta: nada se perde (TCP); o jogo espera e termina consistente."""
        env = {'KIRBY_LINK_TIMEOUT_MS': '8000'}
        t = time.time()
        with Cluster(2, 4000, snaps=[], peek=False, proxies={1: {}}, env=env, extra=['--link-delay', '2']) as c:
            res = drive(c, lambda c: c.proxies[1].stall(3.0), 9.0, 150)
        self.assertEqual([r.rc for r in res], [0, 0])
        self.assert_consistent(res)
        self.assertFalse(link_lines(res[0], 'disconnected'))


# ---------------------------------------------------------------------------------------------------------------
# 5. HELLO invalido / versao / ROM diferente / sala cheia
# ---------------------------------------------------------------------------------------------------------------
def _spawn(c, i):
    import subprocess as sp
    out = open(os.path.join(c.tmp.name, 'o%d' % i), 'wb')
    err = open(os.path.join(c.tmp.name, 'e%d' % i), 'wb')
    p = sp.Popen(c._cmd(i), stdout=out, stderr=err, cwd=H.ROOT,
                 env=dict(os.environ, KIRBY_SIO_LOG=os.path.join(c.tmp.name, 'ex%d.log' % i), **c.env,
                          **c.per_instance_env.get(i, {})))
    c.procs.append((p, out, err, time.time()))
    if i == 0:
        c.started = time.time()


def start_host_only(c):
    _spawn(c, 0)


def start_join(c, i=1):
    _spawn(c, i)


@need_slow
class TestHello(Base):
    ENV = {'KIRBY_LINK_WAIT_S': '20'}      # o host/join desistem em 20 s se ninguem valido entrar

    def stray_then_join(self, payload):
        with Cluster(2, 3001, snaps=[3000], env=self.ENV) as c:
            start_host_only(c)
            time.sleep(1.5)
            s = socket.create_connection(('127.0.0.1', c.port), timeout=2)
            if payload:
                s.sendall(payload)
            time.sleep(0.5)
            s.close()
            start_join(c)
            return c.results(c.wait(90))

    def test_stray_connections_do_not_block_real_join(self):
        """Conexao que fecha sem HELLO, lixo, meia mensagem, tamanho absurdo: rejeitadas (`rejected a connection`) sem
        ocupar a vaga; o join de verdade entra e chega ao co-op."""
        for name, payload in (('fecha', b''), ('lixo', b'\xff' * 464), ('meia', b'\x01' * 100),
                              ('tamanho', b'\x01\xff\xff' + b'x' * 50)):
            with self.subTest(name):
                res = self.stray_then_join(payload)
                self.assertTrue(link_lines(res[0], 'rejected a connection'), res[0].link_lines()[:5])
                self.assert_coop(res, 3000, 2)

    def refused(self, res, why):
        self.assertTrue(link_lines(res[0], 'rejected a connection'), res[0].link_lines()[:6])
        self.assertTrue(link_lines(res[1], 'rejected this connection', why), res[1].link_lines()[:6])
        for r in res:
            self.assertEqual(r.rc, 0)                       # os dois terminam normalmente (o join roda sem cabo)
        self.assertTrue(link_lines(res[1], 'running without cable'))

    def test_wrong_protocol_version_is_refused(self):
        with Cluster(2, 1500, snaps=[], peek=False, env=self.ENV, per_instance_env={1: {'KIRBY_LINK_TEST_BADVER': '1'}}) as c:
            self.refused(c.run(90), 'protocol version')

    def test_different_rom_hash_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            alt = os.path.join(d, 'alt.gba')
            data = bytearray(open(H.ROM, 'rb').read())
            data[-4] ^= 0xFF                                  # 1 byte do arquivo: outro SHA-1
            with open(alt, 'wb') as f:
                f.write(data)
            with Cluster(2, 1500, snaps=[], peek=False, env=self.ENV) as c:
                start_host_only(c)
                time.sleep(1.5)
                c.rom = alt                                   # so o join usa a ROM alterada
                start_join(c)
                self.refused(c.results(c.wait(90)), 'different ROM')

    def test_room_full_third_join_not_accepted(self):
        with Cluster(3, 1500, snaps=[], peek=False, env=self.ENV) as c:
            c.n = 2
            start_host_only(c)
            time.sleep(1.5)
            start_join(c, 1)
            time.sleep(1.5)
            c.n = 3
            start_join(c, 2)
            res = c.results(c.wait(90))
        self.assertTrue(link_lines(res[2], 'could not join', 'rejected', 'running without cable'), res[2].link_lines())
        self.assertEqual([r.rc for r in res], [0, 0, 0])


# ---------------------------------------------------------------------------------------------------------------
# 6. Dessincronizacao
# ---------------------------------------------------------------------------------------------------------------
@need_slow
class TestDesync(Base):
    def test_injected_hash_fault_is_detected_on_both_sides(self):
        """KIRBY_LINK_FAULT=quadro (no filho) falsifica 1 bit do hash no ponto de controle (multiplo de 60)."""
        res = run_retry(lambda: Cluster(2, COOP + 1, extra=['--link-delay', '2'],
                                        per_instance_env={1: {'KIRBY_LINK_FAULT': '1200'}}))
        for r in res:
            self.assertEqual(r.rc, 0)
            self.assertTrue(link_lines(r, 'DESYNC'), (r.idx, r.link_lines()[:5]))

    def test_real_corruption_in_transit_is_detected(self):
        """MITM: o proxy inverte 1 bit dentro de um FRAME do host (fase gLink). O filho executa trocas diferentes: o hash
        do fluxo diverge no proximo ponto de controle (<= 60 quadros) e os DOIS lados registram `DESYNC`; os fluxos de
        trocas divergem de verdade (nao e so o aviso)."""
        with Cluster(2, COOP + 1, extra=['--link-delay', '2'], proxies={1: dict(corrupt_frame=300)}) as c:
            res = c.run()
            corrupted = c.proxies[1].corrupted
        self.assertTrue(corrupted, 'o proxy nao chegou a corromper (poucos FRAMEs grandes)')
        for r in res:
            self.assertEqual(r.rc, 0)
            self.assertTrue(link_lines(r, 'DESYNC'), (r.idx, r.link_lines()[:5]))
        ok, where = exchange_prefix_ok(res[0], res[1])
        self.assertFalse(ok, 'os fluxos de trocas deveriam divergir')

    def test_no_false_alarm_in_clean_runs(self):
        res = run_retry(lambda: Cluster(2, COOP + 1, extra=['--link-delay', '2'], proxies={1: dict(latency_ms=5, jitter_ms=5)}),
                        timeout=300)
        for r in res:
            self.assertFalse(link_lines(r, 'DESYNC'), r.link_lines()[:5])


if __name__ == '__main__':
    unittest.main()
