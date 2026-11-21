# Notes for agents working on this repository

Snes9x 1.63 with a PlayStation 2 port. The upstream core lives at the top
level; all port code is in `ps2/`. Branch: `ps2-port`. The base upstream
commit is `1bcc369`.

## Ground rules

- Edits to core files are guarded by `#ifdef _EE` (port) or
  `_MIPS_ARCH_R5900` (real hardware only), so the tree still builds as
  plain upstream for the host oracle.
- Speed-ups must be exact unless they sit behind a user setting. "Exact"
  means the host oracle's checksums match pristine upstream (below).
  Approximate paths so far: `Sound: Fast (SPU2 voices)`, and
  `Renderer: GS` - which is checked against the software renderer instead:
  `gs_compare=1` in `ps2.cfg` renders every GS band both ways, reads the GS
  picture back and logs differing pixels (`GSCMP` lines). It should report
  none; bands the GS path does not handle fall back to software.
- Do not commit agent tooling or its output (plans, specs, session state);
  see `.gitignore`. Anything other agents need goes in this file.
- The R5900 has a 16 KB instruction cache, an 8 KB data cache, traps on
  misaligned loads and takes ~37 cycles for a division. PCSX2 models none
  of the first three: "works in PCSX2" is necessary, not sufficient.

## Building

```sh
# PS2 ELF (ps2/snes9x.elf), in the ps2dev container
podman run --rm -v "$PWD":/src -w /src/ps2 localhost/snes9x-ps2build make -j16
# the container: FROM ghcr.io/ps2dev/ps2dev:latest + `apk add make bash`
```

`ps2/Makefile` does not track core headers: after editing one (for example
`apu/bapu/smp/smp.hpp`), `touch` the `.cpp` files that include it.
`ps2/snesspu/` is the port's own IOP module; it is built by the same make
and embedded in the ELF.

## Verifying

- `make -C ps2/tests` - host unit tests (pad mapping, pixel format, SPC700
  timers, BRR to SPU2 ADPCM transcoder). All must print `GREEN/PASS`.
- Host oracle, `ps2/tools/oracle/`: a headless build of any core tree that
  prints `PFRAME <n> <video crc> apu <state crc>` every 60 frames.
  Build it twice, out of tree - pristine upstream with
  `DEFS=-DPIXEL_FORMAT=RGB555`, and this tree with
  `DEFS="-D_EE -DPIXEL_FORMAT=BGR555 -DORACLE_IDLE_STATS -I<repo>/ps2"` and
  `EXTRA_SRCS` = `ps2/ps2_idle.cpp ps2/ps2_spu_core.cpp
  ps2/ps2_brr_adpcm.cpp ps2/tools/oracle/spu2sim.cpp` - and diff the
  `PFRAME` lines over a long run (20000 frames, several games). Its
  Makefile does not track headers either: delete the affected objects.
  - `--smp-selftest N`: the port's SPC700 loop against the reference
    `op_step()` from random states.
  - `--wav F` / `--fast-wav F` and `sndcmp.py`: record the accurate sound
    output and the fast path (through a software SPU2) and compare them.
- On the PS2 (or PCSX2) the same `PFRAME` lines, plus `PROF` (time per
  frame by bucket), `PMC` (cache misses and other counters) and `PSAMP`
  (PC samples), are printed with Settings > Overlay > Profiler and sent
  over UDP when the network log is on (`ps2/tools/netlog.py` receives).
  The video half of `PFRAME` is comparable with the oracle; the APU half
  only in accurate sound mode.
- Measure before optimising. Counting opcodes and memory accesses in an
  instrumented oracle build has been more reliable than guessing from a
  sample profile, and several plausible micro-optimisations measured as
  losses.

## Where things are

| Area | Files |
| --- | --- |
| Main loop, settings, pacing | `ps2/ps2_main.cpp`, `ps2_config.*`, `ps2_menu.*` |
| Video (GS, double buffer) | `ps2/ps2_video.*` |
| GS renderer (tiles as GS sprites) | `ps2/ps2_gs.*`, hooks in `gfx.cpp` / `gfx.h` / `ppu.h` (`S9xGS*`) |
| IOP modules | `ps2/ps2_iop.*`, `ps2/snesspu/` |
| Accurate sound output | `ps2/s9x_sound_driver_audsrv.*` |
| Fast sound (SPU2 voices) | `ps2/ps2_spu.*`, `ps2_spu_core.*`, `ps2_brr_adpcm.*`, `apu/bapu/dsp/SPC_DSP.*` (`fast_*`) |
| 65816 idle-loop skipping | `ps2/ps2_idle.*`, hook in `cpumacro.h` |
| SPC700 loop, idle skip, lazy timers | `apu/bapu/smp/` (`ee_*`) |
| Deferred APU queue, handshake skip | `apu/apu.cpp` |
| Profiler | `ps2/ps2_prof.*` |
| Network log | `ps2/ps2_net.*` (settings from `mc0:/SYS-CONF/IPCONFIG.DAT`) |

## Hardware pitfalls already hit

- `FAST_LSB_WORD_ACCESS` must stay off on the R5900 (misaligned word reads
  trap; PCSX2 does not).
- EE RPC clients spin forever binding to an IOP module that was not
  loaded: check `ps2_iop_*_ok()` first.
- ps2sdk libsd: `sceSdSetCoreAttr(SD_CORE_NOISE_CLK, ...)` corrupts the
  core attribute register (clears the unmute bit: silence on a real SPU2,
  fine in PCSX2). Write the field directly.
- SIF RPC replies are transferred in 16-byte units: pad receive buffers.
- PCSX2's log drops `printf` lines longer than about 250 characters.
- GIF REGLIST packets ignore the tag's PRIM field: set the PRIM register
  with an A+D packet first.
- A 4-bit texture's palette is the 8x2 image at its CLUT base pointer; the
  CSA field picks the slot in the GS's internal table, not the source.
- A zeroed `Settings` struct is not the core's default configuration
  (`MaxSpriteTilesPerLine`, `InterpolationMethod`,
  `SuperFXClockMultiplier` must be set).
