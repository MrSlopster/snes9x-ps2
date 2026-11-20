# Host-side unit tests for the PS2 port. Run: make -C ps2/tests
# (all paths are relative to this tests/ directory).

all: run

build:
	g++ -I../.. -I../../apu -I.. -DHAVE_STDINT_H test_map_buttons.cpp test_stubs.cpp ../ps2_input_map.cpp ../../controls.cpp -o /tmp/snes9x_ps2_test_map
	g++ -I.. test_convert.cpp ../ps2_video_convert.cpp -o /tmp/snes9x_ps2_test_conv

run: build
	/tmp/snes9x_ps2_test_map
	/tmp/snes9x_ps2_test_conv
