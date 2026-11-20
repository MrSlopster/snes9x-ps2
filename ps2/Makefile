EE_BIN = snes9x.elf

# Baseline optimization: -O3 (spec); -Ofast left for hardware testing.
EE_OPTFLAGS = -O3

SNES9X_FLAGS := -DZLIB -DUNZIP_SUPPORT -DJMA_SUPPORT -DSDD1_DECOMP \
	-DHAVE_STDINT_H -DHAVE_STRINGS_H -DIOAPI_NO_64 \
	-DVERSION=\"1.63\" \
	-DPIXEL_FORMAT=BGR555

# EE performance profile carried over from the 2011 fork (spec §Build):
# R5900 pipeline tuning + hard float + single-precision fast math.
EE_CFLAGS += $(SNES9X_FLAGS) -mtune=r5900 -mhard-float \
	-fsingle-precision-constant -mfused-madd -ffast-math
EE_CXXFLAGS += $(SNES9X_FLAGS) -mtune=r5900 -mhard-float \
	-fsingle-precision-constant -mfused-madd -ffast-math \
	-fno-rtti -fno-exceptions
EE_INCS += -I.. -I../apu -I../unzip -I../jma -I$(PS2SDK)/ports/include
EE_LDFLAGS += -L$(PS2SDK)/ports/lib
# Profiler hooks on core functions (ps2_prof.cpp), mangled C++ names.
EE_LDFLAGS += -Wl,--wrap=_Z17S9xAPUEndScanlinev -Wl,--wrap=_Z14S9xAPUReadPorti \
	-Wl,--wrap=_Z15S9xAPUWritePortih -Wl,--wrap=_Z15S9xUpdateScreenv \
	-Wl,--wrap=_Z19S9xEndScreenRefreshv
EE_LIBS += -lpatches -lstdc++ -lgraph -ldraw -ldma -lpacket -lgs -laudsrv -lpad -lmtap \
	-lfileXio -lmc -lz -lkernel -lcdvd -lcglue -lm -lc

VPATH = .. ../apu ../apu/bapu/dsp ../apu/bapu/smp ../unzip ../filter ../jma

# IOP drivers embedded in the ELF (loaded by ps2_iop.cpp).
IRX_MODULES = iomanX fileXio sio2man mcman mcserv padman mtapman libsd audsrv \
	usbd bdm bdmfs_fatfs usbmass_bd
IRX_OBJS = $(IRX_MODULES:%=irx_%.o)

EE_OBJS = \
	$(IRX_OBJS) ps2_iop.o \
	ps2_main.o ps2_menu.o ps2_config.o ps2_rom.o ps2_prof.o ps2_video.o ps2_controls.o ps2_input_map.o ps2_dirs.o s9x_sound_driver_audsrv.o \
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

clean:
	rm -f $(EE_OBJS) $(EE_BIN) $(IRX_OBJS:.o=.c)

irx_%.c: $(PS2SDK)/iop/irx/%.irx
	$(PS2SDK)/bin/bin2c $< $@ $*_irx

# apu/bapu objects share basenames with nothing else, but their sources are
# nested; build them with explicit rules.
bapu-dsp-sdsp.o: ../apu/bapu/dsp/sdsp.cpp
	$(EE_CXX_COMPILE) -c $< -o $@
bapu-smp-smp.o: ../apu/bapu/smp/smp.cpp
	$(EE_CXX_COMPILE) -c $< -o $@
bapu-smp-smp_state.o: ../apu/bapu/smp/smp_state.cpp
	$(EE_CXX_COMPILE) -c $< -o $@

# JMA uses C++ exceptions; build those objects with -fexceptions while the
# rest of the port stays -fno-exceptions (mirrors the 2011 fork's libjma.a).
JMA_CXX_COMPILE = $(EE_CXX) $(EE_CXXFLAGS) -fexceptions $(EE_INCS)
s9x-jma.o 7zlzma.o crc32.o iiostrm.o inbyte.o jma.o lzma.o lzmadec.o winout.o: %.o: ../jma/%.cpp
	$(JMA_CXX_COMPILE) -c $< -o $@

include Makefile.pref
include Makefile.eeglobal
