"""Tests of the launcher flow (no UI, no compiler): python -m unittest tests.test_launcher_flow"""
import os
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'launcher'))

from flow import (Controller, FakeUI, BuildMonitor, STATE_PREPARING, STATE_FAST, STATE_READY,  # noqa: E402
                  STATE_ERROR, EXIT_BACK_TO_LAUNCHER, read_ini_value, read_scaling,
                  KL_OPEN_SAVE_DIR, KL_EXPORT_SAVE, KL_IMPORT_SAVE, OnlineSession, set_ini_value,
                  online_params, KLO_OFFLINE, KLO_HOSTING, KLO_CONNECTING, KLO_CONNECTED, KLO_PLAYING,
                  KLO_DESYNC, KLO_LOST, KLS_EMPTY, KLS_CONNECTED, KLS_READY, KLS_LOST, EXIT_NO_LINK,
                  KL_PAGE_ONLINE, KlOnlineC, link_quality, KLQ_UNKNOWN, KLQ_GOOD, KLQ_FAIR, KLQ_POOR)


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

    save_actions = None

    def _act(self, name):
        (self.save_actions if self.save_actions is not None else []).append(name)
        if getattr(self, 'boom', None) == name:
            raise OSError('simulated failure')

    def open_save_dir(self):
        self._act('open')

    def export_save(self):
        self._act('export')

    def import_save(self):
        self._act('import')


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


class SaveActionTests(unittest.TestCase):
    """The three SAVE buttons return codes 3/4/5; the controller runs the action and shows the screen again."""

    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.tmp = self._t.name

    def tearDown(self):
        self._t.cleanup()

    class ScriptUI:
        def __init__(self, codes):
            self.codes = list(codes)

        def run(self, ini, poll):
            return self.codes.pop(0) if self.codes else 0

    def test_each_save_button_runs_its_action_and_returns_to_the_screen(self):
        env = FakeEnv(self.tmp, compiled=True)
        env.save_actions = []
        ui = self.ScriptUI([KL_OPEN_SAVE_DIR, KL_EXPORT_SAVE, KL_IMPORT_SAVE, 0])
        self.assertEqual(Controller(env).run(ui, make_ini(self.tmp)), 0)
        self.assertEqual(env.save_actions, ['open', 'export', 'import'])
        self.assertEqual(env.ran, [])                         # none of them starts the game

    def test_a_failing_save_action_does_not_take_the_launcher_down(self):
        env = FakeEnv(self.tmp, compiled=True)
        env.save_actions, env.boom = [], 'export'
        ui = self.ScriptUI([KL_EXPORT_SAVE, KL_PLAY_CODE, 0])
        code = Controller(env).run(ui, make_ini(self.tmp))
        self.assertEqual(code, 0)
        self.assertEqual(env.ran, ['compiled.exe'])           # it kept running and Play still worked
        self.assertTrue(any('export_save failed' in m for m in env.logs))

    def test_unknown_return_codes_quit(self):
        env = FakeEnv(self.tmp, compiled=True)
        self.assertEqual(Controller(env).run(self.ScriptUI([99]), make_ini(self.tmp)), 0)
        self.assertEqual(env.ran, [])


KL_PLAY_CODE = 1


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


class FakeProc:
    def __init__(self):
        self.rc = None
        self.killed = False

    def poll(self):
        return self.rc

    def terminate(self):
        self.killed = True
        self.rc = 1


class OnlineEnv(FakeEnv):
    """FakeEnv + the online_* side effects; the status JSON is written by the test like kirby.exe would."""
    def __init__(self, tmp, **kw):
        FakeEnv.__init__(self, tmp, **kw)
        self.launched = []
        self.proc = FakeProc()
        self.starts = []
        self.ready_calls = []
        self.hint = ''
        self.status = os.path.join(tmp, 'link_status.json')
        self.flag = os.path.join(tmp, 'start.flag')

    def online_paths(self):
        return self.status, self.flag

    def online_address(self, port):
        return '192.168.1.5:%s' % port

    def online_addresses(self, port):
        return ['192.168.1.5:%s' % port, '26.4.3.2:%s' % port]

    def online_launch(self, role, p, status, flag):
        self.launched.append((role, dict(p)))
        return self.proc

    def online_request_start(self, flag):
        self.starts.append(flag)

    def online_set_ready(self, flag, on):
        self.ready_calls.append((flag, bool(on)))

    def online_failure_hint(self):
        return self.hint

    def write_status(self, **js):
        import json
        with open(self.status, 'w') as f:
            json.dump(js, f)


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def wait_running(sess):
    for _ in range(500):
        if sess.phase != 'preparing':
            return
        time.sleep(0.01)
    raise AssertionError('session never left preparing')


class OnlineSessionTests(unittest.TestCase):
    PARAMS = dict(host='10.0.0.7', port=47900, players=3, delay='auto', name='Pink')

    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.env = OnlineEnv(self._t.name)
        self.clock = Clock()
        self.s = OnlineSession(self.env, clock=self.clock)

    def tearDown(self):
        self._t.cleanup()

    def test_idle_is_offline_and_empty(self):
        snap = self.s.snapshot()
        self.assertEqual(snap['state'], KLO_OFFLINE)
        self.assertEqual(snap['slots'], [KLS_EMPTY] * 4)
        self.assertFalse(self.s.active())

    def test_host_lifecycle_waiting_then_playing(self):
        self.s.begin('host', self.PARAMS)
        self.assertEqual(self.s.snapshot()['address'], '192.168.1.5:47900')
        wait_running(self.s)
        self.assertEqual(self.env.launched, [('host', self.PARAMS)])
        snap = self.s.snapshot()
        self.assertEqual((snap['state'], snap['self_slot'], snap['names'][0]), (KLO_HOSTING, 0, 'Pink'))
        self.assertEqual(snap['slots'], [KLS_CONNECTED, KLS_EMPTY, KLS_EMPTY, KLS_EMPTY])
        self.clock.t += 1
        self.env.write_status(state='waiting', role='host', players=3, mask=3, player=0, delay=0, delay_max=1)
        snap = self.s.snapshot()
        self.assertEqual(snap['state'], KLO_HOSTING)
        self.assertEqual(snap['slots'], [KLS_CONNECTED, KLS_CONNECTED, KLS_EMPTY, KLS_EMPTY])
        self.clock.t += 1
        self.env.write_status(state='lockstep', role='host', players=3, mask=3, player=0, delay=1, delay_max=1,
                              rtt_ms=[0, 23.4, 0, 0], desync=False)
        snap = self.s.snapshot()
        self.assertEqual(snap['state'], KLO_PLAYING)
        self.assertEqual(snap['slots'], [KLS_READY, KLS_READY, KLS_EMPTY, KLS_EMPTY])
        self.assertEqual(snap['ping'][1], 23)
        self.assertIn('Delay: 1', snap['message'])

    def test_a_player_that_leaves_shows_as_lost(self):
        self.s.begin('host', self.PARAMS)
        wait_running(self.s)
        self.env.write_status(state='lockstep', mask=7, players=3, player=0, rtt_ms=[0, 5, 6, 0])
        self.s.snapshot()
        self.clock.t += 1
        self.env.write_status(state='lockstep', mask=5, players=3, player=0, rtt_ms=[0, 5, 6, 0])
        snap = self.s.snapshot()
        self.assertEqual(snap['slots'][1], KLS_LOST)
        self.assertEqual(snap['slots'][2], KLS_READY)

    def test_join_states_and_own_slot(self):
        self.s.begin('join', self.PARAMS)
        wait_running(self.s)
        snap = self.s.snapshot()
        self.assertEqual(snap['state'], KLO_CONNECTING)
        self.assertIn('10.0.0.7:47900', snap['message'])
        self.clock.t += 1
        self.env.write_status(state='waiting', role='join', players=3, mask=7, player=2)
        snap = self.s.snapshot()
        self.assertEqual((snap['state'], snap['self_slot'], snap['names'][2]), (KLO_CONNECTED, 2, 'Pink'))
        self.clock.t += 1
        self.env.write_status(state='lockstep', role='join', players=3, mask=7, player=2)
        self.assertEqual(self.s.snapshot()['state'], KLO_PLAYING)

    def test_join_that_never_connects_times_out_with_a_clear_message(self):
        self.s.begin('join', self.PARAMS)
        wait_running(self.s)
        self.clock.t += OnlineSession.CONNECT_TIMEOUT + 1
        snap = self.s.snapshot()
        self.assertEqual(snap['state'], KLO_OFFLINE)
        self.assertIn('Could not reach 10.0.0.7:47900', snap['message'])
        self.assertTrue(self.env.proc.killed)
        self.assertFalse(self.s.active())

    def test_exit_codes_become_messages(self):
        for rc, want in ((EXIT_NO_LINK, 'Could not connect: different ROM'), (0, 'ended'), (77, 'ended'), (9, 'code 9')):
            env = OnlineEnv(self._t.name)
            env.hint = 'different ROM'
            s = OnlineSession(env, clock=self.clock)
            s.begin('join', self.PARAMS)
            wait_running(s)
            env.proc.rc = rc
            snap = s.snapshot()
            self.assertEqual(snap['state'], KLO_OFFLINE)
            self.assertIn(want, snap['message'])
            self.assertFalse(s.active())

    def test_no_link_without_a_hint_still_explains(self):
        self.s.begin('join', self.PARAMS)
        wait_running(self.s)
        self.env.proc.rc = EXIT_NO_LINK
        self.assertIn('address', self.s.snapshot()['message'])

    def test_desync_and_dropped(self):
        self.s.begin('host', self.PARAMS)
        wait_running(self.s)
        self.env.write_status(state='lockstep', mask=3, players=2, player=0, desync=True)
        self.assertEqual(self.s.snapshot()['state'], KLO_DESYNC)
        self.clock.t += 1
        self.env.write_status(state='dropped', mask=1, players=2, player=0)
        self.assertEqual(self.s.snapshot()['state'], KLO_LOST)

    def test_leave_kills_the_process_and_resets(self):
        self.s.begin('host', self.PARAMS)
        wait_running(self.s)
        self.s.leave()
        self.assertTrue(self.env.proc.killed)
        self.assertEqual(self.s.snapshot()['state'], KLO_OFFLINE)
        self.assertFalse(self.s.active())

    def test_start_is_only_forwarded_by_a_running_host(self):
        self.s.begin('join', self.PARAMS)
        wait_running(self.s)
        self.s.start_game()
        self.assertEqual(self.env.starts, [])
        self.s.leave()
        env = OnlineEnv(self._t.name)
        h = OnlineSession(env, clock=self.clock)
        h.begin('host', self.PARAMS)
        wait_running(h)
        h.start_game()
        self.assertEqual(env.starts, [env.flag])

    def test_ready_toggles_a_file_and_the_lobby_shows_names_ready_and_pings(self):
        self.s.begin('host', self.PARAMS)
        wait_running(self.s)
        rp = self.env.flag + '.ready'
        self.assertEqual(self.env.ready_calls[0], (rp, False))          # a new session starts not ready
        self.s.toggle_ready()
        self.s.toggle_ready()
        self.assertEqual(self.env.ready_calls[1:], [(rp, True), (rp, False)])
        self.clock.t += 1
        self.env.write_status(state='waiting', role='host', players=3, mask=7, player=0, ready=5,
                              names=['Pink', 'José', '', ''], lobby_ping=[0, 48, 120, 0])
        snap = self.s.snapshot()
        self.assertEqual(snap['slots'][:3], [KLS_READY, KLS_CONNECTED, KLS_READY])
        self.assertEqual(snap['names'][:3], ['Pink', 'José', ''])
        self.assertEqual(snap['ping'][1:3], [48, 120])
        self.assertIn('READY', snap['message'])
        self.clock.t += 1
        self.env.write_status(state='waiting', role='host', players=3, mask=7, ready=7, names=['Pink', 'A', 'B', ''])
        self.assertIn('Everybody is ready', self.s.snapshot()['message'])

    def test_ready_is_ignored_when_not_running(self):
        self.s.toggle_ready()
        self.assertEqual(self.env.ready_calls, [])

    def test_a_joiner_sees_its_own_ping_as_the_host_row(self):
        self.s.begin('join', self.PARAMS)
        wait_running(self.s)
        self.clock.t += 1
        self.env.write_status(state='waiting', role='join', players=3, mask=7, player=2, ready=1,
                              names=['Host', 'Ana', 'Bia', ''], lobby_ping=[0, 30, 90, 0])
        snap = self.s.snapshot()
        self.assertEqual(snap['self_slot'], 2)
        self.assertEqual(snap['ping'][0], 90)
        self.assertEqual(snap['ping'][1], 30)
        self.assertEqual(snap['names'][:3], ['Host', 'Ana', 'Pink'])    # its own name comes from its settings
        self.assertIn('Press READY', snap['message'])

    def test_utf8_trunc_never_splits_a_letter(self):
        from flow import utf8_trunc
        self.assertEqual(utf8_trunc('José', 15), 'José'.encode())
        self.assertEqual(utf8_trunc('é' * 10, 15), ('é' * 7).encode())     # 14 bytes: the 8th letter does not fit
        self.assertEqual(utf8_trunc('abc', 2), b'ab')

    def test_launch_failure_is_reported(self):
        env = OnlineEnv(self._t.name)
        env.online_launch = lambda *a: None
        s = OnlineSession(env, clock=self.clock)
        s.begin('host', self.PARAMS)
        wait_running(s)
        snap = s.snapshot()
        self.assertEqual(snap['state'], KLO_OFFLINE)
        self.assertIn('Could not prepare', snap['message'])

    def test_corrupt_status_file_is_ignored(self):
        self.s.begin('host', self.PARAMS)
        wait_running(self.s)
        with open(self.env.status, 'w') as f:
            f.write('{ "state": "lock')              # half written
        self.assertEqual(self.s.snapshot()['state'], KLO_HOSTING)

    def test_quality_levels_from_the_status_json(self):
        base = dict(frames_total=3600, rtt_p95_ms=[0, 30.0, 0, 0], stall_gt100=0)
        self.assertEqual(link_quality(base), (KLQ_GOOD, 0, 30))
        self.assertEqual(link_quality(dict(base, frames_total=100))[0], KLQ_UNKNOWN)         # too early
        self.assertEqual(link_quality(dict(base, rtt_p95_ms=[0, 75.0, 0, 0]))[0], KLQ_FAIR)
        self.assertEqual(link_quality(dict(base, stall_gt100=3))[0], KLQ_FAIR)               # 3 per minute
        self.assertEqual(link_quality(dict(base, rtt_p95_ms=[0, 130.0, 0, 0]))[0], KLQ_POOR)
        self.assertEqual(link_quality(dict(base, stall_gt100=8))[0], KLQ_POOR)
        self.assertEqual(link_quality({})[0], KLQ_UNKNOWN)                                    # no json
        self.assertEqual(link_quality(dict(base, rtt_p95_ms=[0, 'x', None, 0])), (KLQ_GOOD, 0, 0))

    def test_playing_snapshot_carries_quality_and_hosting_carries_addresses(self):
        self.s.begin('host', self.PARAMS)
        wait_running(self.s)
        snap = self.s.snapshot()
        self.assertEqual(snap['addresses'], ['192.168.1.5:47900', '26.4.3.2:47900'])
        self.assertEqual(snap['quality'], KLQ_UNKNOWN)
        self.clock.t += 1
        self.env.write_status(state='lockstep', mask=3, players=2, player=0, frames_total=7200, frame=7200,
                              rtt_p95_ms=[0, 150, 0, 0], stall_gt100=5, rtt_ms=[0, 20, 0, 0])
        snap = self.s.snapshot()
        self.assertEqual((snap['quality'], snap['stalls'], snap['p95']), (KLQ_POOR, 5, 150))
        j = OnlineSession(OnlineEnv(self._t.name), clock=self.clock)
        j.begin('join', self.PARAMS)
        wait_running(j)
        self.assertEqual(j.snapshot()['addresses'], [])                                       # only a host lists addresses

    def test_structure_matches_the_c_header(self):
        import ctypes
        self.assertEqual(ctypes.sizeof(KlOnlineC), 452)            # sizeof(KlOnline) in hw/launcher_ui.h
        self.assertEqual(KlOnlineC.quality.offset, 248)
        self.assertEqual(KlOnlineC.addresses.offset, 260)


class OnlineControllerTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.tmp = self._t.name

    def tearDown(self):
        self._t.cleanup()

    def ini(self, extra=''):
        p = os.path.join(self.tmp, 'kirby.ini')
        with open(p, 'w') as f:
            f.write('[launcher]\nshow=1\n[link]\nmode=host\nport=5555\nplayers=3\ndelay=1\nname=Kirby\nhost=1.2.3.4\n' + extra)
        return p

    def test_host_then_leave_then_quit(self):
        env = OnlineEnv(self.tmp, compiled=True)
        ui = FakeUI(script=['host', 'leave', 'quit'])
        ini = self.ini()
        self.assertEqual(Controller(env).run(ui, ini), 0)
        self.assertEqual(env.launched, [('host', dict(host='1.2.3.4', port=5555, players=3, delay='1', name='Kirby',
                                                      transport='auto'))])
        self.assertTrue(env.proc.killed)                           # LEAVE killed it
        self.assertEqual(ui.pages, [0, KL_PAGE_ONLINE, KL_PAGE_ONLINE])   # reopened on the ONLINE page each time
        self.assertEqual(read_ini_value(ini, 'link', 'mode'), 'off')      # a stray mode=host is cleared

    def test_closing_the_launcher_ends_the_session(self):
        env = OnlineEnv(self.tmp, compiled=True)
        ui = FakeUI(script=['join', 'quit'])
        Controller(env).run(ui, self.ini())
        self.assertEqual(env.launched[0][0], 'join')
        self.assertTrue(env.proc.killed)

    def test_play_is_ignored_while_a_session_runs(self):
        env = OnlineEnv(self.tmp, compiled=True)

        class UI2(FakeUI):
            def run(s, ini, poll):
                r = FakeUI.run(s, ini, poll)
                if r == 6:
                    for _ in range(300):                   # let the worker launch
                        if env.launched:
                            break
                        time.sleep(0.01)
                    time.sleep(0.05)
                return r
        Controller(env).run(UI2(script=['host', 'play', 'quit']), self.ini())
        self.assertEqual(env.ran, [])

    def test_online_is_refused_while_the_game_is_still_being_prepared(self):
        env = OnlineEnv(self.tmp, fast_ok=False)
        c = Controller(env)
        c.start()
        for _ in range(300):
            if c.status().state == STATE_ERROR:
                break
            time.sleep(0.01)
        c.run(FakeUI(script=['host', 'quit'], max_polls=30), self.ini())
        self.assertEqual(env.launched, [])

    def test_start_reaches_the_session(self):
        env = OnlineEnv(self.tmp, compiled=True)

        class UI2(FakeUI):
            def run(s, ini, poll):
                r = FakeUI.run(s, ini, poll)
                if r == 6:
                    for _ in range(300):                   # let the worker launch
                        if env.launched:
                            break
                        time.sleep(0.01)
                    time.sleep(0.05)
                return r
        Controller(env).run(UI2(script=['host', 'start', 'quit']), self.ini())
        self.assertEqual(env.starts, [env.flag])

    def test_plain_play_still_works_and_exits_normally(self):
        env = OnlineEnv(self.tmp, compiled=True)
        self.assertEqual(Controller(env).run(FakeUI(script=['play']), self.ini()), 0)
        self.assertEqual(env.ran, ['compiled.exe'])


class IniWriteTests(unittest.TestCase):
    def test_set_value_in_place_and_create_missing_parts(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'k.ini')
            with open(p, 'w') as f:
                f.write('[video]\nscaling=fit\n[Link]\nMode = host\nport=1\n[audio]\nvolume=5\n')
            set_ini_value(p, 'link', 'mode', 'off')
            self.assertEqual(read_ini_value(p, 'link', 'mode'), 'off')
            self.assertEqual(read_ini_value(p, 'link', 'port'), '1')
            self.assertEqual(read_ini_value(p, 'audio', 'volume'), '5')
            self.assertEqual(read_ini_value(p, 'video', 'scaling'), 'fit')
            set_ini_value(p, 'link', 'name', 'X')                   # new key inside an existing section
            self.assertEqual(read_ini_value(p, 'link', 'name'), 'X')
            self.assertEqual(read_ini_value(p, 'audio', 'volume'), '5')
            set_ini_value(p, 'newsec', 'a', '1')                    # new section
            self.assertEqual(read_ini_value(p, 'newsec', 'a'), '1')
            q = os.path.join(tmp, 'missing.ini')
            set_ini_value(q, 'link', 'mode', 'off')                 # file that does not exist yet
            self.assertEqual(read_ini_value(q, 'link', 'mode'), 'off')

    def test_online_params_defaults_and_clamps(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'k.ini')
            self.assertEqual(online_params(p), dict(host='127.0.0.1', port=47900, players=2, delay='auto', name='Player',
                                                    transport='auto'))
            with open(p, 'w') as f:
                f.write('[link]\nport=70000\nplayers=9\ndelay=77\nname=ABCDEFGHIJKLMNOPQRS\nhost= 10.0.0.2 \n')
            r = online_params(p)
            self.assertEqual((r['port'], r['players'], r['delay'], r['host']), (47900, 2, '8', '10.0.0.2'))
            self.assertEqual(len(r['name']), 15)
            with open(p, 'w') as f:
                f.write('[link]\ntransport=TCP\n')
            self.assertEqual(online_params(p)['transport'], 'tcp')
            with open(p, 'w') as f:
                f.write('[link]\ntransport=quic\n')
            self.assertEqual(online_params(p)['transport'], 'auto')


if __name__ == '__main__':
    unittest.main()
