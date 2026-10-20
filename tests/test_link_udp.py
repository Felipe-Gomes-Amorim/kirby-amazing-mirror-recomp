"""Transporte UDP do link (Poppy): "Wi-Fi ruim" e robustez. So roda quando o rt_boot.exe tiver o transporte UDP
(`--link-transport udp`, P2d da Nasus; ajuste netharness.TRANSPORT_FLAG se o nome mudar). Lento: RECOMP_SLOW=1.

    set RECOMP_SLOW=1 & .venv\\Scripts\\python.exe -m unittest tests.test_link_udp -v

Os mesmos cenarios do TCP (2/4 jogadores, queda, HELLO, desync) rodam como `*UDP` em tests/test_link_network.py;
aqui ficam os que so fazem sentido em UDP: perda/jitter/reordenacao/duplicacao, rajadas, e a seguranca/robustez do
socket (datagramas lixo, replay, tamanho, sala cheia, sessao iniciada). A matriz TCP x UDP com numeros esta em
tests/netbench.py (NOTES, secao 15). Criterios: nenhum travamento, nenhum crash (rc 0), nenhum DESYNC silencioso
(fluxos divergentes SEM aviso e a falha mais grave).
"""
import os
import random
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netharness as H                                   # noqa: E402
from netharness import Cluster, LT, exchange_prefix_ok   # noqa: E402
import test_link_network as T                            # noqa: E402
from test_link_network import run_retry, link_lines, COOP, start_host_only, start_join   # noqa: E402

need_udp = unittest.skipUnless(H.AVAILABLE and H.udp_supported() and T.SLOW,
                               'RECOMP_SLOW=1 + rt_boot.exe com --link-transport udp (P2d)')


def silent_divergence(res):
    """Fluxos de trocas divergentes SEM nenhum `DESYNC` no log = falha grave (deteccao furou)."""
    for r in res[1:]:
        ok, _ = exchange_prefix_ok(res[0], r)
        if not ok and not (link_lines(res[0], 'DESYNC') or link_lines(r, 'DESYNC')):
            return True
    return False


class UdpBase(T.Base):
    TRANSPORT = 'udp'

    def assert_survives(self, res, frame=COOP, players=2):
        """Sessao inteira: todos rc 0, no co-op, fluxo consistente, sem DESYNC, sem queda de jogador."""
        self.assert_coop(res, frame, players)
        for r in res:
            self.assertFalse(link_lines(r, 'DESYNC'), (r.idx, link_lines(r, 'DESYNC')[:2]))
        self.assertFalse(link_lines(res[0], 'disconnected'), link_lines(res[0], 'disconnected')[:2])


# ---------------------------------------------------------------------------------------------------------------
# Wi-Fi ruim: o jogo tem que seguir ate onde o UDP aguenta; acima disso, falhar LIMPO
# ---------------------------------------------------------------------------------------------------------------
@need_udp
class TestUdpWifi(UdpBase):
    BASE = dict(latency_ms=15, jitter_ms=10)

    def run_px(self, players=2, timeout=900, **cfg):
        px = {i: dict(self.BASE, seed=7 + i, **cfg) for i in range(1, players)}
        return run_retry(lambda: Cluster(players, COOP + 1, proxies=px), timeout=timeout)

    def test_light_loss_1_and_3_percent_is_invisible(self):
        for loss in (1, 3):
            with self.subTest(loss=loss):
                self.assert_survives(self.run_px(loss_pct=loss))

    def test_loss_5_percent(self):
        self.assert_survives(self.run_px(loss_pct=5))

    def test_loss_10_percent_fails_clean_or_survives(self):
        """Pode nao ser jogavel; o que NAO pode: travar, crashar ou divergir calado. Registrado em netbench."""
        res = self.run_px(loss_pct=10)
        for r in res:
            self.assertEqual(r.rc, 0, (r.idx, r.err[-200:]))
        self.assertFalse(silent_divergence(res))

    def test_jitter_10_to_40_ms(self):
        for j in (10, 25, 40):
            with self.subTest(jitter=j):
                px = {1: dict(latency_ms=15, jitter_ms=j, seed=3)}
                self.assert_survives(run_retry(lambda: Cluster(2, COOP + 1, proxies=px), timeout=900))

    def test_reordering_and_duplication(self):
        self.assert_survives(self.run_px(reorder_pct=10, reorder_ms=40, dup_pct=5))

    def test_loss_bursts_100_and_300_ms(self):
        for ms in (100, 300):
            with self.subTest(burst_ms=ms):
                self.assert_survives(self.run_px(burst=(2.0, ms)))

    def test_vpn_profile_with_620_ms_spikes(self):
        """Perfil medido em Wi-Fi + VPN: RTT 12-42 ms e picos de 389/621 ms."""
        px = {1: dict(latency_ms=8, jitter_ms=15, burst=(10.0, 620), seed=5)}
        res = run_retry(lambda: Cluster(2, COOP + 1, proxies=px), timeout=900)
        for r in res:
            self.assertEqual(r.rc, 0)
        self.assertFalse(silent_divergence(res))

    def test_four_players_with_loss_and_jitter(self):
        self.assert_survives(self.run_px(players=4, loss_pct=2), players=4)

    def test_bit_flips_are_never_silent(self):
        """1% dos datagramas com 1 bit invertido (nos dois sentidos): ou o transporte descarta (checksum) e tudo segue,
        ou o hash acusa DESYNC; jamais divergir sem aviso."""
        with Cluster(2, COOP + 1, proxies={1: dict(latency_ms=5, corrupt_pct=1, seed=9)}) as c:
            res = c.run(900)
            flips = c.proxies[1].corrupted
        self.assertGreater(flips, 0)
        for r in res:
            self.assertEqual(r.rc, 0)
        self.assertFalse(silent_divergence(res))


# ---------------------------------------------------------------------------------------------------------------
# Seguranca / robustez do socket UDP
# ---------------------------------------------------------------------------------------------------------------
def junk_datagrams(seed=1):
    rng = random.Random(seed)
    out = [b'', b'\x00', b'\xff', b'KBRL', b'KBRL' + b'\x02' * 10, bytes(26), b'\xff' * 26]
    for n in (3, 27, 100, 512, 1200, 1472, 1500, 4096, 9000, 65000, 65507):          # inclusive acima do limite do protocolo
        out.append(bytes(rng.randrange(256) for _ in range(n)))
    out.append(b'\x07\xff\xff' + b'x' * 64)                                          # tamanho absurdo no cabecalho
    out.append(b'\x01\x1a\x00' + b'KBRL' + b'\x63\x00' + bytes(20))                  # HELLO plausivel, versao 99
    return out


def blast(port, datagrams, repeat=1, gap=0.001):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for _ in range(repeat):
            for d in datagrams:
                try:
                    s.sendto(d, ('127.0.0.1', port))
                except OSError:
                    pass                                   # 65507 pode falhar localmente: ok
                time.sleep(gap)
    finally:
        s.close()


@need_udp
class TestUdpRobustness(UdpBase):
    ENV = {'KIRBY_LINK_WAIT_S': '20', 'KIRBY_LINK_TIMEOUT_MS': '3000'}

    def test_junk_datagrams_while_waiting_and_during_the_game(self):
        """Lixo de todos os tamanhos ANTES do join e durante a partida: o host nao cai, o join entra, o co-op chega."""
        with Cluster(2, COOP + 1, env=self.ENV) as c:
            start_host_only(c)
            time.sleep(1.5)
            blast(c.port, junk_datagrams(), repeat=3)
            start_join(c)
            time.sleep(8)
            blast(c.port, junk_datagrams(2), repeat=5, gap=0.002)          # com a sessao rodando
            res = c.results(c.wait(300))
        self.assert_coop(res, COOP, 2)
        self.assertFalse(link_lines(res[0], 'DESYNC'))

    def test_replay_of_old_datagrams_does_not_disturb_the_session(self):
        """O proxy captura os datagramas do filho e, com a partida andando, REPETE antigos (inclusive o HELLO) de outra
        porta: nao pode derrubar, dessincronizar nem duplicar o jogador."""
        px = {1: dict(latency_ms=3, seed=2, keep=300)}
        with Cluster(2, COOP + 1, proxies=px, env=self.ENV) as c:
            c.start()
            time.sleep(10)
            p = c.proxies[1]
            n = len(p.captured)
            self.assertGreater(n, 50, 'nada capturado')
            for i in list(range(0, min(n, 300), 3)):
                p.replay(i)
                time.sleep(0.002)
            for i in range(0, min(n, 20)):                                   # os primeiros (HELLO/aperto de mao) de novo
                p.replay(i)
            res = c.results(c.wait(300))
        self.assert_coop(res, COOP, 2)
        for r in res:
            self.assertFalse(link_lines(r, 'DESYNC'))
        self.assertFalse(link_lines(res[0], 'player 2 disconnected'))

    def test_oversized_datagram(self):
        for n in (1473, 9000, 65507):
            with self.subTest(size=n):
                with Cluster(2, 1500, snaps=[], peek=False, env=self.ENV) as c:
                    start_host_only(c)
                    time.sleep(1.5)
                    blast(c.port, [b'\xab' * n], repeat=3, gap=0.01)
                    start_join(c)
                    res = c.results(c.wait(120))
                self.assertEqual([r.rc for r in res], [0, 0], [r.err[-200:] for r in res])
                self.assertTrue(link_lines(res[1], 'connected as player 2'), res[1].link_lines()[:5])

    def test_wrong_rom_and_version_refused_without_hang(self):
        for envk in ('KIRBY_LINK_TEST_BADROM', 'KIRBY_LINK_TEST_BADVER'):
            with self.subTest(envk):
                with Cluster(2, 1500, snaps=[], peek=False, env=self.ENV, per_instance_env={1: {envk: '1'}}) as c:
                    res = c.run(120)
                self.assertEqual([r.rc for r in res], [0, 0])
                self.assertTrue(link_lines(res[0], 'rejected'), res[0].link_lines()[:6])
                self.assertTrue(link_lines(res[1], 'rejected', 'running without cable', 'unavailable'), res[1].link_lines()[:6])

    def test_room_full_and_session_already_started(self):
        with Cluster(3, 2500, snaps=[], peek=False, env=self.ENV) as c:
            c.n = 2
            start_host_only(c)
            time.sleep(1.5)
            start_join(c, 1)
            time.sleep(1.5)
            c.n = 3
            start_join(c, 2)                                  # sala cheia (2 jogadores)
            time.sleep(20)
            c.n = 4
            c.keys = c.keys + [c.keys[1]]
            start_join(c, 3)                                  # sessao ja iniciada
            res = c.results(c.wait(150))
        self.assertEqual([r.rc for r in res], [0, 0, 0, 0])
        for r in res[2:]:
            self.assertTrue(link_lines(r, 'could not join', 'rejected', 'running without cable'), (r.idx, r.link_lines()[:5]))
        self.assertFalse(link_lines(res[0], 'player 3 connected'))

    def test_spoofed_second_client_with_same_hello_cannot_hijack(self):
        """Um terceiro envia o HELLO captado (replay) de OUTRA porta durante a sala: nao pode ocupar a vaga do join
        verdadeiro nem derrubar a sessao."""
        px = {1: dict(latency_ms=3, keep=50)}
        with Cluster(2, COOP + 1, proxies=px, env=self.ENV) as c:
            c.start()
            time.sleep(3)
            for i in range(min(10, len(c.proxies[1].captured))):
                c.proxies[1].replay(i)
            res = c.results(c.wait(300))
        self.assert_coop(res, COOP, 2)

    def test_host_vanishes_and_child_vanishes(self):
        for victim in (0, 1):
            with self.subTest(victim=victim):
                with Cluster(2, 9000, snaps=[], peek=False, env=self.ENV, proxies={1: {}}) as c:
                    res = T.drive(c, lambda c: c.procs[victim][0].kill(), 9.0, 120)
                survivor = res[1 - victim]
                self.assertEqual(survivor.rc, 0)
                self.assertTrue(link_lines(survivor, 'disconnected', 'timeout'), survivor.link_lines()[-4:])
                self.assertLess(survivor.seconds, 100)

    def test_total_outage_does_not_hang(self):
        """Rede some para sempre no meio: as esperas tem timeout e os dois terminam sozinhos (sem travar)."""
        with Cluster(2, 9000, snaps=[], peek=False, env=self.ENV, proxies={1: {}}) as c:
            res = T.drive(c, lambda c: c.proxies[1].cut(), 9.0, 150)
        self.assertEqual([r.rc for r in res], [0, 0])
        self.assertLess(max(r.seconds for r in res), 140)

    def test_short_outage_is_survived(self):
        """Corte de 1 s (menor que o timeout): a sessao segue e termina consistente."""
        env = dict(self.ENV, KIRBY_LINK_TIMEOUT_MS='8000')
        with Cluster(2, 4000, snaps=[], peek=False, env=env, proxies={1: {}}) as c:
            res = T.drive(c, lambda c: c.proxies[1].outage(1.0), 9.0, 200)
        self.assertEqual([r.rc for r in res], [0, 0])
        self.assertFalse(silent_divergence(res))


if __name__ == '__main__':
    unittest.main()
