"""Testes de aceite do protocolo de link do KatAM (Poppy). Sem rede: roda o codigo REAL da ROM no Unicorn
(tools/link_sim.py) ligado por um cabo multi-play simulado.

  .venv\\Scripts\\python.exe -m unittest tests.test_link_protocol -v

Pula sozinho se faltar unicorn, baserom.gba ou (so o teste de regressao) build/recomp/program/rt_boot.exe.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
ROM_PATH = os.path.join(ROOT, 'baserom.gba')
BOOT_EXE = os.path.join(ROOT, 'build', 'recomp', 'program', 'rt_boot.exe')

try:
    import link_sim as L
    HAVE_SIM = os.path.exists(ROM_PATH)
except ImportError:                                  # sem unicorn
    L = None
    HAVE_SIM = False

_ROM = None


def rom():
    global _ROM
    if _ROM is None:
        _ROM = L.load_rom(ROM_PATH)
    return _ROM


def payload(frame, unit):
    """20 bytes diferentes por quadro e por unidade (nunca todos zero)."""
    return bytes(((frame * 13 + unit * 37 + i * 7) & 0xFF) | 1 for i in range(20))


def packets(session, unit):
    """Decodifica o que a unidade `unit` colocou no cabo: lista de pacotes = 14 halfwords depois de cada 0xFEFE.
    (hw0..hw13). Cada pacote completo tem 1 troca 0xFEFE + 14 trocas = 15 trocas."""
    stream = [e['sent'][unit] for e in session.cable.log]
    out = []
    i = 0
    while i < len(stream):
        if stream[i] == L.FEFE and i + 1 < len(stream) and stream[i + 1] == L.FEFE:
            i += 1                                   # filho: o 0xFEFE aparece repetido (atraso de 1 troca); vale o ultimo
        elif stream[i] == L.FEFE:
            body = stream[i + 1:i + 15]
            if len(body) == 14:
                out.append((i, body))
            i += 15
        else:
            i += 1
    return out


def free_run(n_units, frames, present=None, mid=None):
    s = L.Session(rom(), n_units, present)
    for f in range(frames):
        if mid:
            mid(s, f)
        s.link_frame([payload(f, u) for u in range(n_units)])
    return s


@unittest.skipUnless(HAVE_SIM, 'precisa de unicorn (.venv) e baserom.gba')
class TestPacketFormat(unittest.TestCase):
    def test_sum_is_0xFFF1_in_every_sent_packet(self):
        for n in (2, 3, 4):
            s = free_run(n, 12)
            for u in range(n):
                pk = packets(s, u)
                self.assertGreaterEqual(len(pk), 8, 'n=%d unidade %d' % (n, u))
                for pos, body in pk:
                    self.assertEqual(sum(body[:12]) & 0xFFFF, 0xFFF1, 'n=%d u=%d pos=%d %s' % (n, u, pos, body))

    def test_exchange_order_and_layout(self):
        s = free_run(2, 10)
        for u in (0, 1):
            pk = packets(s, u)
            self.assertGreaterEqual(len(pk), 6)
            frames = []
            for pos, body in pk:
                hw0, hw1 = body[0], body[1]
                frames.append(hw0 & 0xFF)
                self.assertEqual(body[12], 0, 'hw12 deve ser 0')
                self.assertEqual(body[13], 0, 'hw13 deve ser 0')
                # hw1 = ~(soma de hw0..hw11 com hw1=0) - 14  (equivale a soma total == 0xFFF1)
                rest = (body[0] + sum(body[2:12])) & 0xFFFF
                self.assertEqual(hw1, (~rest - 14) & 0xFFFF)
            # o contador de quadro (byte baixo de hw0) sobe de 1 em 1 entre pacotes consecutivos
            for a, b in zip(frames, frames[1:]):
                self.assertEqual((b - a) & 0xFF, 1, frames)

    def test_payload_goes_in_hw2_to_hw11(self):
        s = L.Session(rom(), 2)
        data = bytes(range(1, 21))
        for _ in range(4):
            s.link_frame([data, data])
        found = [body for _, body in packets(s, 0) if body[2:12] == [data[i] | data[i + 1] << 8 for i in range(0, 20, 2)]]
        self.assertTrue(found, 'o payload de 20 bytes nao apareceu em hw2..hw11')

    def test_fifteen_exchanges_per_packet(self):
        s = free_run(2, 10)
        stream = [e['sent'][0] for e in s.cable.log]
        starts = [i for i, v in enumerate(stream) if v == L.FEFE]
        # pacote = 0xFEFE + 14 trocas (15); depois vem 1 troca ociosa (0x0000) ate o proximo 0xFEFE: 16 por quadro
        self.assertGreaterEqual(len(starts), 6)
        for a, b in zip(starts, starts[1:]):
            self.assertEqual(b - a, 16)
            self.assertEqual(stream[b - 1], 0)       # troca ociosa
            self.assertEqual(stream[a + 13:a + 15], [0, 0])   # hw12, hw13

    def test_both_sides_receive_same_data(self):
        s = free_run(3, 8)
        for e in s.cable.log:
            self.assertEqual(e['sent'], e['got'])
        # o mestre recebeu algo do filho 1 (area de recepcao: 20 bytes por jogador)
        recv = s.units[0].rb(L.G_RECV, 80)
        self.assertNotEqual(recv[20:40], bytes(20))


@unittest.skipUnless(HAVE_SIM, 'precisa de unicorn (.venv) e baserom.gba')
class TestAbsentPlayer(unittest.TestCase):
    def test_empty_slots_read_0xFFFF(self):
        s = free_run(2, 6)                           # slots 2 e 3 sem console
        for e in s.cable.log:
            self.assertEqual(e['sent'][2], 0xFFFF)
            self.assertEqual(e['sent'][3], 0xFFFF)

    def test_unplug_midrun_discards_packet(self):
        def mid(s, f):
            if f == 4:
                s.cable.present[2] = False
        s = L.Session(rom(), 3)
        for f in range(4):
            s.link_frame([payload(f, u) for u in range(3)])
        before = s.units[0].rb(L.G_RECV, 80)
        self.assertEqual(s.status[0] & 0x7, 7, 'os 3 jogadores deviam estar conectados')
        s.cable.present[2] = False
        for f in range(4, 10):
            s.link_frame([payload(f, u) for u in range(3)])
        after = s.units[0].rb(L.G_RECV, 80)
        # jogador 2: slot do cabo vira 0xFFFF em todas as trocas, bit de "conectado agora" some do status
        for e in s.cable.log[-30:]:
            self.assertEqual(e['sent'][2], 0xFFFF)
        self.assertEqual(s.status[0] & 0x4, 0, 'status ainda marca o jogador 2')
        self.assertEqual(s.status[1] & 0x4, 0)
        self.assertEqual(s.status[0] & 0x3, 3)
        # pacote descartado: a copia do jogador 2 NAO foi atualizada (continua igual a antes da queda
        # ou ao ultimo pacote bom), enquanto o jogador 1 continuou atualizando
        self.assertEqual(after[40:60], s.units[0].rb(L.G_RECV + 40, 20))
        s.link_frame([payload(99, u) for u in range(3)])
        self.assertEqual(s.units[0].rb(L.G_RECV + 40, 20), after[40:60])
        self.assertNotEqual(s.units[0].rb(L.G_RECV + 20, 20), before[20:40])

    def test_absent_child_in_two_player_game_is_not_connected(self):
        s = L.Session(rom(), 2, present=[True, False])   # master sozinho: sem SD
        for f in range(6):
            s.link_frame([payload(f, 0), payload(f, 1)])
        self.assertEqual(s.status[0] & 0x2, 0)


def _lockstep(n=2, check=False, warm=8):
    s = L.Session(rom(), n)
    s.start_lockstep(0x5A, check)
    for _ in range(warm):                       # as primeiras trocas ainda nao tem pacote valido (erro 3/6 de aquecimento)
        s.link_frame()
    for u in s.units:
        u.w32(L.LS_ERR, 0)
        u.w16(L.LS + 4, u.r16(L.LS + 4) & 0x7FFF)
    return s


def _run(s, frames, tamper=None, unplug=None):
    first = None
    n = len(s.units)
    for f in range(frames):
        if unplug and f == unplug[0]:
            s.cable.present[unplug[1]] = False
        s.link_frame(tamper=(lambda i, u, f=f: tamper(f, i, u)) if tamper else None)
        if first is None:
            codes = [s.error_code(i) for i in range(n)]
            if any(codes):
                first = (f, codes)
    return first


@unittest.skipUnless(HAVE_SIM, 'precisa de unicorn (.venv) e baserom.gba')
class TestLockstepErrors(unittest.TestCase):
    """Cenarios de erro de LinkFrameSync (0x08030FE0): codigo em 0x02038580."""

    def test_baseline_no_error_and_frames_advance(self):
        s = _lockstep()
        self.assertIsNone(_run(s, 30))
        self.assertEqual(s.game_frame(0), s.game_frame(1))
        self.assertGreaterEqual(s.game_frame(0), 30)

    def test_error3_bad_packet_type(self):
        s = _lockstep()
        first = _run(s, 30, tamper=lambda f, i, u: u.w8(L.G_SEND, 0x21) if (f == 10 and i == 1) else None)
        self.assertEqual(first[1][0], 3)             # o mestre ve o tipo errado do filho

    def test_error4_session_mismatch(self):
        s = _lockstep()
        s.units[1].w8(L.LS + 6, 0x77)                # o filho acha que a sessao e outra
        first = _run(s, 10)
        self.assertEqual(first[1][1], 4)

    def test_error6_peer_lost(self):
        s = _lockstep()
        first = _run(s, 30, unplug=(10, 1))
        self.assertEqual(first[1][0], 6)
        self.assertLess(s.game_frame(0) - 11, 8, 'o jogo devia parar de avancar')

    def test_error7_stalled_needs_target_depth(self):
        s = _lockstep()
        for u in s.units:
            for p in range(2):
                u.w8(L.LS + 0x2A0 + p, 2)            # "profundidade alvo" C >= 2: liga a espera (bit2)
        first = _run(s, 60, unplug=(10, 1))
        self.assertEqual(first[1][0], 7)

    def test_error8_state_checksum_mismatch(self):
        s = _lockstep(check=True)
        first = _run(s, 30, tamper=lambda f, i, u: u.w8(L.G_SEND + 3, u.r8(L.G_SEND + 3) ^ 0x0C) if (f == 10 and i == 1) else None)
        self.assertEqual(first[1][0], 8)

    def test_error8_not_raised_when_checksums_match(self):
        s = _lockstep(check=True)
        self.assertIsNone(_run(s, 30))

    def test_error5_not_reachable_from_framesync(self):
        """Erro 5 vem de 0x08030D4C (busca de quadro N-5 no anel), chamada so de 0x0801AA04/0x08020140/0x08032AD4/
        0x08032DE4 (lobby/sessao), nao de LinkFrameSync: apagar o anel ou saltar o quadro nao o dispara."""
        s = _lockstep()

        def wipe(f, i, u):
            if f == 10 and i == 0:
                for p in range(2):
                    u.wb(L.LS + 0xA0 + 128 * p, bytes(128))
        self.assertIsNone(_run(s, 30, tamper=wipe))


@unittest.skipUnless(os.path.exists(BOOT_EXE) and os.path.exists(ROM_PATH), 'precisa de rt_boot.exe e baserom.gba')
class TestLinkOffRegression(unittest.TestCase):
    """Sem rede: o quadro N com link off/solo tem que ser identico ao do build sem flag nenhuma."""

    def shot(self, *extra):
        with tempfile.TemporaryDirectory() as d:
            ppm = os.path.join(d, 'a.ppm')
            r = subprocess.run([BOOT_EXE, ROM_PATH, '--frames', '400', '--ppm', ppm, '--sav', 'none', '--mute', *extra],
                               capture_output=True, timeout=120, cwd=ROOT)
            self.assertEqual(r.returncode, 0, r.stderr[-300:])
            with open(ppm, 'rb') as f:
                data = f.read()
        self.assertGreater(len(data), 1000)
        return hashlib.md5(data).hexdigest()

    def test_link_off_and_solo_match_default(self):
        base = self.shot()
        self.assertEqual(self.shot('--link', 'off'), base)
        self.assertEqual(self.shot('--link', 'solo'), base)

    def test_deterministic(self):
        self.assertEqual(self.shot(), self.shot())


# ----------------------------------------------------------------------------------------------------------------
# Decodificador de tracos (tools/link_trace.py) - offline, so precisa do Python
# ----------------------------------------------------------------------------------------------------------------
import link_trace as LT   # noqa: E402  (tools/ ja esta no sys.path)

# bytes reais capturados do host no menu -> co-op (quadros 1718, 1745/1746, 2017 do roteiro COOP_KEYS)
REAL_LOBBY = bytes.fromhex('020123010000ff03b31e14080100200000000000')
REAL_KEYS = bytes.fromhex('2002000000000000000000000000000001000000')
REAL_T3 = bytes.fromhex('0302000000000000000000000000000010010000')


def peek_line(frame, send=None, recv=None, **extra):
    words = {}
    for base, data, n in ((LT.SEND, send, 5), (LT.RECV, recv, 20)):
        data = (data or b'').ljust(4 * n, b'\0')
        for i in range(n):
            words[base + 4 * i] = int.from_bytes(data[4 * i:4 * i + 4], 'little')
    words.update({LT.STATUS: 0, LT.FLAGS: 0, LT.LINKON: 1, LT.SESSION: 0, LT.NPLAYERS: 2})
    words.update(extra)
    return '[peek] frame=%d %s' % (frame, ' '.join('%08X=%08X' % (a, v) for a, v in words.items()))


class TestTraceDecoder(unittest.TestCase):
    def test_peek_arg_fits_runtime_limit(self):
        addrs = LT.peek_arg().split(',')
        self.assertLessEqual(len(addrs), 32)             # rt_script.c: peeks[32]
        self.assertEqual(len(set(addrs)), len(addrs))

    def test_lobby_packet(self):
        d = LT.decode_packet(REAL_LOBBY)
        self.assertEqual((d['kind'], d['version'], d['ack'], d['phase'], d['phase_name']), ('lobby', 1, 1, 0x20, 'hello'))
        self.assertEqual(d['profile'], '0000ff03b31e1408')

    def test_lobby_phase_codes(self):
        for code, name in ((0x20, 'hello'), (0x22, 'host-ready'), (0x40, 'child-hello'), (0x41, 'child-ack'), (0x42, 'child-ready')):
            b = bytearray(REAL_LOBBY)
            b[14] = code
            self.assertEqual(LT.decode_packet(bytes(b))['phase_name'], name)

    def test_keys_packet(self):
        d = LT.decode_packet(REAL_KEYS)
        self.assertEqual((d['kind'], d['session'], d['frame']), ('keys', 2, 1))
        self.assertEqual(d['inputs'], [0] * 8)

    def test_keys_packet_inputs_are_12_bit_newest_first(self):
        b = bytearray(20)
        b[0], b[1] = 0x20, 5
        vals = [0x3FE, 0x001, 0xABC, 0x000, 0x123, 0x456, 0x789, 0xFFF]
        acc = 0
        for i, v in enumerate(vals):
            acc |= v << (12 * i)
        b[2:14] = acc.to_bytes(12, 'little')
        b[16:20] = (77).to_bytes(4, 'little')
        d = LT.decode_packet(bytes(b))
        self.assertEqual(d['inputs'], vals)
        self.assertEqual(d['frame'], 77)

    def test_other_and_empty(self):
        self.assertEqual(LT.decode_packet(REAL_T3)['kind'], 'other')
        self.assertEqual(LT.decode_packet(bytes(20))['kind'], 'empty')
        self.assertEqual(LT.decode_packet(None)['kind'], 'none')

    def test_parse_ignores_noise_and_truncated_pairs(self):
        text = 'SWI 0x06 x5\n[snap] frame=1 foo\n[peek] frame=3 030036B0=00000001 030036B4=0000\n[peek] frame=4 030036B0=0000000A\n'
        rec = LT.parse_peek_log(text)
        self.assertEqual(sorted(rec), [3, 4])
        self.assertEqual(rec[4][LT.SEND], 10)
        self.assertEqual(rec[3][LT.SEND], 1)

    def test_events_lobby_sequence(self):
        def lobby(phase, ack):
            b = bytearray(REAL_LOBBY)
            b[12], b[14] = ack, phase
            return bytes(b)
        frames = [(1, lobby(0x20, 1)), (2, lobby(0x20, 1)), (3, lobby(0x20, 3)), (4, lobby(0x22, 0x13)), (5, REAL_KEYS)]
        text = '\n'.join(peek_line(f, send=b) for f, b in frames)
        ev = [t for _, t in LT.events(LT.parse_peek_log(text)) if t.startswith('envia')]
        self.assertEqual(len(ev), 4)                      # o quadro repetido (2) nao gera evento
        self.assertIn('fase=0x20', ev[0])
        self.assertIn('ack=0003', ev[1])
        self.assertIn('fase=0x22', ev[2])
        self.assertTrue(ev[3].startswith('envia      KEYS sessao=02'))

    def test_coop_script_shape(self):
        for who in ('host', 'join'):
            self.assertTrue(LT.COOP_KEYS[who].startswith(LT.COOP_KEYS_COMMON))
        self.assertIn('A@1700+8', LT.COOP_KEYS['host'])
        self.assertNotIn('A@1700+8', LT.COOP_KEYS['join'])
        self.assertLess(len(LT.COOP_KEYS['host']), 1023)   # o rt_boot aceita ate 128 eventos / 1023 caracteres


# ----------------------------------------------------------------------------------------------------------------
# Duas instancias reais ate o co-op (sem rede externa: 127.0.0.1)
# ----------------------------------------------------------------------------------------------------------------
import socket   # noqa: E402
import time     # noqa: E402


def _free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


@unittest.skipUnless(os.path.exists(BOOT_EXE) and os.path.exists(ROM_PATH), 'precisa de rt_boot.exe e baserom.gba')
class TestTwoInstancesCoop(unittest.TestCase):
    """Roteiro COOP_KEYS leva host e join do titulo ate o co-op, medido pelo --peek. ~10 s, deterministico."""

    @classmethod
    def setUpClass(cls):
        port = _free_port()
        window = ','.join(str(f) for f in range(1700, 1770)) + ',2020,2400,%d' % LT.COOP_FRAMES
        cls.logs = {}
        procs, files = {}, {}
        with tempfile.TemporaryDirectory() as d:
            common = [BOOT_EXE, ROM_PATH, '--frames', str(LT.COOP_FRAMES + 1), '--sav', 'none', '--mute',
                      '--snap', window, '--peek', LT.peek_arg()]
            extra = {'host': ['--link', 'host', '--link-players', '2', '--link-port', str(port)],
                     'join': ['--link', 'join', '--link-host', '127.0.0.1', '--link-port', str(port)]}
            for who in ('host', 'join'):
                files[who] = open(os.path.join(d, who + '.log'), 'wb')       # arquivo, nao pipe: os dois logs sao grandes
                procs[who] = subprocess.Popen(common + extra[who] + ['--keys', LT.COOP_KEYS[who], '--frames-dir', os.path.join(d, who)],
                                              stdout=subprocess.DEVNULL, stderr=files[who], cwd=ROOT)
                time.sleep(1)
            for who, p in procs.items():
                try:
                    p.wait(timeout=150)
                    note = ''
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
                    note = 'TIMEOUT\n'
                files[who].close()
                with open(os.path.join(d, who + '.log'), 'rb') as fh:
                    cls.logs[who] = note + fh.read().decode('utf-8', 'replace')
        cls.rec = {who: LT.parse_peek_log(txt) for who, txt in cls.logs.items()}

    def test_both_reach_coop_session(self):
        for who, status in (('host', 0x8393), ('join', 0x8303)):
            self.assertNotIn('TIMEOUT', self.logs[who][:20], who)
            self.assertIn(LT.COOP_FRAMES, self.rec[who], who)
            d = self.rec[who][LT.COOP_FRAMES]
            self.assertEqual(d[LT.SESSION], 2, who)            # modo "jogo em link"
            self.assertEqual(d[LT.NPLAYERS], 2, who)
            self.assertEqual(d[LT.LINKON], 1, who)
            self.assertEqual(d[LT.STATUS], status, who)        # bit15 = sincronizado; 0x80 = mestre so no host

    def _send_phases(self, who):
        out = []
        for fr in sorted(self.rec[who]):
            p = LT.decode_packet(LT.words_to_bytes(self.rec[who][fr], LT.SEND, 5))
            if p['kind'] == 'lobby' and (not out or out[-1] != p['phase']):
                out.append(p['phase'])
        return out

    def test_lobby_phase_order(self):
        host = self._send_phases('host')
        join = self._send_phases('join')
        # host: idle(0x00) -> hello(0x20) -> host-ready(0x22); join: idle -> child-hello(0x40) -> child-ack(0x41) -> child-ready(0x42)
        self.assertEqual([p for p in host if p in (0x20, 0x22)][:2], [0x20, 0x22], host)
        for want in (0x40, 0x41, 0x42):
            self.assertIn(want, join, join)
        self.assertLess(join.index(0x40), join.index(0x41))
        self.assertLess(join.index(0x41), join.index(0x42))

    def test_keys_start_with_session_byte_2_and_frames_count_up(self):
        seen = []
        for fr in sorted(self.rec['host']):
            if fr >= 1770:
                break
            p = LT.decode_packet(LT.words_to_bytes(self.rec['host'][fr], LT.SEND, 5))
            if p['kind'] == 'keys':
                seen.append((p['session'], p['frame']))
        self.assertTrue(seen, 'nenhum pacote de teclas no host entre 1700 e 1769')
        self.assertEqual({s for s, _ in seen}, {2})
        frames = [f for _, f in seen]
        self.assertEqual(frames, sorted(frames))
        self.assertEqual(frames[0], 0)


if __name__ == '__main__':
    unittest.main()
