"""QA do gerenciamento de save (Poppy) - contrato em NOTES.md, "Gerenciamento de save: plano e contrato".

Rodar:  python -m unittest tests.test_save_qa -v          (da raiz do repo)

Blocos:
  1. SaveModel + ModelSpecTest: o contrato como codigo (tamanho 32768, validacao do import, nomes e rotacao
     de 10 backups). Roda sempre; e o oraculo do bloco do launcher.
  2. LauncherSaveContractTest: backup/rotacao/import/export do launcher (Python). PULA ate existir
     launcher/savemgr.py com as funcoes da secao "Interface de teste do save" do NOTES.
  3. RuntimeSaveTest: roda o recomp/runtime/rt_save.c REAL (relogio falso, fwrite interceptado para
     simular kill -9 no meio da gravacao): criacao, ida e volta, debounce de ~1 s, teto de 5 s, rajada
     coalescida, flush ao sair, atomicidade, falha + nova tentativa, caminhos com acento/espaco/nao-ANSI.
"""
import datetime as dt
import hashlib
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
SAVE_SIZE = 32768
SHA = '274b102b6d940f46861a92b4e65f89a51815c12c'
ROM_NAME = 'Kirby & The Amazing Mirror (USA)'
TMP = None
CC = None


# ============================ 1. o contrato como codigo ============================
class SaveModel:
    """Referencia do contrato (Riven/Nasus). Nada daqui depende do codigo do produto."""
    KEEP = 10
    DEBOUNCE_MS = 1000

    @staticmethod
    def validate_import(data):
        """('reject', motivo) | ('warn', motivo) | ('ok', '')."""
        if len(data) == 0:
            return 'reject', 'file is empty'
        if len(data) != SAVE_SIZE:
            return 'reject', f'wrong size: {len(data)} bytes (expected {SAVE_SIZE})'
        if data == b'\xff' * SAVE_SIZE or data == b'\x00' * SAVE_SIZE:
            return 'warn', 'save looks empty (all 0xFF/0x00)'
        return 'ok', ''

    @staticmethod
    def backup_name(rom_name, now):
        return f'{rom_name}_{now:%Y%m%d_%H%M%S}.sav'

    @classmethod
    def rotate(cls, names):
        """Mantem os KEEP mais recentes (ordem lexicografica == cronologica) e devolve (mantidos, removidos)."""
        ordered = sorted(names)
        return ordered[-cls.KEEP:], ordered[:-cls.KEEP]

    @classmethod
    def should_backup(cls, current, latest_backup):
        return latest_backup is None or current != latest_backup


class ModelSpecTest(unittest.TestCase):
    def test_validation_rules(self):
        v = SaveModel.validate_import
        self.assertEqual(v(b'')[0], 'reject')
        for n in (1, 100, 32767, 32769, 65536, 131072):
            self.assertEqual(v(b'\x11' * n)[0], 'reject', n)
        self.assertEqual(v(b'\xff' * SAVE_SIZE)[0], 'warn')
        self.assertEqual(v(b'\x00' * SAVE_SIZE)[0], 'warn')
        self.assertEqual(v(bytes(range(256)) * 128)[0], 'ok')

    def test_backup_names_sort_chronologically(self):
        base = dt.datetime(2026, 10, 15, 23, 59, 58)
        names = [SaveModel.backup_name(ROM_NAME, base + dt.timedelta(seconds=s)) for s in range(0, 400, 7)]
        self.assertEqual(names, sorted(names))
        self.assertTrue(re.fullmatch(r'.+_\d{8}_\d{6}\.sav', names[0]))

    def test_rotation_keeps_the_ten_most_recent(self):
        names = [f'k_20261015_{h:02d}0000.sav' for h in range(15)]
        kept, removed = SaveModel.rotate(names)
        self.assertEqual((len(kept), len(removed)), (10, 5))
        self.assertEqual(kept[0], names[5])
        self.assertEqual(SaveModel.rotate(names[:7]), (names[:7], []))


# ============================ 2. utilitarios de compilacao ============================
def _cc_list():
    out = []
    try:
        from recomp.toolchain import find_zig_cmd
        c = find_zig_cmd()
        if c:
            out.append(list(c))
    except Exception:
        pass
    venv_py = os.path.join(ROOT, '.venv', 'Scripts', 'python.exe')
    if os.path.exists(venv_py):
        out.append([venv_py, '-m', 'ziglang', 'cc'])
    dirs = [os.environ.get('PATH', '')]
    la = os.environ.get('LOCALAPPDATA')
    if la:
        pk = os.path.join(la, 'Microsoft', 'WinGet', 'Packages')
        if os.path.isdir(pk):
            for d in os.listdir(pk):
                if d.startswith('BrechtSanders.WinLibs'):
                    for dp, _, files in os.walk(os.path.join(pk, d)):
                        if 'gcc.exe' in files:
                            dirs.append(dp)
    path = os.pathsep.join(dirs)
    for name in (os.environ.get('CC'), 'gcc', 'clang', 'cc'):
        if name and shutil.which(name, path=path):
            out.append([shutil.which(name, path=path)])
    return out


def _compile(src_path, exe, extra=()):
    err = 'no C compiler'
    for cc in _cc_list():
        r = subprocess.run(cc + ['-O1', '-w', *extra, src_path, '-o', exe], capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(exe):
            return None
        err = (r.stderr or r.stdout)[-300:]
    return err


def setUpModule():
    global TMP
    TMP = tempfile.mkdtemp(prefix='kirby_save_qa_')


def tearDownModule():
    shutil.rmtree(TMP, ignore_errors=True)


# ============================ 3. launcher (Python): backups, import, export ============================
def _load_savemgr():
    sys.path.insert(0, os.path.join(ROOT, 'launcher'))
    try:
        import importlib
        return importlib.import_module('savemgr'), None
    except ImportError:
        return None, 'launcher/savemgr.py not delivered yet (see NOTES "Interface de teste do save")'
    finally:
        sys.path.pop(0)


class LauncherSaveContractTest(unittest.TestCase):
    NEEDED = ('backup_save', 'import_save', 'export_save')

    def setUp(self):
        self.mod, why = _load_savemgr()
        if self.mod is None:
            self.skipTest(why)
        missing = [n for n in self.NEEDED if not hasattr(self.mod, n)]
        if missing:
            self.skipTest('savemgr.py lacks: ' + ', '.join(missing))
        base = tempfile.mkdtemp(dir=TMP)
        self.game = os.path.join(base, 'Meu Jogo João')            # acento + espaco
        self.bk = os.path.join(base, 'Kirby Recomp', 'Backups')
        os.makedirs(self.game)
        self.sav = os.path.join(self.game, ROM_NAME + '.sav')
        self.t0 = dt.datetime(2026, 10, 15, 12, 0, 0)

    # --- util ---
    def put(self, path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)

    def get(self, path):
        with open(path, 'rb') as f:
            return f.read()

    def backups(self, sha=SHA):
        d = os.path.join(self.bk, sha)
        return sorted(os.listdir(d)) if os.path.isdir(d) else []

    def backup(self, now):
        return self.mod.backup_save(self.sav, self.bk, SHA, ROM_NAME, now=now)

    def imp(self, src, now=None):
        r = self.mod.import_save(src, self.sav, self.bk, SHA, ROM_NAME, now=now or self.t0)
        ok, msg = (r if isinstance(r, tuple) else (bool(r), ''))
        return ok, str(msg)

    @staticmethod
    def blob(seed, n=SAVE_SIZE):
        random.seed(seed)
        return bytes(random.randrange(256) for _ in range(n))

    # --- backup ---
    def test_backup_is_made_when_the_save_changed_and_skipped_when_identical(self):
        self.put(self.sav, self.blob(1))
        first = self.backup(self.t0)
        self.assertTrue(first and os.path.exists(first))
        self.assertEqual(self.get(first), self.blob(1))
        self.assertIsNone(self.backup(self.t0 + dt.timedelta(seconds=30)), 'identical content must not duplicate')
        self.assertEqual(len(self.backups()), 1)
        self.put(self.sav, self.blob(2))
        self.assertTrue(self.backup(self.t0 + dt.timedelta(seconds=60)))
        self.assertEqual(len(self.backups()), 2)

    def test_backup_name_has_the_agreed_pattern_and_lives_under_the_rom_sha1(self):
        self.put(self.sav, self.blob(1))
        path = self.backup(self.t0)
        self.assertEqual(os.path.basename(os.path.dirname(path)), SHA)
        self.assertEqual(os.path.basename(path), SaveModel.backup_name(ROM_NAME, self.t0))

    def test_rotation_keeps_only_the_ten_most_recent(self):
        for i in range(13):
            self.put(self.sav, self.blob(100 + i))
            self.backup(self.t0 + dt.timedelta(minutes=i))
        names = self.backups()
        self.assertEqual(len(names), 10)
        want = [SaveModel.backup_name(ROM_NAME, self.t0 + dt.timedelta(minutes=i)) for i in range(3, 13)]
        self.assertEqual(names, want, 'the 3 oldest must be the ones removed')
        self.assertEqual(self.get(os.path.join(self.bk, SHA, names[-1])), self.blob(112))

    def test_two_different_saves_in_the_same_second_are_both_kept(self):
        self.put(self.sav, self.blob(1))
        a = self.backup(self.t0)
        self.put(self.sav, self.blob(2))
        b = self.backup(self.t0)
        self.assertTrue(a and b and a != b, 'second backup overwrote the first (same timestamp)')
        self.assertEqual({self.get(a), self.get(b)}, {self.blob(1), self.blob(2)})

    def test_backups_are_separated_per_rom(self):
        self.put(self.sav, self.blob(1))
        self.backup(self.t0)
        other = 'f' * 40
        self.mod.backup_save(self.sav, self.bk, other, ROM_NAME, now=self.t0)
        self.assertEqual((len(self.backups()), len(self.backups(other))), (1, 1))

    def test_backup_without_a_save_file_is_a_no_op(self):
        self.assertIsNone(self.backup(self.t0))
        self.assertEqual(self.backups(), [])

    # --- import ---
    def test_import_rejects_wrong_sizes_and_empty_files_without_touching_anything(self):
        original = self.blob(5)
        self.put(self.sav, original)
        for n in (0, 1, 100, 32767, 32769, 65536, 131072):
            with self.subTest(size=n):
                src = os.path.join(self.game, f'in_{n}.sav')
                self.put(src, b'\x77' * n)
                ok, msg = self.imp(src)
                self.assertFalse(ok, msg)
                self.assertTrue(msg, 'the user needs a reason')
                self.assertEqual(self.get(self.sav), original, 'a rejected import changed the current save')
                self.assertEqual(self.backups(), [], 'a rejected import must not create backups')
        self.assertEqual([n for n in os.listdir(self.game) if n.endswith('.tmp')], [])

    def test_import_of_a_blank_save_warns(self):
        self.put(self.sav, self.blob(5))
        for data in (b'\xff' * SAVE_SIZE, b'\x00' * SAVE_SIZE):
            with self.subTest(first=data[:1]):
                src = os.path.join(self.game, 'blank.sav')
                self.put(src, data)
                ok, msg = self.imp(src)
                self.assertTrue(re.search(r'empty|blank|0xff|erased', msg, re.I), f'no warning: {ok} {msg!r}')

    def test_import_backs_up_the_current_save_first_then_replaces_it(self):
        current, new = self.blob(5), self.blob(6)
        self.put(self.sav, current)
        src = os.path.join(self.game, 'novo.sav')
        self.put(src, new)
        ok, msg = self.imp(src)
        self.assertTrue(ok, msg)
        self.assertEqual(self.get(self.sav), new)
        names = self.backups()
        self.assertEqual(len(names), 1)
        self.assertEqual(self.get(os.path.join(self.bk, SHA, names[0])), current, 'backup must hold the OLD save')
        self.assertEqual([n for n in os.listdir(self.game) if n.endswith('.tmp')], [], 'tmp file left behind')

    def test_import_when_there_is_no_current_save(self):
        src = os.path.join(self.game, 'novo.sav')
        self.put(src, self.blob(6))
        ok, msg = self.imp(src)
        self.assertTrue(ok, msg)
        self.assertEqual(self.get(self.sav), self.blob(6))

    def test_import_of_a_missing_file_reports_an_error(self):
        self.put(self.sav, self.blob(5))
        ok, msg = self.imp(os.path.join(self.game, 'nao_existe.sav'))
        self.assertFalse(ok)
        self.assertEqual(self.get(self.sav), self.blob(5))

    def test_failure_while_replacing_leaves_the_original_save_intact(self):
        original = self.blob(5)
        self.put(self.sav, original)
        src = os.path.join(self.game, 'novo.sav')
        self.put(src, self.blob(6))
        real_replace, real_rename = os.replace, os.rename

        def boom(*a, **k):
            raise OSError('simulated failure during the swap')
        os.replace = os.rename = boom
        try:
            try:
                ok, msg = self.imp(src)
            except OSError:
                ok = False
        finally:
            os.replace, os.rename = real_replace, real_rename
        if ok:
            self.skipTest('savemgr does not use os.replace/os.rename for the swap (atomicity: see the runtime kill test)')
        self.assertEqual(self.get(self.sav), original)
        self.assertEqual([n for n in os.listdir(self.game) if n.endswith('.tmp')], [], 'tmp file left behind')

    # --- export ---
    def test_export_then_import_round_trip_is_identical(self):
        data = self.blob(9)
        self.put(self.sav, data)
        out_dir = os.path.join(self.game, 'Exportados ç ã')
        os.makedirs(out_dir)
        out = os.path.join(out_dir, 'copia de segurança.sav')
        self.mod.export_save(self.sav, out)
        self.assertEqual(hashlib.sha1(self.get(out)).hexdigest(), hashlib.sha1(data).hexdigest())
        self.put(self.sav, self.blob(10))                     # progresso novo que sera descartado
        ok, msg = self.imp(out, now=self.t0 + dt.timedelta(minutes=5))
        self.assertTrue(ok, msg)
        self.assertEqual(self.get(self.sav), data)
        newest = self.backups()[-1]
        self.assertEqual(self.get(os.path.join(self.bk, SHA, newest)), self.blob(10), 'import must back up first')

    def test_export_does_not_modify_the_save_and_overwrites_the_target(self):
        data = self.blob(9)
        self.put(self.sav, data)
        out = os.path.join(self.game, 'out.sav')
        self.put(out, b'old')
        self.mod.export_save(self.sav, out)
        self.assertEqual((self.get(self.sav), self.get(out)), (data, data))

    def test_every_path_may_contain_accents_and_spaces(self):
        for sub in ('João Silva', 'Meu Jogo (cópia)', 'a b  c'):
            with self.subTest(folder=sub):
                self.sav = os.path.join(self.game, sub, ROM_NAME + '.sav')
                self.put(self.sav, self.blob(3))
                self.bk = os.path.join(self.game, sub, 'Backups à é')
                self.assertTrue(self.backup(self.t0))
                out = os.path.join(self.game, sub, 'exp.sav')
                self.mod.export_save(self.sav, out)
                self.assertEqual(self.get(out), self.blob(3))


# ============================ 4. runtime: recomp/runtime/rt_save.c (gravador atomico) ============================
# O harness INCLUI o rt_save.c real e troca so o que e do resto do runtime (barramento da SRAM, relogio) por
# versoes minimas. `fwrite` e interceptado para simular "kill -9" no meio da gravacao do .tmp.
RT_SAVE_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <io.h>
#endif
static uint8_t g_sram[0x8000];
uint32_t gba_sram_gen;
void gba_sram_set(const uint8_t *b) { memcpy(g_sram, b, 0x8000); gba_sram_gen++; }
void gba_sram_get(uint8_t *b) { memcpy(b, g_sram, 0x8000); }
void gba_write8(uint32_t a, uint32_t v) {
    uint32_t o = a & 0x7FFF;
    if (g_sram[o] != (uint8_t)v) { g_sram[o] = (uint8_t)v; gba_sram_gen++; }
}
double rt_now_us(void) { return 0.0; }
static size_t kill_fwrite(const void *p, size_t s, size_t n, FILE *f) {
    const char *cut = getenv("KILL_AFTER_BYTES");
    if (cut) {
        size_t k = (size_t)atoll(cut);
        if (k > s * n) k = s * n;
        fwrite(p, 1, k, f);
        fflush(f);
        _exit(9);
    }
    return fwrite(p, s, n, f);
}
#define fwrite kill_fwrite
#include "RT_SAVE_C_PATH"
#undef fwrite
static double hx_t;
static double hx_clock(void) { return hx_t; }
int main(void) {
    char line[1200], cmd[40], arg[1100];
    rt_save_set_clock(hx_clock);
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        arg[0] = 0;
        if (sscanf(line, "%39s %1099[^\n]", cmd, arg) < 1) continue;
        if (!strcmp(cmd, "open")) rt_save_open(arg);
        else if (!strcmp(cmd, "time")) hx_t = atof(arg);
        else if (!strcmp(cmd, "write")) { unsigned o = 0, v = 0; sscanf(arg, "%u %x", &o, &v); gba_write8(0x0E000000u + o, v); }
        else if (!strcmp(cmd, "fill")) { unsigned v = 0; sscanf(arg, "%x", &v); for (unsigned i = 0; i < 0x8000; i++) gba_write8(0x0E000000u + i, v); }
        else if (!strcmp(cmd, "tick")) rt_save_tick();
        else if (!strcmp(cmd, "flush")) rt_save_flush("test", 0);
        else if (!strcmp(cmd, "flushforce")) rt_save_flush("test", 1);
        else if (!strcmp(cmd, "stat")) printf("flushes=%llu failures=%llu\n", (unsigned long long)rt_save_flushes, (unsigned long long)rt_save_failures);
        else if (!strcmp(cmd, "selftest")) printf("selftest=%d\n", rt_selftest_save(arg));
        else if (!strcmp(cmd, "exit")) exit(0);          /* roda atexit */
        else if (!strcmp(cmd, "die")) _exit(9);          /* kill -9: nao roda atexit */
        fflush(stdout);
    }
    return 0;
}
"""


class RuntimeSaveTest(unittest.TestCase):
    """Gravador do runtime (rt_save.c): dirty -> debounce ~1 s (teto 5 s) -> `<sav>.tmp` -> rename atomico."""
    exe, skip = None, None

    @classmethod
    def setUpClass(cls):
        src = os.path.join(ROOT, 'recomp', 'runtime', 'rt_save.c')
        if not os.path.exists(src):
            cls.skip = 'recomp/runtime/rt_save.c not found'
            return
        with open(src, encoding='utf-8', errors='replace') as f:
            body = f.read()
        for name in ('rt_save_open', 'rt_save_tick', 'rt_save_flush', 'rt_save_set_clock', 'rt_selftest_save'):
            if name not in body:
                cls.skip = f'rt_save.c no longer defines {name} (test harness needs updating)'
                return
        d = tempfile.mkdtemp(dir=TMP)
        c = os.path.join(d, 'save_harness.c')
        with open(c, 'w') as f:
            f.write(RT_SAVE_C.replace('RT_SAVE_C_PATH', src.replace('\\', '/')))
        exe = os.path.join(d, 'save_harness.exe' if os.name == 'nt' else 'save_harness')
        err = _compile(c, exe, extra=('-I', os.path.join(ROOT, 'recomp', 'runtime')))
        if err:
            cls.skip = 'could not build the rt_save harness: ' + err
        else:
            cls.exe = exe

    def setUp(self):
        if self.skip:
            self.skipTest(self.skip)
        self.dir = tempfile.mkdtemp(dir=TMP)
        self.sav = os.path.join(self.dir, 'João Silva', 'Kirby & The Amazing Mirror (USA).sav')
        os.makedirs(os.path.dirname(self.sav))

    # --- util ---
    def run_script(self, lines, env=None, expect_rc=0):
        e = dict(os.environ)
        e.update(env or {})
        r = subprocess.run([self.exe], input=('\n'.join(lines) + '\n').encode('utf-8'), capture_output=True, env=e)
        r.out, r.err = r.stdout.decode('utf-8', 'replace'), r.stderr.decode('utf-8', 'replace')
        if expect_rc is not None:
            self.assertEqual(r.returncode, expect_rc, r.err)
        return r

    def read(self, path=None):
        with open(path or self.sav, 'rb') as f:
            return f.read()

    def put(self, data, path=None):
        with open(path or self.sav, 'wb') as f:
            f.write(data)

    def stats(self, r):
        return [tuple(int(x.split('=')[1]) for x in l.split()) for l in r.out.split('\n') if l.startswith('flushes=')]

    def tmp_files(self):
        return [n for n in os.listdir(os.path.dirname(self.sav)) if n.endswith('.tmp')]

    S = 1_000_000   # 1 s em microssegundos (relogio falso)

    # --- criacao / ida e volta ---
    def test_first_flush_creates_a_32k_ff_file_when_missing(self):
        self.run_script([f'open {self.sav}', 'flushforce'])
        self.assertEqual(self.read(), b'\xff' * SAVE_SIZE)

    def test_existing_save_round_trips_byte_for_byte(self):
        random.seed(3)
        data = bytes(random.randrange(256) for _ in range(SAVE_SIZE))
        self.put(data)
        self.run_script([f'open {self.sav}', 'flushforce'])
        self.assertEqual(self.read(), data)

    def test_short_and_oversized_files_are_normalised_to_32k(self):
        self.put(b'\x11' * 100)
        self.run_script([f'open {self.sav}', 'flushforce'])
        self.assertEqual(self.read(), b'\x11' * 100 + b'\xff' * (SAVE_SIZE - 100))
        self.put(b'\x22' * 40000)
        self.run_script([f'open {self.sav}', 'flushforce'])
        self.assertEqual(self.read(), b'\x22' * SAVE_SIZE)

    # --- debounce ---
    def test_flush_about_one_second_after_the_last_write(self):
        self.put(b'\x11' * SAVE_SIZE)
        r = self.run_script([f'open {self.sav}', f'time {self.S}', 'write 16 42', 'tick', 'stat',
                             f'time {self.S + 500_000}', 'tick', 'stat', f'time {self.S + 1_100_000}', 'tick', 'stat'])
        self.assertEqual([s[0] for s in self.stats(r)], [0, 0, 1], 'flush at 0 / 0.5 / 1.1 s after the write')
        data = self.read()
        self.assertEqual((len(data), data[16]), (SAVE_SIZE, 0x42))

    def test_nothing_is_written_while_clean(self):
        self.put(b'\x11' * SAVE_SIZE)
        before = os.stat(self.sav).st_mtime_ns
        cmds = [f'open {self.sav}']
        for i in range(1, 40):
            cmds += [f'time {i * self.S}', 'tick']
        cmds.append('stat')
        r = self.run_script(cmds)
        self.assertEqual(self.stats(r), [(0, 0)])
        self.assertEqual(os.stat(self.sav).st_mtime_ns, before)

    def test_write_of_the_same_value_does_not_dirty_the_save(self):
        self.put(b'\x11' * SAVE_SIZE)
        r = self.run_script([f'open {self.sav}', f'time {self.S}', 'write 5 11', f'time {self.S * 5}', 'tick', 'stat'])
        self.assertEqual(self.stats(r), [(0, 0)])

    def test_burst_of_writes_is_coalesced_into_one_flush(self):
        self.put(b'\x11' * SAVE_SIZE)
        cmds = [f'open {self.sav}']
        t = self.S
        for i in range(100):                                    # 100 escritas em ~200 ms, um tick por escrita
            cmds += [f'time {t + i * 2000}', f'write {i} 99', 'tick']
        for k in range(1, 40):
            cmds += [f'time {t + 200_000 + k * 100_000}', 'tick']
        cmds.append('stat')
        r = self.run_script(cmds)
        self.assertEqual(self.stats(r)[0][0], 1, 'a burst must not cause one flush per write')
        self.assertEqual(self.read()[:100], b'\x99' * 100)

    def test_continuous_writing_is_still_flushed_at_least_every_five_seconds(self):
        self.put(b'\x11' * SAVE_SIZE)
        cmds = [f'open {self.sav}']
        t = self.S
        for fr in range(60 * 12):                               # 12 s escrevendo todo quadro
            cmds += [f'time {t + fr * 16743}', f'write {fr % 4096} {1 + fr % 200:x}', 'tick']
        cmds.append('stat')
        r = self.run_script(cmds)
        flushes = self.stats(r)[0][0]
        self.assertTrue(2 <= flushes <= 3, f'{flushes} flushes in 12 s of continuous writing (max delay 5 s)')

    # --- saida ---
    def test_flush_on_normal_exit_without_any_tick(self):
        self.put(b'\x11' * SAVE_SIZE)
        self.run_script([f'open {self.sav}', 'fill 33', 'exit'])
        self.assertEqual(self.read(), b'\x33' * SAVE_SIZE)

    def test_kill_9_loses_at_most_the_unflushed_window(self):
        self.put(b'\x11' * SAVE_SIZE)
        # escreveu e morreu antes do debounce: o save fica como estava (inteiro), sem arquivo parcial
        self.run_script([f'open {self.sav}', f'time {self.S}', 'fill 44', 'tick', 'die'], expect_rc=9)
        self.assertEqual(self.read(), b'\x11' * SAVE_SIZE)
        # depois do debounce o progresso ja esta em disco, mesmo morrendo em seguida
        self.run_script([f'open {self.sav}', f'time {self.S}', 'fill 44', 'tick', f'time {self.S * 3}', 'tick', 'die'],
                        expect_rc=9)
        self.assertEqual(self.read(), b'\x44' * SAVE_SIZE)

    # --- atomicidade ---
    def test_kill_in_the_middle_of_the_write_never_leaves_a_partial_sav(self):
        old, new = b'\x11' * SAVE_SIZE, b'\x22' * SAVE_SIZE
        for cut in (0, 1, 100, SAVE_SIZE // 2, SAVE_SIZE - 1):
            with self.subTest(killed_after_bytes=cut):
                self.put(old)
                r = self.run_script([f'open {self.sav}', f'time {self.S}', 'fill 22', 'tick', f'time {self.S * 3}', 'tick'],
                                    env={'KILL_AFTER_BYTES': str(cut)}, expect_rc=None)
                self.assertEqual(r.returncode, 9, 'the hook should have killed the process mid-write')
                self.assertIn(self.read(), (old, new), 'partial or corrupt .sav after a kill')
                self.assertEqual(len(self.read()), SAVE_SIZE)
                # a proxima execucao segue normal e nao deixa .tmp
                self.run_script([f'open {self.sav}', 'fill 44', 'flushforce'])
                self.assertEqual(self.read(), b'\x44' * SAVE_SIZE)
                self.assertEqual(self.tmp_files(), [])

    def test_kill_in_the_middle_of_the_very_first_write_leaves_no_sav_or_a_whole_one(self):
        r = self.run_script([f'open {self.sav}', f'time {self.S}', 'fill 22', 'tick', f'time {self.S * 3}', 'tick'],
                            env={'KILL_AFTER_BYTES': '5000'}, expect_rc=None)
        self.assertEqual(r.returncode, 9)
        if os.path.exists(self.sav):
            self.assertEqual(len(self.read()), SAVE_SIZE)

    def test_no_stray_tmp_after_a_normal_flush(self):
        self.run_script([f'open {self.sav}', f'time {self.S}', 'fill 66', 'tick', f'time {self.S * 3}', 'tick', 'exit'])
        self.assertEqual(self.tmp_files(), [])

    # --- falha e nova tentativa ---
    def test_write_failure_is_logged_once_retried_and_recovers(self):
        self.put(b'\x11' * SAVE_SIZE)
        os.makedirs(self.sav + '.tmp')                          # um DIRETORIO onde iria o .tmp: abrir falha
        cmds = [f'open {self.sav}', f'time {self.S}', 'fill 55', 'tick']
        for k in range(1, 5):                                    # 4 ciclos com falha
            cmds += [f'time {self.S + k * 1_300_000}', 'tick']
        cmds.append('stat')
        r1 = self.run_script(cmds)
        flushes, failures = self.stats(r1)[0]
        self.assertEqual(flushes, 0)
        self.assertGreaterEqual(failures, 3, 'a failed write must be retried every cycle')
        self.assertEqual(self.read(), b'\x11' * SAVE_SIZE, 'a failed write must not touch the .sav')
        self.assertEqual(r1.err.count('could not write'), 1, 'warn once per failure sequence, not every retry')
        os.rmdir(self.sav + '.tmp')
        r2 = self.run_script([f'open {self.sav}', f'time {self.S}', 'fill 55', 'tick', f'time {self.S + 1_300_000}',
                              'tick', 'stat'])
        self.assertEqual(self.read(), b'\x55' * SAVE_SIZE)
        self.assertEqual(self.stats(r2)[0][1], 0)

    # --- caminhos ---
    def test_paths_with_accents_spaces_and_non_ansi_characters(self):
        for sub in ('João Silva', 'Meu Jogo (cópia)', 'a b  c', '日本語', 'Привет'):
            with self.subTest(folder=sub):
                d = os.path.join(self.dir, sub)
                os.makedirs(d, exist_ok=True)
                p = os.path.join(d, 'Kirby & The Amazing Mirror (USA).sav')
                self.put(b'\x33' * SAVE_SIZE, p)
                self.run_script([f'open {p}', 'fill 44', 'exit'])
                self.assertEqual(self.read(p), b'\x44' * SAVE_SIZE)

    def test_the_runtimes_own_selftest_passes(self):
        d = tempfile.mkdtemp(dir=TMP)
        r = self.run_script([f'selftest {d}'])
        self.assertIn('selftest=0', r.out, r.err)


if __name__ == '__main__':
    unittest.main()
