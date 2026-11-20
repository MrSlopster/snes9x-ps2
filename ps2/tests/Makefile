# Host-side unit tests for the PS2 port. Run: make -C ps2/tests
# (all paths are relative to this tests/ directory).

all: run

build:
	g++ -I../.. -I../../apu -I.. -DHAVE_STDINT_H test_map_buttons.cpp test_stubs.cpp ../ps2_input_map.cpp ../../controls.cpp -o /tmp/snes9x_ps2_test_map
	g++ -O1 -I../.. -I../../apu -I.. -DHAVE_STDINT_H -DPIXEL_FORMAT=BGR555 test_pixform.cpp -o /tmp/snes9x_ps2_test_pixform
	g++ -O2 -D_EE -std=c++11 test_smp_timers.cpp -o /tmp/snes9x_ps2_test_smp_timers

run: build
	/tmp/snes9x_ps2_test_map
	/tmp/snes9x_ps2_test_pixform
	/tmp/snes9x_ps2_test_smp_timers
