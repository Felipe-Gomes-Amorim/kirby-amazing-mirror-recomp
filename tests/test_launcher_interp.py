"""build_interp reuses the interpreter exe only when it was built from the same sources (stamp)."""
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'launcher'))
import launcher  # noqa: E402


class FakePopen:
    runs = 0

    def __init__(self, cmd, **kw):
        FakePopen.runs += 1
        out = cmd[cmd.index('--out') + 1]
        os.makedirs(os.path.join(out, 'interp'), exist_ok=True)
        with open(os.path.join(out, 'interp', 'kirby.exe'), 'wb') as f:
            f.write(b'new')
        self.returncode = 0

        class _O:
            def read(self_):
                return ''
        self.stdout = _O()

    def poll(self):
        return 0


class BuildInterpStampTests(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.out = self._t.name
        self._popen, self._log, self._work = launcher.subprocess.Popen, launcher.log, launcher.WORK
        launcher.subprocess.Popen = FakePopen
        launcher.log = lambda m: None
        launcher.WORK = self.out
        FakePopen.runs = 0
        os.makedirs(os.path.join(self.out, 'interp'))
        self.exe = os.path.join(self.out, 'interp', 'kirby.exe')
        with open(self.exe, 'wb') as f:
            f.write(b'old')

    def tearDown(self):
        launcher.subprocess.Popen, launcher.log, launcher.WORK = self._popen, self._log, self._work
        self._t.cleanup()

    def test_exe_without_a_stamp_is_rebuilt_when_a_stamp_is_given(self):
        self.assertEqual(launcher.build_interp('rom', self.out, None, 'S2'), self.exe)
        self.assertEqual(FakePopen.runs, 1)
        self.assertEqual(open(self.exe, 'rb').read(), b'new')
        self.assertEqual(open(os.path.join(self.out, 'interp', 'build.stamp')).read(), 'S2')

    def test_same_stamp_is_reused_and_another_one_rebuilds(self):
        launcher.build_interp('rom', self.out, None, 'S2')
        launcher.build_interp('rom', self.out, None, 'S2')
        self.assertEqual(FakePopen.runs, 1)
        launcher.build_interp('rom', self.out, None, 'S3')
        self.assertEqual(FakePopen.runs, 2)

    def test_no_stamp_keeps_the_old_behaviour(self):
        self.assertEqual(launcher.build_interp('rom', self.out), self.exe)
        self.assertEqual(FakePopen.runs, 0)


if __name__ == '__main__':
    unittest.main()
