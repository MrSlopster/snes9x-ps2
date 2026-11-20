EE_BIN = snes9x.elf

# Baseline optimization: -O3 (spec); -Ofast left for hardware testing.
EE_OPTFLAGS = -O3

SNES9X_FLAGS := -DZLIB -DUNZIP_SUPPORT -DJMA_SUPPORT -DSDD1_DECOMP \
	-DHAVE_STDINT_H -DHAVE_STRINGS_H -DIOAPI_NO_64 \
	-DVERSION=\"1.63\"

EE_CFLAGS += $(SNES9X_FLAGS)
EE_CXXFLAGS += $(SNES9X_FLAGS) -fno-rtti -fno-exceptions
EE_INCS += -I.. -I../apu -I../unzip -I../jma -I$(PS2SDK)/ports/include
EE_LDFLAGS += -L$(PS2SDK)/ports/lib
EE_LIBS += -lstdc++ -lgraph -ldraw -ldma -lpacket -lgs -laudsrv -lpad -lmtap \
	-lfileXio -lmc -lz -lkernel -lcdvd -lcglue -lm -lc

VPATH = .. ../apu ../apu/bapu/dsp ../apu/bapu/smp ../unzip ../filter ../jma

EE_OBJS = \
	ps2_main.o ps2_video.o ps2_controls.o ps2_dirs.o s9x_sound_driver_audsrv.o \
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

clean:
	rm -f $(EE_OBJS) $(EE_BIN)

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
