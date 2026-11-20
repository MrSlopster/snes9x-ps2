# Snes9x 1.63 for PlayStation 2

A port of the current upstream **Snes9x 1.63** core
([snes9xgit/snes9x](https://github.com/snes9xgit/snes9x), master as of
2026-09-24) to the PlayStation 2, replacing the 2011 Snes9x-1.53-based port
(BoGanon/snes9x).

All port code lives in this directory. The upstream emulation core is used
essentially unmodified (two small, documented edits; see "Repo
relationship"), so every accuracy fix from the last 15 years (PPU, APU, DSP,
SA-1, SuperFX, MSU-1 support, ...) applies automatically.

## What works (v1)

- ROM loading: `.sfc`, `.smc`, `.zip`, `.jma`, `.bin`, `.fig`, `.swc`
- Video through the Graphics Synthesizer, 640x448, stretch-scaled,
  double-buffered and flipped at vblank (no tearing, no blocking)
- Audio through `audsrv` (48 kHz stereo), sound-sync throttling
- Two digital pads mapped to SNES joypads (multitap is a follow-up; v1
  opens two controller ports only)
- SRAM saves (auto-saved every 30 s and on shutdown)
- Save states, SPC dumps, IPS/BPS patching (core features, file-based)
- MSU-1 (file-based; no port work needed)

## Not in v1

- The old on-screen GUI/config browser (the 2011 `interface` library pins
  ps2sdk libraries that no longer exist; needs its own modernization)
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

Output: `ps2/snes9x.elf` (EE, ~3.2 MB loadable).

Compiler profile: `-O3 -mtune=r5900 -mhard-float -fsingle-precision-constant
-mfused-madd -ffast-math` (R5900 pipeline tuning, hard float). `-Ofast` is a
candidate for later hardware testing only.

## Deploying / testing on hardware

1. Create the data layout on your storage device:

```
<base>/snes9x/roms/      your ROMs go here
<base>/snes9x/srm/       SRAM saves (created automatically)
<base>/snes9x/saves/     save states
<base>/snes9x/shots/     screenshots
<base>/snes9x/spc/       SPC dumps
<base>/snes9x/cheats/
<base>/snes9x/patch/     IPS/BPS patches next to the ROM name
<base>/snes9x/bios/
```

`<base>` is chosen at boot: first `mass:` (USB), then `mc0:` (memory card
slot 1), then `host:` (ps2link). All three mounts are probed at startup.

2. Copy `snes9x.elf` anywhere you can boot it from:
   - **ps2link:** `ps2client execee host:snes9x.elf` (dev/test)
   - **USB**: `mass:/BOOT/BOOT.ELF` style, or via OPL / wLaunchELF
   - **Memory card**: boot from a bootable ELF launcher

3. The emulator boots to a console printout; it loads `argv[1]` when the
   loader passes one, otherwise the first ROM found in `roms/`.

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

- **Performance is untested on hardware.** The 1.63 core does more per
  frame than the 2011 1.53 core; frame-rate comparison against the old fork
  is a hardware test item.
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

- `snes9x-1.63-ps2` = upstream master + `ps2/` port directory, plus two
  core edits:
  - `memmap.cpp`: `XPSdecode` signature (portability; see the commit
    history).
  - `pixform.h`: an additive `BGR555` pixel format (selected by the PS2
    Makefile only). It reuses RGB555's masks because the core's colour
    math treats the two outer fields identically; `tests/test_pixform.cpp`
    checks that property over ~6.3M operand pairs.
- The 2011 fork (`BoGanon/snes9x`) is kept as reference only.
