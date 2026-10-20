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
KL_OPEN_SAVE_DIR, KL_EXPORT_SAVE, KL_IMPORT_SAVE = 3, 4, 5
KL_ONLINE_HOST, KL_ONLINE_JOIN, KL_ONLINE_LEAVE, KL_ONLINE_START = 6, 7, 8, 9
KL_PAGE_MAIN, KL_PAGE_ONLINE = 0, 3
KLO_OFFLINE, KLO_HOSTING, KLO_CONNECTING, KLO_CONNECTED, KLO_PLAYING, KLO_DESYNC, KLO_LOST = range(7)
KLS_EMPTY, KLS_CONNECTING, KLS_CONNECTED, KLS_READY, KLS_LOST = range(5)
KLQ_UNKNOWN, KLQ_GOOD, KLQ_FAIR, KLQ_POOR = range(4)
EXIT_BACK_TO_LAUNCHER = 77       # kirby.exe exits with this when the user presses F1
EXIT_NO_LINK = 78                # kirby.exe --link-required exits with this when it could not set up the cable

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


class KlOnlineC(ctypes.Structure):
    """Mirror of KlOnline in hw/launcher_ui.h."""
    _fields_ = [('state', ctypes.c_int), ('slots', ctypes.c_int * 4), ('ping_ms', ctypes.c_int * 4),
                ('names', (ctypes.c_char * 16) * 4), ('self_slot', ctypes.c_int),
                ('my_address', ctypes.c_char * 48), ('message', ctypes.c_char * 96),
                # added at the end of the C struct (NOTES 18b)
                ('quality', ctypes.c_int), ('stalls_100ms', ctypes.c_int), ('ping_p95_ms', ctypes.c_int),
                ('addresses', (ctypes.c_char * 48) * 4)]


KlOnlinePoll = ctypes.CFUNCTYPE(None, ctypes.POINTER(KlOnlineC))


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


def read_scaling(ini_path):
    """[video] scaling = integer | fit | stretch (default fit). Legacy files: integer_scale=1 -> integer,
    integer_scale=0 -> fit; an explicit `scaling=` wins; invalid values fall back to fit."""
    val = (read_ini_value(ini_path, 'video', 'scaling') or '').strip().lower()
    if val in ('integer', 'fit', 'stretch'):
        return val
    if val:                                            # present but invalid
        return 'fit'
    legacy = (read_ini_value(ini_path, 'video', 'integer_scale') or '').strip()
    if legacy == '1':
        return 'integer'
    return 'fit'


def set_ini_value(path, section, key, value):
    """Set one key of kirby.ini in place (creates the key/section when missing; atomic replace). Never raises."""
    try:
        try:
            with open(path, encoding='utf-8-sig', errors='replace') as f:
                lines = f.read().splitlines()
        except OSError:
            lines = []
        cur, sec_at, done = None, None, False
        for i, line in enumerate(lines):
            t = line.split(';', 1)[0].split('#', 1)[0].strip()
            if t.startswith('[') and t.endswith(']'):
                if cur == section.lower() and not done:
                    lines.insert(i, '%s=%s' % (key, value))
                    done = True
                    break
                cur = t[1:-1].strip().lower()
                if cur == section.lower():
                    sec_at = i
            elif cur == section.lower() and '=' in t and t.split('=', 1)[0].strip().lower() == key.lower():
                lines[i] = '%s=%s' % (key, value)
                done = True
                break
        if not done:
            if sec_at is None:
                lines += ['', '[%s]' % section]
            lines.append('%s=%s' % (key, value))
        tmp = path + '.tmp'
        with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        os.replace(tmp, path)
    except OSError:
        pass


def read_link_status(path):
    """The JSON that `kirby.exe --link-status FILE` keeps up to date; {} when missing or half written."""
    import json
    try:
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def link_quality(js):
    """(quality, stalls_100ms, ping_p95_ms) from the status JSON of kirby.exe.
    GOOD: p95 < 60 ms and fewer than 1 pause > 100 ms per minute; POOR: p95 >= 120 ms or >= 6 such pauses per minute;
    FAIR in between. Unknown for the first ~5 seconds (300 frames)."""
    def num(v, d=0.0):
        return float(v) if isinstance(v, (int, float)) else d
    frames = num(js.get('frames_total', js.get('frame', 0)))
    p95s = [num(x) for x in (js.get('rtt_p95_ms') or []) if isinstance(x, (int, float)) and x > 0]
    p95 = int(round(max(p95s))) if p95s else 0
    gt100 = int(num(js.get('stall_gt100')))
    if frames < 300:
        return KLQ_UNKNOWN, gt100, p95
    per_min = gt100 / max(frames / 3600.0, 1.0 / 60)
    if p95 >= 120 or per_min >= 6:
        return KLQ_POOR, gt100, p95
    if p95 >= 60 or per_min >= 1:
        return KLQ_FAIR, gt100, p95
    return KLQ_GOOD, gt100, p95


class OnlineSession:
    """One online session = one `kirby.exe --link host|join` process, seen through its status JSON.
    Pure logic (the process, the files and the address come from `env`), unit-tested with a fake env.

    env.online_paths() -> (status_json, start_flag)      fresh files for this session
    env.online_launch(role, params, status, start_flag) -> process-like (poll(), terminate()) or None
    env.online_request_start(start_flag)                 host: stop waiting for more players
    env.online_address(port) -> 'ip:port'
    env.online_addresses(port) -> up to 4 'ip:port' of this PC (the UI lets the user pick one)
    env.online_failure_hint() -> last [link] line of kirby.log ('' if none)
    """
    CONNECT_TIMEOUT = 15.0

    def __init__(self, env, clock=time.time):
        self.env, self.clock = env, clock
        self.lock = threading.Lock()
        self.phase = 'idle'                       # idle | preparing | running | ended
        self.role, self.params = None, {}
        self.proc = None
        self.status_path = self.start_path = None
        self.message, self.seen, self.t0 = '', 0, 0.0
        self.address = ''
        self.addresses = []
        self._js, self._js_t = {}, 0.0

    def active(self):
        return self.phase in ('preparing', 'running')

    def begin(self, role, params):
        with self.lock:
            if self.active():
                return
            self.role, self.params = role, dict(params)
            self.phase, self.message, self.seen, self.t0 = 'preparing', '', 0, self.clock()
            self.address = self.env.online_address(params['port']) if role == 'host' else ''
            try:
                self.addresses = list(self.env.online_addresses(params['port']))[:4] if role == 'host' else []
            except Exception:
                self.addresses = []
            self._js, self._js_t = {}, 0.0
        threading.Thread(target=self._launch, daemon=True).start()

    def _launch(self):
        proc, err = None, ''
        try:
            self.status_path, self.start_path = self.env.online_paths()
            proc = self.env.online_launch(self.role, self.params, self.status_path, self.start_path)
        except Exception as e:                           # never let the worker die silently
            self.env.log('online launch failed: %r' % (e,))
            err = 'Could not start the game. See launcher.log'
        with self.lock:
            if self.phase != 'preparing':                # the user left while we were preparing
                if proc is not None:
                    self._kill(proc)
                return
            if proc is None:
                self.phase, self.message = 'ended', err or 'Could not prepare the game. See launcher.log'
            else:
                self.proc, self.phase, self.t0 = proc, 'running', self.clock()

    @staticmethod
    def _kill(proc):
        try:
            proc.terminate()
        except Exception:
            pass

    def leave(self):
        with self.lock:
            if self.proc is not None:
                self._kill(self.proc)
            self.proc, self.phase, self.message = None, 'idle', ''

    def start_game(self):
        if self.phase == 'running' and self.role == 'host' and self.start_path:
            self.env.online_request_start(self.start_path)

    def _status(self):
        if self.clock() - self._js_t >= 0.25:
            self._js, self._js_t = read_link_status(self.status_path) if self.status_path else {}, self.clock()
        return self._js

    def _reap(self):
        rc = self.proc.poll()
        if rc is None:
            return
        self.proc, self.phase = None, 'ended'
        if rc == EXIT_NO_LINK:
            hint = self.env.online_failure_hint()
            self.message = ('Could not connect: ' + hint) if hint else 'Could not connect. Check the address and the port.'
        elif rc in (0, EXIT_BACK_TO_LAUNCHER):
            self.message = 'The session ended.'
        else:
            self.message = 'The game closed unexpectedly (code %d). See kirby.log' % rc

    def snapshot(self):
        """dict(state, slots[4], ping[4], names[4], self_slot, address, message) for the UI poll."""
        with self.lock:
            if self.phase == 'running':
                self._reap()
            if self.phase == 'running' and self.role == 'join':
                st = self._status().get('state')
                if st in (None, 'connecting') and self.clock() - self.t0 > self.CONNECT_TIMEOUT:
                    self._kill(self.proc)
                    self.proc, self.phase = None, 'ended'
                    self.message = 'Could not reach %s:%s. Check the address, the port and that the host is waiting.' % (
                        self.params.get('host', '?'), self.params.get('port', '?'))
            out = dict(state=KLO_OFFLINE, slots=[KLS_EMPTY] * 4, ping=[-1] * 4, names=[''] * 4, self_slot=-1,
                       address='', message=self.message, quality=KLQ_UNKNOWN, stalls=0, p95=0, addresses=[])
            if self.phase == 'idle' or self.phase == 'ended':
                return out
            host = self.role == 'host'
            out['address'] = self.address
            out['addresses'] = list(self.addresses)
            if self.phase == 'preparing':
                out['state'] = KLO_HOSTING if host else KLO_CONNECTING
                out['message'] = 'Preparing the game...'
                out['self_slot'] = 0 if host else -1
                return out
            js = self._status()
            st = js.get('state')
            mask = int(js.get('mask', 0) or 0) | (1 if host else 0)
            self.seen |= mask
            self_slot = 0 if host else int(js.get('player', -1) if js.get('player', -1) is not None else -1)
            playing = st in ('lockstep', 'free')
            for i in range(4):
                if mask >> i & 1:
                    out['slots'][i] = KLS_READY if playing else KLS_CONNECTED
                elif self.seen >> i & 1:
                    out['slots'][i] = KLS_LOST
            if not host and not mask:
                out['slots'][0] = KLS_CONNECTING
            rtt = js.get('rtt_ms') or []
            for i in range(1, min(4, len(rtt))):
                try:
                    if mask >> i & 1 and i != self_slot and float(rtt[i]) > 0:
                        out['ping'][i] = int(round(float(rtt[i])))
                except (TypeError, ValueError):
                    pass
            if not host and self_slot != 0:                  # a child measures nothing: show the host's RTT if known
                best = [float(r) for r in rtt if isinstance(r, (int, float)) and r > 0]
                if best:
                    out['ping'][0] = int(round(max(best)))
            out['self_slot'] = self_slot if 0 <= self_slot < 4 else -1
            if 0 <= out['self_slot'] < 4:
                out['names'][out['self_slot']] = str(self.params.get('name', ''))[:15]
            if playing:
                out['quality'], out['stalls'], out['p95'] = link_quality(js)
            if js.get('desync'):
                out['state'], out['message'] = KLO_DESYNC, 'The two games differ (desync). Press LEAVE and start again.'
            elif st == 'dropped':
                out['state'], out['message'] = KLO_LOST, 'The connection was lost. Press LEAVE.'
            elif playing:
                out['state'] = KLO_PLAYING
                out['message'] = ('In the game: START GAME > MULTI PLAYER (host confirms OK?). Delay: %s frame(s).'
                                  % js.get('delay', '?'))
            elif host:
                out['state'] = KLO_HOSTING
                out['message'] = 'Give your friends the address above. START begins with who is connected.'
            elif st in ('waiting', 'starting'):
                out['state'] = KLO_CONNECTED
                out['message'] = 'Connected. Waiting for the host to start.'
            else:
                out['state'] = KLO_CONNECTING
                out['message'] = 'Connecting to %s:%s ...' % (self.params.get('host', '?'), self.params.get('port', '?'))
            return out


def online_params(ini_path):
    """[link] section of kirby.ini as the launcher needs it (same defaults as the runtime and the UI)."""
    def num(key, default, lo, hi):
        try:
            v = int(read_ini_value(ini_path, 'link', key, default))
        except (TypeError, ValueError):
            return default
        return v if lo <= v <= hi else default
    delay = (read_ini_value(ini_path, 'link', 'delay', 'auto') or 'auto').strip().lower()
    if delay != 'auto':
        try:
            delay = str(max(0, min(8, int(delay))))
        except ValueError:
            delay = 'auto'
    transport = (read_ini_value(ini_path, 'link', 'transport', 'auto') or 'auto').strip().lower()
    if transport not in ('auto', 'udp', 'tcp'):
        transport = 'auto'
    return dict(host=(read_ini_value(ini_path, 'link', 'host', '127.0.0.1') or '127.0.0.1').strip(),
                port=num('port', 47900, 1, 65535), players=num('players', 2, 2, 4), delay=delay,
                name=(read_ini_value(ini_path, 'link', 'name', 'Player') or 'Player').strip()[:15],
                transport=transport)


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
        self._set(STATE_PREPARING, 5, 'Preparing fast mode...')
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
        self._set(STATE_FAST, 0, 'Building the compiled version')

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
                    st = Status(STATE_FAST, 0, 'Background build stopped', self.env.rom, 1)
                else:
                    st = Status(STATE_FAST, pct, label, self.env.rom, 1)
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
        self.online = OnlineSession(self.env)
        if hasattr(ui, 'set_online_poll'):
            ui.set_online_poll(self._make_online_poll())
        try:
            return self._loop(ui, ini_path, poll)
        finally:
            self.online.leave()                                # closing the launcher ends the session
            set_ini_value(ini_path, 'link', 'mode', 'off')      # a stray mode=host must not outlive the launcher

    def _loop(self, ui, ini_path, poll):
        page = KL_PAGE_MAIN
        while True:
            if hasattr(ui, 'set_start_page'):
                ui.set_start_page(page)
            page = KL_PAGE_MAIN
            r = ui.run(ini_path, poll)
            if r in (KL_ONLINE_HOST, KL_ONLINE_JOIN, KL_ONLINE_LEAVE, KL_ONLINE_START):
                page = KL_PAGE_ONLINE                           # reopen on the ONLINE page after the action
                self._online_action(r, ini_path)
                continue
            if r == KL_QUIT:
                return 0
            if r == KL_PICK_ROM:
                if self.env.pick_new_rom():
                    self._worker = None
                    self.start()
                continue
            if r in (KL_OPEN_SAVE_DIR, KL_EXPORT_SAVE, KL_IMPORT_SAVE):
                action = {KL_OPEN_SAVE_DIR: 'open_save_dir', KL_EXPORT_SAVE: 'export_save',
                          KL_IMPORT_SAVE: 'import_save'}[r]
                try:
                    getattr(self.env, action)()
                except Exception as e:                       # a save action must never take the launcher down
                    self.env.log('%s failed: %r' % (action, e))
                continue
            if r == KL_PLAY:
                if not self.playable() or self.online.active():
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

    def _online_action(self, code, ini_path):
        """HOST / JOIN / LEAVE / START from the ONLINE page. The UI already saved the fields into [link]."""
        try:
            if code == KL_ONLINE_LEAVE:
                self.online.leave()
            elif code == KL_ONLINE_START:
                self.online.start_game()
            elif not self.playable():
                self.online.message = 'The game is still being prepared. Try again in a moment.'
            else:
                self.online.begin('host' if code == KL_ONLINE_HOST else 'join', online_params(ini_path))
        except Exception as e:                               # an online failure must never take the launcher down
            self.env.log('online action %d failed: %r' % (code, e))
        set_ini_value(ini_path, 'link', 'mode', 'off')       # the session got its settings on the command line

    def _make_online_poll(self):
        def poll(out):
            snap = self.online.snapshot()
            o = out.contents
            o.state = snap['state']
            for i in range(4):
                o.slots[i] = snap['slots'][i]
                o.ping_ms[i] = snap['ping'][i]
                o.names[i] = snap['names'][i].encode('ascii', 'replace')[:15]
            o.self_slot = snap['self_slot']
            o.my_address = snap['address'].encode('ascii', 'replace')[:47]
            o.message = snap['message'].encode('ascii', 'replace')[:95]
            o.quality = snap['quality']
            o.stalls_100ms = snap['stalls']
            o.ping_p95_ms = snap['p95']
            for i in range(4):
                o.addresses[i] = (snap['addresses'][i] if i < len(snap['addresses']) else '').encode('ascii', 'replace')[:47]
        cb = KlOnlinePoll(poll)
        self._online_poll_ref = cb
        return cb

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
        self.pages = []                                   # start pages requested before each run
        self.online_poll = None

    def set_start_page(self, page):
        self.pages.append(page)

    def set_online_poll(self, cb):
        self.online_poll = cb

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
                        'wait-ready': KL_PLAY, 'wait-fast': KL_PLAY, 'host': KL_ONLINE_HOST,
                        'join': KL_ONLINE_JOIN, 'leave': KL_ONLINE_LEAVE, 'start': KL_ONLINE_START}.get(want, KL_QUIT)
            if st.state in (STATE_FAST, STATE_READY):
                return KL_PLAY
            n += 1
            time.sleep(self.interval)
        return KL_QUIT
