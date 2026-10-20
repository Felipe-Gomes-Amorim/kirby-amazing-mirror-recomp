# Kirby & The Amazing Mirror (GBA) — static recompilation

**English** · [Português](README.pt-BR.md)

## About

An educational project that statically recompiles *Kirby & The Amazing Mirror* (Game Boy
Advance) into C so it can run natively on a PC, with no emulator. The recompiler translates
the game's ARM7TDMI machine code instruction by instruction and links it against a runtime
that reproduces the console's hardware (video, DMA, timers, interrupts, BIOS).

**This is not a decompilation.** It does not reconstruct the original source code, and it is
not an emulator. Think of it as translating the compiled game once, ahead of time.

> **No ROM, BIOS or game asset is in this repository or in the releases.** You need your own
> legally obtained ROM (USA release, game code `B8KE`). The game is translated *on your machine*,
> from your ROM; everything generated stays in `%LOCALAPPDATA%\KirbyRecomp`.

## Quick start (no Python needed)

Download `KirbyRecomp.zip` from the [Releases](../../releases) page (about 122 MB), extract it
somewhere you can write to, then:

1. **Drag your ROM onto `KirbyRecomp.exe`** (or run it and pick the file).
2. The launcher opens. Choose window size, fullscreen, scaling and volume, then press **Play**.
3. The first run starts with a one-time preparation of a fast mode (a built-in interpreter): from about 15 seconds up to
   about 3 minutes depending on the PC, because the bundled compiler has to build its own runtime libraries once. The game then
   opens while the fully compiled version is built in the background, a few minutes at low priority. The next start uses the
   compiled game and opens in under a second.

Controls (all remappable from the launcher: **Keyboard...** / **Controller...**):

| GBA | Keyboard | Controller (by position) |
|---|---|---|
| A / B | X / Z | east / south button |
| L / R | A / S | shoulder buttons |
| Start / Select | Enter / Backspace | Start / Back |
| D-pad | arrow keys | D-pad or left stick |

`F1` returns to the launcher, `F11` toggles fullscreen, `Esc` quits (these three are reserved).
Windows may warn about an unsigned program: the launcher runs a compiler and the program it produces.

## Status

| Area | State |
|---|---|
| ROM analysis (code vs. data, ARM vs. Thumb, function map) | done: 5,675 functions mapped, frozen and regression-tested |
| ARM/Thumb → C recompiler | working: the game boots and runs 8,000+ frames without stopping |
| Hardware model | tiled/affine backgrounds, sprites, windows, blending, mosaic, DMA, timers, PCM and PSG audio |
| Picture | title screen, opening cutscene and the first level render correctly |
| Launcher | build status, window size 1x–6x, fullscreen, Integer / Fit / Stretch scaling, volume, Play |
| Input | keyboard and one controller (tested with a generic pad), remapping screens, analog stick with adjustable dead zone |
| Sound | PCM and PSG through SDL2 audio (not compared against a reference yet) |
| Save | 32 KiB `<rom>.sav` next to the ROM; written within ~1 s of the game saving (atomic), automatic backups, export/import from the launcher |
| Play while building | interpreter now, compiled game on the next start |
| Online (link cable) | 2–4 players over LAN/VPN/internet from the launcher (UDP with redundancy, TCP fallback); tested between two PCs over Wi-Fi + VPN |

### Saves

The save is the raw 32 KiB `.sav` other emulators use, kept next to your ROM. It is written about a second after the game
saves, through a temporary file that is renamed over the old one, so a crash cannot leave it half-written. Each time you press
Play the launcher also keeps a backup (the 10 most recent per ROM) in `%LOCALAPPDATA%\KirbyRecomp\backups`. The **Save** section
of the launcher can open the folder, **export** the save to a file and **import** one (the current save is backed up first).

### Online play (link cable, experimental)

The game's multiplayer (the 4-player adventure and the sub-games) talks over the GBA link cable. The launcher can carry that cable over a network: **every player needs their own ROM and the same version of KirbyRecomp** (no game data ever travels, the host only checks the ROM's SHA-1).

1. **Host:** `ONLINE...` > choose `PLAYERS`, press `HOST`, allow the Windows firewall prompt and send the address shown (`COPY`). The game starts when everyone has joined, or press `START` to begin with whoever is connected.
2. **Join:** type the host's **IPv4 address** and port, press `JOIN`.
3. **In the game:** `START GAME` > `MULTI PLAYER`; the host confirms `OK?`, then everyone picks a game pak.

`NET` picks the transport (`AUTO` tries UDP, then TCP) and `DELAY` the input delay (`AUTO` is chosen from the measured ping; the game tolerates at most 2 frames with 2 players and 1 with 3-4). The game runs at full speed up to roughly 45 ms ping (about 29 ms with 3-4 players) and slows down, without errors, above that. Over the internet you need to forward the TCP/UDP port (47900 by default) or use a VPN such as Radmin, Hamachi or Tailscale. Online always uses the interpreter build so that every player runs identical code. There is no encryption and no NAT traversal. 4-player sessions were only tested on one PC.

### Known limitations

- Testing is concentrated in the **first area**; deep levels, bosses and every copy ability are
  unverified. Please report crashes with `%LOCALAPPDATA%\KirbyRecomp\launcher.log`.
- Controller support was **tried with one generic controller** and works; other models (Xbox,
  PlayStation, Switch Pro) are expected to work through SDL's mappings but are untested.
- USA ROM only.
- Online play is new: tested with two PCs over Wi-Fi + VPN, not yet with a real port-forwarded internet link or four separate PCs.
- Timing is approximate (not cycle-accurate); the executable is not code-signed.

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
   run time, since the target is only known while running. An interpreter handles code that
   only exists in RAM at run time.
3. **Runtime and hardware** (`recomp/runtime/`, `hw/`): memory bus, the BIOS calls the game
   actually uses (about 13), and a model of video, DMA, timers, sound and input.
4. **Launcher** (`launcher/`, `hw/launcher_ui.c`): settings screen, build status and the
   play-while-building flow.

### Why this is harder than it looks

- The game dispatches most of its logic through tables of function pointers, so following
  direct calls alone finds fewer than 10% of the functions.
- Most `bx rN` instructions are plain function returns (`pop {r0}; bx r0`), not jumps.
- The original compiler uses `bl` as a *long jump* to the shared epilogue of big functions.
  Treating that as a call produces wrong code.
- Some code is copied from ROM into RAM at boot (the sound mixer, SRAM routines) and has to
  be recognised or interpreted from its RAM image.

## Repository layout

| Folder | What is in it |
|---|---|
| [`tools/`](tools) | ROM analysis, indirect-branch handling, confidence scoring, `build_all.py`, release packaging, soak tests |
| [`recomp/`](recomp) | decoder, C emitter, build driver and runtime |
| [`hw/`](hw) | GBA hardware model, launcher UI, config parser and their tests |
| [`launcher/`](launcher) | launcher flow (Python), Windows stub, UI bridge |
| [`tests/`](tests) | recompiler tests (against Unicorn) and launcher / input QA suites |
| [`NOTES.en.md`](NOTES.en.md), [`NOTES.md`](NOTES.md) | technical summary (English) and full working log (Portuguese) |
| [`ROLES.md`](ROLES.md) | who owns what, for the parallel workstreams |

## Running it from the source tree

Requirements (Windows): Python 3.13 with `capstone`, a C compiler (`zig cc` via the `ziglang`
package, or GCC from WinLibs) and the SDL2 development files for MinGW. Put your ROM at
`baserom.gba`.

```
python tools/build_all.py baserom.gba --play 3     # ROM -> analysis -> compile -> open the game
python tools/package_release.py --zip              # build dist/KirbyRecomp.zip
python -m unittest tests.test_launcher_flow tests.test_launcher_qa
```

`build_all.py` runs the analysis passes and `python -m recomp.program`, which compiles the game
and links `kirby.exe`. The recompiler's own tests use `unicorn`; full commands are in
[`NOTES.md`](NOTES.md), section "Recompilador mínimo".

## Prior art

[N64Recomp](https://github.com/N64Recomp/N64Recomp),
[gba-recomp](https://github.com/JRickey/gba-recomp) and
[gbarecomp](https://github.com/mstan/gbarecomp) were used as references. Details and
licences are in `NOTES.md`.

## Licence

The code and documentation of this project are under the [MIT licence](LICENSE). That covers this
project only: *Kirby & The Amazing Mirror* belongs to Nintendo and HAL Laboratory and is not included.
Bundled third-party components are listed in `THIRD_PARTY_NOTICES.txt` inside the release package.

## Legal

Unofficial, non-commercial project, not affiliated with Nintendo, HAL Laboratory or any
other rights holder of *Kirby & The Amazing Mirror*. This repository contains only tools and
original code; the game itself belongs to the user.
