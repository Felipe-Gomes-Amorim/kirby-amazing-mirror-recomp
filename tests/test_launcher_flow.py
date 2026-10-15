"""Tests of the launcher flow (no UI, no compiler): python -m unittest tests.test_launcher_flow"""
import os
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'launcher'))

from flow import (Controller, FakeUI, BuildMonitor, STATE_PREPARING, STATE_FAST, STATE_READY,  # noqa: E402
                  STATE_ERROR, EXIT_BACK_TO_LAUNCHER, read_ini_value, read_scaling)


class FakeEnv:
    def __init__(self, tmp, compiled=False, fast_ok=True, game_codes=(0,)):
        self.rom = 'C:/roms/kirby.gba'
        self.tmp = tmp
        self.compiled = compiled
        self.fast_ok = fast_ok
        self.game_codes = list(game_codes)
        self.ran = []
        self.bg_started = 0
        self.logs = []
        self.picks = 0
        self.monitor = BuildMonitor(tmp, os.path.join(tmp, 'build.log'), 'S1', pid_alive=lambda p: False)

    def log(self, m):
        self.logs.append(m)

    def compiled_ready(self):
        return self.compiled

    def compiled_exe(self):
        return 'compiled.exe'

    def ensure_fast_exe(self, progress):
        progress(40, 'Preparing fast mode')
        return 'fast.exe' if self.fast_ok else None

    def start_background_build(self):
        self.bg_started += 1

    def run_game(self, exe):
        self.ran.append(exe)
        return self.game_codes.pop(0) if self.game_codes else 0

    def pick_new_rom(self):
        self.picks += 1
        return True


def make_ini(tmp, show=1):
    p = os.path.join(tmp, 'kirby.ini')
    with open(p, 'w') as f:
        f.write('[launcher]\nshow=%d\nlanguage=en\n' % show)
    return p


class FlowTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.tmp = self._t.name

    def tearDown(self):
        self._t.cleanup()

    def test_first_run_prepares_then_plays_fast_and_starts_the_background_build(self):
        env = FakeEnv(self.tmp)
        ui = FakeUI(script=['wait-fast'])
        code = Controller(env).run(ui, make_ini(self.tmp))
        self.assertEqual(code, 0)
        self.assertEqual(env.ran, ['fast.exe'])
        self.assertEqual(env.bg_started, 1)
        states = {s for s, _, _ in ui.seen}
        self.assertTrue(states & {STATE_FAST, STATE_READY})

    def test_compiled_ready_plays_compiled_without_fast_mode(self):
        env = FakeEnv(self.tmp, compiled=True)
        code = Controller(env).run(FakeUI(script=['play']), make_ini(self.tmp))
        self.assertEqual(code, 0)
        self.assertEqual(env.ran, ['compiled.exe'])
        self.assertEqual(env.bg_started, 0)

    def test_f1_returns_to_the_launcher_then_plays_again(self):
        env = FakeEnv(self.tmp, compiled=True, game_codes=(EXIT_BACK_TO_LAUNCHER, 0))
        ui = FakeUI(script=['play', 'play'])
        code = Controller(env).run(ui, make_ini(self.tmp))
        self.assertEqual(code, 0)
        self.assertEqual(env.ran, ['compiled.exe', 'compiled.exe'])

    def test_quit_does_not_start_the_game(self):
        env = FakeEnv(self.tmp, compiled=True)
        self.assertEqual(Controller(env).run(FakeUI(script=['quit']), make_ini(self.tmp)), 0)
        self.assertEqual(env.ran, [])

    def test_fast_mode_failure_reports_error_and_play_does_nothing(self):
        env = FakeEnv(self.tmp, fast_ok=False)
        c = Controller(env)
        c.start()
        for _ in range(300):
            if c.status().state == STATE_ERROR:
                break
            time.sleep(0.01)
        self.assertEqual(c.status().state, STATE_ERROR)
        self.assertFalse(c.playable())
        env2 = FakeEnv(self.tmp, fast_ok=False)
        c2 = Controller(env2)
        self.assertEqual(c2.run(FakeUI(script=['play'], max_polls=30), make_ini(self.tmp)), 0)
        self.assertEqual(env2.ran, [])

    def test_show_zero_skips_the_ui_when_already_playable(self):
        env = FakeEnv(self.tmp, compiled=True)
        code = Controller(env).run(FakeUI(script=['quit']), make_ini(self.tmp, show=0))
        self.assertEqual(env.ran, ['compiled.exe'])
        self.assertEqual(code, 0)

    def test_shift_forces_the_ui_even_with_show_zero(self):
        env = FakeEnv(self.tmp, compiled=True)
        Controller(env).run(FakeUI(script=['quit']), make_ini(self.tmp, show=0), shift_held=True)
        self.assertEqual(env.ran, [])

    def test_pick_rom_restarts_the_preparation(self):
        env = FakeEnv(self.tmp)
        ui = FakeUI(script=['pick', 'wait-fast'])
        Controller(env).run(ui, make_ini(self.tmp))
        self.assertEqual(env.picks, 1)
        self.assertEqual(env.ran, ['fast.exe'])


class MonitorTests(unittest.TestCase):
    def test_progress_follows_the_step_markers(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'build.log')
            m = BuildMonitor(tmp, log, 'S', est_seconds=100.0)
            self.assertEqual(m.progress()[0], 0)
            with open(log, 'w') as f:
                f.write('ROM ok\n\n== 1/5 analysis\n -> ok in 14s\n== 2/5 indirect\n')
            self.assertEqual(m.progress()[0], 5)
            with open(log, 'a') as f:
                f.write(' -> ok in 6s\n== 3/5 score\n -> ok in 6s\n== 4/5 analysis\n -> ok in 13s\n== 5/5 build\n')
            p, label = m.progress()
            self.assertGreaterEqual(p, 14)
            self.assertLess(p, 100)
            self.assertIn('compiled', label)

    def test_done_requires_exe_and_matching_stamp(self):
        with tempfile.TemporaryDirectory() as tmp:
            m = BuildMonitor(tmp, os.path.join(tmp, 'b.log'), 'STAMP')
            self.assertFalse(m.done())
            open(os.path.join(tmp, 'kirby.exe'), 'wb').close()
            self.assertFalse(m.done())
            with open(os.path.join(tmp, 'build.ok'), 'w') as f:
                f.write('OTHER')
            self.assertFalse(m.done())
            with open(os.path.join(tmp, 'build.ok'), 'w') as f:
                f.write('STAMP')
            self.assertTrue(m.done())

    def test_running_uses_the_lock_pid(self):
        with tempfile.TemporaryDirectory() as tmp:
            m = BuildMonitor(tmp, os.path.join(tmp, 'b.log'), 'S', pid_alive=lambda p: p == '123')
            self.assertFalse(m.running())
            with open(os.path.join(tmp, 'build.lock'), 'w') as f:
                f.write('123')
            self.assertTrue(m.running())


class IniTests(unittest.TestCase):
    def test_reads_values_comments_and_missing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'k.ini')
            with open(p, 'w') as f:
                f.write('; c\n[video]\nscale = 4 ; inline\n[Launcher]\nShow=0\n')
            self.assertEqual(read_ini_value(p, 'video', 'scale'), '4')
            self.assertEqual(read_ini_value(p, 'launcher', 'show'), '0')
            self.assertEqual(read_ini_value(p, 'launcher', 'nope', 'x'), 'x')
            self.assertEqual(read_ini_value(os.path.join(tmp, 'missing.ini'), 'a', 'b', 'd'), 'd')


class ScalingTests(unittest.TestCase):
    def _ini(self, tmp, body):
        p = os.path.join(tmp, 'k.ini')
        with open(p, 'w') as f:
            f.write(body)
        return p

    def test_modes_defaults_and_legacy(self):
        with tempfile.TemporaryDirectory() as tmp:
            cases = [
                ('[video]\nscaling=integer\n', 'integer'),
                ('[video]\nscaling=STRETCH\n', 'stretch'),
                ('[video]\nscaling=fit\n', 'fit'),
                ('[video]\n', 'fit'),                                  # default
                ('[video]\nscaling=banana\n', 'fit'),                  # invalid -> fit
                ('[video]\ninteger_scale=1\n', 'integer'),             # legacy
                ('[video]\ninteger_scale=0\n', 'fit'),
                ('[video]\nscaling=stretch\ninteger_scale=1\n', 'stretch'),   # explicit wins
            ]
            for body, want in cases:
                self.assertEqual(read_scaling(self._ini(tmp, body)), want, body)
            self.assertEqual(read_scaling(os.path.join(tmp, 'missing.ini')), 'fit')


if __name__ == '__main__':
    unittest.main()
