"""What the launcher relies on in kirby.exe (Riven, online wiring): status JSON while the host still waits,
START by flag file, a join that waits for the host to start, `--link-required` (exit 78) and a plain game
with `--link off`.

Headless `rt_boot` (no window, no save) with the compiled runtime; needs baserom.gba and rt_boot
(build/recomp/program or $KIRBY_RT_BOOT). Takes ~20 s: only with RECOMP_SLOW=1 like the other link tests."""
import json
import os
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, 'baserom.gba')


def find_exe():
    cands = [os.environ.get('KIRBY_RT_BOOT'), os.path.join(ROOT, 'build', 'recomp', 'program', 'rt_boot.exe'),
             os.path.join(ROOT, 'build', 'recomp', 'program', 'rt_boot')]
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


EXE = find_exe()


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def status(path):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def wait_for(pred, seconds=30.0):
    t0 = time.time()
    while time.time() - t0 < seconds:
        v = pred()
        if v:
            return v
        time.sleep(0.1)
    return None


@unittest.skipUnless(os.environ.get('RECOMP_SLOW') == '1' and EXE and os.path.isfile(ROM),
                     'RECOMP_SLOW=1 + rt_boot + baserom.gba')
class LauncherRuntimeTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.tmp = self._t.name
        self.procs = []
        self.ini = os.path.join(self.tmp, 'empty.ini')
        open(self.ini, 'w').close()

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        self._t.cleanup()

    def spawn(self, *args, **env):
        p = subprocess.Popen([EXE, ROM, '--sav', 'none', '--ini', self.ini, '--mute'] + list(args),
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ, **env))
        self.procs.append(p)
        return p

    def test_host_status_start_flag_and_a_join_that_waits_longer_than_the_frame_timeout(self):
        port = str(free_port())
        hs, js = os.path.join(self.tmp, 'h.json'), os.path.join(self.tmp, 'j.json')
        flag = os.path.join(self.tmp, 'start.flag')
        host = self.spawn('--link', 'host', '--link-players', '3', '--link-port', port, '--link-status', hs,
                          '--link-start-file', flag, '--link-required', '--frames', '240')
        d = wait_for(lambda: status(hs) if status(hs).get('state') == 'waiting' else None)
        self.assertIsNotNone(d, 'the host writes a status while it waits (no window yet)')
        self.assertEqual((d['role'], d['players'], d['mask'], d['player']), ('host', 3, 1, 0))
        self.assertEqual(d['delay_max'], 1)                         # 3 players -> K max 1
        join = self.spawn('--link', 'join', '--link-port', port, '--link-status', js, '--link-required',
                          '--frames', '240')
        d = wait_for(lambda: status(hs) if status(hs).get('mask') == 3 else None)
        self.assertIsNotNone(d, 'the host sees the joiner in its mask')
        j = wait_for(lambda: status(js) if status(js).get('state') == 'waiting' else None)
        self.assertIsNotNone(j)
        self.assertEqual((j['role'], j['player'], j['mask']), ('join', 1, 3))
        time.sleep(9.5)                                             # longer than the 8 s "host sent nothing" timeout
        self.assertIsNone(join.poll(), 'the joiner keeps waiting for the host to start')
        open(flag, 'w').close()                                     # START with who is connected (2 of 3)
        d = wait_for(lambda: status(hs) if status(hs).get('state') == 'lockstep' else None)
        j = wait_for(lambda: status(js) if status(js).get('state') == 'lockstep' else None)
        self.assertIsNotNone(d, 'host reaches lockstep after START')
        self.assertIsNotNone(j, 'join reaches lockstep after START')
        self.assertEqual(j['player'], 1)
        self.assertEqual(host.wait(60), 0)
        self.assertIn(join.wait(60), (0,))

    def test_link_required_exits_78_when_the_host_is_not_there(self):
        p = self.spawn('--link', 'join', '--link-port', str(free_port()), '--link-required', '--frames', '30',
                       KIRBY_LINK_WAIT_S='2')
        self.assertEqual(p.wait(60), 78)

    def test_without_link_required_the_game_runs_on_without_a_cable(self):
        p = self.spawn('--link', 'join', '--link-port', str(free_port()), '--frames', '30', KIRBY_LINK_WAIT_S='2')
        self.assertEqual(p.wait(60), 0)

    def test_link_off_overrides_the_ini_mode(self):
        with open(self.ini, 'w') as f:
            f.write('[link]\nmode=host\nplayers=4\nport=%s\n' % free_port())
        p = self.spawn('--link', 'off', '--frames', '30', KIRBY_LINK_WAIT_S='2')
        self.assertEqual(p.wait(20), 0)                             # would wait for 3 players if the ini won


if __name__ == '__main__':
    unittest.main()
