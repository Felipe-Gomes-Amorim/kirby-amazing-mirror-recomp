"""Launcher flow (Riven): everything the full launcher needs *except* drawing.

Pure Python, no Windows calls, injectable collaborators, so it is unit-tested without a UI or a compiler
(tests/test_launcher_flow.py). The C side is `kirby_ui.dll: kl_run(ini_path, poll)` (see NOTES.md,
"Launcher completo"); until it exists, `FakeUI` stands in for it.

States reported to the UI (KlStatus.state):
  0 NONE       nothing to report
  1 PREPARING  building the fast-mode (interpreter) exe; Play is disabled
  2 FAST       fast mode ready, full build running in the background; Play enabled
  3 READY      compiled game ready; Play enabled
  4 ERROR      could not build anything playable
"""
import ctypes
import os
import re
import threading
import time

STATE_NONE, STATE_PREPARING, STATE_FAST, STATE_READY, STATE_ERROR = 0, 1, 2, 3, 4
KL_QUIT, KL_PLAY, KL_PICK_ROM = 0, 1, 2
EXIT_BACK_TO_LAUNCHER = 77       # kirby.exe exits with this when the user presses F1

# (marker printed by tools/build_all.py, weight in percent)
BUILD_STEPS = [('1/5', 5), ('2/5', 2), ('3/5', 2), ('4/5', 5), ('5/5', 86)]


class Status:
    """Mirror of the C struct KlStatus."""
    def __init__(self, state=STATE_NONE, percent=0, text='', rom='', rom_ok=0):
        self.state, self.percent, self.text, self.rom, self.rom_ok = state, percent, text, rom, rom_ok

    def __repr__(self):
        return 'Status(%d, %d%%, %r)' % (self.state, self.percent, self.text)


class KlStatusC(ctypes.Structure):
    _fields_ = [('state', ctypes.c_int), ('percent', ctypes.c_int), ('text', ctypes.c_char * 96),
                ('rom', ctypes.c_char * 260), ('rom_ok', ctypes.c_int)]


KlPoll = ctypes.CFUNCTYPE(None, ctypes.POINTER(KlStatusC))


def read_ini_value(path, section, key, default=None):
    """Tiny tolerant reader for kirby.ini (a missing or broken file simply yields `default`)."""
    try:
        cur = None
        with open(path, encoding='utf-8-sig', errors='replace') as f:      # utf-8-sig drops a BOM
            for line in f:
                line = line.split(';', 1)[0].split('#', 1)[0].strip()
                if line.startswith('[') and line.endswith(']'):
                    cur = line[1:-1].strip().lower()
                elif '=' in line and cur == section.lower():
                    k, v = line.split('=', 1)
                    if k.strip().lower() == key.lower():
                        return v.strip()
    except OSError:
        pass
    return default


class BuildMonitor:
    """Progress of the *detached* full build, derived from files only (it is another process)."""

    def __init__(self, out_dir, build_log, stamp, est_seconds=240.0, pid_alive=None, clock=time.time):
        self.out_dir, self.build_log, self.stamp = out_dir, build_log, stamp
        self.est, self.clock = est_seconds, clock
        self.pid_alive = pid_alive or (lambda pid: False)

    def done(self):
        mark = os.path.join(self.out_dir, 'build.ok')
        try:
            if not os.path.exists(os.path.join(self.out_dir, 'kirby.exe')):
                return False
            with open(mark) as f:
                return f.read() == self.stamp
        except OSError:
            return False

    def running(self):
        try:
            with open(os.path.join(self.out_dir, 'build.lock')) as f:
                return self.pid_alive(f.read().strip())
        except OSError:
            return False

    def progress(self):
        """(percent 0..99, short label) while running."""
        step, weight_done, t_step = 0, 0, None
        try:
            with open(self.build_log, encoding='utf-8', errors='replace') as f:
                for line in f:
                    m = re.match(r'== (\d/5)', line)
                    if m:
                        for i, (tag, _) in enumerate(BUILD_STEPS):
                            if tag == m.group(1):
                                step, weight_done = i, sum(w for _, w in BUILD_STEPS[:i])
            t_step = os.path.getmtime(self.build_log)
        except OSError:
            pass
        w = BUILD_STEPS[step][1]
        # inside the long step, creep towards the end according to the log's age since the step began
        frac = 0.0
        if step == len(BUILD_STEPS) - 1 and t_step:
            frac = min(0.95, max(0.0, 1 - 1 / (1 + max(0.0, self.clock() - t_step) / self.est)))
        pct = int(min(99, weight_done + w * frac))
        label = 'Building the compiled version' if step >= 4 else 'Analyzing the ROM'
        return pct, label


class Controller:
    """Decides what to show and what Play means. `env` supplies the side-effecting pieces."""

    def __init__(self, env):
        self.env = env
        self._lock = threading.Lock()
        self._status = Status(STATE_NONE, 0, 'Starting...')
        self._worker = None
        self._fast_exe = None
        self._failed = False
        self._bg_t0 = None

    # -- worker -------------------------------------------------------------------------------
    def start(self):
        if self._worker is not None:
            return
        if self.env.compiled_ready():
            self._set(STATE_READY, 100, 'Compiled version ready')
            return
        self._set(STATE_PREPARING, 5, 'Preparing fast mode (first run only)...')
        self._worker = threading.Thread(target=self._prepare, daemon=True)
        self._worker.start()

    def _prepare(self):
        try:
            exe = self.env.ensure_fast_exe(lambda pct, txt: self._set(STATE_PREPARING, pct, txt))
        except Exception as e:                          # never let a worker die silently
            self.env.log('fast mode failed: %r' % (e,))
            exe = None
        if not exe:
            self._failed = True
            self._set(STATE_ERROR, 0, 'Could not prepare the game. See launcher.log')
            return
        self._fast_exe = exe
        self.env.start_background_build()
        self._bg_t0 = time.time()
        self._set(STATE_FAST, 0, 'Playable now. Building the compiled version...')

    def _set(self, state, pct, text):
        with self._lock:
            self._status = Status(state, int(pct), text, self.env.rom, 1)

    # -- what the UI sees ---------------------------------------------------------------------
    def status(self):
        with self._lock:
            st = Status(self._status.state, self._status.percent, self._status.text, self.env.rom, 1)
        if st.state == STATE_FAST:                       # live view of the detached build
            if self.env.monitor.done():
                st = Status(STATE_READY, 100, 'Compiled version ready', self.env.rom, 1)
            else:
                pct, label = self.env.monitor.progress()
                pending = self._bg_t0 is not None and time.time() - self._bg_t0 < 5.0
                if not self.env.monitor.running() and not pending:
                    st = Status(STATE_FAST, 0, 'Playable. Background build is not running', self.env.rom, 1)
                else:
                    st = Status(STATE_FAST, pct, '%s %d%% (you can play now)' % (label, pct), self.env.rom, 1)
        elif st.state == STATE_READY and not self.env.compiled_ready():
            pass
        return st

    def playable(self):
        return self.status().state in (STATE_FAST, STATE_READY)

    def choose_exe(self):
        if self.env.compiled_ready() or self.env.monitor.done():
            return self.env.compiled_exe()
        return self._fast_exe

    # -- the loop -----------------------------------------------------------------------------
    def run(self, ui, ini_path, shift_held=False):
        self.start()
        show = read_ini_value(ini_path, 'launcher', 'show', '1') != '0'
        if not show and not shift_held and self.playable():
            return self._play()
        poll = self._make_poll()
        while True:
            r = ui.run(ini_path, poll)
            if r == KL_QUIT:
                return 0
            if r == KL_PICK_ROM:
                if self.env.pick_new_rom():
                    self._worker = None
                    self.start()
                continue
            if r == KL_PLAY:
                if not self.playable():
                    continue                              # Play is disabled in the UI; be safe anyway
                code = self._play()
                if code == EXIT_BACK_TO_LAUNCHER:
                    continue
                return code
            return 0

    def _play(self):
        exe = self.choose_exe()
        if not exe:
            return 2
        return self.env.run_game(exe)

    def _make_poll(self):
        def poll(out):
            st = self.status()
            out.contents.state = st.state
            out.contents.percent = st.percent
            out.contents.text = st.text.encode('ascii', 'replace')[:95]
            out.contents.rom = st.rom.encode('utf-8', 'replace')[:259]
            out.contents.rom_ok = st.rom_ok
        cb = KlPoll(poll)
        self._poll_ref = cb                               # keep alive for the C side
        return cb


class FakeUI:
    """Stand-in for kl_run until kirby_ui.dll provides it. Scripted: returns Play when playable, or
    follows a script of results (used by the tests)."""

    def __init__(self, script=None, log=None, max_polls=400, interval=0.01):
        self.script, self.log, self.max_polls = list(script or []), log or (lambda m: None), max_polls
        self.interval = interval
        self.seen = []

    def run(self, ini_path, poll):
        st = KlStatusC()
        n = 0
        while n < self.max_polls:
            poll(ctypes.pointer(st))
            self.seen.append((st.state, st.percent, st.text.decode('ascii', 'replace')))
            if self.script:
                want = self.script[0]
                if want == 'wait-ready' and st.state != STATE_READY:
                    n += 1
                    time.sleep(self.interval)
                    continue
                if want == 'wait-fast' and st.state not in (STATE_FAST, STATE_READY):
                    n += 1
                    time.sleep(self.interval)
                    continue
                self.script.pop(0)
                return {'play': KL_PLAY, 'quit': KL_QUIT, 'pick': KL_PICK_ROM,
                        'wait-ready': KL_PLAY, 'wait-fast': KL_PLAY}.get(want, KL_QUIT)
            if st.state in (STATE_FAST, STATE_READY):
                return KL_PLAY
            n += 1
            time.sleep(self.interval)
        return KL_QUIT
