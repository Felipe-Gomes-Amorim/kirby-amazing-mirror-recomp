"""Assemble the KirbyRecomp release folder (Riven).

  python tools/package_release.py [--out dist/KirbyRecomp] [--zip] [--python-zip FILE] [--zig DIR] [--sdl2 DIR]

Layout produced (no ROM, no generated game code, nothing derived from the game):
  KirbyRecomp.exe          stub that runs python\\pythonw.exe app\\launcher\\launcher.py
  python\\                  Python embeddable distribution + capstone
  app\\launcher, recomp, hw, tools   sources (the C runtime is compiled on the user's machine)
  app\\tools\\zig            portable Zig compiler (C compiler)
  app\\sdl2                 SDL2 development files + SDL2.dll
It then AUDITS the folder and fails if a ROM, a save, generated C or a built game exe slipped in.
"""
import argparse, fnmatch, os, re, shutil, subprocess, sys, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY_VER = '%d.%d.%d' % sys.version_info[:3]
PY_URL = f'https://www.python.org/ftp/python/{PY_VER}/python-{PY_VER}-embed-amd64.zip'
FORBIDDEN = ['*.gba', '*.sav', '*.sgm', 'gen_*.c', 'shard_*.c', 'table.c', 'rc_protos.h', 'kirby.exe',
             'rt_boot.exe', 'ram_dump.bin', 'baserom*', 'functions.csv', 'map.json', 'jumptables.csv']
FORBIDDEN_DIRS = {'build', '.git', '.venv', '__pycache__', 'frames'}


def copy_tree(src, dst, skip_dirs=(), only_ext=None):
    n = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in FORBIDDEN_DIRS and d not in skip_dirs]
        for f in files:
            if only_ext and not f.lower().endswith(only_ext):
                continue
            rel = os.path.relpath(os.path.join(root, f), src)
            out = os.path.join(dst, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copy2(os.path.join(root, f), out)
            n += 1
    return n


def find_zig(arg):
    for c in [arg, os.path.join(ROOT, 'tools', 'zig'), os.path.join(ROOT, '.venv', 'Lib', 'site-packages', 'ziglang')]:
        if c and os.path.exists(os.path.join(c, 'zig.exe')):
            return c
    sys.exit('zig.exe not found: pass --zig DIR (the ziglang pip package directory works)')


def find_sdl2(arg):
    for c in [arg, os.environ.get('SDL2_DIR'), os.path.join(ROOT, 'sdl2'),
              'C:/libs/SDL2-2.32.2/x86_64-w64-mingw32', 'C:/libs/SDL2-2.32.2_64-w64-mingw32']:
        if c and os.path.exists(os.path.join(c, 'include', 'SDL2', 'SDL.h')):
            return c
    sys.exit('SDL2 development files (MinGW, x86_64) not found: pass --sdl2 DIR')


def find_capstone():
    for c in [os.path.join(ROOT, '.venv', 'Lib', 'site-packages', 'capstone')]:
        if os.path.isdir(c):
            return c
    try:
        import capstone
        return os.path.dirname(capstone.__file__)
    except ImportError:
        sys.exit('capstone not found: pip install capstone')


def slim_zig(zdir):
    """Keep only what compiling C for x86_64-windows-gnu needs (the compiler is 177 MB by itself; the
    target libc headers for every OS are another ~120 MB). Verified by the build test after packaging."""
    removed = 0
    lib = os.path.join(zdir, 'lib')
    drop = ['libcxx', 'libcxxabi', 'libtsan', 'docs', 'build-web', 'tsan', 'fuzzer', 'init']
    libc = os.path.join(lib, 'libc')
    for d in os.listdir(libc) if os.path.isdir(libc) else []:
        if d not in ('mingw', 'include', 'wasi') and os.path.isdir(os.path.join(libc, d)):
            drop.append(os.path.join('libc', d))
    inc = os.path.join(libc, 'include')
    for d in os.listdir(inc) if os.path.isdir(inc) else []:
        if 'windows' not in d:           # keep any-windows-any / x86_64-windows-gnu ...
            drop.append(os.path.join('libc', 'include', d))
    for d in drop:
        p = os.path.join(lib, d)
        if os.path.isdir(p):
            removed += sum(os.path.getsize(os.path.join(r, n)) for r, _, fs in os.walk(p) for n in fs)
            shutil.rmtree(p)
    print(f'  slimmed zig: -{removed / 1e6:.0f} MB')


def rmtree_retry(path, tries=12):
    """Windows (Defender/indexer) may hold files of a just-built tree for a few seconds."""
    import time
    for i in range(tries):
        if not os.path.exists(path):
            return
        try:
            shutil.rmtree(path)
            return
        except OSError:
            time.sleep(2 + i)
    if os.path.exists(path):
        shutil.rmtree(path)        # let the real error surface


def run(cmd, **kw):
    print('  $', ' '.join(str(c) for c in cmd))
    subprocess.run(cmd, check=True, **kw)


def audit(dist):
    bad, big = [], []
    for root, dirs, files in os.walk(dist):
        for d in dirs:
            if d in FORBIDDEN_DIRS and d != '__pycache__':
                bad.append(os.path.join(root, d) + os.sep)
        for f in files:
            p = os.path.join(root, f)
            if any(fnmatch.fnmatch(f.lower(), pat) for pat in FORBIDDEN):
                bad.append(p)
            if os.path.getsize(p) == 16 * 1024 * 1024:       # a 16 MiB file would be a ROM image
                bad.append(p + '  (16 MiB: looks like a GBA ROM)')
            elif f.lower().endswith('.exe') and 'zig' not in p and 'python' not in p \
                    and f.lower() not in ('kirbyrecomp.exe',):
                bad.append(p + '  (unexpected exe)')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist', 'KirbyRecomp'))
    ap.add_argument('--python-zip')
    ap.add_argument('--zig')
    ap.add_argument('--sdl2')
    ap.add_argument('--zip', action='store_true')
    ap.add_argument('--full-zig', action='store_true', help='do not prune the Zig lib directory')
    a = ap.parse_args()
    dist = os.path.abspath(a.out)
    zig, sdl, cap = find_zig(a.zig), find_sdl2(a.sdl2), find_capstone()
    rmtree_retry(dist)
    os.makedirs(dist)
    print('1/6 python (embeddable) + capstone')
    pz = a.python_zip or os.path.join(ROOT, 'build', 'dist_cache', os.path.basename(PY_URL))
    if not os.path.exists(pz):
        os.makedirs(os.path.dirname(pz), exist_ok=True)
        print('  downloading', PY_URL)
        urllib.request.urlretrieve(PY_URL, pz)
    pydir = os.path.join(dist, 'python')
    with zipfile.ZipFile(pz) as z:
        z.extractall(pydir)
    pth = [f for f in os.listdir(pydir) if f.endswith('._pth')][0]
    stem = pth[:-5]
    with open(os.path.join(pydir, pth), 'w') as f:
        f.write(f'{stem}.zip\n.\nLib\\site-packages\nimport site\n')
    copy_tree(cap, os.path.join(pydir, 'Lib', 'site-packages', 'capstone'))
    # the embeddable Python ignores PYTHONPATH; this .pth lets the launcher add its work tree to sys.path
    with open(os.path.join(pydir, 'Lib', 'site-packages', 'kirby_work.pth'), 'w') as f:
        f.write("import os, sys; w = os.environ.get('KIRBY_WORK'); w and sys.path.insert(0, w)\n")
    print('2/6 sources')
    app = os.path.join(dist, 'app')
    copy_tree(os.path.join(ROOT, 'recomp'), os.path.join(app, 'recomp'), only_ext=('.py', '.c', '.h'))
    copy_tree(os.path.join(ROOT, 'hw'), os.path.join(app, 'hw'), skip_dirs=('tests',), only_ext=('.c', '.h'))
    copy_tree(os.path.join(ROOT, 'tools'), os.path.join(app, 'tools'), skip_dirs=('zig',),
              only_ext=('.py', '.csv'))
    os.makedirs(os.path.join(app, 'launcher'))
    for f in sorted(os.listdir(os.path.join(ROOT, 'launcher'))):
        if f.endswith('.py'):
            shutil.copy2(os.path.join(ROOT, 'launcher', f), os.path.join(app, 'launcher', f))
    print('3/6 zig + SDL2')
    copy_tree(zig, os.path.join(app, 'tools', 'zig'))
    if not a.full_zig:
        slim_zig(os.path.join(app, 'tools', 'zig'))
    copy_tree(sdl, os.path.join(app, 'sdl2'))
    print('4/6 build the progress-window DLL and the launcher stub (with the bundled zig)')
    zexe = os.path.join(app, 'tools', 'zig', 'zig.exe')
    inc, lib = os.path.join(sdl, 'include', 'SDL2'), os.path.join(sdl, 'lib')
    run([zexe, 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-shared', '-DKL_BUILD_DLL', '-I', inc,
         os.path.join(ROOT, 'hw', 'window_sdl.c'), os.path.join(ROOT, 'hw', 'launcher_ui.c'),
         os.path.join(ROOT, 'hw', 'ui_font_data.c'), os.path.join(ROOT, 'hw', 'config_ini.c'),
         os.path.join(ROOT, 'launcher', 'ui_dll.c'),
         os.path.join(lib, 'libSDL2.dll.a'), '-o', os.path.join(app, 'launcher', 'kirby_ui.dll')])   # import lib, not the static one
    shutil.copy2(os.path.join(sdl, 'bin', 'SDL2.dll'), os.path.join(app, 'launcher', 'SDL2.dll'))
    import ctypes
    os.add_dll_directory(os.path.join(app, 'launcher'))
    probe = ctypes.CDLL(os.path.join(app, 'launcher', 'kirby_ui.dll'))
    missing = [n for n in ('kl_run', 'kl_dropped_path', 'kui_open', 'kui_progress', 'kui_close') if not hasattr(probe, n)]
    if missing:
        sys.exit('kirby_ui.dll does not export: ' + ', '.join(missing))
    run([zexe, 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-municode', '-Wl,--subsystem,windows',
         os.path.join(ROOT, 'launcher', 'stub.c'), '-o', os.path.join(dist, 'KirbyRecomp.exe')])
    shutil.copy2(os.path.join(ROOT, 'LICENSE'), os.path.join(dist, 'LICENSE'))
    os.makedirs(os.path.join(app, 'hw', 'fonts'), exist_ok=True)
    shutil.copy2(os.path.join(ROOT, 'hw', 'fonts', 'OFL.txt'), os.path.join(app, 'hw', 'fonts', 'OFL.txt'))
    print('4b/6 warm the Zig cache (the real interpreter-only build on a zero-filled dummy ROM; no game data)')
    zcache = os.path.join(app, 'tools', 'zigcache')
    os.makedirs(zcache, exist_ok=True)
    tmp = os.path.join(ROOT, 'build', 'dist_cache', 'warm')
    rmtree_retry(tmp)
    os.makedirs(tmp)
    dummy = os.path.join(tmp, 'dummy.gba')
    with open(dummy, 'wb') as f:
        f.write(bytes(16 * 1024 * 1024))
    # Run it EXACTLY like the launcher does: from a staged copy of the sources whose directory does NOT contain
    # the Zig lib dir. Zig's cache manifests classify input files by path prefix (cwd / lib dir / cache dir);
    # warming from a cwd that contains the lib dir (e.g. the repo with dist/ inside it) records different
    # prefixes and the shipped cache never hits for users (measured: 127 s instead of 6 s).
    work = os.path.join(tmp, 'work')
    for sub in ('recomp', 'hw', 'tools'):
        shutil.copytree(os.path.join(app, sub), os.path.join(work, sub),
                        ignore=shutil.ignore_patterns('zig', 'zigcache', '__pycache__', '*.pyc'))
    env = dict(os.environ, KIRBY_ZIG=zexe, SDL2_DIR=os.path.join(app, 'sdl2'), ZIG_GLOBAL_CACHE_DIR=zcache,
               PYTHONPATH=work, KIRBY_WORK=work)
    subprocess.run([sys.executable, '-m', 'recomp.program', '--rom', dummy, '--out', os.path.join(tmp, 'out'),
                    '--interp-only'], cwd=work, env=env, check=True, capture_output=True)
    for sub in ('tmp',):
        rmtree_retry(os.path.join(zcache, sub))
    rmtree_retry(tmp)
    zsize = sum(os.path.getsize(os.path.join(r, n)) for r, _, fs in os.walk(zcache) for n in fs)
    print(f'  zig cache shipped: {zsize / 1e6:.0f} MB')
    print('5/6 README + third-party notices')
    with open(os.path.join(dist, 'THIRD_PARTY_NOTICES.txt'), 'w', encoding='utf-8') as f:
        f.write(NOTICES)
    with open(os.path.join(dist, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write(README)
    print('6/6 audit (python syntax of everything shipped, then forbidden files)')
    import ast
    for root, _, files in os.walk(app):
        for f in files:
            if f.endswith('.py') and 'zig' not in root:
                path = os.path.join(root, f)
                try:
                    ast.parse(open(path, encoding='utf-8').read(), path)
                except SyntaxError as e:
                    sys.exit(f'SYNTAX ERROR in shipped file {path}: {e}')
    bad = audit(dist)
    if bad:
        print('AUDIT FAILED - these must not be shipped:')
        for b in bad:
            print('   ', b)
        sys.exit(1)
    total = sum(os.path.getsize(os.path.join(r, n)) for r, _, fs in os.walk(dist) for n in fs)
    print(f'audit ok. {dist}  {total / 1e6:.0f} MB')
    for sub in ('python', 'app', 'app/tools/zig', 'app/sdl2'):
        p = os.path.join(dist, sub)
        s = sum(os.path.getsize(os.path.join(r, n)) for r, _, fs in os.walk(p) for n in fs)
        print(f'   {sub:16s} {s / 1e6:6.1f} MB')
    if a.zip:
        z = shutil.make_archive(dist, 'zip', os.path.dirname(dist), os.path.basename(dist))
        print('zip:', z, f'{os.path.getsize(z) / 1e6:.0f} MB')


NOTICES = """This package bundles third-party software. Each keeps its own licence; full texts are in the
LICENSE files shipped inside the corresponding folder.

  python/                          Python embeddable distribution  - PSF License    https://www.python.org
  python/Lib/site-packages/capstone  Capstone disassembler         - BSD 3-Clause   https://www.capstone-engine.org
  app/tools/zig/                   Zig toolchain (C compiler)      - MIT            https://ziglang.org
                                   (its lib/ directory carries LLVM/clang and mingw-w64 headers under
                                    their own permissive licences; see lib/ and the LICENSE file)
  app/sdl2/                        SDL2 development files, SDL2.dll - zlib          https://www.libsdl.org
  launcher fonts                   Pixelify Sans, Silkscreen (baked in as bitmaps) - SIL OFL 1.1 (app/hw/fonts/OFL.txt)

The launcher, recompiler, runtime and hardware model (app/launcher, app/recomp, app/hw, app/tools/*.py)
are the sources of https://github.com/Felipe-Gomes-Amorim/kirby-amazing-mirror-recomp ,
under the MIT licence (see LICENSE in this folder).
This package contains no ROM, BIOS, or any code or data derived from the game.
"""

README = """Kirby & The Amazing Mirror - Recomp (unofficial)
=================================================

  1. Get YOUR OWN ROM (USA release, game code B8KE).
  2. Drag the ROM onto KirbyRecomp.exe (or double-click it and pick the file).
  3. First run: a one-time preparation of the fast mode (15 seconds to about 3 minutes, depending on the PC), then the game
     starts. In the background it builds the fully recompiled version (a few minutes); the next start uses it and opens instantly.
  4. Online (2-4 players): ONLINE... in the launcher. Everyone needs their own ROM and this same version.

The generated game lives in %LOCALAPPDATA%\KirbyRecomp. No game data is shipped in this package.
Default controls: X = A, Z = B, Enter = Start, Backspace = Select, arrow keys, A / S = L / R,
F11 = fullscreen, Esc = quit. If something goes wrong see %LOCALAPPDATA%\KirbyRecomp\launcher.log and build.log.

Unofficial, non-commercial; not affiliated with Nintendo or HAL Laboratory.
Windows may warn about an unsigned program: the launcher runs a compiler and the program it produces.
"""

if __name__ == '__main__':
    main()
