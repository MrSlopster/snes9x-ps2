# Host-side unit tests for the PS2 port. Run: make -C ps2/tests
# (all paths are relative to this tests/ directory).
# APU_DUMPS="a.bin b.bin": also run the BRR transcoder over APU state
# dumps made with the host oracle (tools/oracle, --dump-apu N).

all: run

build:
	g++ -I../.. -I../../apu -I.. -DHAVE_STDINT_H test_map_buttons.cpp test_stubs.cpp ../ps2_input_map.cpp ../../controls.cpp -o /tmp/snes9x_ps2_test_map
	g++ -O1 -I../.. -I../../apu -I.. -DHAVE_STDINT_H -DPIXEL_FORMAT=BGR555 test_pixform.cpp -o /tmp/snes9x_ps2_test_pixform
	g++ -O2 -D_EE -std=c++11 test_smp_timers.cpp -o /tmp/snes9x_ps2_test_smp_timers
	g++ -O2 -Wall -std=c++11 test_brr_adpcm.cpp ../ps2_brr_adpcm.cpp -o /tmp/snes9x_ps2_test_brr_adpcm

run: build
	/tmp/snes9x_ps2_test_map
	/tmp/snes9x_ps2_test_pixform
	/tmp/snes9x_ps2_test_smp_timers
	/tmp/snes9x_ps2_test_brr_adpcm $(APU_DUMPS)
