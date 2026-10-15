# Kirby & The Amazing Mirror (GBA) — static recompilation

**English** · [Português](README.pt-BR.md)

## About

An educational project that statically recompiles *Kirby & The Amazing Mirror* (Game Boy
Advance) into C so it can run natively on a PC, with no emulator. The recompiler translates
the game's ARM7TDMI machine code instruction by instruction and links it against a runtime
that reproduces the console's hardware (video, DMA, timers, interrupts, BIOS).

**This is not a decompilation.** It does not reconstruct the original source code, and it is
not an emulator. Think of it as translating the compiled game once, ahead of time.

> **No ROM, BIOS or game asset is in this repository.** You need your own legally obtained
> ROM (USA, game code `B8KE`), placed in the project root as `baserom.gba`. It is
> git-ignored, as is everything generated under `build/`.

## Status

| Stage | State |
|---|---|
| ROM analysis (code vs. data, ARM vs. Thumb, function map) | done — 5,669 functions mapped, map frozen and regression-tested |
| ARM/Thumb → C recompiler | working — the game boots and runs 8,000+ frames without aborting |
| Hardware model | wired in: tiled/affine backgrounds, sprites, windows, blending, DMA, timers, PCM audio |
| Picture | the title screen, the opening cutscene and the first level render correctly (saved as images) |
| Launcher | window with build status, size 1x-6x, fullscreen, Integer/Fit/Stretch scaling, volume, Play; F1 returns to it |
| Window and keyboard | works (X = A, Z = B, Enter = Start, Backspace = Select, arrows, A/S = L/R); remapping screen is next |
| Controller | implemented by button position; not yet tested with a physical controller |
| Sound output | PCM and PSG through SDL2 audio |
| Save | works: 32 KiB `<rom>.sav` |

Numbers, decisions and open problems: an English summary in [`NOTES.en.md`](NOTES.en.md), and the
full working log in [`NOTES.md`](NOTES.md) (Portuguese).

## How it works

```
baserom.gba ──► analysis ──► function map ──► recompiler ──► C ──► native program
                                                                       ▲
                                                  runtime (memory, BIOS, hardware)
```

1. **Analysis** (`tools/`): reads the ROM, follows control flow from the entry point and
   decides what is ARM code, Thumb code, tables or plain data. The output is a function map
   with a confidence level per function.
2. **Recompiler** (`recomp/`): each instruction becomes a few lines of C operating on a CPU
   state (`r0`–`r15`, flags). Calls through function pointers go through a lookup table at
   run time, since the target is only known while running.
3. **Runtime and hardware** (`recomp/runtime/`, `hw/`): memory bus, the BIOS calls the game
   actually uses (about 13), and a model of the video, DMA and timer registers.

### Why this is harder than it looks

- The game dispatches most of its logic through tables of function pointers, so following
  direct calls alone finds fewer than 10% of the functions.
- Most `bx rN` instructions are plain function returns (`pop {r0}; bx r0`), not jumps.
- The original compiler uses `bl` as a *long jump* to the shared epilogue of big functions.
  Treating that as a call produces wrong code.
- Some code is copied from ROM into RAM at boot (the sound mixer, for example) and has to be
  recompiled from its RAM image.

## Repository layout

| Folder | What is in it |
|---|---|
| [`tools/`](tools) | ROM analysis, indirect-branch handling, confidence scoring |
| [`recomp/`](recomp) | decoder, C emitter and runtime |
| [`hw/`](hw) | GBA hardware model and its tests |
| [`tests/`](tests) | recompiler tests (instruction-level comparison against Unicorn) |
| [`NOTES.en.md`](NOTES.en.md), [`NOTES.md`](NOTES.md) | technical summary (English) and full working log (Portuguese) |
| [`ROLES.md`](ROLES.md) | who owns what, for the parallel workstreams |

## Quick start (no Python needed)

Download `KirbyRecomp.zip` from the [Releases](../../releases) page (about 95 MB), or build it yourself with
`python tools/package_release.py --zip`. Then:

1. Have your own ROM (USA, game code `B8KE`).
2. **Drag the ROM onto `KirbyRecomp.exe`.** The launcher opens: choose size, scaling, volume, then **Play**.
3. The first run is playable within seconds (interpreter) while the compiled version builds in the
   background (a few minutes). The next runs start in under a second.

The generated game lives in `%LOCALAPPDATA%\KirbyRecomp`; the package itself contains no game data.
Controls: Z = A, X = B, Enter = Start, Backspace = Select, arrows, A/S = L/R, F11 = fullscreen, Esc = quit.
Windows may warn about an unsigned program: the launcher runs a compiler and the program it produces.

## Running it from the source tree

Requirements (Windows): Python 3.13 and a C compiler (GCC from WinLibs, or `zig cc`).

```
pip install capstone

python tools/analyze_rom.py                   # writes build/analysis/ (function map)
python tools/resolve_indirect.py baserom.gba  # classifies indirect branches
python tools/score_functions.py baserom.gba   # per-function confidence
```

The recompiler and its tests use their own environment (`unicorn`, `ziglang`). The full
commands are in [`NOTES.md`](NOTES.md), section "Recompilador mínimo".

## Prior art

[N64Recomp](https://github.com/N64Recomp/N64Recomp),
[gba-recomp](https://github.com/JRickey/gba-recomp) and
[gbarecomp](https://github.com/mstan/gbarecomp) were used as references. Details and
licences are in `NOTES.md`.

## Licence

The code and documentation of this project are under the [MIT licence](LICENSE). That covers this
project only: *Kirby & The Amazing Mirror* belongs to Nintendo and HAL Laboratory and is not included.

## Legal

Unofficial, non-commercial project, not affiliated with Nintendo, HAL Laboratory or any
other rights holder of *Kirby & The Amazing Mirror*. This repository contains only tools and
original code; the game itself belongs to the user.
