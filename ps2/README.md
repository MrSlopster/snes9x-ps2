# Snes9x 1.63 for PlayStation 2

A port of upstream Snes9x 1.63
([snes9xgit/snes9x](https://github.com/snes9xgit/snes9x), master as of
2026-09-24) to the PlayStation 2. All port code is in this directory; the
emulation core is upstream's, with PS2 speed-ups guarded by `#ifdef _EE`.

## Status

| | |
|---|---|
| ROMs | `.sfc`, `.smc`, `.zip`, `.jma`, `.bin`, `.fig`, `.swc`, picked in an on-screen browser |
| Picture | 640x448 (NTSC) or 640x512 (PAL), also 240p/288p and 480p; no tearing |
| Sound | 48 kHz stereo |
| Pads | two, digital; no multitap |
| Saves | battery saves, save states |
| Also | IPS/BPS patches, SPC dumps, MSU-1 (all file-based, from the core) |
| Missing | cheats menu (`.cht` files work), netplay, movies |

Speed on a real console, with the default settings: EarthBound runs at
60 fps with about 6 ms of each 16.7 ms frame to spare. An F-Zero race
needs about 18 ms per frame and drops some frames. Games with an SA-1 or
SuperFX chip have not been measured.

### Accuracy

Two settings trade accuracy for speed, and both default to the fast side:

- **Renderer: GS** draws the picture with the Graphics Synthesizer (tiles
  as textured sprites, Mode 7 as a texture). Parts of a frame it cannot
  draw (hi-res modes, mosaic, direct colour, colour math with reduced
  brightness) are drawn by the software renderer. **Software** is the
  core's own renderer.
- **Sound: Fast** plays the SNES voices on the SPU2's hardware voices. It
  has no echo. **Accurate** is the core's DSP.

Everything else matches upstream bit for bit; `AGENTS.md` at the top of the
repository says how that is checked.

## Installing

Copy `snes9x.elf` to wherever you start homebrew from (wLaunchELF, OPL's
APPS, a memory card) and put ROMs on a USB stick or memory card. With
`<device>` being where the ELF was started from:

```
<device>/snes9x/roms/     where the browser opens (ROMs can be anywhere)
<device>/snes9x/ps2.cfg   settings
<device>/snes9x/saves/    save states (<game>.000)
<device>/snes9x/srm/      battery saves, if the ROM's folder cannot be written
```

If that device cannot be written, settings and those folders go to
`mc0:/snes9x`, then `mc1:/snes9x`.

A battery save is `<rom name>.srm` next to the ROM. It is written to
`<rom name>.sr~` first, read back, and only then put in place, so a reset
during a save leaves the previous one. A save that could not be written is
reported on screen.

The browser lists USB drives (`mass0:`, ...), memory cards and `host:`.
Network shares are not supported.

## Controls

| PS2 | SNES | | Browser | |
|---|---|---|---|---|
| Cross | B | | D-pad up/down | move |
| Circle | A | | L1/R1, left/right | page |
| Square | Y | | Cross | open folder, load ROM |
| Triangle | X | | Circle, Triangle | parent folder, device list |
| L1 / R1 | L / R | | Start | settings |
| Start, Select, D-pad | the same | | | |

**Select+Start** in a game opens the menu: resume, save state, load state,
reset, settings, load another ROM.

## Settings

| Setting | Values |
|---|---|
| Video mode | Auto, NTSC 480i, PAL 576i, NTSC 240p, PAL 288p, 480p (component). A new mode must be confirmed within 10 s or it reverts. |
| Screen filter | Smooth, Sharp |
| Frame skip | Auto, 0-4 |
| Overlay | Off, FPS, Profiler, Profiler+PMC |
| Sound | Off, Accurate, Fast |
| Renderer | Software, GS, GS + check vs software |
| Network log | diagnostics over UDP; the console's address comes from `mc0:/SYS-CONF/IPCONFIG.DAT` |

Keys that exist only in `ps2.cfg`:

| Key | Effect |
|---|---|
| `idle_skip=0` | switch off 65816 idle-loop skipping |
| `netlog_host=` | where the network log is sent |
| `gs_compare=1` | same as Renderer: GS + check |
| `gs_defer=0` | GS renderer: end a band at every fixed-colour write, as the software renderer does |
| `gs_crc=1` | log a checksum of the GS picture every 10th frame |
| `psamp_all=1` | profiler: print every PC-sample bucket, not the top 60 |
| `dump_frames=1` | profiler: raw frame dumps |

## Building

```sh
docker build -t localhost/snes9x-ps2build - <<'EOF'
FROM ghcr.io/ps2dev/ps2dev:latest
RUN apk add --no-cache make bash
EOF

docker run --rm -v "$PWD":/src -w /src/ps2 localhost/snes9x-ps2build make -j8
```

This gives `ps2/snes9x.elf` with debug information; strip it to deploy
(`mips64r5900el-ps2-elf-strip`). `make -C ps2/tests` runs the host unit
tests.

The IOP modules the port needs are embedded in the ELF and loaded after an
IOP reset. Started from `host:` (ps2link), it skips the reset and keeps its
stack and heap out of ps2link's memory, so `ps2client reset` returns to
ps2link:

```sh
ps2client -h <console> execee host:snes9x.elf host:/path/to/rom.sfc
```

## Where the port differs from upstream

- `ps2/`: everything specific to the console (see the file table in
  `AGENTS.md`).
- Core files, under `#ifdef _EE` or `_MIPS_ARCH_R5900`: the 65816 dispatch
  loop and idle-loop hooks, the SPC700 loop and timers, deferred APU work,
  the DSP's fast-sound path, hooks for the GS renderer, hot state in the
  scratchpad.
- Two edits that apply to every build: `XPSdecode`'s signature in
  `memmap.cpp`, and an additive `BGR555` pixel format in `pixform.h` (the
  GS's 16-bit layout, so frames need no conversion).

Save states use upstream's 1.63 format.
