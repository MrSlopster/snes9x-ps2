# Host oracle build. Out-of-tree, from any core source tree:
#   make SRC=/path/to/core OUT=/path/to/build [DEFS="-D..."] [EXTRA_SRCS="..."]
SRC ?= ../../..
OUT ?= build
DEFS ?=
EXTRA_SRCS ?=

CXX = g++
CC = gcc
FLAGS = -O2 -w -DZLIB -DUNZIP_SUPPORT -DJMA_SUPPORT -DSDD1_DECOMP -DHAVE_STDINT_H \
	-DHAVE_STRINGS_H -DVERSION=\"1.63\" $(DEFS) -I$(SRC) -I$(SRC)/apu -I$(SRC)/unzip -I$(SRC)/jma

CORE = bsx c4 c4emu cheats cheats2 clip conffile controls cpu cpuexec cpuops \
	crosshairs dma dsp dsp1 dsp2 dsp3 dsp4 fxinst fxemu gfx globals memmap msu1 movie \
	obc1 ppu stream sa1 sa1cpu screenshot sdd1 sdd1emu seta seta010 seta011 seta018 \
	snapshot snes9x spc7110 srtc tile tileimpl-n1x1 tileimpl-n2x1 tileimpl-h2x1 \
	statemanager sha256 bml fscompat loadzip
JMA = s9x-jma 7zlzma crc32 iiostrm inbyte jma lzma lzmadec winout
UNZ = unzip ioapi zip

OBJS = $(CORE:%=$(OUT)/%.o) $(JMA:%=$(OUT)/jma-%.o) $(UNZ:%=$(OUT)/unz-%.o) \
	$(OUT)/apu.o $(OUT)/bapu-sdsp.o $(OUT)/bapu-smp.o $(OUT)/bapu-smp_state.o \
	$(OUT)/oracle.o $(patsubst %.cpp,$(OUT)/extra-%.o,$(notdir $(EXTRA_SRCS)))

$(OUT)/oracle: $(OBJS)
	$(CXX) -o $@ $(OBJS) -lz

$(OBJS): | $(OUT)
$(OUT):
	mkdir -p $(OUT)

$(OUT)/oracle.o: oracle.cpp
	$(CXX) $(FLAGS) -c $< -o $@
$(OUT)/apu.o: $(SRC)/apu/apu.cpp
	$(CXX) $(FLAGS) -c $< -o $@
$(OUT)/bapu-sdsp.o: $(SRC)/apu/bapu/dsp/sdsp.cpp
	$(CXX) $(FLAGS) -c $< -o $@
$(OUT)/bapu-smp.o: $(SRC)/apu/bapu/smp/smp.cpp
	$(CXX) $(FLAGS) -c $< -o $@
$(OUT)/bapu-smp_state.o: $(SRC)/apu/bapu/smp/smp_state.cpp
	$(CXX) $(FLAGS) -c $< -o $@
$(OUT)/jma-%.o: $(SRC)/jma/%.cpp
	$(CXX) $(FLAGS) -fexceptions -c $< -o $@
$(OUT)/unz-%.o: $(SRC)/unzip/%.c
	$(CC) $(FLAGS) -c $< -o $@
$(OUT)/%.o: $(SRC)/%.cpp
	$(CXX) $(FLAGS) -c $< -o $@

define EXTRA_RULE
$(OUT)/extra-$(basename $(notdir $(1))).o: $(1)
	$$(CXX) $$(FLAGS) -c $$< -o $$@
endef
$(foreach s,$(EXTRA_SRCS),$(eval $(call EXTRA_RULE,$(s))))
