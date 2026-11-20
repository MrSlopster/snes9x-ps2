# snesspu.irx: SNES voices on SPU2 hardware voices (see snesspu.c).
# Built by ../Makefile and embedded in the ELF.
IOP_BIN = snesspu.irx
IOP_OBJS = snesspu.o imports.o

all: $(IOP_BIN)

clean:
	rm -rf obj $(IOP_BIN) snesspu.notiopmod.elf snesspu.notiopmod.stripped.elf

include $(PS2SDK)/Defs.make
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
