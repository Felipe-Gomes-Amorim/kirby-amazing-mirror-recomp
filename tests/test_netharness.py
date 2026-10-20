"""Testes offline dos proxies de rede (tests/netharness.py): sem jogo, com um eco UDP/TCP local (~5 s)."""
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netharness as H   # noqa: E402


class UdpEcho:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.got = []
        self.stop = False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.stop:
            try:
                d, a = self.sock.recvfrom(70000)
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                return
            self.got.append(d)
            try:
                self.sock.sendto(d, a)
            except OSError:
                pass

    def close(self):
        self.stop = True
        self.sock.close()


def send_numbers(px, n, gap=0.002, wait=0.6):
    c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    c.settimeout(0.05)
    for i in range(n):
        c.sendto(i.to_bytes(4, 'big'), ('127.0.0.1', px.port))
        time.sleep(gap)
    time.sleep(wait)
    back = []
    try:
        while True:
            back.append(int.from_bytes(c.recvfrom(100)[0], 'big'))
    except socket.timeout:
        pass
    c.close()
    return back


class TestUdpProxy(unittest.TestCase):
    def setUp(self):
        self.echo = UdpEcho()
        self.proxies = []

    def tearDown(self):
        for p in self.proxies:
            p.close()
        self.echo.close()

    def mk(self, **kw):
        p = H.UdpProxy(self.echo.port, **kw)
        self.proxies.append(p)
        return p

    def test_clean_passes_everything_in_order(self):
        px = self.mk()
        back = send_numbers(px, 100)
        self.assertEqual(back, list(range(100)))

    def test_loss_rate(self):
        px = self.mk(loss_pct=30, seed=4)
        back = send_numbers(px, 400)
        # perda em cada sentido: sobrevive ~ 0,7 * 0,7 = 49%
        self.assertTrue(120 < len(back) < 280, len(back))
        self.assertGreater(px.dropped, 100)

    def test_duplicate_and_reorder(self):
        px = self.mk(dup_pct=20, reorder_pct=40, reorder_ms=30, seed=2)
        back = send_numbers(px, 300)
        self.assertGreater(len(back), 300)                    # duplicados
        self.assertGreater(sum(1 for a, b in zip(back, back[1:]) if b < a), 10)   # fora de ordem
        self.assertGreater(px.duplicated, 20)
        self.assertGreater(px.reordered, 20)

    def test_latency_and_jitter(self):
        px = self.mk(latency_ms=40, jitter_ms=20)
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.settimeout(1)
        t = time.time()
        c.sendto(b'x', ('127.0.0.1', px.port))
        c.recvfrom(10)
        dt = time.time() - t
        self.assertGreater(dt, 0.075)                         # 2 x 40 ms
        self.assertLess(dt, 0.25)
        c.close()

    def test_stall_holds_then_releases(self):
        px = self.mk()
        send_numbers(px, 3)                                   # cria a sessao
        px.stall(0.5)
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.settimeout(0.15)
        c.sendto(b'hold', ('127.0.0.1', px.port))
        with self.assertRaises(socket.timeout):
            c.recvfrom(10)
        c.settimeout(1.5)
        self.assertEqual(c.recvfrom(10)[0], b'hold')          # liberado depois
        c.close()

    def test_outage_drops_and_cut_is_forever(self):
        px = self.mk()
        px.outage(0.4)
        back = send_numbers(px, 20, gap=0.005, wait=0.1)
        self.assertEqual(back, [])
        time.sleep(0.5)
        self.assertEqual(len(send_numbers(px, 10)), 10)       # voltou
        px.cut()
        self.assertEqual(send_numbers(px, 10), [])

    def test_burst_loss_windows(self):
        px = self.mk(burst=(0.3, 150), seed=1)
        back = send_numbers(px, 600, gap=0.005, wait=0.3)
        self.assertTrue(0 < len(back) < 600)
        self.assertGreaterEqual(px.bursts.count, 3)

    def test_capture_replay_inject(self):
        px = self.mk()
        send_numbers(px, 5)
        n = len(self.echo.got)
        px.replay(2)
        px.inject(b'\xff' * 3000)
        time.sleep(0.3)
        self.assertEqual(len(self.echo.got), n + 2)
        self.assertEqual(self.echo.got[-2], (2).to_bytes(4, 'big'))
        self.assertEqual(len(self.echo.got[-1]), 3000)

    def test_gap_stats(self):
        g = H.gap_stats([0.0, 0.01, 0.02, 0.2, 0.21, 0.6])
        self.assertEqual((g['gt33'], g['gt100'], g['gt250']), (2, 2, 1))
        self.assertEqual(g['max_gap_ms'], 390.0)


class TestBursts(unittest.TestCase):
    def test_periodic_windows(self):
        b = H.Bursts(1.0, 200, seed=3)
        inside = sum(1 for k in range(2000) if b.end_of_burst(100 + k * 0.01) is not None)
        self.assertTrue(250 < inside < 550, inside)            # ~20% do tempo
        self.assertGreaterEqual(b.count, 15)


class TestTcpProxyRetransmit(unittest.TestCase):
    def test_loss_becomes_delay_and_order_is_kept(self):
        srv = socket.socket()
        srv.bind(('127.0.0.1', 0))
        srv.listen(1)
        port = srv.getsockname()[1]
        got = []

        def serve():
            c, _ = srv.accept()
            buf = b''
            while True:
                d = c.recv(4096)
                if not d:
                    break
                buf += d
                while len(buf) >= 4:
                    got.append((buf[3], time.time()))
                    buf = buf[4:]
            c.close()

        threading.Thread(target=serve, daemon=True).start()
        px = H.TcpProxy(port, loss_pct=20, loss_model='retransmit', rto_ms=150, seed=5)
        c = socket.create_connection(('127.0.0.1', px.port))
        t0 = time.time()
        for i in range(40):
            c.sendall(bytes([9, 1, 0, i]))                     # [tipo][len u16][1 byte de carga]
            time.sleep(0.005)
        time.sleep(2.5)
        c.close()
        px.close()
        self.assertEqual([g[0] for g in got], list(range(40)))     # nenhuma perdida, ordem mantida
        self.assertGreater(px.dropped, 3)
        self.assertGreater(max(t for _, t in got) - t0, 0.25)      # a retransmissao custou tempo


class TestUdpTestHelpers(unittest.TestCase):
    """Pecas dos testes UDP que nao dependem do jogo (para nao descobrir bug de teste so quando o UDP existir)."""

    def test_silent_divergence_logic(self):
        import test_link_udp as U

        class R:
            def __init__(self, ex, lines):
                self.exlog, self._l, self.idx = ex, lines, 0

            def link_lines(self):
                return self._l

        a = R(['1', '2', '3'], [])
        self.assertFalse(U.silent_divergence([a, R(['1', '2'], [])]))                 # prefixo: ok
        self.assertTrue(U.silent_divergence([a, R(['1', 'X', '3'], [])]))             # diverge calado
        self.assertFalse(U.silent_divergence([a, R(['1', 'X', '3'], ['[link] DESYNC: x'])]))   # diverge e avisou

    def test_junk_and_blast(self):
        import test_link_udp as U
        echo = UdpEcho()
        try:
            junk = U.junk_datagrams()
            self.assertTrue(any(len(d) == 65507 for d in junk) and any(len(d) == 0 for d in junk))
            U.blast(echo.port, junk, repeat=1, gap=0.0)
            time.sleep(0.5)
            self.assertGreaterEqual(len(echo.got), len(junk) - 5)
        finally:
            echo.close()


if __name__ == '__main__':
    unittest.main()
