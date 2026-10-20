"""KirbyRecomp launcher (Riven).

Drag a ROM onto KirbyRecomp.exe (or run it and pick the file). First run: verify the ROM, build the
recompiled game on THIS machine (analysis -> recompile -> compile), then start it. Next runs: start.
Nothing of the game is ever shipped; everything generated stays under %LOCALAPPDATA%\\KirbyRecomp.

  KirbyRecomp.exe [ROM] [--build-only] [--rebuild] [--console] [--version]
"""
import ctypes
import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time

VERSION = '0.6'
SUPPORTED = {'274b102b6d940f46861a92b4e65f89a51815c12c': 'Kirby & The Amazing Mirror (USA)'}
HERE = os.path.dirname(os.path.abspath(__file__))            # <install>/app/launcher
APP = os.path.dirname(HERE)                                  # <install>/app
INSTALL = os.path.dirname(APP)
BASE = os.path.join(os.environ.get('LOCALAPPDATA') or os.path.expanduser('~'), 'KirbyRecomp')
WORK = os.path.join(BASE, 'work')
LOG = os.path.join(BASE, 'launcher.log')
# (tag printed by tools/build_all.py, label, weight in percent)
STEPS = [('1/5', 'Analyzing the ROM', 5), ('2/5', 'Indirect branches', 2), ('3/5', 'Function confidence', 2),
         ('4/5', 'Analyzing (second pass)', 5), ('5/5', 'Compiling the game', 86)]
CONSOLE = False


def log(msg):
    os.makedirs(BASE, exist_ok=True)
    with open(LOG, 'a', encoding='utf-8') as f:
        f.write(time.strftime('%H:%M:%S ') + msg + '\n')
    if CONSOLE:
        print(msg, flush=True)


def message(title, text, error=False):
    log(('ERROR: ' if error else '') + title + ': ' + text.replace('\n', ' | '))
    if CONSOLE:
        return
    try:
        ctypes.windll.user32.MessageBoxW(None, text, title, 0x10 if error else 0x40)
    except Exception:
        print(title, text)


def pick_rom():
    return file_dialog(False, 'Choose your Kirby & The Amazing Mirror (USA) ROM',
                       'GBA ROM (*.gba)\0*.gba\0All files\0*.*\0\0')


def file_dialog(save, title, filt, default_name=''):
    """Win32 open/save dialog through ctypes (no tkinter, nothing extra to ship). Returns a path or None."""
    from ctypes import wintypes

    class OFN(ctypes.Structure):
        _fields_ = [('lStructSize', wintypes.DWORD), ('hwndOwner', wintypes.HWND),
                    ('hInstance', wintypes.HINSTANCE), ('lpstrFilter', wintypes.LPCWSTR),
                    ('lpstrCustomFilter', wintypes.LPWSTR), ('nMaxCustFilter', wintypes.DWORD),
                    ('nFilterIndex', wintypes.DWORD), ('lpstrFile', wintypes.LPWSTR),
                    ('nMaxFile', wintypes.DWORD), ('lpstrFileTitle', wintypes.LPWSTR),
                    ('nMaxFileTitle', wintypes.DWORD), ('lpstrInitialDir', wintypes.LPCWSTR),
                    ('lpstrTitle', wintypes.LPCWSTR), ('Flags', wintypes.DWORD),
                    ('nFileOffset', wintypes.WORD), ('nFileExtension', wintypes.WORD),
                    ('lpstrDefExt', wintypes.LPCWSTR), ('lCustData', wintypes.LPARAM),
                    ('lpfnHook', ctypes.c_void_p), ('lpTemplateName', wintypes.LPCWSTR),
                    ('pvReserved', ctypes.c_void_p), ('dwReserved', wintypes.DWORD),
                    ('FlagsEx', wintypes.DWORD)]
    buf = ctypes.create_unicode_buffer(1024)
    o = OFN()
    o.lStructSize = ctypes.sizeof(OFN)
    # the filter holds embedded NULs: a plain Python str assigned to LPCWSTR would be cut at the first one
    flt = ctypes.create_unicode_buffer(filt, len(filt) + 1)
    o.lpstrFilter = ctypes.cast(flt, wintypes.LPCWSTR)
    o.lpstrFile = ctypes.cast(buf, wintypes.LPWSTR)
    o.nMaxFile = 1024
    o.lpstrTitle = title
    if default_name:
        buf.value = default_name
    if save:
        o.Flags = 0x00000002 | 0x00000800 | 0x00080000      # OVERWRITEPROMPT | PATHMUSTEXIST | EXPLORER
        ok = ctypes.windll.comdlg32.GetSaveFileNameW(ctypes.byref(o))
    else:
        o.Flags = 0x00001000 | 0x00000800 | 0x00080000      # FILEMUSTEXIST | PATHMUSTEXIST | EXPLORER
        ok = ctypes.windll.comdlg32.GetOpenFileNameW(ctypes.byref(o))
    return buf.value if ok else None


def find_rom(arg):
    if arg:
        return arg
    last = os.path.join(BASE, 'last_rom.txt')
    cands = [open(last).read().strip()] if os.path.exists(last) else []
    for d in (INSTALL, os.getcwd()):
        cands.append(os.path.join(d, 'baserom.gba'))
        if os.path.isdir(d):
            cands += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.lower().endswith('.gba')]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return pick_rom()


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def source_stamp():
    """Changes whenever the shipped sources change; decides restaging and rebuilding."""
    h = hashlib.sha1(VERSION.encode())
    for sub in ('recomp', 'hw', 'tools'):
        for root, dirs, files in os.walk(os.path.join(APP, sub)):
            dirs[:] = sorted(d for d in dirs if d not in ('zig', '__pycache__'))
            for n in sorted(files):
                if n.endswith(('.py', '.c', '.h', '.csv', '.ps1')):
                    p = os.path.join(root, n)
                    h.update(os.path.relpath(p, APP).encode())
                    h.update(str(os.path.getsize(p)).encode())
    return h.hexdigest()[:16]


def stage_sources(stamp):
    """Copy the (small) sources into a writable per-user work tree; the tools write build/ next to themselves."""
    mark = os.path.join(WORK, '.stamp')
    if os.path.exists(mark) and open(mark).read() == stamp:
        return
    log('preparing work area at ' + WORK)
    for sub in ('recomp', 'hw', 'tools'):
        dst = os.path.join(WORK, sub)
        if os.path.exists(dst):
            shutil.rmtree(dst)
        shutil.copytree(os.path.join(APP, sub), dst,
                        ignore=shutil.ignore_patterns('zig', '__pycache__', '*.pyc'))
    with open(mark, 'w') as f:
        f.write(stamp)


class UI:
    """Progress window from hw/window_sdl.c (kirby_ui.dll). Falls back to log-only."""

    def __init__(self):
        self.dll = None
        path = os.path.join(HERE, 'kirby_ui.dll')
        if os.path.exists(path) and not CONSOLE:
            try:
                os.add_dll_directory(HERE)
                self.dll = ctypes.CDLL(path)
                self.dll.kui_open.argtypes = [ctypes.c_char_p]
                self.dll.kui_progress.argtypes = [ctypes.c_int, ctypes.c_char_p]
                if self.dll.kui_open(b'Kirby Recomp') != 0:
                    self.dll = None
            except OSError as e:
                log('progress window unavailable: %s' % e)
                self.dll = None

    def update(self, pct, text):
        now = time.time()
        label = text.split('  (')[0]
        if now - getattr(self, '_last', 0) >= 5 or label != getattr(self, '_txt', None):
            log('[%3d%%] %s' % (pct, text))
            self._last, self._txt = now, label
        if self.dll:
            return bool(self.dll.kui_progress(int(pct), text.encode('ascii', 'replace')))
        return True

    def close(self):
        if self.dll:
            self.dll.kui_close()
            self.dll = None


def cancel_confirmed():
    if CONSOLE:
        return True
    try:
        # MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST ; IDYES == 6
        return ctypes.windll.user32.MessageBoxW(
            None, 'Cancel building the game?\n\nIf you did not close the progress window, choose No.',
            'Kirby Recomp', 0x00000004 | 0x00000020 | 0x00000100 | 0x00040000) == 6
    except Exception:
        return False


def build(rom, out_dir, stamp, ui):
    env = dict(os.environ)
    env['KIRBY_ZIG'] = os.path.join(APP, 'tools', 'zig', 'zig.exe')
    env['SDL2_DIR'] = os.path.join(APP, 'sdl2')
    env['PYTHONIOENCODING'] = 'utf-8'
    env['PYTHONPATH'] = WORK
    env['KIRBY_WORK'] = WORK      # the embeddable Python ignores PYTHONPATH; kirby_work.pth honours this
    cmd = [sys.executable, os.path.join(WORK, 'tools', 'build_all.py'), rom,
           '--jobs', str(max(2, os.cpu_count() or 4))]
    est = 200.0
    tfile = os.path.join(BASE, 'timings.json')
    if os.path.exists(tfile):
        try:
            est = float(json.load(open(tfile)).get('build_s', est))
        except Exception:
            pass
    q = queue.Queue()
    p = subprocess.Popen(cmd, cwd=WORK, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         creationflags=0x08000000, text=True, encoding='utf-8', errors='replace')

    def pump():
        for line in p.stdout:
            q.put(line.rstrip())
        q.put(None)
    threading.Thread(target=pump, daemon=True).start()
    done_w, cur = 0, 0
    t0 = t_step = time.time()
    tail = []
    buildlog = open(os.path.join(BASE, 'build.log'), 'w', encoding='utf-8')
    alive = True
    while alive or not q.empty():
        try:
            line = q.get(timeout=0.5)
            if line is None:
                alive = False
                continue
            buildlog.write(line + '\n')
            tail = (tail + [line])[-25:]
            for i, (tag, _, _) in enumerate(STEPS):
                if line.startswith('== ' + tag):
                    done_w = sum(w for _, _, w in STEPS[:i])
                    cur, t_step = i, time.time()
        except queue.Empty:
            pass
        _, name, w = STEPS[cur]
        el = time.time() - t_step
        frac = 1 - 1 / (1 + el / (est if cur == len(STEPS) - 1 else 30.0))   # smooth, never reaches 100%
        pct = min(99, done_w + w * frac)
        mm, ss = divmod(int(time.time() - t0), 60)
        if not ui.update(pct, '%s %s  (%d:%02d)' % (STEPS[cur][0], name, mm, ss)):
            # the window reported "closed": never cancel silently, ask first (and keep going on "No")
            log('progress window reported a close event (%d:%02d); asking for confirmation' % (mm, ss))
            if cancel_confirmed():
                p.kill()
                return False, ['cancelado']
            ui.close()                   # keep building, without the window
            ui.dll = None
    rc = p.wait()
    buildlog.close()
    if rc == 0:
        with open(tfile, 'w') as f:
            json.dump({'build_s': time.time() - t0}, f)
        with open(os.path.join(out_dir, 'build.ok'), 'w') as f:
            f.write(stamp)
    return rc == 0, tail


def zig_cache_dir():
    """Writable per-user copy of the Zig cache shipped in the package (it makes the first build seconds
    instead of ~1 minute). None when the package carries no cache (dev tree)."""
    shipped = os.path.join(APP, 'tools', 'zigcache')
    if not os.path.isdir(shipped):
        return None
    dst = os.path.join(BASE, 'zigcache')
    mark = os.path.join(dst, '.seeded')
    if not os.path.exists(mark):
        try:
            if os.path.isdir(dst):
                shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(shipped, dst)
            with open(mark, 'w') as f:
                f.write('1')
            log('seeded the Zig cache from the package')
        except OSError as e:
            log('could not seed the Zig cache (%s); the first build will be slower' % e)
    return dst


def tool_env():
    env = dict(os.environ)
    env.setdefault('KIRBY_ZIG', os.path.join(APP, 'tools', 'zig', 'zig.exe'))   # setdefault: dev overrides
    zc = zig_cache_dir()
    if zc:
        env['ZIG_GLOBAL_CACHE_DIR'] = zc
    env.setdefault('SDL2_DIR', os.path.join(APP, 'sdl2'))
    env['PYTHONIOENCODING'] = 'utf-8'
    env['PYTHONPATH'] = WORK
    env['KIRBY_WORK'] = WORK
    return env


def pid_alive(pid):
    try:
        h = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(h)
        return code.value == 259                                          # STILL_ACTIVE
    except Exception:
        return False


def build_interp(rom, out_dir, ui=None, stamp=None):
    """Interpreter-only kirby.exe: runtime + SDL + interpreter, none of the game compiled (seconds).
    Runs as a child process while the progress window keeps pumping events (otherwise Windows flags the
    window as "not responding"). `stamp` (source_stamp) marks which sources built it: an exe left by an
    older version is rebuilt instead of reused (it would lack newer runtime options)."""
    exe = os.path.join(out_dir, 'interp', 'kirby.exe')
    mark = os.path.join(out_dir, 'interp', 'build.stamp')
    if os.path.exists(exe):
        try:
            fresh = stamp is None or open(mark).read() == stamp
        except OSError:
            fresh = False
        if fresh:
            return exe
        log('interpreter build is from another version; rebuilding it')
        try:
            os.remove(exe)
        except OSError:
            pass
    cmd = [sys.executable, '-m', 'recomp.program', '--rom', rom, '--out', out_dir, '--interp-only']
    t0 = time.time()
    p = subprocess.Popen(cmd, cwd=WORK, env=tool_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding='utf-8', errors='replace', creationflags=0x08000000)
    out = []
    threading.Thread(target=lambda: out.append(p.stdout.read()), daemon=True).start()
    while p.poll() is None:
        el = time.time() - t0
        pct = 5 + 90 * (1 - 1 / (1 + el / 25.0))            # smooth, never reaches 100%
        if ui is not None:
            ui.update(pct, 'Preparing fast mode (%d s)' % el)
        time.sleep(0.2)
    time.sleep(0.2)
    log('fast mode build: %.0fs rc=%d' % (time.time() - t0, p.returncode))
    if p.returncode or not os.path.exists(exe):
        log(''.join(out)[-800:].replace(chr(10), ' | '))
        return None
    if stamp is not None:
        try:
            with open(mark, 'w') as f:
                f.write(stamp)
        except OSError:
            pass
    return exe


def background_build_main(rom, stamp):
    """Entry point of the detached process (`launcher.py ROM STAMP --bg-build`): runs the full build
    and records success, independently of whether the game window is still open."""
    sys.path.insert(0, WORK)
    from recomp import toolchain
    out_dir = toolchain.default_out(sha1_of(rom))
    os.makedirs(out_dir, exist_ok=True)
    lock = os.path.join(out_dir, 'build.lock')
    with open(lock, 'w') as f:
        f.write(str(os.getpid()))
    jobs = max(2, (os.cpu_count() or 4) - 2)             # leave cores for the game
    cmd = [sys.executable, os.path.join(WORK, 'tools', 'build_all.py'), rom, '--jobs', str(jobs)]
    log('full build in the background (%d jobs)' % jobs)
    with open(os.path.join(BASE, 'build.log'), 'w', encoding='utf-8') as logf:
        rc = subprocess.run(cmd, cwd=WORK, env=tool_env(), stdout=logf, stderr=subprocess.STDOUT,
                            creationflags=0x08000000).returncode
    try:
        os.remove(lock)
    except OSError:
        pass
    if rc == 0 and os.path.exists(os.path.join(out_dir, 'kirby.exe')):
        with open(os.path.join(out_dir, 'build.ok'), 'w') as f:
            f.write(stamp)
        log('full build ready; the next start uses the compiled game')
    else:
        log('full build failed (rc=%s); see build.log' % rc)
    return rc


def start_background_build(rom, out_dir, stamp):
    """Launch the build as a separate low-priority, detached process (survives the game window)."""
    lock = os.path.join(out_dir, 'build.lock')
    if os.path.exists(lock):
        try:
            pid = open(lock).read().strip()
            if pid_alive(pid):
                log('a build is already running (pid %s)' % pid)
                return False
        except OSError:
            pass
    cmd = [sys.executable, os.path.abspath(__file__), rom, stamp, '--bg-build']
    # CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | DETACHED_PROCESS
    subprocess.Popen(cmd, cwd=WORK, env=tool_env(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, creationflags=0x08000000 | 0x00004000 | 0x00000008)
    return True


def play_while_building(rom, out_dir, stamp, flags):
    """First run: open the game NOW through the interpreter, finish the compiled build in the background.
    Returns an exit code, or None to fall back to the blocking build."""
    ui = UI()
    ui.update(5, 'Preparing fast mode...')
    exe_i = build_interp(rom, out_dir, ui, stamp)
    ui.update(100, 'Starting the game...')
    ui.close()
    if not exe_i:
        return None
    start_background_build(rom, out_dir, stamp)
    log('starting (interpreter) ' + exe_i)
    return subprocess.run([exe_i, rom], cwd=os.path.dirname(exe_i)).returncode


class _ProgressAdapter:
    """build_interp() reports through ui.update(pct, text); the new flow reports through a callback."""
    def __init__(self, cb):
        self.cb = cb

    def update(self, pct, text):
        self.cb(pct, text)
        return True


class LauncherEnv:
    """Side-effecting pieces for launcher/flow.py (see tests/test_launcher_flow.py for the fakes)."""

    def __init__(self, rom, out_dir, stamp, flow_mod):
        self.rom, self.out_dir, self.stamp = rom, out_dir, stamp
        est = 240.0
        try:
            with open(os.path.join(BASE, 'timings.json')) as f:
                est = float(json.load(f).get('build_s', est))
        except Exception:
            pass
        self.monitor = flow_mod.BuildMonitor(out_dir, os.path.join(BASE, 'build.log'), stamp,
                                             est_seconds=est, pid_alive=pid_alive)

    def log(self, msg):
        log(msg)

    def compiled_exe(self):
        return os.path.join(self.out_dir, 'kirby.exe')

    def compiled_ready(self):
        return self.monitor.done()

    def ensure_fast_exe(self, progress):
        return build_interp(self.rom, self.out_dir, _ProgressAdapter(progress), self.stamp)

    def start_background_build(self):
        return start_background_build(self.rom, self.out_dir, self.stamp)

    # ---- save management (launcher/savemgr.py; NOTES.md "Gerenciamento de save") -------------------------
    def save_path(self):
        """Where kirby.exe keeps the save: next to the ROM, with the .gba extension swapped for .sav."""
        return os.path.splitext(self.rom)[0] + '.sav'

    def rom_stem(self):
        return os.path.splitext(os.path.basename(self.rom))[0]

    def rom_sha1(self):
        if not getattr(self, '_sha', None):
            self._sha = sha1_of(self.rom)
        return self._sha

    def backup_root(self):
        return os.path.join(BASE, 'backups')

    def backup_now(self):
        try:
            import savemgr
            path = savemgr.backup_save(self.save_path(), self.backup_root(), self.rom_sha1(), self.rom_stem())
            if path:
                log('save backup: ' + path)
        except Exception as e:                         # a failed backup must never stop the game from starting
            log('save backup failed: %r' % (e,))

    def open_save_dir(self):
        sav = self.save_path()
        if os.path.isfile(sav):
            subprocess.Popen(['explorer', '/select,', os.path.normpath(sav)])      # opens the folder, selects the file
        else:
            os.startfile(os.path.dirname(sav))

    def export_save(self):
        import savemgr
        sav = self.save_path()
        if not os.path.isfile(sav):
            message('Kirby Recomp', 'There is no save file yet.\n\nPlay and save in the game first.')
            return
        dst = file_dialog(True, 'Export save', 'GBA save (*.sav)\0*.sav\0All files\0*.*\0\0',
                          self.rom_stem() + '.sav')
        if not dst:
            return
        if not dst.lower().endswith('.sav') and '.' not in os.path.basename(dst):
            dst += '.sav'
        try:
            savemgr.export_save(sav, dst)
            log('save exported to ' + dst)
            message('Kirby Recomp', 'Save exported to:\n' + dst)
        except OSError as e:
            message('Kirby Recomp', 'Could not export the save:\n%s' % e, True)

    def import_save(self):
        import savemgr
        src = file_dialog(False, 'Import save', 'GBA save (*.sav)\0*.sav\0All files\0*.*\0\0')
        if not src:
            return
        ok, msg = savemgr.import_save(src, self.save_path(), self.backup_root(), self.rom_sha1(), self.rom_stem())
        log('save import %s: %s (%s)' % ('ok' if ok else 'rejected', msg, src))
        message('Kirby Recomp', msg, not ok)

    def run_game(self, exe):
        self.backup_now()                              # snapshot of the save before the game can touch it
        log('starting ' + exe)
        # --link off: a plain Play never hosts/joins, whatever a stale [link] mode in kirby.ini says
        return subprocess.run([exe, self.rom, '--gui', '--link', 'off'], cwd=os.path.dirname(exe)).returncode

    # ---- online (launcher/flow.py OnlineSession; NOTES.md "Online: ligacao no launcher") ---------------------
    def online_paths(self):
        d = os.path.join(BASE, 'online')
        os.makedirs(d, exist_ok=True)
        status, flag = os.path.join(d, 'link_status.json'), os.path.join(d, 'start.flag')
        for p in (status, status + '.tmp', flag):
            try:
                os.remove(p)
            except OSError:
                pass
        return status, flag

    def online_address(self, port):
        """The address friends should type: this PC's LAN IP (UDP connect sends nothing) and the port."""
        import socket
        ip = '127.0.0.1'
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(('10.255.255.255', 1))
            ip = s.getsockname()[0]
            s.close()
        except OSError:
            pass
        return '%s:%s' % (ip, port)

    def online_addresses(self, port):
        """Every IPv4 address of this PC as 'ip:port' (the default-route one first). With a VPN (Radmin, Hamachi,
        Tailscale) the address friends need is the VPN adapter's, which is not always the first."""
        import socket
        ips = []
        first = self.online_address(port).rsplit(':', 1)[0]
        for ip in [first] + [a[4][0] for a in _host_addrs()]:
            if ip and ip not in ips and not ip.startswith(('127.', '169.254.', '0.')):
                ips.append(ip)
        if not ips:
            ips = [first]
        return ['%s:%s' % (ip, port) for ip in ips[:4]]

    def online_request_start(self, flag):
        with open(flag, 'w') as f:
            f.write('1')

    def online_failure_hint(self):
        """Last [link] line of kirby.log (it sits next to the ROM), shortened for the status line."""
        try:
            with open(os.path.join(os.path.dirname(self.rom), 'kirby.log'), encoding='utf-8', errors='replace') as f:
                lines = [l.strip() for l in f.readlines()[-60:] if '[link]' in l]
            return lines[-1].split('[link]', 1)[1].strip()[:80] if lines else ''
        except OSError:
            return ''

    def online_launch(self, role, p, status, flag):
        """Start `kirby.exe --link host|join` (the interpreter build: every player runs the same code, which the
        lockstep needs). Returns the Popen, or None when the game could not be prepared."""
        exe = build_interp(self.rom, self.out_dir, _ProgressAdapter(lambda pct, txt: None), self.stamp)
        if not exe:
            return None
        self.backup_now()
        args = [exe, self.rom, '--gui', '--link', role, '--link-port', str(p['port']), '--link-status', status, '--link-required',
                '--link-transport', p.get('transport', 'auto')]
        if role == 'host':
            args += ['--link-players', str(p['players']), '--link-delay', str(p['delay']),
                     '--link-bind', '0.0.0.0', '--link-start-file', flag]
        else:
            args += ['--link-host', p['host']]
        env = dict(os.environ)
        env['KIRBY_LINK_WAIT_S'] = '300'               # a host waits for friends for up to 5 minutes
        log('starting online (%s): %s' % (role, ' '.join(args[2:])))
        return subprocess.Popen(args, cwd=os.path.dirname(exe), env=env)

    def pick_new_rom(self):
        rom = pick_rom()
        if not rom or sha1_of(rom) not in SUPPORTED:
            if rom:
                message('Kirby Recomp', 'This ROM is not supported.', True)
            return False
        self.rom = os.path.abspath(rom)
        with open(os.path.join(BASE, 'last_rom.txt'), 'w') as f:
            f.write(self.rom)
        return True


class KlUI:
    """kirby_ui.dll: kl_run(ini_path, poll) -> KL_PLAY | KL_QUIT | KL_PICK_ROM (written by the UI chat)."""

    def __init__(self, flow_mod):
        self.flow = flow_mod
        self.dll = None
        path = os.path.join(HERE, 'kirby_ui.dll')
        if os.path.exists(path):
            try:
                os.add_dll_directory(HERE)
                dll = ctypes.CDLL(path)
                if hasattr(dll, 'kl_run'):
                    dll.kl_run.argtypes = [ctypes.c_char_p, flow_mod.KlPoll]
                    dll.kl_run.restype = ctypes.c_int
                    self.dll = dll
            except OSError as e:
                log('kirby_ui.dll unavailable: %s' % e)

    def available(self):
        return self.dll is not None

    def run(self, ini_path, poll):
        return self.dll.kl_run(ini_path.encode('utf-8'), poll)

    def set_online_poll(self, cb):
        if hasattr(self.dll, 'kl_set_online_poll'):
            self.dll.kl_set_online_poll.argtypes = [self.flow.KlOnlinePoll]
            self.dll.kl_set_online_poll(cb)

    def set_start_page(self, page):
        if hasattr(self.dll, 'kl_set_start_page'):
            self.dll.kl_set_start_page.argtypes = [ctypes.c_int]
            self.dll.kl_set_start_page(int(page))


def _host_addrs():
    import socket
    try:
        return socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    except OSError:
        return []


def shift_held():
    try:
        return bool(ctypes.windll.user32.GetAsyncKeyState(0x10) & 0x8000)
    except Exception:
        return False


def run_full_launcher(rom, out_dir, stamp, flags):
    """The full launcher (settings screen + Play). Returns an exit code, or None when the UI DLL has no
    kl_run yet and no fake UI was requested (the caller then keeps the previous behaviour)."""
    sys.path.insert(0, HERE)
    import flow
    if os.environ.get('KIRBY_FAKE_UI'):                  # automation: no window, "press" Play when playable
        ui = flow.FakeUI(log=log, max_polls=5000, interval=0.2)
        log('using the fake UI (KIRBY_FAKE_UI)')
    else:
        ui = KlUI(flow)
        if not ui.available():
            return None
    env = LauncherEnv(rom, out_dir, stamp, flow)
    ini_path = os.path.join(os.path.dirname(rom), 'kirby.ini')
    return flow.Controller(env).run(ui, ini_path, shift_held=shift_held())


def main(argv):
    global CONSOLE
    flags = [a for a in argv if a.startswith('--')]
    pos = [a for a in argv if not a.startswith('--')]
    CONSOLE = '--console' in flags
    if '--version' in flags:
        print('KirbyRecomp launcher', VERSION)
        return 0
    if '--bg-build' in flags:
        return background_build_main(pos[0], pos[1])
    rom = find_rom(pos[0] if pos else None)
    if not rom:
        return 1
    rom = os.path.abspath(rom)
    if not os.path.isfile(rom):
        message('Kirby Recomp', 'File not found:\n' + rom, True)
        return 1
    sha = sha1_of(rom)
    if sha not in SUPPORTED:
        message('Kirby Recomp', 'This ROM is not supported.\n\nSHA-1: %s\n\nSupported: %s (SHA-1 %s).'
                % (sha, ', '.join(SUPPORTED.values()), next(iter(SUPPORTED))), True)
        return 1
    os.makedirs(BASE, exist_ok=True)
    with open(os.path.join(BASE, 'last_rom.txt'), 'w') as f:
        f.write(rom)

    stamp = source_stamp()
    stage_sources(stamp)
    sys.path.insert(0, WORK)
    from recomp import toolchain                      # resolved from the staged copy
    out_dir = toolchain.default_out(sha)
    exe = os.path.join(out_dir, 'kirby.exe')
    ok_mark = os.path.join(out_dir, 'build.ok')
    ready = os.path.exists(exe) and os.path.exists(ok_mark) and open(ok_mark).read() == stamp
    if '--rebuild' in flags:
        ready = False
    if '--build-only' not in flags and '--no-fast' not in flags and '--rebuild' not in flags:
        code = run_full_launcher(rom, out_dir, stamp, flags)
        if code is not None:
            return code
    if not ready:
        os.makedirs(out_dir, exist_ok=True)
        fast = '--no-fast' not in flags and '--build-only' not in flags
        if fast:
            code = play_while_building(rom, out_dir, stamp, flags)
            if code is not None:
                return code
            log('fast mode unavailable; building before starting')
        ui = UI()
        ui.update(0, 'Preparing... (first run only; takes a few minutes)')
        good, tail = build(rom, out_dir, stamp, ui)
        ui.close()
        if not good or not os.path.exists(exe):
            if tail != ['cancelado']:
                message('Kirby Recomp - build failed',
                        'The build failed.\n\nLast lines:\n' + '\n'.join(tail[-8:]) +
                        '\n\nFull log: ' + os.path.join(BASE, 'build.log'), True)
            return 2
    if '--build-only' in flags:
        log('ready: ' + exe)
        return 0
    log('starting ' + exe)
    return subprocess.run([exe, rom], cwd=out_dir).returncode


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except SystemExit:
        raise
    except Exception:
        import traceback
        message('Kirby Recomp - unexpected error', traceback.format_exc()[-1500:], True)
        sys.exit(3)
