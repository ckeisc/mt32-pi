# Host tests for the MU50 engine (PR #15 and later).
#
# These run on the host (x86_64, g++) against the repo's own sources --
# no Pi, no Circle build. Two tiers:
#
#   Unit (libmu50 only, no ROMs, no stubs):
#     smoke          Synth smoke: ROM guards, register readback, audible voice
#     meg_state      MegState unit test (offsets/constants/dry path)
#     swp00_0277     Swp0277Core unit test (key-on state, DPCM, formats)
#     keyon_test     KeyOnStatus: 7 hand-derived goldens + 4000 seeded
#                    random cases vs a directly-driven scratch core
#     render_guards  Synth::render guards: 7 cases (null/rate/ROM)
#
#   Engine (full CMU50Synth + H8 firmware; needs MU50 ROMs at
#   $HOME/roms/yamaha, found via the engstub fatfs mapping):
#     reg_scan       split-mode register readback scan: 896 never-written
#                    voice regs + 66 write/readback pairs (Finding 7 mask)
#     xg_render      full MIDI render harness (single/split, filter mode,
#                    FIFO cap for the tiny-queue stress)
#
# Build:  make           (all binaries into build/)
#         make unit      (unit tier only)
#         make engine    (engine tier only)
# Run:    make check     (build + run the unit tier + reg_scan)
#         make gates     (render gates: E1M1 + MegaMan 2 single vs split;
#                         needs the MIDI events files -- see README.md)
#         make stress    (tiny-queue E1M1 stress; needs the events files)
#
# The stub/ dir holds the Circle-API shims the engine tier needs
# (copied from the mu50-dualsplit host harnesses' engstub; they are
# test support, not firmware code).

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall

ROOT := $(abspath ../..)
LIBINC := -I$(ROOT)/external/mu50/include -I$(ROOT)/external/mu50/src
STUBINC := -I$(ROOT)/tests/host/stub
# NOTE: the stub dir comes FIRST: it shadows the Pi-only headers of
# the same names in include/ (config.h, diaglog.h, circle/*, fatfs/*,
# lcd/*) with host shims. The engine sources include those by bare
# name, so include order is what selects the host build.
ENGINC := $(STUBINC) -I$(ROOT)/include $(LIBINC)

LIBMU50 := $(ROOT)/external/mu50/src/synth.cpp \
           $(ROOT)/external/mu50/src/swp00_0277.cpp \
           $(ROOT)/external/mu50/src/swp00_meg.cpp
ENGINE := $(ROOT)/src/synth/mu50synth.cpp \
          $(ROOT)/src/synth/mu50h8.cpp \
          $(LIBMU50) \
          $(ROOT)/tests/host/stub/ffdir.cpp

BUILD := build

UNIT_BINS := $(BUILD)/smoke $(BUILD)/meg_state $(BUILD)/swp00_0277 \
             $(BUILD)/keyon_test $(BUILD)/render_guards
ENGINE_BINS := $(BUILD)/reg_scan $(BUILD)/xg_render $(BUILD)/meg_offsets

.PHONY: all unit engine check clean gates stress

all: unit engine

unit: $(UNIT_BINS)

engine: $(ENGINE_BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/smoke: smoke.cpp $(LIBMU50) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LIBINC) -o $@ smoke.cpp $(LIBMU50)

$(BUILD)/meg_state: meg_state.cpp $(ROOT)/external/mu50/src/swp00_meg.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LIBINC) -o $@ meg_state.cpp $(ROOT)/external/mu50/src/swp00_meg.cpp

$(BUILD)/swp00_0277: swp00_0277.cpp $(ROOT)/external/mu50/src/swp00_0277.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LIBINC) -o $@ swp00_0277.cpp $(ROOT)/external/mu50/src/swp00_0277.cpp

$(BUILD)/keyon_test: keyon_test.cpp $(LIBMU50) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LIBINC) -o $@ keyon_test.cpp $(LIBMU50)

$(BUILD)/render_guards: render_guards.cpp $(LIBMU50) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LIBINC) -o $@ render_guards.cpp $(LIBMU50)

$(BUILD)/reg_scan: reg_scan.cpp $(ENGINE) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(ENGINC) -pthread -o $@ reg_scan.cpp $(ENGINE)

$(BUILD)/xg_render: xg_render.cpp $(ENGINE) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(ENGINC) -pthread -o $@ xg_render.cpp $(ENGINE)

$(BUILD)/meg_offsets: meg_offsets.cpp $(ENGINE) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(ENGINC) -pthread -o $@ meg_offsets.cpp $(ENGINE)

check: unit engine
	./$(BUILD)/smoke
	./$(BUILD)/meg_state
	./$(BUILD)/swp00_0277
	./$(BUILD)/keyon_test
	./$(BUILD)/render_guards
	./$(BUILD)/reg_scan

clean:
	rm -rf $(BUILD)

# Render gates / stress: need MIDI events files (not in the repo --
# generate them with midi_to_events.py, see README.md). These targets
# just print the commands; run them once the events files exist.
gates:
	@echo 'See README.md "Render gates". Example:'
	@echo '  ./build/xg_render e1m1_events.txt e1m1_single.pcm 100 1 10 0'
	@echo '  ./build/xg_render e1m1_events.txt e1m1_split.pcm 100 1 10 1'
	@echo '  python3 compare_raw.py e1m1_single.pcm e1m1_split.pcm'

stress:
	@echo 'See README.md "Tiny-queue stress". Example:'
	@echo '  ./build/xg_render e1m1_events.txt e1m1_tiny.pcm 100 1 10 1 15'
	@echo '  python3 compare_raw.py e1m1_single.pcm e1m1_tiny.pcm'
