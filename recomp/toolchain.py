"""Localizacao de ferramentas e pastas, sem depender do repositorio nem do ambiente do dev.

Ordem de busca (a primeira que existir):
  zig : $KIRBY_ZIG (exe ou pasta) | <raiz>/tools/zig | <pasta do launcher>/tools/zig |
        `zig` no PATH | `python -m ziglang` (ambiente de desenvolvimento)
  SDL2: $SDL2_DIR | <raiz>/sdl2 | <raiz>/tools/sdl2 | C:/libs/SDL2-2.32.2/x86_64-w64-mingw32
saida (cache): --out DIR | $KIRBY_OUT | <raiz>/build/recomp/program (repo de desenvolvimento,
        se houver .git) | %LOCALAPPDATA%/KirbyRecomp/program (instalacao)
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = '.exe' if os.name == 'nt' else ''


def _launcher_dir():
    """Pasta do launcher/exe congelado (PyInstaller) ou a raiz do pacote."""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return ROOT


def _zig_in(d):
    for cand in (d, os.path.join(d, 'zig' + EXE)):
        if os.path.isfile(cand) and os.path.basename(cand).lower().startswith('zig'):
            return cand
    return None


def find_zig_cmd():
    """Lista de comando para `zig cc` (sem usar sys.executable -m ziglang quando houver zig)."""
    env = os.environ.get('KIRBY_ZIG')
    cands = []
    if env:
        cands.append(env if os.path.isfile(env) else os.path.join(env, 'zig' + EXE))
    for base in (ROOT, _launcher_dir()):
        cands.append(os.path.join(base, 'tools', 'zig', 'zig' + EXE))
        cands.append(os.path.join(base, 'zig', 'zig' + EXE))
    tgt = _target_flags()
    for c in cands:
        if os.path.isfile(c):
            return [c, 'cc'] + tgt
    p = shutil.which('zig')
    if p:
        return [p, 'cc'] + tgt
    return [sys.executable, '-m', 'ziglang', 'cc'] + tgt   # fallback: ambiente de desenvolvimento


def _target_flags():
    """Compile for a FIXED target instead of `native`. With `native`, Zig's cached runtime libraries (compiler-rt,
    mingw) are keyed to this machine's CPU, so a cache shipped in the release package never hits on another PC
    (measured: 0.24 s with a relocated generic cache vs 55 s with the native target). Set KIRBY_ZIG_TARGET=native
    to get the old behaviour, or any other target triple."""
    t = os.environ.get('KIRBY_ZIG_TARGET', 'x86_64-windows-gnu' if os.name == 'nt' else '')
    return ['-target', t] if t and t != 'native' else []


def find_sdl():
    """Raiz do SDL2 devel MinGW (com include/SDL2, lib/libSDL2.dll.a, bin/SDL2.dll) ou None."""
    cands = []
    if os.environ.get('SDL2_DIR'):
        cands.append(os.environ['SDL2_DIR'])
    for base in (ROOT, _launcher_dir()):
        for sub in ('sdl2', os.path.join('tools', 'sdl2')):
            cands.append(os.path.join(base, sub))
    cands.append('C:/libs/SDL2-2.32.2')
    for c in cands:
        for d in (c, os.path.join(c, 'x86_64-w64-mingw32')):
            if os.path.isdir(os.path.join(d, 'include', 'SDL2')):
                return d
    return None


def default_out(rom_sha1=None):
    env = os.environ.get('KIRBY_OUT')
    if env:
        return env
    if os.path.isdir(os.path.join(ROOT, '.git')):
        return os.path.join(ROOT, 'build', 'recomp', 'program')
    base = os.environ.get('LOCALAPPDATA') or os.path.join(os.path.expanduser('~'), '.cache')
    d = os.path.join(base, 'KirbyRecomp')
    return os.path.join(d, rom_sha1[:12] if rom_sha1 else 'program')


def find_rom(explicit=None):
    cands = [explicit] if explicit else []
    cands += [os.path.join(os.getcwd(), 'baserom.gba'), os.path.join(_launcher_dir(), 'baserom.gba'),
              os.path.join(ROOT, 'baserom.gba')]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return explicit or os.path.join(ROOT, 'baserom.gba')


def run(cmd, **kw):
    """subprocess.run sem abrir janela de console no Windows (zig.exe e cia sao apps de console;
    chamados de um launcher grafico apareciam e sumiam varias janelas de cmd)."""
    import subprocess
    if os.name == 'nt':
        kw.setdefault('creationflags', 0x08000000)   # CREATE_NO_WINDOW
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0                            # SW_HIDE
        kw.setdefault('startupinfo', si)
    return subprocess.run(cmd, **kw)
