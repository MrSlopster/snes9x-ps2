# Snes9x 1.63 for PlayStation 2

A port of the current upstream **Snes9x 1.63** core
([snes9xgit/snes9x](https://github.com/snes9xgit/snes9x), master as of
2026-09-24) to the PlayStation 2, replacing the 2011 Snes9x-1.53-based port
(BoGanon/snes9x).

All port code lives in this directory. The upstream emulation core is used
unmodified, so every accuracy fix from the last 15 years (PPU, APU, DSP,
SA-1, SuperFX, MSU-1 support, ...) applies automatically.

## What works (v1)

- ROM loading: `.sfc`, `.smc`, `.zip`, `.jma`, `.bin`, `.fig`, `.swc`
- RGB565 video through the Graphics Synthesizer, 640x448, stretch-scaled
- Audio through `audsrv` (48 kHz stereo), sound-sync throttling
- Two digital pads (+ optional multitap) mapped to SNES joypads
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

Compiler profile: `-O3 -mtune=r5900 -mhard-float` (r5900 pipeline tuning,
hard float, single-precision constants). `-Ofast` is a candidate for later
hardware testing only.

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

- Pads are locked to digital mode; multitap is detected best-effort and
  silently falls back to two pads.
- Audio buffers are audsrv-managed; when the ring fills up the emulator
  throttles in the samples-available callback (no frame-skipping logic yet —
  the core's `Settings.SkipFrames = AUTO_FRAMERATE` handles slow games).
- If neither USB nor memcard mounts resolve, the port falls back to
  `host:/snes9x` which only exists under ps2link.

## Known risks / follow-ups

- **Performance is untested on hardware.** The 1.63 core does more per
  frame than the 2011 1.53 core; frame-rate comparison against the old fork
  is a hardware test item. The main v1 wins over the old fork: no core
  hacks to maintain, single pixel format, DMA-friendly staging.
- Follow-up optimization hooks: VU0/DMA-assisted scaling, uncached staging
  audit (`SyncDCache` currently covers the whole staged frame), frame-skip
  policy tuning, `-Ofast` evaluation.
- Save-state format is upstream 1.63 (incompatible with 2011 fork states).

## Repo relationship

- `snes9x-1.63-ps2` = upstream master + `ps2/` port directory (+ two
  documented portability token edits in `memmap.cpp`; see the commit
  history).
- The 2011 fork (`BoGanon/snes9x`) is kept as reference only.
