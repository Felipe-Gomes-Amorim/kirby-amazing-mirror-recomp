# Technical notes (English summary)

The full working log is [NOTES.md](NOTES.md), written in Portuguese by four parallel workstreams.
This file is a **curated English summary** of what matters for a newcomer: what exists, how it
works, what was learned, and what is still open. It is a snapshot: numbers may drift as the
log moves on, and section names below refer to headings in `NOTES.md`.

## 1. What this is

A static recompiler for *Kirby & The Amazing Mirror* (GBA, USA, game code `B8KE`). It reads the
user's own ROM, translates the ARM7TDMI code (ARM and Thumb) to C, and links it to a runtime
that reproduces the console's hardware. It is **not** a decompilation (no source is rebuilt) and
**not** an emulator (the game code is translated once, ahead of time).

No ROM, BIOS or generated output is committed.

## 2. Status

| Area | State |
|---|---|
| ROM analysis, function map | done; map frozen (about 5,700 functions) with a regression check |
| Recompiler (ARM/Thumb → C) | runs the game: 8,000+ frames, 0 aborts, 0 unresolved addresses |
| Hardware | tiled/affine backgrounds, sprites, windows, blending, mosaic, DMA, timers, PCM FIFO, PSG |
| Picture | title screen, opening cutscene and first level render correctly |
| Window / input | SDL2 window, keyboard and one controller, remappable, analog stick as D-pad |
| Sound output | PCM + PSG through SDL2 audio, heard in game; not compared against a reference |
| Save | 32 KiB SRAM file, atomic debounced writer, 10 automatic backups per ROM, import/export |
| Launcher | Python + SDL2 UI: ROM check, build/progress, play-while-building (interpreter first, compiled later), settings, saves, ONLINE page |
| Online | link-cable multi-play (SIO) emulated; deterministic per-frame lockstep between instances; TCP or UDP-with-redundancy transport; 2-4 players; tested on two PCs over Wi-Fi + VPN |
| Speed | about 9.7× real time without frame limiting; PPU rendering is the main cost |

Not done: a whole-program differential oracle, cycle-accurate timing, save states (the compiled build keeps state on the
native stack), regions other than USA, and a full playthrough check (testing so far concentrates on the first area).
A fallback interpreter for code the analysis did not predict exists and is used when needed.

## 3. How to run it

Requirements (Windows): Python 3.13, a C compiler (GCC from WinLibs, or `zig cc`), SDL2 development
files for the window. Put your ROM at `baserom.gba`.

```
python tools/analyze_rom.py                       # function map -> build/analysis/
.venv/Scripts/python -m recomp.program --window 3 # recompile, then play in a window (scale 3)
.venv/Scripts/python -m recomp.program --frames 2000 --keys "START@620+8,A@800" --snap 2000
```

Keys in the window (defaults): X = A, Z = B, Enter = Start, Backspace = Select, arrow keys, S = R, A = L,
F1 = back to the launcher, F11 = fullscreen, Esc = quit. A launcher window (size, fullscreen, scaling, volume, Play)
is shown first; settings live in `kirby.ini` next to the ROM.

## 4. Architecture

```
baserom.gba ─► analysis ─► function map ─► recompiler ─► C shards ─► kirby.exe
                                                                       ▲
                                              runtime: bus + BIOS HLE + hw/ model
```

- **`tools/`**: recursive-descent analysis (ARM/Thumb), jump tables, literal pools, indirect-branch
  classification, per-function confidence, `map.json` export and regression check.
- **`recomp/`**: own ARMv4T decoder → IR → C emitter. One guest function becomes one C function over
  a CPU state; unknown or indirect targets go through a lookup table (`rt_lookup`). Code copied to
  RAM at boot (the sound mixer, SRAM routines) is found at run time and emitted as overlays keyed
  by address and body hash.
- **`recomp/runtime/`**: memory bus (ROM, EWRAM, IWRAM, SRAM), BIOS high-level emulation of the
  13 calls the game uses, IRQ delivery at VBlank, key scripting and frame snapshots.
- **`hw/`**: scanline PPU, DMA, timers, interrupts, PCM FIFO and PSG, SDL2 window and audio.

## 5. What we learned (the non-obvious parts)

1. **Direct calls find fewer than 1% of the functions.** The game dispatches almost everything through
   tables of function pointers. Scanning the ROM for Thumb pointers to `push {…, lr}` prologues
   brings the map to about 5,700 functions.
2. **Most `bx rN` are returns, not jumps.** The compiler (agbcc) returns with `pop {r0}; bx r0`.
   Of about 5,100 apparent indirect branches, 12 remain unresolved. Real indirect calls go through
   libgcc stubs (`_call_via_rN`, 321 call sites) and cannot be resolved statically.
3. **`bl` is used as a long jump.** Thumb `b` reaches only ±2 KB, so in big functions the compiler
   jumps to the shared epilogue with `bl`. A naive analyzer treats that as a call and splits the
   function. 49 such continuations are merged into their parents.
4. **Leaf functions without `push {lr}`** (empty callbacks, setters) are referenced only by pointer
   tables and were missed by the first scan.
5. **Function-pointer tables are code-adjacent data.** The sound engine's command handlers live in
   a ROM jump table (`0x08B58490`); only the entries reachable by `bl` were in the map until the
   runtime hit the others.
6. **Validation by emulator breakpoints did not scale.** Thousands of breakpoints made mGBA unusably
   slow. Validation instead uses structural scoring of every function and, now, what the recompiled
   game actually executes.

## 6. Sound and save (M4A, SRAM)

The game uses Nintendo's standard **M4A / MusicPlayer2000** engine, run as ordinary game code, not
the BIOS sound driver. What it asks of the hardware:

| Item | Value |
|---|---|
| Output | PCM through FIFO A/B, fed by DMA1/DMA2 in sound-FIFO mode (`DMAxCNT_H = 0xB600`) |
| Routing (`SOUNDCNT_H`) | `0xA90E`: channel A → right, channel B → left, both on Timer 0 |
| Sample rate | 15,768 Hz: Timer 0 reload `-1064` cycles, 264 samples per frame, DMA restarted every 6 frames |
| Mixer | 1 KiB routine copied from ROM `0x0814F46C` to IWRAM `0x03000060` (Thumb entry `0x03000061`) |
| Per frame | `m4aSoundVSync` (`0x0814FA3C`) then `SoundMain` (`0x0814F3E8`) |
| PSG channels | also driven by the engine (square, wave, noise) |

**Save:** the standard `SRAM_V113` library, 32 KiB at `0x0E000000` on an 8-bit bus, with four
routines (read, fast read, write, verify). A raw 32,768-byte file is enough to persist it.
`ReadSramFast` copies a routine to the stack and runs it there; the copy is byte-identical to the
ROM function, so it can be aliased.

## 7. Verification, honestly

- **Instruction level:** about 420 valid instructions per mode, three states each, compared against
  Unicorn: 0 divergences. 40 real ROM functions × 30 states: 0 divergences.
- **Oracle caveat:** Unicorn/QEMU emulates an **ARM926 (ARMv5)**, not the ARM7TDMI. Cases where they
  differ (unaligned loads, PC reads with register shifts, some `bx`/`ldm pc`) are excluded, so they
  are **not** validated.
- **Not covered:** calls (`bl`), stack, IRQ and SWI behaviour against an oracle; only the boot and
  the first level exercise them. `ArcTan2` and `RegisterRamReset` were written from memory of the
  BIOS and only spot-checked.
- **Sound** was never listened to against a reference.

## 8. Open problems

- The read of unmapped address `0x100F0D55` is explained: the game reads `table[0xFFFF]` (an "empty slot"
  sentinel) at `0x08034FA8`, an original quirk that is harmless on hardware (open bus). Not confirmed against
  a reference emulator.
- A whole-program oracle to catch silent errors.
- Online: real-internet test with a forwarded port, four separate PCs, and measuring UDP against TCP on a real network.

## 9. Where to look in `NOTES.md`

| Topic | Section |
|---|---|
| ARM/Thumb crash course with real examples from this ROM | 2. Mini-curso ARM7TDMI / Thumb |
| GBA memory map | 3. Mapa de memoria do GBA |
| Analysis tooling and map schema | Como rodar: ambiente e analise da ROM |
| Indirect branches | Saltos indiretos; Revisao das 86 funcoes de baixa confianca |
| Hardware model | Hardware GBA (hw/) |
| Recompiler and runtime | Recompilador minimo |
| Sound and save | Som (M4A) e save (SRAM) |

## 10. Prior art

[N64Recomp](https://github.com/N64Recomp/N64Recomp), [gba-recomp](https://github.com/JRickey/gba-recomp)
and [gbarecomp](https://github.com/mstan/gbarecomp) were studied for ideas; no code was copied.
