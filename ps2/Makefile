EE_BIN = snes9x.elf

# Baseline optimization: -O3; -Ofast left for hardware testing.
EE_OPTFLAGS = -O3

SNES9X_FLAGS := -DZLIB -DUNZIP_SUPPORT -DJMA_SUPPORT -DSDD1_DECOMP \
	-DHAVE_STDINT_H -DHAVE_STRINGS_H -DIOAPI_NO_64 \
	-DVERSION=\"1.63\" \
	-DPIXEL_FORMAT=BGR555

# EE performance profile carried over from the 2011 fork:
# R5900 pipeline tuning + hard float + single-precision fast math.
EE_CFLAGS += $(SNES9X_FLAGS) -mtune=r5900 -mhard-float \
	-fsingle-precision-constant -mfused-madd -ffast-math
EE_CXXFLAGS += $(SNES9X_FLAGS) -mtune=r5900 -mhard-float \
	-fsingle-precision-constant -mfused-madd -ffast-math \
	-fno-rtti -fno-exceptions
EE_INCS += -I.. -I../apu -I../unzip -I../jma -I$(PS2SDK)/ports/include
EE_LDFLAGS += -L$(PS2SDK)/ports/lib
# The 65816's always-hot state lives in the EE's 16 KB scratchpad RAM
# (0x70000000: never misses, not part of the 8 KB D-cache). globals.cpp
# does not define these on the R5900; the linker gives the symbols their
# addresses, so no ELF segment is added. ps2_main.cpp zeroes the area at
# startup and static_asserts the sizes against this layout.
EE_LDFLAGS += -Wl,--defsym,Registers=0x70000000 -Wl,--defsym,ICPU=0x70000020 \
	-Wl,--defsym,CPU=0x70000040 -Wl,--defsym,Timings=0x70000090 \
	-Wl,--defsym,OpenBus=0x700000e0

# Profiler hooks on core functions (ps2_prof.cpp), mangled C++ names.
# Stay below 0x01ee8000: ps2link keeps its EE part in the top 1.1 MB of
# RAM (linkfile.loadhigh), and the SDK's default puts the stack there. With
# the stack on top of it, "ps2client reset" left ps2link half alive.
EE_LDFLAGS += -Wl,--defsym,_stack=0x01ec8000
EE_LDFLAGS += -Wl,--wrap=_Z17S9xAPUEndScanlinev -Wl,--wrap=_Z14S9xAPUReadPorti \
	-Wl,--wrap=_Z15S9xAPUWritePortih -Wl,--wrap=_Z15S9xUpdateScreenv \
	-Wl,--wrap=_Z19S9xEndScreenRefreshv
EE_LIBS += -ldebug -lps2ip -lnetman -lpatches -lstdc++ -lgraph -ldraw -ldma -lpacket -lgs -laudsrv -lpad -lmtap \
	-lfileXio -lmc -lz -lkernel -lcdvd -lcglue -lm -lc

VPATH = .. ../apu ../apu/bapu/dsp ../apu/bapu/smp ../unzip ../filter ../jma

# IOP drivers embedded in the ELF (loaded by ps2_iop.cpp).
IRX_MODULES = iomanX fileXio sio2man mcman mcserv padman mtapman libsd audsrv \
	usbd bdm bdmfs_fatfs usbmass_bd ps2dev9 netman smap
# ... and the port's own sound module, built here (snesspu/).
IRX_OBJS = $(IRX_MODULES:%=irx_%.o) irx_snesspu.o

EE_OBJS = \
	$(IRX_OBJS) ps2_iop.o \
	ps2_main.o ps2_menu.o ps2_config.o ps2_rom.o ps2_prof.o ps2_net.o ps2_idle.o ps2_watchdog.o ps2_gs.o ps2_spu.o ps2_spu_core.o ps2_brr_adpcm.o ps2_video.o ps2_controls.o ps2_input_map.o ps2_dirs.o s9x_sound_driver_audsrv.o \
	apu.o bapu-dsp-sdsp.o bapu-smp-smp.o bapu-smp-smp_state.o \
	bsx.o c4.o c4emu.o cheats.o cheats2.o clip.o conffile.o \
	controls.o cpu.o cpuexec.o cpuops.o crosshairs.o dma.o \
	dsp.o dsp1.o dsp2.o dsp3.o dsp4.o fxinst.o fxemu.o \
	gfx.o globals.o memmap.o msu1.o movie.o obc1.o ppu.o \
	stream.o sa1.o sa1cpu.o screenshot.o sdd1.o sdd1emu.o \
	seta.o seta010.o seta011.o seta018.o snapshot.o snes9x.o \
	spc7110.o srtc.o tile.o tileimpl-n1x1.o tileimpl-n2x1.o \
	tileimpl-h2x1.o 2xsai.o blit.o epx.o hq2x.o snes_ntsc.o \
	statemanager.o sha256.o bml.o fscompat.o \
	loadzip.o unzip.o ioapi.o zip.o \
	s9x-jma.o 7zlzma.o crc32.o iiostrm.o inbyte.o jma.o lzma.o lzmadec.o winout.o

all: $(EE_BIN)

# No header dependency tracking in Makefile.eeglobal: rebuild everything
# when build flags or the pixel format change, so RGB565 and BGR555
# objects can never be linked together.
$(EE_OBJS): Makefile ../pixform.h ../port.h

# Port sources include each other's headers; rebuild them when any changes.
PORT_OBJS = $(filter ps2_%.o s9x_sound_driver_audsrv.o,$(EE_OBJS))
$(PORT_OBJS): $(wildcard ps2_*.h) s9x_sound_driver_audsrv.h snesspu/snesspu.h

clean:
	rm -f $(EE_OBJS) $(EE_BIN) $(IRX_OBJS:.o=.c)
	$(MAKE) -C snesspu clean

irx_%.c: $(PS2SDK)/iop/irx/%.irx
	$(PS2SDK)/bin/bin2c $< $@ $*_irx

snesspu/snesspu.irx: snesspu/snesspu.c snesspu/snesspu.h snesspu/imports.lst snesspu/Makefile
	$(MAKE) -C snesspu
irx_snesspu.c: snesspu/snesspu.irx
	$(PS2SDK)/bin/bin2c $< $@ snesspu_irx

# apu/bapu objects share basenames with nothing else, but their sources are
# nested; build them with explicit rules.
# Code size matters more than -O3 here: the R5900 has a 16 KB instruction
# cache and the APU runs SPC700 and DSP code alternately every scanline
# slice. -O3 inlines the DSP voice clocks into a 9.7 KB run() (26.5 KB of
# DSP code; 16.9 KB at -O2) and op_step is 38 KB (27 KB at -Os). Judge
# changes here with the hardware counters (Overlay: Profiler+PMC), not
# PCSX2, which does not model the cache.
bapu-dsp-sdsp.o: ../apu/bapu/dsp/sdsp.cpp
	$(EE_CXX_COMPILE) -O2 -c $< -o $@
bapu-smp-smp.o: ../apu/bapu/smp/smp.cpp
	$(EE_CXX_COMPILE) -Os -c $< -o $@
bapu-smp-smp_state.o: ../apu/bapu/smp/smp_state.cpp
	$(EE_CXX_COMPILE) -c $< -o $@

# JMA uses C++ exceptions; build those objects with -fexceptions while the
# rest of the port stays -fno-exceptions (mirrors the 2011 fork's libjma.a).
JMA_CXX_COMPILE = $(EE_CXX) $(EE_CXXFLAGS) -fexceptions $(EE_INCS)
s9x-jma.o 7zlzma.o crc32.o iiostrm.o inbyte.o jma.o lzma.o lzmadec.o winout.o: %.o: ../jma/%.cpp
	$(JMA_CXX_COMPILE) -c $< -o $@

include Makefile.pref
include Makefile.eeglobal
