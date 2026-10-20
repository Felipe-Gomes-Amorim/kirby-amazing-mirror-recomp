"""Matriz "Wi-Fi ruim" TCP x UDP (Poppy): roda o co-op (host + joins reais em 127.0.0.1) atraves dos proxies de
tests/netharness.py e imprime uma tabela por cenario: quadros/s, paradas do fluxo host->filho > 33/100/250 ms, erros.

    .venv\\Scripts\\python.exe tests\\netbench.py                       # todos os cenarios, so os transportes disponiveis
    .venv\\Scripts\\python.exe tests\\netbench.py --transports tcp,udp --scen loss3,burst300 --players 2 --frames 3001
    .venv\\Scripts\\python.exe tests\\netbench.py --out build\\netbench.json

TCP = o proxy simula TCP de verdade (perda vira atraso de retransmissao RTO 200 ms com bloqueio de cabeca de fila);
UDP = perda real (o transporte tem que se virar). Como o teste roda sem pacing (--link-nopace), "q/s" e a velocidade
BRUTA do par sob a rede (60 q/s = tempo real) e "paradas" sao as lacunas na ENTREGA ao filho medidas no proxy.
Metricas do proprio runtime (--link-status: RTT p95/p99, paradas) entram na tabela quando existirem no JSON.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netharness as H   # noqa: E402
from netharness import Cluster, LT, gap_stats   # noqa: E402

BASE = dict(latency_ms=15, jitter_ms=10)          # RTT ~30 ms: Wi-Fi bom

# nome -> parametros do proxy (comuns a TCP/UDP; burst = (a_cada_s, ms))
SCENARIOS = {
    'clean':    dict(BASE),
    'loss1':    dict(BASE, loss_pct=1),
    'loss3':    dict(BASE, loss_pct=3),
    'loss5':    dict(BASE, loss_pct=5),
    'loss10':   dict(BASE, loss_pct=10),
    'jitter10': dict(latency_ms=15, jitter_ms=10),
    'jitter25': dict(latency_ms=15, jitter_ms=25),
    'jitter40': dict(latency_ms=15, jitter_ms=40),
    'burst100': dict(BASE, burst=(2.0, 100)),
    'burst300': dict(BASE, burst=(2.0, 300)),
    # perfil medido por voce (Wi-Fi + VPN): RTT 12-42 ms e picos de 389/621 ms
    'real':     dict(latency_ms=8, jitter_ms=15, burst=(15.0, 400)),
    'real_x':   dict(latency_ms=8, jitter_ms=15, loss_pct=2, burst=(10.0, 620)),
}
UDP_ONLY = dict(reorder_pct=5, reorder_ms=30, dup_pct=2)       # adicionados quando transport=udp ('udp_chaos')
SCENARIOS['udp_chaos'] = dict(BASE, loss_pct=3, **UDP_ONLY)


def proxy_cfg(name, transport):
    cfg = dict(SCENARIOS[name])
    if transport == 'tcp':
        for k in UDP_ONLY:
            cfg.pop(k, None)
        cfg['loss_model'] = 'retransmit'
    return cfg


def run_one(name, transport, players=2, frames=LT.COOP_FRAMES + 1, timeout=900, extra=None, seed=3):
    cfg = proxy_cfg(name, transport)
    cfg['seed'] = seed
    proxies = {i: dict(cfg, seed=seed + i) for i in range(1, players)}
    for attempt in range(3):          # rc=1 sem relatorio = processo morto de fora (relink do rt_boot.exe em outro chat)
        t = time.time()
        with Cluster(players, frames, proxies=proxies, transport=transport, extra=extra or [], timeout=timeout) as c:
            res = c.run()
            wall = time.time() - t
            js = res[0].status()                  # le o JSON antes de a pasta temporaria sumir
            if any(r.rc == 1 and r.hash is None for r in res) and attempt < 2:
                continue
            break
    if True:
        stalls = {}
        dropped = 0
        for i, px in c.proxies.items():
            g = gap_stats(px.delivered[True])
            for k, v in g.items():
                if k == 'max_gap_ms':
                    stalls[k] = max(stalls.get(k, 0), v)
                elif k == 'per_s':
                    stalls[k] = v
                else:
                    stalls[k] = stalls.get(k, 0) + v
            dropped += px.dropped
    errors = []
    for r in res:
        if r.rc != 0:
            errors.append('p%d rc=%s' % (r.idx + 1, r.rc))
        elif not H.exchange_prefix_ok(res[0], r)[0] if r.idx else False:
            errors.append('p%d fluxo diverge' % (r.idx + 1))
        if any('DESYNC' in l for l in r.link_lines()):
            errors.append('p%d DESYNC' % (r.idx + 1))
        if any('disconnected' in l for l in r.link_lines()) and r.idx == 0:
            errors.append('host perdeu jogador')
        st = r.state(LT.COOP_FRAMES)
        if not st or st[0] != 2 or st[4] != 0:
            errors.append('p%d fora do co-op %s' % (r.idx + 1, st))
    extra_metrics = {k: v for k, v in js.items() if any(w in k for w in ('stall', 'p95', 'p99', 'resend', 'retr', 'lost', 'dup', 'reorder'))}
    return dict(scenario=name, transport=transport, players=players, wall_s=round(wall, 1),
                fps=round(frames / wall, 1), dropped=dropped, errors=sorted(set(errors)), stalls=stalls,
                runtime=extra_metrics)


def fmt(rows):
    head = '%-9s %-4s %3s %8s %7s %6s %6s %6s %8s  %s' % ('cenario', 'tr', 'jog', 'q/s', 'perdas', '>33', '>100', '>250', 'maxgap', 'erros / metricas do runtime')
    out = [head, '-' * len(head)]
    for r in rows:
        s = r['stalls']
        out.append('%-9s %-4s %3d %8.1f %7d %6s %6s %6s %6s ms  %s %s' % (
            r['scenario'], r['transport'], r['players'], r['fps'], r['dropped'], s.get('gt33', '-'), s.get('gt100', '-'),
            s.get('gt250', '-'), s.get('max_gap_ms', '-'), ';'.join(r['errors']) or 'ok', json.dumps(r['runtime']) if r['runtime'] else ''))
    return '\n'.join(out)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--transports', default='tcp,udp')
    ap.add_argument('--scen', default=','.join(n for n in SCENARIOS if n != 'udp_chaos'))
    ap.add_argument('--players', type=int, default=2)
    ap.add_argument('--frames', type=int, default=LT.COOP_FRAMES + 1)
    ap.add_argument('--out')
    ap.add_argument('--extra', default='', help='argumentos extras do rt_boot (ex.: "--link-delay 2")')
    a = ap.parse_args(argv)
    if not H.AVAILABLE:
        print('precisa de rt_boot.exe e baserom.gba')
        return 2
    rows = []
    for tr in a.transports.split(','):
        if tr == 'udp' and not H.udp_supported():
            print('[udp] o rt_boot.exe ainda nao tem %s: pulando' % H.TRANSPORT_FLAG)
            continue
        for name in a.scen.split(','):
            r = run_one(name, tr, a.players, a.frames, extra=a.extra.split() if a.extra else None)
            rows.append(r)
            print(fmt([r]).splitlines()[-1], flush=True)
    print()
    print(fmt(rows))
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            json.dump(rows, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
