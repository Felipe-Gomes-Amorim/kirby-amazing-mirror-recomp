#!/usr/bin/env python3
"""Decodifica tracos do link do KatAM (Poppy, P0c): o que o jogo coloca no cabo e o que recebe, quadro a quadro.

A fonte e o proprio `--peek` do runtime (sem mexer no runtime): uma linha `[peek] frame=N ADDR=VALOR ...` por
`--snap`. Gere assim (host e join separados; --snap aceita ate 128 quadros por execucao):

    python tools/link_trace.py --peek-arg                      # imprime a lista de enderecos para --peek
    python tools/link_trace.py --coop-keys host|join           # roteiro de teclas ate o co-op (ver COOP_KEYS)
    rt_boot.exe ROM --link host ... --snap 1700,1701,... --peek $(python tools/link_trace.py --peek-arg) 2> host.log
    python tools/link_trace.py host.log join.log               # tabela de mudancas, 1 linha por mudanca

Layout (enderecos fixos do jogo, ver NOTES 'Protocolo de link do KatAM'):
  0x030036B0  20 bytes  buffer de ENVIO do pacote gLink (o que este console manda no quadro)
  0x03002490  80 bytes  area de RECEPCAO: 4 jogadores x 20 bytes (P1..P4); o proprio jogador tambem aparece
  0x03002554  status devolvido por LinkMain (bits 0-3 conectados agora, bit4 recebeu, bit7 mestre, bit8-11 mascara, bit15 sincronizado)
  0x03002440  flags do jogo (bit19 = link permitido, bit20 = link ligado)
  0x03002558  link ligado (LinkEnable escreve 1)
  0x0203AD10  modo da sessao (0 = lobby/menu, 2 = jogo em link, 4 = 1 jogador, 6 = lobby de sub-jogo)
  0x0203AD30  numero de jogadores
  0x03000490  [0]=meu ID [1]=jogadores detectados [2]=estado da deteccao (1,2,3) [3]=flags (biblioteca de deteccao)
  0x0300050C  modo da deteccao (-1 desligado, 0 procurando, 1 mestre/filho detectados)
"""
import re
import struct
import sys

SEND, RECV, STATUS, FLAGS, LINKON, SESSION, NPLAYERS = 0x030036B0, 0x03002490, 0x03002554, 0x03002440, 0x03002558, 0x0203AD10, 0x0203AD30
DETECT, DETECT_MODE, ERR = 0x03000490, 0x0300050C, 0x02038580

PEEK_ADDRS = ([SEND + 4 * i for i in range(5)] + [RECV + 4 * i for i in range(20)] +
              [STATUS, FLAGS, LINKON, SESSION, NPLAYERS])           # 30 palavras (limite do --peek: 32)

# Roteiro de teclas (formato --keys do rt_boot) que leva DUAS instancias (--link host / --link join) do titulo ate a
# sessao cooperativa "ADVENTURE WITH 4 KIRBYS! > MULTI PLAYER". Verificado: 5/5 execucoes identicas (md5 do quadro 3000).
# NAO remova nenhuma tecla: o A@800 parece inocuo mas sem ele o jogo entra no modo 1 jogador (0x0203AD10=4).
COOP_KEYS_COMMON = 'A@700+8,A@800+8,A@1060+8,A@1300+8,DOWN@1350+4,A@1400+8'
COOP_KEYS = {
    'host': COOP_KEYS_COMMON + ',A@1700+8,A@2000+8',     # 1700: confirma "OK?" (so o P1); 2000: escolhe o jogo (GAME PAK SELECTION)
    'join': COOP_KEYS_COMMON + ',A@2000+8',
}
COOP_FRAMES = 3000          # quadro em que o co-op ja esta rodando (fase do jogo, 0x0203AD10 == 2)

LOBBY_MAGIC = 0x0123
PHASES = {0x00: 'idle', 0x20: 'hello', 0x22: 'host-ready', 0x40: 'child-hello', 0x41: 'child-ack', 0x42: 'child-ready'}


def peek_arg():
    return ','.join('%08x' % a for a in PEEK_ADDRS)


def parse_peek_log(text):
    """{quadro: {endereco: valor}} a partir de linhas `[peek] frame=N A=V ...` (ignora o resto do log)."""
    rec = {}
    for ln in text.splitlines():
        m = re.match(r'\[peek\] frame=(\d+) (.*)$', ln.strip())
        if not m:
            continue
        d = {}
        for kv in m.group(2).split():
            if '=' not in kv:
                continue                                        # linha cortada: ignora o par incompleto
            a, v = kv.split('=', 1)
            try:
                d[int(a, 16)] = int(v, 16)
            except ValueError:
                pass
        rec[int(m.group(1))] = d
    return rec


def words_to_bytes(d, base, nwords):
    if any((base + 4 * i) not in d for i in range(nwords)):
        return None
    return b''.join(struct.pack('<I', d[base + 4 * i]) for i in range(nwords))


def decode_packet(b):
    """Decodifica 20 bytes de payload: pacote de lobby (byte0=2), de teclas (byte0=0x20), vazio ou desconhecido."""
    if b is None or len(b) < 16:
        return {'kind': 'none'}
    if not any(b):
        return {'kind': 'empty'}
    if b[0] == 2 and (b[2] | b[3] << 8) == LOBBY_MAGIC:
        ph = b[14]
        return {'kind': 'lobby', 'version': b[1], 'profile': b[4:12].hex(), 'ack': b[12] | b[13] << 8, 'phase': ph,
                'phase_name': PHASES.get(ph, '?')}
    if b[0] == 0x20:
        nin = []
        for i in range(8):                                      # 8 entradas de 12 bits, a mais nova primeiro (LSB primeiro)
            bit = i * 12
            v = (b[2 + bit // 8] | b[3 + bit // 8] << 8) >> (bit % 8)
            nin.append(v & 0xFFF)
        return {'kind': 'keys', 'session': b[1], 'inputs': nin, 'frame': struct.unpack_from('<I', b, 16)[0] if len(b) >= 20 else None}
    return {'kind': 'other', 'type': b[0], 'hex': b.hex()}


def fmt(p):
    k = p['kind']
    if k == 'lobby':
        return 'LOBBY v%d ack=%04X fase=0x%02X(%s) perfil=%s' % (p['version'], p['ack'], p['phase'], p['phase_name'], p['profile'])
    if k == 'keys':
        return 'KEYS sessao=%02X quadro=%s entradas=%s' % (p['session'], p['frame'], ' '.join('%03X' % x for x in p['inputs'][:2]))
    if k == 'other':
        return 'OUTRO tipo=%02X %s' % (p['type'], p['hex'])
    return k.upper()


def events(rec):
    """Lista de mudancas de estado: (quadro, texto). Pacote de teclas so aparece na 1a vez e quando muda a sessao."""
    out = []
    prev = {}
    last_keys_session = None
    for fr in sorted(rec):
        d = rec[fr]
        snd = decode_packet(words_to_bytes(d, SEND, 5))
        rcv = [decode_packet((words_to_bytes(d, RECV, 20) or b'\0' * 80)[20 * p:20 * p + 20]) for p in range(4)]
        cur = {'status': d.get(STATUS), 'link': d.get(LINKON), 'session': d.get(SESSION), 'n': d.get(NPLAYERS),
               'flags': d.get(FLAGS)}
        for k, v in cur.items():
            if v is not None and prev.get(k) != v:
                fmtv = '0x%04X' % v if k in ('status', 'flags') else str(v)
                out.append((fr, 'estado %s = %s' % (k, fmtv)))
        prev.update({k: v for k, v in cur.items() if v is not None})
        for who, p in [('envia', snd)] + [('recebe P%d' % (i + 1), rcv[i]) for i in range(4)]:
            if p['kind'] in ('none', 'empty'):
                key = (who, 'vazio')
            elif p['kind'] == 'keys':
                key = (who, 'keys', p['session'])
            else:
                key = (who, fmt(p))
            if prev.get(who) != key:
                prev[who] = key
                if p['kind'] in ('none',):
                    continue
                if p['kind'] == 'empty' and prev.get('seen_' + who) is None:
                    continue
                prev['seen_' + who] = True
                out.append((fr, '%-10s %s' % (who, fmt(p))))
    return out


def main(argv):
    if '--peek-arg' in argv:
        print(peek_arg())
        return 0
    if '--coop-keys' in argv:
        print(COOP_KEYS[argv[argv.index('--coop-keys') + 1]])
        return 0
    files = [a for a in argv if not a.startswith('-')]
    if not files:
        print(__doc__)
        return 2
    for f in files:
        with open(f, encoding='utf-8', errors='replace') as fh:
            rec = parse_peek_log(fh.read())
        print('=== %s: %d quadros (%s..%s)' % (f, len(rec), min(rec, default='-'), max(rec, default='-')))
        for fr, txt in events(rec):
            print('%6d  %s' % (fr, txt))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
