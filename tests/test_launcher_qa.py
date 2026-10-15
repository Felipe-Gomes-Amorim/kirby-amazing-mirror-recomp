"""QA automatizado do launcher / controle: kirby.ini e o arquivo compartilhado (Poppy).

Rodar:  python -m unittest tests.test_launcher_qa -v      (da raiz do repo)

O que e testado de verdade: o `hw/config_ini.c` REAL, compilado num harness C minimo
(`cfg_defaults` / `cfg_load` / `cfg_save` / `cfg_path_beside`), e verificado pelo arquivo que ele grava.
Assim os testes nao dependem do nome dos campos da struct, so das 4 funcoes publicas do contrato.

Estado em 2026-10 (ver NOTES.md, secao "QA do launcher"):
  - kirby.ini v1 (4 chaves: scale, mute, fullscreen, vsync): testado agora; e o piso de compatibilidade
    que a v2 tem que manter ("v1 antigo").
  - kirby.ini v2 (secoes [video] [audio] [keyboard] [controller] [launcher]): testes escritos a partir do
    contrato; PULAM com motivo explicito ate `hw/config_ini.c` gravar a v2. Ativam sozinhos.
  - "launcher.py e kirby.exe leem o mesmo arquivo": idem; pulam ate os dois chamarem `cfg_load` /
    uma funcao `ini_path_*` no launcher.
  - `expectedFailure` = bug real de config_ini.c achado por estes testes (pedido ao dono no ROLES.md);
    quando for corrigido o teste vira "unexpected success" e o decorador deve ser removido.
"""
import ntpath
import os
import posixpath
import re
import types
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HW = os.path.join(ROOT, 'hw')
EXE = None          # harness de hw/config_ini.c (setUpModule)
RT_EXE = None       # harness do parser do jogo, recomp/runtime/rt_input.c sem SDL (setUpModule)
RT_SKIP = None
TMP = None
SKIP_REASON = None  # preenchido se nao houver compilador


HARNESS_C = r'''
#include <stdio.h>
#include <string.h>
#include "config_ini.h"
int main(int argc, char **argv) {
    KirbyConfig c;
    if (argc >= 3 && !strcmp(argv[1], "defaults")) {
        cfg_defaults(&c);
        return cfg_save(argv[2], &c) < 0 ? 3 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "rt")) {      /* load IN, save OUT; exit 0 = lido, 11 = inexistente */
        int rc = cfg_load(argv[2], &c);
        if (rc < 0) return 2;
        if (cfg_save(argv[3], &c) < 0) return 3;
        return rc == 0 ? 0 : 10 + rc;
    }
    if (argc >= 3 && !strcmp(argv[1], "path")) {
        char b[600];
        cfg_path_beside(argv[2], b, sizeof b);
        printf("%s\n", b);
        return 0;
    }
    return 64;
}
'''


RT_HARNESS_C = r'''
#include <stdio.h>
extern int rt_cfg_load(const char *path);
typedef struct { int scale, fullscreen, integer_scale, vsync, volume, mute, stick_as_dpad, deadzone;
                 char kb[10][32]; char pad[10][32]; int scaling; } RtCfg;
extern RtCfg rt_cfg;
int main(int argc, char **argv) {
    int rc = rt_cfg_load(argv[1]);
    printf("rc=%d\nscale=%d\nfullscreen=%d\ninteger_scale=%d\nvsync=%d\nvolume=%d\nmute=%d\n"
           "stick_as_dpad=%d\ndeadzone=%d\n", rc, rt_cfg.scale, rt_cfg.fullscreen, rt_cfg.integer_scale,
           rt_cfg.vsync, rt_cfg.volume, rt_cfg.mute, rt_cfg.stick_as_dpad, rt_cfg.deadzone);
    printf("scaling=%s\n", rt_cfg.scaling == 0 ? "integer" : rt_cfg.scaling == 2 ? "stretch" : "fit");
    const char *bk[10] = {"a","b","select","start","right","left","up","down","r","l"};
    for (int i = 0; i < 10; i++) printf("kb_%s=%s\npad_%s=%s\n", bk[i], rt_cfg.kb[i], bk[i], rt_cfg.pad[i]);
    return 0;
}
'''


def _cc_candidates():
    """Compiladores C em ordem de preferencia (cada um e testado compilando de verdade)."""
    out = []
    try:
        sys.path.insert(0, ROOT)
        from recomp.toolchain import find_zig_cmd
        cmd = find_zig_cmd()
        if cmd:
            out.append(list(cmd))
    except Exception:
        pass
    venv_py = os.path.join(ROOT, '.venv', 'Scripts' if os.name == 'nt' else 'bin',
                           'python.exe' if os.name == 'nt' else 'python')
    if os.path.exists(venv_py):
        out.append([venv_py, '-m', 'ziglang', 'cc'])
    dirs = [os.environ.get('PATH', '')]
    la = os.environ.get('LOCALAPPDATA')
    if la:   # WinLibs instalado via winget (o PATH da sessao pode estar velho)
        pk = os.path.join(la, 'Microsoft', 'WinGet', 'Packages')
        if os.path.isdir(pk):
            for d in os.listdir(pk):
                if d.startswith('BrechtSanders.WinLibs'):
                    for dp, _, files in os.walk(os.path.join(pk, d)):
                        if 'gcc.exe' in files:
                            dirs.append(dp)
    path = os.pathsep.join(dirs)
    for name in (os.environ.get('CC'), 'gcc', 'clang', 'cc'):
        if name:
            w = shutil.which(name, path=path)
            if w:
                out.append([w])
    return out


def setUpModule():
    global EXE, TMP, SKIP_REASON
    TMP = tempfile.mkdtemp(prefix='kirby_qa_')
    src = os.path.join(TMP, 'ini_harness.c')
    with open(src, 'w') as f:
        f.write(HARNESS_C)
    exe = os.path.join(TMP, 'ini_harness' + ('.exe' if os.name == 'nt' else ''))
    errors = []
    for cc in _cc_candidates():
        r = subprocess.run(cc + ['-O1', '-w', '-I', HW, src, os.path.join(HW, 'config_ini.c'), '-o', exe],
                           capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(exe):
            EXE = exe
            _build_rt_harness(cc)
            return
        errors.append(f"{os.path.basename(cc[0])}: {(r.stderr or r.stdout).strip()[-120:]}")
    SKIP_REASON = 'no working C compiler (' + ('; '.join(errors) or 'none found') + ')'


def _build_rt_harness(cc):
    """Parser do jogo (rt_cfg_load) isolado: rt_input.c compila sem SDL quando RT_WINDOW nao esta definido."""
    global RT_EXE, RT_SKIP
    src = os.path.join(TMP, 'rt_harness.c')
    with open(src, 'w') as f:
        f.write(RT_HARNESS_C)
    exe = os.path.join(TMP, 'rt_harness' + ('.exe' if os.name == 'nt' else ''))
    r = subprocess.run(cc + ['-O1', '-w', src, os.path.join(ROOT, 'recomp', 'runtime', 'rt_input.c'), '-o', exe],
                       capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(exe):
        RT_EXE = exe
    else:
        RT_SKIP = 'could not compile recomp/runtime/rt_input.c without SDL: ' + (r.stderr or r.stdout)[-200:]


def tearDownModule():
    if TMP:
        shutil.rmtree(TMP, ignore_errors=True)


# ----------------------------- utilidades -----------------------------
def parse_ini(text):
    """{(secao, chave): valor}, tudo em minusculas; ignora comentarios; secao '' antes da 1a."""
    out, sec = {}, ''
    for raw in text.replace('\r\n', '\n').split('\n'):
        line = raw.strip()
        if not line or line[0] in ';#':
            continue
        if line.startswith('[') and line.endswith(']'):
            sec = line[1:-1].strip().lower()
            continue
        if '=' in line:
            k, v = line.split('=', 1)
            out[(sec, k.strip().lower())] = v.split(';')[0].split('#')[0].strip()
    return out


def find_key(cfg, key):
    """Valor de `key` em qualquer secao (v1: [kirby]; v2: [video]/[audio]/...). None se ausente."""
    for (sec, k), v in cfg.items():
        if k == key:
            return v
    return None


class IniCase(unittest.TestCase):
    def setUp(self):
        if SKIP_REASON:
            self.skipTest(SKIP_REASON)
        self.dir = tempfile.mkdtemp(dir=TMP)

    def write(self, content, name='in.ini'):
        p = os.path.join(self.dir, name)
        with open(p, 'wb') as f:
            f.write(content if isinstance(content, bytes) else content.encode('utf-8'))
        return p

    def roundtrip(self, content):
        """Grava `content`, roda cfg_load + cfg_save, devolve (exit_code, dict do arquivo gravado)."""
        src = self.write(content)
        out = os.path.join(self.dir, 'out.ini')
        r = subprocess.run([EXE, 'rt', src, out], capture_output=True, text=True)
        cfg = {}
        if os.path.exists(out):
            with open(out, 'r', encoding='utf-8', errors='replace') as f:
                cfg = parse_ini(f.read())
        return r.returncode, cfg

    def defaults(self):
        out = os.path.join(self.dir, 'defaults.ini')
        self.assertEqual(subprocess.run([EXE, 'defaults', out]).returncode, 0)
        with open(out, 'r', encoding='utf-8') as f:
            return parse_ini(f.read())

    def beside(self, p):
        r = subprocess.run([EXE, 'path', p], capture_output=True, text=True)
        return r.stdout.strip()


# ----------------------------- v1: piso de compatibilidade -----------------------------
V1_KEYS = {'scale': (1, 6, 3), 'mute': (0, 1, 0), 'fullscreen': (0, 1, 0), 'vsync': (0, 1, 0)}


class IniV1CompatTest(IniCase):
    """kirby.ini v1 (e o que a v2 tem que continuar entendendo)."""

    def test_defaults_file_has_every_v1_key_with_its_default(self):
        cfg = self.defaults()
        for key, (_, _, dflt) in V1_KEYS.items():
            self.assertEqual(find_key(cfg, key), str(dflt), key)

    def test_missing_file_reports_not_found_and_keeps_defaults(self):
        out = os.path.join(self.dir, 'out.ini')
        r = subprocess.run([EXE, 'rt', os.path.join(self.dir, 'nope.ini'), out])
        self.assertEqual(r.returncode, 11)   # cfg_load == 1
        with open(out) as f:
            cfg = parse_ini(f.read())
        for key, (_, _, dflt) in V1_KEYS.items():
            self.assertEqual(find_key(cfg, key), str(dflt), key)

    def test_valid_values_are_loaded(self):
        rc, cfg = self.roundtrip('[kirby]\r\nscale=5\r\nmute=1\r\nfullscreen=1\r\nvsync=1\r\n')
        self.assertEqual(rc, 0)
        self.assertEqual([find_key(cfg, k) for k in V1_KEYS], ['5', '1', '1', '1'])

    def test_every_boundary_value(self):
        for key, (lo, hi, _) in V1_KEYS.items():
            for v in (lo, hi):
                with self.subTest(key=key, value=v):
                    _, cfg = self.roundtrip(f'{key}={v}\n')
                    self.assertEqual(find_key(cfg, key), str(v))

    def test_invalid_values_fall_back_to_default(self):
        bad = ['9', '0', '-1', '7', '2x', 'abc', '', ' ', '3.5', '0x2', '99999999999999999999', '--1']
        for key, (lo, hi, dflt) in V1_KEYS.items():
            for v in bad:
                n = None
                try:
                    n = int(v)
                except ValueError:
                    pass
                if n is not None and lo <= n <= hi:
                    continue
                with self.subTest(key=key, value=v):
                    _, cfg = self.roundtrip(f'{key}={v}\n')
                    self.assertEqual(find_key(cfg, key), str(dflt))

    def test_one_bad_key_does_not_poison_the_others(self):
        _, cfg = self.roundtrip('scale=abc\nmute=1\nfullscreen=zz\nvsync=1\n')
        self.assertEqual([find_key(cfg, k) for k in V1_KEYS], ['3', '1', '0', '1'])

    def test_comments_whitespace_case_and_crlf(self):
        text = ('; leading comment\r\n# another\r\n[Kirby]\r\n  SCALE = 2 ; inline\r\n'
                '\tMute\t=\t1 # inline\r\nFULLSCREEN=1\r\n\r\n   \r\nvsync=1')   # sem \n final
        _, cfg = self.roundtrip(text)
        self.assertEqual([find_key(cfg, k) for k in V1_KEYS], ['2', '1', '1', '1'])

    def test_unknown_keys_and_garbage_lines_are_ignored(self):
        text = 'foo=bar\nthis line has no equals\n=novalue\nscale=4\n[[weird\nmute=1\n'
        rc, cfg = self.roundtrip(text)
        self.assertEqual(rc, 0)
        self.assertEqual(find_key(cfg, 'scale'), '4')
        self.assertEqual(find_key(cfg, 'mute'), '1')
        self.assertIsNone(find_key(cfg, 'foo'))

    def test_last_duplicate_wins(self):
        _, cfg = self.roundtrip('scale=2\nscale=6\n')
        self.assertEqual(find_key(cfg, 'scale'), '6')

    def test_roundtrip_is_idempotent(self):
        _, once = self.roundtrip('scale=2\nmute=1\nfullscreen=abc\n')
        text = '\n'.join(f'{k}={v}' for (_, k), v in once.items())
        _, twice = self.roundtrip(text)
        self.assertEqual({k: v for (_, k), v in once.items()}, {k: v for (_, k), v in twice.items()})

    def test_empty_file_and_binary_garbage_do_not_crash(self):
        for name, data in (('empty', b''), ('nul', b'\x00\x00\x00'), ('noise', bytes(range(256)) * 20),
                           ('only_newlines', b'\n' * 5000)):
            with self.subTest(name):
                src = self.write(data, name + '.ini')
                r = subprocess.run([EXE, 'rt', src, os.path.join(self.dir, name + '.out')], capture_output=True)
                self.assertEqual(r.returncode, 0, r.stderr)

    def test_path_beside_follows_the_save_or_rom_directory(self):
        cases = [
            ('C:\\games\\Kirby & The Amazing Mirror (USA).sav', 'C:\\games\\kirby.ini'),
            ('/home/u/roms/baserom.gba', '/home/u/roms/kirby.ini'),
            ('baserom.sav', 'kirby.ini'),
            ('C:\\Users\\Jo\u00e3o Silva\\Documents\\k.sav', 'C:\\Users\\Jo\u00e3o Silva\\Documents\\kirby.ini'),
            ('D:/mixed\\slashes/file.sav', 'D:/mixed\\slashes/kirby.ini'),
        ]
        for src, want in cases:
            with self.subTest(src=src):
                self.assertEqual(self.beside(src), want)

    def test_long_line_tail_is_not_parsed_as_a_new_line(self):
        """Regressao (corrigido na v2): o resto de uma linha longa nao pode virar linha nova.
        Um comentario >255 bytes cujo final contem `scale=6` NAO pode mudar a escala."""
        text = '; ' + 'x' * 400 + ' scale=6\nmute=1\n'
        _, cfg = self.roundtrip(text)
        self.assertEqual(find_key(cfg, 'scale'), '3')
        self.assertEqual(find_key(cfg, 'mute'), '1')

    def test_utf8_bom_at_start_is_accepted(self):
        """Regressao (corrigido): o Bloco de Notas antigo grava BOM UTF-8; a 1a chave nao pode se perder."""
        _, cfg = self.roundtrip(b'\xef\xbb\xbfscale=5\r\nmute=1\r\n')
        self.assertEqual(find_key(cfg, 'scale'), '5')


# ----------------------------- v2: contrato (NOTES.md) -----------------------------
# secao -> chave -> (tipo, parametros, padrao)
V2_SPEC = {
    'video': {'scale': ('int', (1, 6), '3'), 'fullscreen': ('int', (0, 1), '0'),
              'scaling': ('enum', ('integer', 'fit', 'stretch'), 'fit'), 'vsync': ('int', (0, 1), '0')},
    'audio': {'volume': ('int', (0, 100), '100'), 'mute': ('int', (0, 1), '0')},
    'keyboard': {'a': ('key', None, 'x'), 'b': ('key', None, 'z'), 'l': ('key', None, 'a'),
                 'r': ('key', None, 's'), 'start': ('key', None, 'return'),
                 'select': ('key', None, 'backspace'), 'up': ('key', None, 'up'),
                 'down': ('key', None, 'down'), 'left': ('key', None, 'left'), 'right': ('key', None, 'right')},
    'controller': {'a': ('btn', None, 'b'), 'b': ('btn', None, 'a'), 'l': ('btn', None, 'leftshoulder'),
                   'r': ('btn', None, 'rightshoulder'), 'start': ('btn', None, 'start'),
                   'select': ('btn', None, 'back'), 'up': ('btn', None, 'dpup'),
                   'down': ('btn', None, 'dpdown'), 'left': ('btn', None, 'dpleft'),
                   'right': ('btn', None, 'dpright'), 'stick_as_dpad': ('int', (0, 1), '1'),
                   'deadzone': ('int', (0, 32767), '8000')},
    'launcher': {'show': ('int', (0, 1), '1'), 'language': ('enum', ('en',), 'en')},
}
RESERVED_KEYS = ['Escape', 'F11', 'F1']   # nao remapeaveis (contrato)


class IniV2ContractTest(IniCase):
    """kirby.ini v2. Pula ate hw/config_ini.c gravar a v2 (secao [keyboard])."""

    def setUp(self):
        super().setUp()
        d = self.defaults()
        if not any(sec == 'keyboard' for sec, _ in d):
            self.skipTest('kirby.ini v2 not implemented yet in hw/config_ini.c '
                          '(cfg_save writes no [keyboard] section); tests activate automatically')
        self.dflt = d

    def get(self, cfg, sec, key):
        return cfg.get((sec, key))

    def test_defaults_file_has_every_contract_key(self):
        for sec, keys in V2_SPEC.items():
            for key, (_, _, dflt) in keys.items():
                with self.subTest(section=sec, key=key):
                    self.assertEqual(str(self.get(self.dflt, sec, key)).lower(), dflt)

    def test_missing_sections_and_keys_use_defaults(self):
        _, cfg = self.roundtrip('[video]\nscale=2\n')
        self.assertEqual(self.get(cfg, 'video', 'scale'), '2')
        self.assertEqual(self.get(cfg, 'audio', 'volume'), '100')
        self.assertEqual(str(self.get(cfg, 'keyboard', 'a')).lower(), 'x')

    def test_int_ranges_boundaries_and_invalid(self):
        for sec, keys in V2_SPEC.items():
            for key, (typ, par, dflt) in keys.items():
                if typ != 'int':
                    continue
                lo, hi = par
                for v in (lo, hi):
                    with self.subTest(section=sec, key=key, value=v):
                        _, cfg = self.roundtrip(f'[{sec}]\n{key}={v}\n')
                        self.assertEqual(self.get(cfg, sec, key), str(v))
                for v in (lo - 1, hi + 1, 'abc', '', '1.5'):
                    with self.subTest(section=sec, key=key, invalid=v):
                        _, cfg = self.roundtrip(f'[{sec}]\n{key}={v}\n')
                        self.assertEqual(self.get(cfg, sec, key), dflt)

    def test_valid_custom_keyboard_binding_is_kept(self):
        _, cfg = self.roundtrip('[keyboard]\na=Q\nb=W\n')
        self.assertEqual(str(self.get(cfg, 'keyboard', 'a')).lower(), 'q')
        self.assertEqual(str(self.get(cfg, 'keyboard', 'b')).lower(), 'w')

    def test_duplicate_keyboard_binding_resets_the_whole_map(self):
        """Contrato (hw/config_ini.h): binding duplicado -> mapa do teclado inteiro volta ao padrao."""
        _, cfg = self.roundtrip('[keyboard]\na=Z\nl=Q\n')    # Z ja e o padrao do botao B
        for key, (_, _, dflt) in V2_SPEC['keyboard'].items():
            self.assertEqual(str(self.get(cfg, 'keyboard', key)).lower(), dflt, key)

    def test_empty_name_falls_back(self):
        _, cfg = self.roundtrip('[keyboard]\na=\n[controller]\na=\n')
        self.assertEqual(str(self.get(cfg, 'keyboard', 'a')).lower(), 'x')
        self.assertEqual(self.get(cfg, 'controller', 'a'), 'b')

    def test_overlong_values_do_not_overflow_or_poison_the_file(self):
        long = 'Q' * 300
        rc, cfg = self.roundtrip(f'[keyboard]\na={long}\n[controller]\nb={long}\n[launcher]\nlanguage={long}\n'
                                 f'[video]\nscale=4\n')
        self.assertEqual(rc, 0)
        self.assertEqual(self.get(cfg, 'video', 'scale'), '4')
        with open(os.path.join(self.dir, 'out.ini'), encoding='utf-8') as f:
            saved = f.read()
        rc2, _ = self.roundtrip(saved)
        self.assertEqual(rc2, 0)   # o que foi gravado volta a ser lido

    def test_reserved_keys_cannot_be_bound(self):
        for name in RESERVED_KEYS:
            with self.subTest(key=name):
                _, cfg = self.roundtrip(f'[keyboard]\na={name}\n')
                self.assertEqual(str(self.get(cfg, 'keyboard', 'a')).lower(), 'x')

    def test_v1_keys_are_accepted_flat_and_in_kirby_section(self):
        for text in ('scale=5\nmute=1\n', '[kirby]\nscale=5\nmute=1\n'):
            with self.subTest(text=text):
                _, cfg = self.roundtrip(text)
                self.assertEqual(self.get(cfg, 'video', 'scale'), '5')
                self.assertEqual(self.get(cfg, 'audio', 'mute'), '1')

    def test_unknown_section_keys_are_ignored(self):
        _, cfg = self.roundtrip('[nonsense]\nscale=6\nvolume=7\n')
        self.assertEqual(self.get(cfg, 'video', 'scale'), '3')
        self.assertEqual(self.get(cfg, 'audio', 'volume'), '100')

    def test_v1_file_migrates_into_v2_sections(self):
        _, cfg = self.roundtrip('[kirby]\nscale=5\nmute=1\nfullscreen=1\nvsync=1\n')
        self.assertEqual(self.get(cfg, 'video', 'scale'), '5')
        self.assertEqual(self.get(cfg, 'audio', 'mute'), '1')
        self.assertEqual(self.get(cfg, 'video', 'fullscreen'), '1')
        self.assertEqual(self.get(cfg, 'video', 'vsync'), '1')
        self.assertEqual(self.get(cfg, 'audio', 'volume'), '100')   # nao existia na v1

    def test_unknown_section_is_ignored_and_roundtrip_is_stable(self):
        _, once = self.roundtrip('[nonsense]\nscale=6\n[video]\nscale=2\n')
        self.assertEqual(self.get(once, 'video', 'scale'), '2')
        text = '\n'.join(f'[{s}]\n' if False else '' for s in ()) + ''.join(
            f'[{s}]\n' + ''.join(f'{k}={v}\n' for (ss, k), v in once.items() if ss == s)
            for s in sorted({s for s, _ in once}))
        _, twice = self.roundtrip(text)
        self.assertEqual(once, twice)


# ----------------------------- paridade: UI (config_ini.c) x jogo (rt_input.c) x launcher (flow.py) ----
FIELD_SECTION = {'scale': 'video', 'fullscreen': 'video', 'scaling': 'video', 'vsync': 'video',
                 'volume': 'audio', 'mute': 'audio', 'stick_as_dpad': 'controller', 'deadzone': 'controller'}


class ParserParityTest(IniCase):
    """Tres leitores do MESMO kirby.ini: hw/config_ini.c (UI, referencia: valida e e quem grava),
    recomp/runtime/rt_input.c (kirby.exe) e launcher/flow.py (so `[launcher] show`). Em qualquer arquivo,
    os valores efetivos tem que ser iguais; os `expectedFailure` abaixo sao divergencias reais conhecidas."""

    def setUp(self):
        super().setUp()
        if RT_EXE is None:
            self.skipTest(RT_SKIP or 'rt harness not built')

    def game(self, content):
        path = self.write(content, 'g.ini')
        out = subprocess.run([RT_EXE, path], capture_output=True, text=True).stdout
        return dict(l.split('=', 1) for l in out.split('\n') if '=' in l)

    def launcher_shows(self, content):
        sys.path.insert(0, os.path.join(ROOT, 'launcher'))
        try:
            import flow
        finally:
            sys.path.pop(0)
        path = self.write(content, 'f.ini')
        return flow.read_ini_value(path, 'launcher', 'show', '1') != '0'

    def diffs(self, content, kb=('a', 'b', 'l')):
        _, ref = self.roundtrip(content)
        g = self.game(content)
        out = []
        for key, sec in FIELD_SECTION.items():
            if str(ref.get((sec, key))) != str(g.get(key)):
                out.append(f'{sec}.{key}: config_ini={ref.get((sec, key))} game={g.get(key)}')
        for b in kb:
            if str(ref.get(('keyboard', b))).lower() != str(g.get('kb_' + b)).lower():
                out.append(f'keyboard.{b}: config_ini={ref.get(("keyboard", b))} game={g.get("kb_" + b)}')
        want_show = ref.get(('launcher', 'show')) != '0'
        if want_show != self.launcher_shows(content):
            out.append(f'launcher.show: config_ini={ref.get(("launcher", "show"))} flow.py shows={not want_show}')
        return out

    def test_the_three_parsers_agree_on_common_files(self):
        corpus = {
            'v2 basic': '[video]\nscale=4\n[audio]\nmute=1\nvolume=50\n[launcher]\nshow=0\n',
            'v1 flat': 'scale=5\nmute=1\n',
            'v1 [kirby]': '[kirby]\nscale=5\nmute=1\nfullscreen=1\nvsync=1\n',
            'upper-case names': '[VIDEO]\nSCALE=5\n[Launcher]\nSHOW=0\n',
            'crlf': '[video]\r\nscale=5\r\n[launcher]\r\nshow=0\r\n',
            'unknown section': '[nonsense]\nscale=6\n',
            'key in wrong section': 'scale=2\n[audio]\nscale=6\n',
            'out of range': '[video]\nscale=99\n[audio]\nvolume=101\n[controller]\ndeadzone=-1\n',
            'integer_scale=0': '[video]\ninteger_scale=0\n',
            'inline ; comment': '[launcher]\nshow=0 ; hidden\n',
            'show invalid': '[launcher]\nshow=abc\n',
            'custom keyboard': '[keyboard]\na=Q\nb=W\n',
            'long comment': '; ' + 'x' * 400 + ' scale=6\nmute=1\n',
            'empty': '',
        }
        for name, text in corpus.items():
            with self.subTest(file=name):
                self.assertEqual(self.diffs(text), [])

    def test_game_rejects_trailing_garbage_like_the_ui(self):
        """BUG (recomp/runtime/rt_input.c to_int): `scale=2x` vale 2 no jogo e 3 (padrao) na UI."""
        self.assertEqual(self.diffs('[video]\nscale=2x\n'), [])

    def test_game_accepts_the_full_deadzone_range(self):
        """BUG (rt_input.c): deadzone maximo 32000 no jogo; 32767 na UI/contrato."""
        self.assertEqual(self.diffs('[controller]\ndeadzone=32500\n'), [])

    def test_game_resets_reserved_and_duplicate_bindings_like_the_ui(self):
        """BUG (rt_input.c): `a=Escape` deixa o botao A SEM tecla no jogo e volta a X na UI; binding
        duplicado: o jogo mantem as duas (uma tecla = 2 botoes), a UI reseta o teclado inteiro."""
        self.assertEqual(self.diffs('[keyboard]\na=Escape\n'), [])
        self.assertEqual(self.diffs('[keyboard]\na=Z\nl=Q\n'), [])

    def test_launcher_honours_hash_comments_in_show(self):
        """BUG (launcher/flow.py read_ini_value): so corta `;`; `show=0 # hidden` mostra o launcher,
        enquanto config_ini.c/rt_input.c leem 0 (abrir direto)."""
        self.assertEqual(self.diffs('[launcher]\nshow=0 # hidden\n'), [])


# ----------------------------- launcher.py x kirby.exe: mesmo arquivo -----------------------------
class SharedIniFileTest(unittest.TestCase):
    """O launcher (Python) e o kirby.exe (C, hw/) tem que resolver o MESMO caminho de kirby.ini."""

    @staticmethod
    def _read(*parts):
        with open(os.path.join(ROOT, *parts), encoding='utf-8', errors='replace') as f:
            return f.read()

    def test_game_loads_kirby_ini_beside_the_save(self):
        src = self._read('recomp', 'runtime', 'rt_main.c')
        if 'rt_cfg_load' not in src:
            self.skipTest('kirby.exe does not load kirby.ini yet (settings -> game not delivered)')
        self.assertIn('kirby.ini', src)
        self.assertIn('--ini', src, 'the launcher must be able to point the game at an explicit file')

    def test_game_uses_the_shared_parser(self):
        """Design (causa das divergencias de ParserParityTest): o jogo tem parser proprio (rt_cfg_load em
        rt_input.c) em vez de hw/config_ini.c, que ja nao depende de SDL. Uma fonte so elimina as 4 divergencias."""
        src = self._read('recomp', 'runtime', 'rt_main.c') + self._read('recomp', 'runtime', 'rt_input.c')
        self.assertTrue(re.search(r'\bcfg_load\s*\(', src.replace('rt_cfg_load', '')) is not None)

    def test_launcher_and_game_resolve_the_same_ini_path(self):
        """launcher.py: `ini_path = <expr>` (a ROM); jogo: kirby.ini ao lado de `<rom>.sav` (rt_main.c)."""
        if EXE is None:
            self.skipTest(SKIP_REASON or 'harness not built')
        with open(os.path.join(ROOT, 'launcher', 'launcher.py'), encoding='utf-8') as f:
            m = re.search(r"^\s*ini_path\s*=\s*(.+)$", f.read(), re.M)
        if not m:
            self.skipTest('launcher.py does not compute ini_path yet')
        expr = m.group(1).strip()
        self.assertIn('kirby.ini', expr)
        for rom in ('C:\\games\\Kirby & The Amazing Mirror (USA).gba', 'C:\\Users\\Jo\u00e3o\\Docs\\k.gba',
                    '/home/u/roms/baserom.gba', 'baserom.gba'):
            with self.subTest(rom=rom):
                fake_os = types.SimpleNamespace(path=ntpath if '\\' in rom else posixpath)
                got = eval(expr, {'os': fake_os, 'rom': rom})
                save = os.path.splitext(rom)[0] + '.sav'   # rt_main.c: <rom>.sav
                want = subprocess.run([EXE, 'path', save], capture_output=True, text=True).stdout.strip()
                self.assertEqual(got.replace('/', '\\'), want.replace('/', '\\'))

    def test_launcher_declares_the_kl_run_contract(self):
        """Contrato com kirby_ui.dll: kl_run(ini_path, poll) e codigo de saida 77 (F1)."""
        src = ''
        for name in ('launcher.py', 'flow.py'):
            with open(os.path.join(ROOT, 'launcher', name), encoding='utf-8') as f:
                src += f.read()
        if 'kl_run' not in src:
            self.skipTest('launcher does not use kl_run yet (UI DLL phase 1 not delivered)')
        self.assertTrue(re.search(r'\b77\b', src) is not None,
                        'F1 must be handled: the game exits with code 77 and the launcher reopens')


# ----------------------------- scaling=integer|fit|stretch e retangulo de destino -----------------------------
# CONTRATO (hw/config_ini.h + NOTES "Ajustes do launcher"): [video] scaling=integer|fit|stretch, padrao FIT.
#   integer : s = max(1, min(W // 240, H // 160)); tamanho 240*s x 160*s, centralizado (divisao inteira)
#   fit     : escala real k = min(W/240, H/160); tamanho floor(240*k) x floor(160*k), centralizado
#   stretch : a janela inteira (0, 0, W, H)
# Legado: integer_scale=1 -> integer, integer_scale=0 -> fit; um `scaling=` valido sempre vence.
GBA_W, GBA_H = 240, 160
RECT_TABLE = {   # (W, H) -> {modo: (x, y, w, h)}  (calculado a mao, ver NOTES)
    (1920, 1080): {'integer': (240, 60, 1440, 960), 'fit': (150, 0, 1620, 1080), 'stretch': (0, 0, 1920, 1080)},
    (1366, 768):  {'integer': (203, 64, 960, 640),  'fit': (107, 0, 1152, 768),  'stretch': (0, 0, 1366, 768)},
    (2560, 1440): {'integer': (200, 0, 2160, 1440), 'fit': (200, 0, 2160, 1440), 'stretch': (0, 0, 2560, 1440)},
    (1280, 720):  {'integer': (160, 40, 960, 640),  'fit': (100, 0, 1080, 720),  'stretch': (0, 0, 1280, 720)},
}


def ref_dest_rect(win_w, win_h, mode):
    """Implementacao de referencia do contrato (so inteiros; sem float para nao depender de arredondamento)."""
    if mode == 'stretch':
        return (0, 0, win_w, win_h)
    if mode == 'integer':
        k = max(1, min(win_w // GBA_W, win_h // GBA_H))
        w, h = GBA_W * k, GBA_H * k
    elif mode == 'fit':
        # min(W/240, H/160) em inteiros: compara W*160 com H*240
        if win_w * GBA_H <= win_h * GBA_W:
            w, h = win_w, (win_w * GBA_H) // GBA_W
        else:
            w, h = (win_h * GBA_W) // GBA_H, win_h
    else:
        raise ValueError(mode)
    return ((win_w - w) // 2, (win_h - h) // 2, w, h)


class ScalingReferenceSpecTest(unittest.TestCase):
    """Autoverificacao do contrato: a implementacao de referencia reproduz a tabela feita a mao."""

    def test_reference_matches_hand_computed_table(self):
        for (w, h), modes in RECT_TABLE.items():
            for mode, want in modes.items():
                with self.subTest(res=f'{w}x{h}', mode=mode):
                    self.assertEqual(ref_dest_rect(w, h, mode), want)

    def test_geometric_invariants(self):
        for (w, h) in list(RECT_TABLE) + [(640, 480), (3840, 2160), (800, 600), (1024, 768)]:
            for mode in ('integer', 'fit', 'stretch'):
                x, y, rw, rh = ref_dest_rect(w, h, mode)
                with self.subTest(res=f'{w}x{h}', mode=mode):
                    self.assertTrue(0 <= x and 0 <= y and x + rw <= w and y + rh <= h, 'inside the window')
                    self.assertLessEqual(abs((w - rw) - 2 * x), 1)      # centralizado (+-1 px)
                    self.assertLessEqual(abs((h - rh) - 2 * y), 1)
                    if mode == 'integer':
                        self.assertEqual((rw % GBA_W, rh % GBA_H), (0, 0))
                        self.assertEqual(rw // GBA_W, rh // GBA_H)
                    if mode == 'fit':
                        self.assertLessEqual(abs(rw * GBA_H - rh * GBA_W), GBA_W)   # 3:2 (+-1 px)
                        self.assertTrue(rw == w or rh == h, 'touches two opposite edges')
                    if mode == 'stretch':
                        self.assertEqual((rw, rh), (w, h))

    def test_integer_is_never_larger_than_fit(self):
        for (w, h) in RECT_TABLE:
            self.assertLessEqual(ref_dest_rect(w, h, 'integer')[2], ref_dest_rect(w, h, 'fit')[2])


def _video_has_scaling(defaults):
    return ('video', 'scaling') in defaults


class ScalingParseUITest(IniCase):
    """[video] scaling= no leitor da UI (hw/config_ini.c). Pula ate o campo existir."""

    def setUp(self):
        super().setUp()
        if not _video_has_scaling(self.defaults()):
            self.skipTest('[video] scaling= not implemented in hw/config_ini.c yet (only integer_scale=0/1); '
                          'tests activate automatically when cfg_save writes it')

    def scaling_of(self, text):  # noqa: D401
        _, cfg = self.roundtrip(text)
        return str(cfg.get(('video', 'scaling'))).lower()

    def test_default_is_fit(self):
        self.assertEqual(str(self.defaults().get(('video', 'scaling'))).lower(), 'fit')

    def test_valid_values_case_insensitive(self):
        for v in ('integer', 'fit', 'stretch', 'INTEGER', 'Fit', 'STRETCH'):
            with self.subTest(v=v):
                self.assertEqual(self.scaling_of(f'[video]\nscaling={v}\n'), v.lower())

    def test_invalid_values_fall_back_to_fit(self):
        for v in ('bilinear', '', '2', 'integer2', 'fit stretch', 'linear', '-1'):
            with self.subTest(v=v):
                self.assertEqual(self.scaling_of(f'[video]\nscaling={v}\n'), 'fit')

    def test_invalid_scaling_does_not_hide_a_valid_legacy_key(self):
        self.assertEqual(self.scaling_of('[video]\nscaling=bilinear\ninteger_scale=1\n'), 'integer')

    def test_legacy_integer_scale_maps_to_scaling(self):
        self.assertEqual(self.scaling_of('[video]\ninteger_scale=1\n'), 'integer')
        self.assertEqual(self.scaling_of('[video]\ninteger_scale=0\n'), 'fit')
        self.assertEqual(self.scaling_of('integer_scale=0\n'), 'fit')                # v1 solto
        self.assertEqual(self.scaling_of('[kirby]\nscale=3\n'), 'fit')                # v1 sem a chave: padrao

    def test_scaling_wins_over_legacy_key(self):
        self.assertEqual(self.scaling_of('[video]\ninteger_scale=0\nscaling=stretch\n'), 'stretch')
        self.assertEqual(self.scaling_of('[video]\nscaling=fit\ninteger_scale=1\n'), 'fit')

    def test_roundtrip_is_stable(self):
        for v in ('integer', 'fit', 'stretch'):
            once = self.scaling_of(f'[video]\nscaling={v}\n')
            with open(os.path.join(self.dir, 'out.ini'), encoding='utf-8') as f:
                again = self.scaling_of(f.read())
            self.assertEqual((once, again), (v, v))


RT_SCALING_C = r"""
#include <stdio.h>
#include "RT_INPUT_C_PATH"
int main(int argc, char **argv) {
    rt_cfg_load(argv[1]);
    printf("%s\n", _Generic((rt_cfg.scaling), int: "", default: (const char *)rt_cfg.scaling));
    printf("%d\n", _Generic((rt_cfg.scaling), int: rt_cfg.scaling, default: -1));
    return 0;
}
"""


class ScalingParseParityTest(IniCase):
    """scaling= no JOGO (rt_input.c) e no LAUNCHER (flow.py) tem que dar o valor da UI."""
    CORPUS = ['[video]\nscaling=fit\n', '[video]\nscaling=STRETCH\n', '[video]\nscaling=bilinear\n',
              '[video]\ninteger_scale=0\n', '[video]\ninteger_scale=1\n', 'scale=3\n',
              '[video]\ninteger_scale=0\nscaling=stretch\n', '']

    def setUp(self):
        super().setUp()
        if not _video_has_scaling(self.defaults()):
            self.skipTest('[video] scaling= not implemented in hw/config_ini.c yet (reference reader missing)')

    def ui(self, text):
        _, cfg = self.roundtrip(text)
        return str(cfg.get(('video', 'scaling'))).lower()

    def test_game_reader_matches_ui(self):
        with open(os.path.join(ROOT, 'recomp', 'runtime', 'rt_input.c'), encoding='utf-8', errors='replace') as f:
            if 'scaling' not in f.read():
                self.skipTest('rt_input.c (game) does not parse scaling= yet')
        cc = _cc_candidates()
        src = os.path.join(self.dir, 'rt_scaling.c')
        path = os.path.join(ROOT, 'recomp', 'runtime', 'rt_input.c').replace('\\', '/')
        with open(src, 'w') as f:
            f.write(RT_SCALING_C.replace('RT_INPUT_C_PATH', path))
        exe = os.path.join(self.dir, 'rt_scaling.exe')
        for c in cc:
            r = subprocess.run(c + ['-O1', '-w', src, '-o', exe], capture_output=True, text=True)
            if r.returncode == 0:
                break
        else:
            self.skipTest('could not compile the game reader with a scaling field: ' + r.stderr[-200:])
        names = {0: 'integer', 1: 'fit', 2: 'stretch'}   # se for enum numerico: ordem integer, fit, stretch
        for text in self.CORPUS:
            with self.subTest(file=text):
                p = self.write(text, 'g.ini')
                out = subprocess.run([exe, p], capture_output=True, text=True).stdout.split('\n')
                got = out[0].strip().lower() or names.get(int(out[1] or -1), '?')
                self.assertEqual(got, self.ui(text))

    def test_launcher_reader_matches_ui(self):
        sys.path.insert(0, os.path.join(ROOT, 'launcher'))
        try:
            import flow
        finally:
            sys.path.pop(0)
        fn = getattr(flow, 'read_scaling', None)
        if fn is None:
            self.skipTest('launcher/flow.py has no read_scaling(ini_path) yet')
        for text in self.CORPUS:
            with self.subTest(file=text):
                self.assertEqual(str(fn(self.write(text, 'f.ini'))).lower(), self.ui(text))


SDL_RECT_C = r"""
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
/*FUNCS*/
int main(int argc, char **argv) {
    int W = atoi(argv[1]), H = atoi(argv[2]), mode = atoi(argv[3]);
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("ERR init %s\n", SDL_GetError()); return 1; }
    SDL_Window *w = SDL_CreateWindow("t", 0, 0, W, H, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = w ? SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE) : NULL;
    if (!r) { printf("ERR renderer %s\n", SDL_GetError()); return 1; }
    int ow, oh; SDL_GetRendererOutputSize(r, &ow, &oh);
    rt_apply_scaling(r, mode);
    int x, y, ww, hh;
    dest_rect(r, &x, &y, &ww, &hh);
    printf("%d %d %d %d %d %d\n", ow, oh, x, y, ww, hh);
    return 0;
}
"""


def _extract_c_function(text, signature_regex):
    """Texto de uma funcao C (do inicio da assinatura ate a `}` na coluna 0)."""
    m = re.search(signature_regex, text, re.M)
    if not m:
        return None
    end = text.index('\n}\n', m.start()) + 3
    return text[m.start():end]


def _find_sdl():
    cands = [os.environ.get('SDL2_DIR'), os.path.join(ROOT, 'sdl2'), os.path.join(ROOT, 'tools', 'sdl2'),
             'C:/libs/SDL2-2.32.2/x86_64-w64-mingw32', 'C:/libs/SDL2-2.32.2']
    for c in cands:
        if not c:
            continue
        for inc in (os.path.join(c, 'include', 'SDL2'), os.path.join(c, 'include')):
            if os.path.exists(os.path.join(inc, 'SDL.h')):
                lib = os.path.join(c, 'lib')
                dll = os.path.join(c, 'bin', 'SDL2.dll')
                if os.path.isdir(lib):
                    return inc, lib, os.path.dirname(dll) if os.path.exists(dll) else None
    return None


class GameDestRectTest(unittest.TestCase):
    """Retangulo de destino do JOGO (recomp/runtime/rt_window.c: rt_apply_scaling + dest_rect, via SDL
    com o driver `dummy`) contra a referencia, em 4 resolucoes x 3 modos. Tolerancia: 1 px (arredondamento
    do SDL)."""

    @classmethod
    def setUpClass(cls):
        cls.exe, cls.skip = None, None
        if SKIP_REASON:
            cls.skip = SKIP_REASON
            return
        sdl = _find_sdl()
        if not sdl:
            cls.skip = 'SDL2 development files not found (set SDL2_DIR or install to C:/libs/SDL2-2.32.2)'
            return
        with open(os.path.join(ROOT, 'recomp', 'runtime', 'rt_window.c'), encoding='utf-8', errors='replace') as f:
            text = f.read()
        apply_fn = _extract_c_function(text, r'^void rt_apply_scaling\(')
        rect_fn = _extract_c_function(text, r'^static void dest_rect\(')
        if not apply_fn or not rect_fn:
            cls.skip = 'rt_apply_scaling()/dest_rect() not found in recomp/runtime/rt_window.c (moved or renamed?)'
            return
        inc, lib, bindir = sdl
        d = tempfile.mkdtemp(dir=TMP)
        src = os.path.join(d, 'rect_sdl.c')
        with open(src, 'w') as f:
            f.write(SDL_RECT_C.replace('/*FUNCS*/', apply_fn + '\n' + rect_fn))
        exe = os.path.join(d, 'rect_sdl.exe' if os.name == 'nt' else 'rect_sdl')
        err = ''
        for cc in _cc_candidates():
            r = subprocess.run(cc + ['-O1', '-w', '-I', inc, src, '-L', lib, '-lSDL2', '-o', exe],
                               capture_output=True, text=True)
            if r.returncode == 0:
                cls.exe, cls.bindir = exe, bindir
                return
            err = r.stderr[-200:]
        cls.skip = 'could not compile the SDL rect harness: ' + err

    def rect(self, w, h, mode):
        env = dict(os.environ)
        if self.bindir:
            env['PATH'] = self.bindir + os.pathsep + env.get('PATH', '')
        r = subprocess.run([self.exe, str(w), str(h), str(mode)], capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return tuple(int(v) for v in r.stdout.split())

    def setUp(self):
        if self.skip:
            self.skipTest(self.skip)

    def test_game_rect_matches_reference(self):
        for (w, h), modes in RECT_TABLE.items():
            for code, mode in enumerate(('integer', 'fit', 'stretch')):
                with self.subTest(res=f'{w}x{h}', mode=mode):
                    ow, oh, x, y, rw, rh = self.rect(w, h, code)
                    self.assertEqual((ow, oh), (w, h), 'dummy window did not get the requested size')
                    for got, want, name in zip((x, y, rw, rh), modes[mode], 'x y w h'.split()):
                        self.assertLessEqual(abs(got - want), 1, f'{name}: SDL gave {(x, y, rw, rh)}, want {modes[mode]}')

    def test_modes_are_distinct_where_the_contract_says_so(self):
        # 1920x1080: integer (1440x960) != fit (1620x1080) != stretch (1920x1080)
        sizes = {m: self.rect(1920, 1080, c)[4:6] for c, m in enumerate(('integer', 'fit', 'stretch'))}
        self.assertEqual(len(set(sizes.values())), 3, sizes)
        # 2560x1440 = 9x exato: integer == fit (nao ha borda extra em nenhum dos dois)
        self.assertEqual(self.rect(2560, 1440, 0)[4:6], self.rect(2560, 1440, 1)[4:6])


class UIScalingModeTest(unittest.TestCase):
    """A UI do launcher tambem tem que refletir os 3 modos (hoje: so o checkbox INTEGER SCALING).
    Pula ate `launcher_ui.c` conhecer `scaling`; entao exige que o desenho/preview use o MESMO modo."""

    def test_launcher_ui_exposes_the_three_modes(self):
        with open(os.path.join(HW, 'launcher_ui.c'), encoding='utf-8', errors='replace') as f:
            src = f.read()
        if 'scaling' not in src:
            self.skipTest('launcher_ui.c still has only the INTEGER SCALING checkbox (3-mode selector pending)')
        for word in ('integer', 'fit', 'stretch'):
            self.assertIn(word, src.lower())


# ----------------------------- [keyboard]/[controller] e analogico: SDL real com camada falsa -------------------
# O harness inclui recomp/runtime/rt_input.c (RT_WINDOW) trocando, por macro, as chamadas de controle/teclado do
# SDL por falsas controlaveis por stdin. Assim roda o rt_input_init()/rt_input_poll() REAIS, sem hardware.
SDL_INPUT_C = r"""
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int fk_joys = 0;
static int fk_axis[2];
static Uint8 fk_btn[SDL_CONTROLLER_BUTTON_MAX + 8];
static Uint8 fk_keys[SDL_NUM_SCANCODES];
static char fk_obj;
static int fk_NumJoysticks(void) { return fk_joys; }
static SDL_bool fk_IsGameController(int i) { return SDL_TRUE; }
static SDL_GameController *fk_Open(int i) { return (SDL_GameController *)&fk_obj; }
static void fk_Close(SDL_GameController *c) {}
static SDL_bool fk_Attached(SDL_GameController *c) { return fk_joys > 0 ? SDL_TRUE : SDL_FALSE; }
static Sint16 fk_Axis(SDL_GameController *c, SDL_GameControllerAxis a) {
    if (fk_joys <= 0) return 0;
    return a == SDL_CONTROLLER_AXIS_LEFTX ? (Sint16)fk_axis[0] : a == SDL_CONTROLLER_AXIS_LEFTY ? (Sint16)fk_axis[1] : 0;
}
static Uint8 fk_Button(SDL_GameController *c, SDL_GameControllerButton b) { return fk_joys > 0 ? fk_btn[b] : 0; }
static const Uint8 *fk_Keys(int *n) { if (n) *n = SDL_NUM_SCANCODES; return fk_keys; }
#define SDL_NumJoysticks fk_NumJoysticks
#define SDL_IsGameController fk_IsGameController
#define SDL_GameControllerOpen fk_Open
#define SDL_GameControllerClose fk_Close
#define SDL_GameControllerGetAttached fk_Attached
#define SDL_GameControllerGetAxis fk_Axis
#define SDL_GameControllerGetButton fk_Button
#define SDL_GetKeyboardState fk_Keys
#define RT_WINDOW
#include "RT_INPUT_C_PATH"
static const char *BN[NBTN] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L"};
static int scan_of(const char *name) {
    SDL_Keycode k = SDL_GetKeyFromName(name);
    SDL_Scancode sc = k != SDLK_UNKNOWN ? SDL_GetScancodeFromKey(k) : SDL_SCANCODE_UNKNOWN;
    if (sc == SDL_SCANCODE_UNKNOWN) sc = SDL_GetScancodeFromName(name);
    return (int)sc;
}
int main(void) {
    char line[600], cmd[40], arg[520];
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        arg[0] = 0;
        if (sscanf(line, "%39s %519[^\n]", cmd, arg) < 1) continue;
        if (!strcmp(cmd, "load")) {
            int rc = rt_cfg_load(arg);
            input_ready = 0;
            rt_input_init();
            printf("rc=%d\n", rc);
        } else if (!strcmp(cmd, "dump")) {
            for (int i = 0; i < NBTN; i++)
                printf("%s %d %d\n", BN[i], (int)kb_sc[i], (int)pad_btn[i]);
        } else if (!strcmp(cmd, "cfg")) {
            printf("deadzone=%d stick=%d\n", rt_cfg.deadzone, rt_cfg.stick_as_dpad);
        } else if (!strcmp(cmd, "scan")) {
            printf("%d\n", scan_of(arg));
        } else if (!strcmp(cmd, "pad")) {
            printf("%d\n", (int)SDL_GameControllerGetButtonFromString(arg));
        } else if (!strcmp(cmd, "joy")) {
            fk_joys = atoi(arg);
        } else if (!strcmp(cmd, "axis")) {
            int x = 0, y = 0;
            sscanf(arg, "%d %d", &x, &y);
            fk_axis[0] = x; fk_axis[1] = y;
        } else if (!strcmp(cmd, "btn")) {
            char nm[64]; int v = 0;
            sscanf(arg, "%63s %d", nm, &v);
            int b = (int)SDL_GameControllerGetButtonFromString(nm);
            if (b >= 0) fk_btn[b] = (Uint8)v;
        } else if (!strcmp(cmd, "key")) {
            char nm[64]; int v = 0;
            sscanf(arg, "%63s %d", nm, &v);
            int sc = scan_of(nm);
            if (sc > 0) fk_keys[sc] = (Uint8)v;
        } else if (!strcmp(cmd, "poll")) {
            int cnt = atoi(arg) > 0 ? atoi(arg) : 1;
            unsigned v = 0;
            for (int i = 0; i < cnt; i++) v = rt_input_poll();
            printf("poll=%04x\n", v);
        }
        fflush(stdout);
    }
    return 0;
}
"""

GBA_BITS = ['A', 'B', 'SELECT', 'START', 'RIGHT', 'LEFT', 'UP', 'DOWN', 'R', 'L']   # ordem de KEYINPUT em rt_input.c
UNKNOWN_SC, INVALID_BTN = 0, -1
INPUT_EXE, INPUT_BIN, INPUT_SKIP = None, None, None


def _build_input_harness():
    global INPUT_EXE, INPUT_BIN, INPUT_SKIP
    if INPUT_EXE or INPUT_SKIP:
        return
    if SKIP_REASON:
        INPUT_SKIP = SKIP_REASON
        return
    sdl = _find_sdl()
    if not sdl:
        INPUT_SKIP = 'SDL2 development files not found (set SDL2_DIR or install to C:/libs/SDL2-2.32.2)'
        return
    inc, lib, bindir = sdl
    d = tempfile.mkdtemp(dir=TMP)
    src = os.path.join(d, 'input_sdl.c')
    path = os.path.join(ROOT, 'recomp', 'runtime', 'rt_input.c').replace('\\', '/')
    with open(src, 'w') as f:
        f.write(SDL_INPUT_C.replace('RT_INPUT_C_PATH', path))
    exe = os.path.join(d, 'input_sdl.exe' if os.name == 'nt' else 'input_sdl')
    err = ''
    for cc in _cc_candidates():
        r = subprocess.run(cc + ['-O1', '-w', '-I', inc, src, '-L', lib, '-lSDL2', '-o', exe],
                           capture_output=True, text=True)
        if r.returncode == 0:
            INPUT_EXE, INPUT_BIN = exe, bindir
            return
        err = r.stderr[-250:]
    INPUT_SKIP = 'could not compile rt_input.c with the fake SDL layer: ' + err


class InputCase(unittest.TestCase):
    """Base: roda o harness (rt_input.c real + SDL falso) com um script de comandos."""

    def setUp(self):
        _build_input_harness()
        if INPUT_SKIP:
            self.skipTest(INPUT_SKIP)
        self.dir = tempfile.mkdtemp(dir=TMP)

    def ini(self, text, name='k.ini'):
        p = os.path.join(self.dir, name)
        with open(p, 'wb') as f:
            f.write(text.encode('utf-8'))
        return p

    def run_script(self, lines):
        env = dict(os.environ)
        if INPUT_BIN:
            env['PATH'] = INPUT_BIN + os.pathsep + env.get('PATH', '')
        r = subprocess.run([INPUT_EXE], input='\n'.join(lines) + '\n', capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip().split('\n') if r.stdout.strip() else []

    def scan(self, name):
        return int(self.run_script([f'scan {name}'])[0])

    def pad(self, name):
        return int(self.run_script([f'pad {name}'])[0])

    def effective(self, ini_text):
        """{botao GBA: (scancode, botao do controle)} que o JOGO realmente usa para esse arquivo."""
        out = self.run_script([f'load {self.ini(ini_text)}', 'dump'])
        res = {}
        for line in out[1:]:
            b, sc, pad = line.split()
            res[b] = (int(sc), int(pad))
        return res

    def pressed(self, value_hex):
        v = int(value_hex.split('=')[1], 16)
        return {n for i, n in enumerate(GBA_BITS) if not (v >> i) & 1}


DEFAULT_KB = {'A': 'X', 'B': 'Z', 'L': 'A', 'R': 'S', 'START': 'Return', 'SELECT': 'Backspace',
              'UP': 'Up', 'DOWN': 'Down', 'LEFT': 'Left', 'RIGHT': 'Right'}
DEFAULT_PAD = {'A': 'b', 'B': 'a', 'L': 'leftshoulder', 'R': 'rightshoulder', 'START': 'start', 'SELECT': 'back',
               'UP': 'dpup', 'DOWN': 'dpdown', 'LEFT': 'dpleft', 'RIGHT': 'dpright'}
KEY_SECTION = {'A': 'a', 'B': 'b', 'L': 'l', 'R': 'r', 'START': 'start', 'SELECT': 'select', 'UP': 'up',
               'DOWN': 'down', 'LEFT': 'left', 'RIGHT': 'right'}


class KeyboardControllerBindingTest(InputCase):
    """[keyboard] / [controller]: nomes SDL validos, invalidos, reservados, duplicados, v1/v2 (jogo, SDL real)."""

    def setUp(self):
        super().setUp()
        self.def_kb = {b: self.scan(n) for b, n in DEFAULT_KB.items()}
        self.def_pad = {b: self.pad(n) for b, n in DEFAULT_PAD.items()}

    def test_every_default_name_is_a_real_sdl_name(self):
        for b in GBA_BITS:
            with self.subTest(button=b):
                self.assertNotEqual(self.def_kb[b], UNKNOWN_SC, DEFAULT_KB[b])
                self.assertNotEqual(self.def_pad[b], INVALID_BTN, DEFAULT_PAD[b])

    def test_defaults_without_any_file_or_with_v1_file(self):
        for text in ('', 'scale=5\nmute=1\n', '[kirby]\nscale=2\n'):
            with self.subTest(file=text):
                eff = self.effective(text)
                for b in GBA_BITS:
                    self.assertEqual(eff[b], (self.def_kb[b], self.def_pad[b]), b)

    def test_valid_custom_keyboard_names(self):
        for name in ('Q', 'q', 'Space', 'Tab', 'Left Shift', 'F5', '1', 'Keypad 5', 'Right Ctrl'):
            with self.subTest(name=name):
                want = self.scan(name)
                self.assertNotEqual(want, UNKNOWN_SC)
                eff = self.effective(f'[keyboard]\na={name}\n')
                self.assertEqual(eff['A'][0], want)
                for b in GBA_BITS:           # os outros ficam como estavam
                    if b != 'A':
                        self.assertEqual(eff[b][0], self.def_kb[b], b)

    def test_valid_controller_names(self):
        for name in ('x', 'y', 'guide', 'leftstick', 'rightstick'):
            with self.subTest(name=name):
                eff = self.effective(f'[controller]\na={name}\n')
                self.assertEqual(eff['A'][1], self.pad(name))

    def test_duplicate_controller_binding_resets_only_the_controller_map(self):
        # `back` ja e o padrao de SELECT: duplicata -> mapa do CONTROLE volta ao padrao; teclado intacto
        eff = self.effective('[keyboard]\na=Q\n[controller]\na=back\n')
        self.assertEqual(eff['A'], (self.scan('Q'), self.def_pad['A']))
        for b in GBA_BITS:
            self.assertEqual(eff[b][1], self.def_pad[b], b)

    def test_duplicate_keyboard_binding_does_not_touch_the_controller_map(self):
        eff = self.effective('[keyboard]\na=Z\n[controller]\na=x\n')
        self.assertEqual(eff['A'], (self.def_kb['A'], self.pad('x')))

    def test_names_are_case_insensitive(self):
        for kb in ('RETURN', 'return', 'ReTuRn'):
            with self.subTest(kb=kb):
                self.assertEqual(self.effective(f'[keyboard]\nstart={kb}\n')['START'][0], self.def_kb['START'])
        for pd in ('LEFTSHOULDER', 'LeftShoulder'):
            with self.subTest(pad=pd):
                self.assertEqual(self.effective(f'[controller]\nl={pd}\n')['L'][1], self.def_pad['L'])

    def test_reserved_keys_fall_back_to_the_default_key(self):
        for name in ('Escape', 'escape', 'F11', 'F1', 'f1'):
            with self.subTest(name=name):
                eff = self.effective(f'[keyboard]\na={name}\n')
                self.assertEqual(eff['A'][0], self.def_kb['A'])

    def test_reserved_keys_stay_unbound_for_every_gba_button(self):
        for b, key in KEY_SECTION.items():
            with self.subTest(button=b):
                eff = self.effective(f'[keyboard]\n{key}=F11\n')
                self.assertNotIn(eff[b][0], (self.scan('F11'), self.scan('F1'), self.scan('Escape')))

    def test_duplicate_keyboard_binding_resets_the_whole_map(self):
        eff = self.effective('[keyboard]\na=Z\nl=Q\n')       # Z ja e o padrao de B
        for b in GBA_BITS:
            self.assertEqual(eff[b][0], self.def_kb[b], b)

    def test_two_distinct_custom_keys_are_kept(self):
        eff = self.effective('[keyboard]\na=Q\nb=W\n')
        self.assertEqual((eff['A'][0], eff['B'][0]), (self.scan('Q'), self.scan('W')))

    @unittest.expectedFailure
    def test_unknown_keyboard_name_falls_back_to_the_default_key(self):
        """FALHA CONHECIDA (rt_input.c rt_input_init): nome que o SDL nao conhece deixa o botao SEM tecla
        (kb_sc = UNKNOWN) em vez de voltar ao padrao ("invalid value falls back to its default")."""
        for name in ('NotAKey', 'Q W E', 'Ctrl+Z', 'xx'):
            with self.subTest(name=name):
                eff = self.effective(f'[keyboard]\na={name}\n')
                self.assertEqual(eff['A'][0], self.def_kb['A'])

    @unittest.expectedFailure
    def test_unknown_controller_name_falls_back_to_the_default_button(self):
        """FALHA CONHECIDA (rt_input.c): `a=banana` deixa o A do controle sem botao (INVALID)."""
        for name in ('banana', 'button1', 'triangle'):
            with self.subTest(name=name):
                eff = self.effective(f'[controller]\na={name}\n')
                self.assertEqual(eff['A'][1], self.def_pad['A'])

    def test_keyboard_and_controller_drive_the_same_gba_bit(self):
        out = self.run_script([f'load {self.ini("[keyboard]" + chr(10) + "a=Q" + chr(10))}', 'joy 1', 'poll 2',
                               'key Q 1', 'poll 1', 'key Q 0', 'btn b 1', 'poll 1', 'btn b 0', 'poll 1'])
        polls = [l for l in out if l.startswith('poll=')]
        self.assertEqual([self.pressed(p) for p in polls], [set(), {'A'}, {'A'}, set()])

    def test_ui_and_game_resolve_the_same_keys_and_buttons(self):
        """Leitor da UI (config_ini.c, nomes) -> SDL  ==  o que o jogo usa de fato, para varios arquivos."""
        if EXE is None:
            self.skipTest(SKIP_REASON or 'config_ini harness not built')
        corpus = ['', '[keyboard]\na=Q\nb=W\nl=1\n', '[keyboard]\na=Escape\n', '[keyboard]\na=Z\nl=Q\n',
                  '[controller]\na=x\nb=y\nl=guide\n', 'scale=4\n', '[KEYBOARD]\nSTART=space\n',
                  '[keyboard]\nup=W\ndown=S\nleft=A\nright=D\n']
        for text in corpus:
            with self.subTest(file=text):
                src = os.path.join(self.dir, 'ui_in.ini')
                with open(src, 'w', encoding='utf-8') as f:
                    f.write(text)
                out = os.path.join(self.dir, 'ui_out.ini')
                subprocess.run([EXE, 'rt', src, out], check=False)
                with open(out, encoding='utf-8', errors='replace') as f:
                    ui = parse_ini(f.read())
                game = self.effective(text)
                for b, key in KEY_SECTION.items():
                    kb_name = ui.get(('keyboard', key), DEFAULT_KB[b])
                    pad_name = ui.get(('controller', key), DEFAULT_PAD[b])
                    self.assertEqual(self.scan(kb_name), game[b][0], f'{b} keyboard {kb_name}')
                    self.assertEqual(self.pad(pad_name), game[b][1], f'{b} controller {pad_name}')

    def test_launcher_reader_matches_for_keys_and_buttons(self):
        sys.path.insert(0, os.path.join(ROOT, 'launcher'))
        try:
            import flow
        finally:
            sys.path.pop(0)
        fn = getattr(flow, 'read_bindings', None)
        if fn is None:
            self.skipTest('launcher/flow.py does not read key/button bindings (only show/scaling); '
                          'keys are kept across F1 -> Play because the launcher never rewrites them')
        text = '[keyboard]\na=Q\n[controller]\nb=x\n'
        got = fn(self.ini(text))
        self.assertEqual(str(got['keyboard']['a']).lower(), 'q')


class AnalogStickTest(InputCase):
    """Analogico como direcional (stick_as_dpad/deadzone), pelo rt_input_poll() real com SDL falso."""
    DZ = 8000   # entra em |v| >= DZ (inclusivo) e so solta abaixo de 85% de DZ (histerese)

    def stick(self, x, y, dz=None, stick=1, extra=None, polls=1):
        """Conecta um controle, aplica (x, y) e devolve o conjunto de botoes GBA pressionados."""
        dz = self.DZ if dz is None else dz
        text = f'[controller]\nstick_as_dpad={stick}\ndeadzone={dz}\n'
        cmds = [f'load {self.ini(text)}', 'joy 1'] + (extra or []) + [f'axis {x} {y}', f'poll {polls}']
        return self.pressed([l for l in self.run_script(cmds) if l.startswith('poll=')][-1])

    def test_below_the_deadzone_is_ignored(self):
        d = self.DZ - 1
        for x, y in ((0, 0), (d, 0), (-d, 0), (0, d), (0, -d), (d, d), (5000, -5000), (-d, d)):
            with self.subTest(axis=(x, y)):
                self.assertEqual(self.stick(x, y), set())

    def test_at_and_above_the_deadzone_presses_the_direction(self):
        for d in (self.DZ, self.DZ + 1):
            with self.subTest(value=d):
                self.assertEqual(self.stick(d, 0), {'RIGHT'})
                self.assertEqual(self.stick(-d, 0), {'LEFT'})
                self.assertEqual(self.stick(0, d), {'DOWN'})
                self.assertEqual(self.stick(0, -d), {'UP'})
        self.assertEqual(self.stick(32767, 0), {'RIGHT'})
        self.assertEqual(self.stick(-32768, 0), {'LEFT'})

    def test_diagonals(self):
        self.assertEqual(self.stick(20000, 20000), {'RIGHT', 'DOWN'})
        self.assertEqual(self.stick(20000, -20000), {'RIGHT', 'UP'})
        self.assertEqual(self.stick(-20000, 20000), {'LEFT', 'DOWN'})
        self.assertEqual(self.stick(-20000, -20000), {'LEFT', 'UP'})
        # um eixo dentro da zona morta: so o outro vale (sem diagonal "falsa")
        self.assertEqual(self.stick(20000, self.DZ - 1), {'RIGHT'})
        self.assertEqual(self.stick(-(self.DZ - 1), -20000), {'UP'})

    def test_stick_as_dpad_off_ignores_the_stick_but_not_the_dpad(self):
        self.assertEqual(self.stick(32767, -32768, stick=0), set())
        self.assertEqual(self.stick(0, 0, stick=0, extra=['btn dpleft 1']), {'LEFT'})

    def test_stick_and_opposite_dpad_cancel_each_other(self):
        self.assertEqual(self.stick(20000, 0, extra=['btn dpleft 1']), set())

    def test_deadzone_zero_reacts_to_any_deflection_but_not_to_rest(self):
        self.assertEqual(self.stick(0, 0, dz=0), set())
        self.assertEqual(self.stick(1, 0, dz=0), {'RIGHT'})
        self.assertEqual(self.stick(-1, 0, dz=0), {'LEFT'})

    def test_deadzone_at_the_upper_limit_only_full_deflection_counts(self):
        out = self.run_script([f'load {self.ini("[controller]" + chr(10) + "deadzone=32767" + chr(10))}', 'cfg'])
        self.assertIn('deadzone=32767', out[-1])
        self.assertEqual(self.stick(32766, 0, dz=32767), set())
        self.assertEqual(self.stick(-32766, 0, dz=32767), set())
        # deflexao total conta nos DOIS sentidos (simetrico, inclusive o -32768 do int16)
        self.assertEqual(self.stick(32767, 0, dz=32767), {'RIGHT'})
        self.assertEqual(self.stick(-32768, 0, dz=32767), {'LEFT'})
        self.assertEqual(self.stick(0, 32767, dz=32767), {'DOWN'})
        self.assertEqual(self.stick(0, -32768, dz=32767), {'UP'})

    def test_deadzone_out_of_range_in_the_file_falls_back(self):
        for v in ('32768', '-1', 'abc', ''):
            with self.subTest(v=v):
                out = self.run_script([f'load {self.ini("[controller]" + chr(10) + "deadzone=" + v + chr(10))}', 'cfg'])
                self.assertIn('deadzone=8000', out[-1])

    def test_hysteresis_prevents_flicker_around_the_limit(self):
        """Entra em |v| >= dz; depois de apertado so solta abaixo de 85% de dz (rt_input.c `hyst`)."""
        cmds = [f'load {self.ini("[controller]" + chr(10) + "deadzone=8000" + chr(10))}', 'joy 1']
        seq = [9000, 7900, 8100, 7900, 8100, 7950, 8050]
        for v in seq:
            cmds += [f'axis {v} 0', 'poll 1']
        cmds += ['axis 0 0', 'poll 1']
        states = [('RIGHT' in self.pressed(l)) for l in self.run_script(cmds) if l.startswith('poll=')]
        transitions = sum(1 for a, b in zip(states, states[1:]) if a != b)
        self.assertEqual(states[0], True)
        self.assertEqual(transitions, 1, f'direction flickered: {states}')   # so 1: o soltar em (0, 0)
        self.assertEqual(states[-1], False)

    def test_hysteresis_release_threshold_and_re_arm(self):
        cmds = [f'load {self.ini("[controller]" + chr(10) + "deadzone=8000" + chr(10))}', 'joy 1']
        for v in (8000, 7000, 6800, 6799, 7900, 7999, 8000):   # 85% de 8000 = 6800
            cmds += [f'axis {v} 0', 'poll 1']
        states = [('RIGHT' in self.pressed(l)) for l in self.run_script(cmds) if l.startswith('poll=')]
        # aperta em 8000; segura em 7000 e 6800; solta em 6799; so volta a apertar em >= 8000
        self.assertEqual(states, [True, True, True, False, False, False, True])

    def test_hysteresis_works_per_axis_and_per_direction(self):
        cmds = [f'load {self.ini("[controller]" + chr(10) + "deadzone=8000" + chr(10))}', 'joy 1',
                'axis 9000 -9000', 'poll 1', 'axis 7000 -7000', 'poll 1', 'axis -9000 7000', 'poll 1']
        polls = [self.pressed(l) for l in self.run_script(cmds) if l.startswith('poll=')]
        self.assertEqual(polls[0], {'RIGHT', 'UP'})
        self.assertEqual(polls[1], {'RIGHT', 'UP'})          # ainda dentro da histerese
        self.assertEqual(polls[2], {'LEFT'})                 # X inverteu (entra em LEFT); Y passou para +7000 (< 8000): solta o UP e nao entra em DOWN

    def test_disconnect_in_the_middle_of_a_movement_releases_the_direction(self):
        cmds = [f'load {self.ini("[controller]" + chr(10) + "deadzone=8000" + chr(10))}', 'joy 1',
                'axis 20000 -20000', 'poll 2', 'joy 0', 'poll 1']
        polls = [self.pressed(l) for l in self.run_script(cmds) if l.startswith('poll=')]
        self.assertEqual(polls[0], {'RIGHT', 'UP'})
        self.assertEqual(polls[1], set(), 'direction stayed latched right after the controller vanished')

    def test_disconnect_then_reconnect_works_without_restarting(self):
        cmds = [f'load {self.ini("[controller]" + chr(10) + "deadzone=8000" + chr(10))}', 'joy 1',
                'axis 20000 0', 'poll 2', 'joy 0', 'poll 40', 'joy 1', 'poll 40']
        polls = [self.pressed(l) for l in self.run_script(cmds) if l.startswith('poll=')]
        self.assertEqual(polls, [{'RIGHT'}, set(), {'RIGHT'}])

    def test_keyboard_keeps_working_while_the_controller_is_gone(self):
        cmds = [f'load {self.ini("")}', 'joy 1', 'poll 2', 'joy 0', 'poll 40', 'key Right 1', 'poll 1']
        last = [l for l in self.run_script(cmds) if l.startswith('poll=')][-1]
        self.assertEqual(self.pressed(last), {'RIGHT'})


if __name__ == '__main__':
    unittest.main()
