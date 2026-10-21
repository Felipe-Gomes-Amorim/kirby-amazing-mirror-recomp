"""Bancada de testes de REDE do link (Poppy): sobe N instancias reais do jogo (rt_boot --link host/join) em 127.0.0.1,
com um proxy TCP opcional entre cada join e o host para injetar latencia, jitter, perda de mensagem, travamento e
corte de conexao SEM depender de flags do runtime.

Uso tipico (veja tests/test_link_network.py):
    with Cluster(n=2, frames=3000) as c:
        res = c.run()
        res[0].hash, res[1].hash, res[0].peek(3000)[0x0203AD10] ...

Regras: logs vao para ARQUIVO (com pipe os processos travam por buffer cheio); todo processo tem timeout e e morto no
fim; as portas sao livres (bind em 0). Nada aqui toca a rede externa.
"""
import os
import random
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import link_trace as LT   # noqa: E402

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
DEFAULT_TRANSPORT = 'tcp'              # os testes UDP trocam isto em setUp (test_link_network.Base)
TRANSPORT_FLAG = '--link-transport'      # nome proposto; ajuste quando a Nasus publicar (P2d)
AVAILABLE = bool(EXE and os.path.exists(ROM))


def exe_has(text):
    """True se o executavel contem a string (ex.: '--link-latency'): detecta flags que a Nasus ainda vai criar."""
    if not EXE:
        return False
    with open(EXE, 'rb') as f:
        return text.encode() in f.read()


def udp_supported():
    return exe_has(TRANSPORT_FLAG) or exe_has('KIRBY_LINK_TRANSPORT')


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]



class Bursts:
    """Rajadas de perda: a cada `every_s` s (+-20%) a rede some por `length_ms` ms (Wi-Fi ruim: roaming, interferencia).
    Comeca `start_after_s` depois do primeiro trafego (para nao pegar a fase de menu)."""

    def __init__(self, every_s, length_ms, seed=11, start_after_s=0.0):
        self.every, self.length = every_s, length_ms / 1000.0
        self.rng = random.Random(seed)
        self.t0 = None
        self.start_after = start_after_s
        self.next = None
        self.count = 0

    def end_of_burst(self, now):
        """Se `now` esta dentro de uma rajada devolve o instante em que ela acaba; senao None."""
        if self.t0 is None:
            self.t0 = now
            self.next = now + self.start_after + self.every * (0.8 + 0.4 * self.rng.random())
        while now >= self.next + self.length:
            self.next += self.every * (0.8 + 0.4 * self.rng.random())
        if self.next <= now < self.next + self.length:
            if getattr(self, '_counted', None) != self.next:
                self._counted = self.next
                self.count += 1
            return self.next + self.length
        return None


def gap_stats(times, thresholds=(0.033, 0.100, 0.250)):
    """Paradas no fluxo host->filho: quantos intervalos entre entregas consecutivas passaram de cada limite (s), maior
    intervalo e entregas/s. `times` = instantes de entrega no proxy."""
    t = sorted(times)
    gaps = [b - a for a, b in zip(t, t[1:])]
    out = {'n': len(t), 'max_gap_ms': round(max(gaps) * 1000, 1) if gaps else 0.0}
    for th in thresholds:
        out['gt%d' % round(th * 1000)] = sum(1 for g in gaps if g > th)
    out['per_s'] = round(len(t) / (t[-1] - t[0]), 1) if len(t) > 1 and t[-1] > t[0] else 0.0
    return out


# ---------------------------------------------------------------------------------------------------------------
# Proxy TCP com defeitos de rede
# ---------------------------------------------------------------------------------------------------------------
class TcpProxy:
    """Fica entre UM join e o host. latency_ms/jitter_ms valem por mensagem e em CADA sentido (RTT = 2*latency);
    loss_pct descarta mensagens inteiras (protocolo v2 do rt_sio.c: [tipo u8][tamanho u16 LE][carga]); corrupt_frame=N inverte
    1 bit na carga do N-esimo FRAME (tipo 7) host->join (MITM: dessincronizacao REAL na rede);
    stall(s) congela o trafego por s segundos; cut() fecha as duas pontas; blackhole() para de repassar sem fechar."""

    def __init__(self, host_port, latency_ms=0.0, jitter_ms=0.0, loss_pct=0.0, seed=1, listen_port=None, corrupt_frame=None,
                 loss_model='drop', rto_ms=200.0, burst=None):
        self.host_port = host_port
        self.latency = latency_ms / 1000.0
        self.jitter = jitter_ms / 1000.0
        self.loss = loss_pct / 100.0
        self.rng = random.Random(seed)
        self.port = listen_port or free_port()
        self.corrupt_frame = corrupt_frame
        self.loss_model = loss_model          # 'drop' = mensagem some (mata a sessao); 'retransmit' = TCP real: atraso de RTO, ordem mantida
        self.rto = rto_ms / 1000.0
        self.bursts = Bursts(*burst) if burst else None
        self.delivered = {True: [], False: []}   # instantes de entrega: True = host->join
        self.frames_seen = 0
        self.corrupted = False
        self.stalled_until = 0.0
        self.blackholed = False
        self.dropped = 0
        self.forwarded = 0
        self._socks = []
        self._stop = False
        self._lsock = socket.socket()
        self._lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._lsock.bind(('127.0.0.1', self.port))
        self._lsock.listen(4)
        self._lsock.settimeout(0.2)
        self._thr = threading.Thread(target=self._accept_loop, daemon=True)
        self._thr.start()

    # ---- controles ----
    def stall(self, seconds):
        self.stalled_until = time.time() + seconds

    def blackhole(self):
        self.blackholed = True

    def cut(self):
        for s in self._socks:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                s.close()
            except OSError:
                pass

    def close(self):
        self._stop = True
        self.cut()
        try:
            self._lsock.close()
        except OSError:
            pass

    # ---- interno ----
    def _accept_loop(self):
        while not self._stop:
            try:
                c, _ = self._lsock.accept()
            except (socket.timeout, ConnectionResetError):      # Windows: ICMP 'porta fechada' vira ConnectionResetError
                continue
            except OSError:
                return
            try:
                h = socket.create_connection(('127.0.0.1', self.host_port), timeout=5)
            except OSError:
                c.close()
                continue
            for s in (c, h):
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._socks += [c, h]
            threading.Thread(target=self._pipe, args=(h, c, True), daemon=True).start()    # host -> join
            threading.Thread(target=self._pipe, args=(c, h, False), daemon=True).start()   # join -> host

    def _pipe(self, src, dst, from_host):
        import queue
        q = queue.Queue()
        done = object()

        def sender():
            last = 0.0
            while True:
                item = q.get()
                if item is done:
                    break
                due, data = item
                due = max(due, last)                      # TCP nao reordena
                last = due
                wait = due - time.time()
                if wait > 0:
                    time.sleep(wait)
                while time.time() < self.stalled_until and not self._stop:
                    time.sleep(0.01)
                if self.blackholed:
                    continue
                try:
                    dst.sendall(data)
                    self.forwarded += 1
                    self.delivered[from_host].append(time.time())
                except OSError:
                    break
            try:
                dst.shutdown(socket.SHUT_WR)
            except OSError:
                pass

        threading.Thread(target=sender, daemon=True).start()
        buf = b''
        while not self._stop:
            try:
                chunk = src.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 3:
                ln = buf[1] | buf[2] << 8
                if ln > 1024:                              # nao e protocolo v2: repassa cru
                    msg, buf = buf, b''
                elif len(buf) >= 3 + ln:
                    msg, buf = buf[:3 + ln], buf[3 + ln:]
                else:
                    break
                if len(msg) > 40 and msg[0] == 7 and from_host:          # FRAME grande (fase gLink) host -> join
                    self.frames_seen += 1
                    if self.corrupt_frame and self.frames_seen == self.corrupt_frame and len(msg) > 40:
                        m = bytearray(msg)
                        m[len(m) - 8] ^= 0x04                  # 1 bit dentro das trocas do quadro
                        msg = bytes(m)
                        self.corrupted = True
                now = time.time()
                d = self.latency + (self.rng.random() * self.jitter if self.jitter else 0.0)
                lost = bool(self.loss and self.rng.random() < self.loss)
                if self.bursts:
                    end = self.bursts.end_of_burst(now)
                    if end is not None:
                        lost = True
                        if self.loss_model == 'retransmit':
                            d = max(d, end - now)
                if lost:
                    self.dropped += 1
                    if self.loss_model == 'drop':
                        continue
                    d += self.rto                          # TCP: retransmissao depois do RTO; as seguintes esperam (ordem)
                q.put((now + d, msg))
        q.put(done)



class UdpProxy:
    """Proxy UDP entre UM join e o host, sem conhecer o formato dos datagramas. Defeitos (cada sentido): latency_ms,
    jitter_ms, loss_pct (aleatoria), burst=(a_cada_s, ms) (perda total em rajadas), reorder_pct (o datagrama recebe ate
    reorder_ms de atraso extra e pode ser ultrapassado), dup_pct (entrega 2x). Controles: stall(s) segura e libera
    depois; outage(s) descarta tudo por s segundos; cut() descarta para sempre (blackhole). Para segurança:
    corrupt_pct inverte 1 bit aleatorio de cada datagrama sorteado (nos dois sentidos).
    capture (primeiros `keep` datagramas join->host), replay(i), inject(data) = datagrama avulso ao host."""

    def __init__(self, host_port, latency_ms=0.0, jitter_ms=0.0, loss_pct=0.0, seed=1, listen_port=None,
                 burst=None, reorder_pct=0.0, reorder_ms=40.0, dup_pct=0.0, keep=400, corrupt_pct=0.0):
        import heapq
        self._hq = heapq
        self.host_port = host_port
        self.latency, self.jitter = latency_ms / 1000.0, jitter_ms / 1000.0
        self.loss, self.reorder, self.reorder_s, self.dup = loss_pct / 100.0, reorder_pct / 100.0, reorder_ms / 1000.0, dup_pct / 100.0
        self.bursts = Bursts(*burst) if burst else None
        self.corrupt = corrupt_pct / 100.0
        self.corrupted = 0
        self.rng = random.Random(seed)
        self.stalled_until = 0.0
        self.outage_until = 0.0
        self.cut_all = False
        self.dropped = self.forwarded = self.duplicated = self.reordered = 0
        self.delivered = {True: [], False: []}
        self.captured = []
        self.keep = keep
        self._clients = {}                                   # addr do join -> socket "upstream" ate o host
        self._heap, self._seq = [], 0
        self._cv = threading.Condition()
        self._stop = False
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(('127.0.0.1', listen_port or 0))
        self.port = self._sock.getsockname()[1]
        self._sock.settimeout(0.2)
        threading.Thread(target=self._rx_join, daemon=True).start()
        threading.Thread(target=self._scheduler, daemon=True).start()

    # ---- controles ----
    def stall(self, seconds):
        self.stalled_until = time.time() + seconds

    def outage(self, seconds):
        self.outage_until = time.time() + seconds

    def cut(self):
        self.cut_all = True

    blackhole = cut

    def replay(self, index):
        """Reenvia ao host (de um socket NOVO) o datagrama capturado de numero `index` (replay de datagrama antigo)."""
        self.inject(self.captured[index])

    def inject(self, data):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.sendto(data, ('127.0.0.1', self.host_port))

    def close(self):
        self._stop = True
        with self._cv:
            self._cv.notify_all()
        for u in list(self._clients.values()):
            try:
                u.close()
            except OSError:
                pass
        try:
            self._sock.close()
        except OSError:
            pass

    # ---- interno ----
    def _schedule(self, due, send, from_host):
        with self._cv:
            self._seq += 1
            self._hq.heappush(self._heap, (due, self._seq, send, from_host))
            self._cv.notify()

    def _fate(self, from_host):
        """Lista de atrasos de entrega (vazia = descartado)."""
        now = time.time()
        if self.cut_all or now < self.outage_until:
            self.dropped += 1
            return []
        if self.bursts and self.bursts.end_of_burst(now) is not None:
            self.dropped += 1
            return []
        if self.loss and self.rng.random() < self.loss:
            self.dropped += 1
            return []
        d = self.latency + (self.rng.random() * self.jitter if self.jitter else 0.0)
        if self.reorder and self.rng.random() < self.reorder:
            d += self.rng.random() * self.reorder_s
            self.reordered += 1
        out = [d]
        if self.dup and self.rng.random() < self.dup:
            out.append(d + self.rng.random() * 0.01)
            self.duplicated += 1
        return out

    def _maybe_corrupt(self, data):
        if self.corrupt and data and self.rng.random() < self.corrupt:
            b = bytearray(data)
            b[self.rng.randrange(len(b))] ^= 1 << self.rng.randrange(8)
            self.corrupted += 1
            return bytes(b)
        return data

    def _rx_join(self):
        while not self._stop:
            try:
                data, addr = self._sock.recvfrom(65535)
            except (socket.timeout, ConnectionResetError):      # Windows: ICMP 'porta fechada' vira ConnectionResetError
                continue
            except OSError:
                return
            up = self._clients.get(addr)
            if up is None:
                up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                up.bind(('127.0.0.1', 0))
                up.settimeout(0.2)
                self._clients[addr] = up
                threading.Thread(target=self._rx_host, args=(up, addr), daemon=True).start()
            if len(self.captured) < self.keep:
                self.captured.append(data)
            data = self._maybe_corrupt(data)
            for d in self._fate(False):
                self._schedule(time.time() + d, lambda up=up, data=data: up.sendto(data, ('127.0.0.1', self.host_port)), False)

    def _rx_host(self, up, addr):
        while not self._stop:
            try:
                data, _ = up.recvfrom(65535)
            except (socket.timeout, ConnectionResetError):      # Windows: ICMP 'porta fechada' vira ConnectionResetError
                continue
            except OSError:
                return
            data = self._maybe_corrupt(data)
            for d in self._fate(True):
                self._schedule(time.time() + d, lambda data=data, addr=addr: self._sock.sendto(data, addr), True)

    def _scheduler(self):
        while not self._stop:
            with self._cv:
                while not self._heap and not self._stop:
                    self._cv.wait(0.2)
                if self._stop:
                    return
                due = self._heap[0][0]
                now = time.time()
                if due > now:
                    self._cv.wait(min(0.05, due - now))
                    continue
                _, _, send, from_host = self._hq.heappop(self._heap)
            if time.time() < self.stalled_until:                 # stall: segura e libera junto
                self._schedule(self.stalled_until + self.rng.random() * 0.002, send, from_host)
                continue
            try:
                send()
                self.forwarded += 1
                self.delivered[from_host].append(time.time())
            except OSError:
                pass


# ---------------------------------------------------------------------------------------------------------------
# Cluster de instancias
# ---------------------------------------------------------------------------------------------------------------
class Result:
    def __init__(self, idx, rc, out, err, seconds):
        self.idx, self.rc, self.out, self.err, self.seconds = idx, rc, out, err, seconds
        self.rec = LT.parse_peek_log(err)
        self.exlog = []

    @property
    def hash(self):
        m = re.search(r'LINK exchange hash: (\w+) over (\d+) exchanges, (\d+) frames', self.out)
        return m.groups() if m else None

    def valid(self):
        """{slot: (validos, invalidos)} dos pacotes gLink vistos por esta instancia."""
        m = re.search(r'LINK packets.*', self.out)
        return {int(p): (int(v), int(i)) for p, v, i in re.findall(r'P(\d) valid=(\d+) invalid=(\d+)', m.group(0))} if m else {}

    def status(self):
        """JSON de `--link-status` desta instancia (ultima gravacao) ou {}."""
        import json
        try:
            with open(self.status_path, encoding='utf-8') as f:
                return json.load(f)
        except (OSError, ValueError, AttributeError):
            return {}

    def link_lines(self):
        return [ln for ln in self.err.splitlines() + self.out.splitlines() if ln.startswith('[link]')]

    def peek(self, frame):
        return self.rec.get(frame)

    def state(self, frame):
        """(sessao 0x0203AD10, jogadores 0x0203AD30, status 0x03002554, link 0x03002558, erro 0x02038580) no quadro."""
        d = self.rec.get(frame)
        if not d:
            return None
        return d.get(LT.SESSION), d.get(LT.NPLAYERS), d.get(LT.STATUS), d.get(LT.LINKON), d.get(LT.ERR)


class Cluster:
    """host (instancia 0) + n-1 joins. keys: lista (uma string --keys por instancia) ou None = roteiro do co-op.
    proxies: dict {indice_do_join: dict(latency_ms=..., jitter_ms=..., loss_pct=...)} -> o join passa por um TcpProxy."""

    def __init__(self, n=2, frames=3000, keys=None, env=None, extra=None, proxies=None, snaps=None, timeout=240,
                 stagger=1.0, rom=None, per_instance_extra=None, per_instance_env=None, bench=True, peek=True, join_frames=None,
                 transport=None, status=True):
        self.n, self.frames, self.env, self.extra = n, frames, env or {}, extra or []
        self.keys = keys or [LT.COOP_KEYS['host']] + [LT.COOP_KEYS['join']] * (n - 1)
        self.proxy_cfg = proxies or {}
        self.snaps = snaps if snaps is not None else [LT.COOP_FRAMES]
        self.timeout, self.stagger, self.rom = timeout, stagger, rom or ROM
        self.per_instance_extra = per_instance_extra or {}
        self.per_instance_env = per_instance_env or {}
        self.bench, self.peek, self.join_frames = bench, peek, join_frames
        self.transport, self.status = transport or DEFAULT_TRANSPORT, status
        self.proxies = {}
        self.procs = []
        self.tmp = None
        self.port = None
        self.started = None

    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.port = free_port()
        for i, cfg in self.proxy_cfg.items():
            self.proxies[i] = (UdpProxy if self.transport == 'udp' else TcpProxy)(self.port, **cfg)
        return self

    def __exit__(self, *a):
        for p in self.procs:
            if p[0].poll() is None:
                p[0].kill()
        for p in self.procs:
            try:
                p[0].wait(5)
            except Exception:
                pass
        for px in self.proxies.values():
            px.close()
        self.tmp.cleanup()

    def _cmd(self, i):
        d = self.tmp.name
        ini = os.path.join(d, 'empty.ini')
        open(ini, 'w').close()
        cmd = [EXE, self.rom, '--frames', str(self.join_frames if (i and self.join_frames) else self.frames), '--sav', 'none',
               '--mute', '--ini', ini, '--link-nopace', '--keys', self.keys[i], '--frames-dir', os.path.join(d, 'f%d' % i)]
        if self.bench:
            cmd.append('--bench')
        if self.snaps:
            cmd += ['--snap', ','.join(str(s) for s in self.snaps)]
        if self.peek:
            cmd += ['--peek', LT.peek_arg()]
        if i == 0:
            cmd += ['--link', 'host', '--link-players', str(self.n), '--link-port', str(self.port)]
        else:
            port = self.proxies[i].port if i in self.proxies else self.port
            cmd += ['--link', 'join', '--link-host', '127.0.0.1', '--link-port', str(port)]
        if self.status:
            cmd += ['--link-status', os.path.join(d, 'st%d.json' % i)]
        if self.transport == 'udp':
            cmd += [TRANSPORT_FLAG, 'udp']
        return cmd + self.extra + self.per_instance_extra.get(i, [])

    def start(self):
        self.started = time.time()
        for i in range(self.n):
            out = open(os.path.join(self.tmp.name, 'out%d.txt' % i), 'wb')
            err = open(os.path.join(self.tmp.name, 'err%d.txt' % i), 'wb')
            p = subprocess.Popen(self._cmd(i), stdout=out, stderr=err, cwd=ROOT,
                                 env=dict(os.environ, KIRBY_SIO_LOG=os.path.join(self.tmp.name, 'ex%d.log' % i), **self.env, **self.per_instance_env.get(i, {})))
            self.procs.append((p, out, err, time.time()))
            time.sleep(self.stagger)

    def wait(self, timeout=None):
        """Espera todos (ate `timeout` s); mata quem passar do prazo e marca rc = None."""
        deadline = time.time() + (timeout or self.timeout)
        res = []
        for i, (p, out, err, t0) in enumerate(self.procs):
            try:
                p.wait(max(0.1, deadline - time.time()))
                rc = p.returncode
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
                rc = None
            res.append(rc)
        return res

    def results(self, rcs):
        out = []
        for i, (p, fo, fe, t0) in enumerate(self.procs):
            fo.close()
            fe.close()
            with open(fo.name, 'rb') as f:
                o = f.read().decode('utf-8', 'replace')
            with open(fe.name, 'rb') as f:
                e = f.read().decode('utf-8', 'replace')
            r = Result(i, rcs[i], o, e, time.time() - self.started)
            r.status_path = os.path.join(self.tmp.name, 'st%d.json' % i)
            try:
                with open(os.path.join(self.tmp.name, 'ex%d.log' % i), encoding='ascii', errors='replace') as f:
                    r.exlog = f.read().splitlines()
            except OSError:
                r.exlog = []
            out.append(r)
        return out

    def run(self, timeout=None):
        self.start()
        return self.results(self.wait(timeout))


def exchange_prefix_ok(host, other, slack_lines=80):
    """O fluxo de trocas (`quadro ciclo s0 s1 s2 s3` por linha, KIRBY_SIO_LOG) de um filho tem que ser PREFIXO do do host
    (com K quadros de folga o host sai antes de o filho consumir os ultimos quadros). Devolve (ok, indice_da_divergencia)."""
    a, b = host.exlog, other.exlog
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return False, i
    return abs(len(a) - len(b)) <= slack_lines, n
