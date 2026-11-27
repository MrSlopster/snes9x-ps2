# Snes9x 1.63 for PlayStation 2

A port of the current upstream **Snes9x 1.63** core
([snes9xgit/snes9x](https://github.com/snes9xgit/snes9x), master as of
2026-09-24) to the PlayStation 2, replacing the 2011 Snes9x-1.53-based port
(BoGanon/snes9x).

All port code lives in this directory. The emulation core is upstream's,
so every accuracy fix from the last 15 years (PPU, APU, DSP, SA-1, SuperFX,
MSU-1 support, ...) applies. It carries PS2 speed-ups, all guarded by
`#ifdef _EE`: without that definition the tree builds as plain upstream
(see "Repo relationship", and `AGENTS.md` at the top of the repository for
how they are verified).

## What works

- ROM loading: `.sfc`, `.smc`, `.zip`, `.jma`, `.bin`, `.fig`, `.swc`, from
  an on-screen browser
- Video through the Graphics Synthesizer, 640x448 (NTSC) / 640x512 (PAL),
  stretch-scaled, double-buffered and flipped at vblank (no tearing, no
  blocking). Two renderers: the GS draws the SNES picture itself (tiles as
  textured sprites; the default), or the core's software renderer
- Sound in two ways: "Fast" plays the SNES voices on SPU2 hardware voices
  (the default; no echo), "Accurate" is the core's DSP through `audsrv`
  (48 kHz stereo)
- Two digital pads mapped to SNES joypads (multitap is a follow-up; only
  two controller ports are opened)
- SRAM saves (auto-saved when the game writes them and on leaving a game)
- Save states, SPC dumps, IPS/BPS patching (core features, file-based)
- MSU-1 (file-based; no port work needed)

## Not there

- Cheats UI (the cheat engine works; enable via `.cht` files later)
- Netplay, movies

## Building

Requires Docker:

```sh
docker build -t localhost/snes9x-ps2build - <<'EOF'
FROM ghcr.io/ps2dev/ps2dev:latest
RUN apk add --no-cache make bash
EOF

docker run --rm -v "$PWD":/src -w /src/ps2 localhost/snes9x-ps2build make -j8
```

Output: `ps2/snes9x.elf` (EE, about 4.2 MB of code and data; the file has
debug information, strip it to deploy).

Compiler profile: `-O3 -mtune=r5900 -mhard-float -fsingle-precision-constant
-mfused-madd -ffast-math`, with `-O2`/`-Os` for the files whose code has to
fit the 16 KB instruction cache (see the Makefile).

## Deploying / testing on hardware

1. Create the data layout on your storage device:

```
<base>/snes9x/roms/      your ROMs go here (or anywhere: the browser can go there)
<base>/snes9x/srm/       SRAM saves, when the ROM's own folder cannot be written
<base>/snes9x/saves/     save states
<base>/snes9x/shots/     screenshots
<base>/snes9x/spc/       SPC dumps
<base>/snes9x/cheats/
<base>/snes9x/patch/     IPS/BPS patches next to the ROM name
<base>/snes9x/bios/
```

`<base>` is chosen at boot: first the device the ELF was launched from,
then `mass:` (USB), `mc0:` (memory card slot 1), `host:` (ps2link). A USB
stick gets up to ~2 s to mount before the probe. If that place cannot be
written, settings and the folders above move to `mc0:/snes9x` (then
`mc1:/snes9x`).

A game's battery save is `<rom name>.srm` next to the ROM. It is written to
`<rom name>.sr~` first, read back, and only then put in place, so a reset or
power cut during a save leaves the previous one (a leftover `.sr~` is picked
up at the next load). A save that could not be written is reported on
screen.

The IOP drivers (iomanX, fileXio, sio2man, mcman/mcserv, padman, mtapman,
libsd, audsrv, usbd, bdm, bdmfs_fatfs, usbmass_bd) are embedded in the ELF
and loaded at boot after an IOP reset. A launch from `host:` skips the
reset so ps2link keeps working. If a module fails to load, the feature that
needs it is skipped (no audio / no pads) rather than hanging boot.

2. Copy `snes9x.elf` anywhere you can boot it from:
   - **ps2link:** `ps2client execee host:snes9x.elf` (dev/test)
   - **USB**: `mass:/BOOT/BOOT.ELF` style, or via OPL / wLaunchELF
   - **Memory card**: boot from a bootable ELF launcher

3. The emulator loads `argv[1]` when the loader passes one. Otherwise it
   opens an on-screen ROM browser in `<base>/snes9x/roms/` (or the device
   list if that folder doesn't exist). It lists USB drives (`mass0:`,
   `mass1:`, ...), memory cards (`mc0:`, `mc1:`) and `host:`, showing folders
   and ROM files only. A ROM that fails to load returns to the browser with
   the error shown. SMB/network shares are not supported.

   | Button          | Browser                     |
   |-----------------|-----------------------------|
   | D-pad up/down   | move                        |
   | L1/R1, left/right | page                      |
   | Cross           | open folder / load ROM      |
   | Circle/Triangle | parent folder / device list |
   | Start           | settings                    |

4. Settings (Start in the browser, or from the in-game menu) are saved to
   `<base>/snes9x/ps2.cfg`: video mode (Auto, NTSC 480i, PAL 576i, NTSC
   240p, PAL 288p, 480p over component), screen filter (smooth/sharp), frame
   skip (Auto, 0-4), overlay (frame rate, profiler), sound (Off, Accurate,
   Fast), renderer (Software, GS, GS + check against software), network log
   (diagnostics over UDP; address from `mc0:/SYS-CONF/IPCONFIG.DAT`). A new
   video mode must be confirmed with Cross within 10 s or it reverts.

5. In game, hold **Select+Start** for the in-game menu: resume, save/load
   state (`<base>/snes9x/saves/<game>.000`), reset, settings, load another
   ROM (SRAM is saved first).

### Controls

| PS2 pad    | SNES        |
|------------|-------------|
| Cross      | B           |
| Circle     | A           |
| Triangle   | X           |
| Square     | Y           |
| L1 / R1    | L / R       |
| Start      | Start       |
| Select     | Select      |
| D-Pad      | D-Pad       |

### Notes

- Pads are locked to digital mode. Multitap is not usable in v1 (only two
  controller ports are opened); its detection is a follow-up.
- Audio buffers are audsrv-managed; when the ring fills up the emulator
  throttles in the samples-available callback. Samples are pushed to the
  IOP in ~10.7 ms batches (`AUDIO_BATCH`), not on every 48-sample core
  callback: each push is two blocking SIF RPCs. When no sound device is
  available the port paces with wall-clock timing and unix-style auto
  frameskip (`Settings.SkipFrames = AUTO_FRAMERATE`).
- If neither USB nor memcard mounts resolve, the port falls back to
  `host:/snes9x` which only exists under ps2link.

## Known risks / follow-ups

- Performance on a real console (GS renderer, Fast sound): EarthBound
  holds 60 fps in ordinary scenes; F-Zero's races and loading phases keep
  the 65816 busy for most of a frame and need frame skipping. Games using
  SA-1 or SuperFX have not been measured.
- The GS renderer and Fast sound are approximations by design (Settings
  has the exact alternatives); everything else is checked to match upstream
  bit for bit on the host (`AGENTS.md`).
- PS2-specific work already done in the port layer:
  - The core renders `BGR555`, which is the GS PSMCT16 layout, so frames
    need no CPU colour conversion.
  - Frames are uploaded zero-copy: one DMA chain whose REF tags point
    straight at `GFX.Screen` rows, sending only the rendered width.
    `FlushCache(0)` (a fixed 8 KB D-cache walk) replaces a per-line
    `SyncDCache` over the frame.
  - Display flips happen in the vblank interrupt, so the emulator never
    waits on the TV.
  - Audio is batched to cut IOP round trips about tenfold.
- Follow-up candidates that need hardware measurement first: `-O2` vs
  `-O3` (the R5900 has a 16 KB I-cache, so -O3 inlining can lose), 240p
  non-interlaced output (halves VRAM and fill, sharper image, TV
  compatibility to check), `-ffunction-sections`/`--gc-sections` to shed
  unused core code (filters, netplay), frame-skip policy tuning.
- Save-state format is upstream 1.63 (incompatible with 2011 fork states).

## Repo relationship

- `snes9x-1.63-ps2` = upstream master + `ps2/` port directory, plus core
  edits under `#ifdef _EE` (65816 dispatch and idle-loop hook, SPC700 loop
  and timers, deferred APU work, the DSP's fast-sound path, hooks for the
  GS renderer) and two that apply everywhere:
  - `memmap.cpp`: `XPSdecode` signature (portability; see the commit
    history).
  - `pixform.h`: an additive `BGR555` pixel format (selected by the PS2
    Makefile only). It reuses RGB555's masks because the core's colour
    math treats the two outer fields identically; `tests/test_pixform.cpp`
    checks that property over ~6.3M operand pairs.
- The 2011 fork (`BoGanon/snes9x`) is kept as reference only.
