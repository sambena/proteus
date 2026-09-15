# SPDX-License-Identifier: GPL-3.0-or-later
# Build from an MSYS2 UCRT64 shell (or any gcc toolchain on Linux/macOS).
#   make          build build/proteus_libretro.<ext>
#   make test     build the test core + harness and run the scenarios
#   make cli      build/proteus-cli (song scans and reference songs from the command line)
#   make ZLIB=0   build without compressed .vgz support

CC      ?= gcc
CXX     ?= g++
BUILD   := build
OBJ     := $(BUILD)/obj
TESTDIR := $(BUILD)/test
ZLIB    ?= 1

ifeq ($(OS),Windows_NT)
  EXT     := dll
  EXE     := .exe
  LDLIBS  :=
  DSP_LIBS := -lpsapi
  # Link the C++ runtime and zlib statically so the DLL has no MinGW dependencies.
  SHARED  := -shared -static -Wl,--no-undefined
else
  EXT     := so
  EXE     :=
  LDLIBS  := -ldl
  SHARED  := -shared -Wl,--no-undefined
  PIC     := -fPIC
endif

WARN     := -Wall -Wextra
CFLAGS   += -std=gnu11 -O2 $(WARN) $(PIC) -Ideps -Ideps/compat -Ideps/gme -Isrc
CXXFLAGS += -std=gnu++11 -O2 -w $(PIC) -Ideps/gme -DVGM_YM2612_NUKED
LDLIBS   += -lm

# libgme needs the byte order spelled out (its CMake build normally does this).
ENDIAN := $(shell printf '\#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__\nbig\n\#endif\n' | $(CC) -E -P -x c - 2>/dev/null)
ifeq ($(strip $(ENDIAN)),big)
  CXXFLAGS += -DBLARGG_BIG_ENDIAN=1
else
  CXXFLAGS += -DBLARGG_LITTLE_ENDIAN=1
endif

ifeq ($(ZLIB),1)
  CXXFLAGS += -DHAVE_ZLIB_H
  LDLIBS   += -lz
endif

CORE     := $(BUILD)/proteus_libretro.$(EXT)
DSP      := $(BUILD)/proteus_dsp.$(EXT)
SOURCES  := src/proteus.c src/engine.c src/profile.c src/music.c src/decoders.c src/usf_play.c src/options.c src/util.c src/game_options.c
DSP_SRC  := src/dsp.c src/engine.c src/profile.c src/music.c src/decoders.c src/usf_play.c src/util.c src/game_options.c
GME_SRC  := $(wildcard deps/gme/*.cpp)
GME_OBJ  := $(GME_SRC:%.cpp=$(OBJ)/%.o) $(OBJ)/deps/gme/ext/emu2413.o
# lazyusf2 (N64 USF rips): the cached interpreter, without the recompilers; psflib reads the files.
USF_SRC  := ai/ai_controller.c api/callbacks.c debugger/dbg_decoder.c main/main.c main/rom.c main/savestates.c \
            main/util.c memory/memory.c pi/cart_rom.c pi/pi_controller.c r4300/cached_interp.c r4300/cp0.c \
            r4300/cp1.c r4300/exception.c r4300/interupt.c r4300/mi_controller.c r4300/pure_interp.c r4300/r4300.c \
            r4300/r4300_core.c r4300/recomp.c r4300/reset.c r4300/tlb.c r4300/empty_dynarec.c rdp/rdp_core.c \
            ri/rdram.c ri/rdram_detection_hack.c ri/ri_controller.c rsp/rsp_core.c rsp_hle/alist.c \
            rsp_hle/alist_audio.c rsp_hle/alist_naudio.c rsp_hle/alist_nead.c rsp_hle/audio.c rsp_hle/cicx105.c \
            rsp_hle/hle.c rsp_hle/hvqm.c rsp_hle/jpeg.c rsp_hle/memory.c rsp_hle/mp3.c rsp_hle/musyx.c \
            rsp_hle/plugin.c rsp_hle/re2.c rsp_lle/rsp.c si/cic.c si/game_controller.c si/n64_cic_nus_6105.c \
            si/pif.c si/si_controller.c usf/usf.c usf/barray.c usf/resampler.c vi/vi_controller.c
USF_OBJ  := $(USF_SRC:%.c=$(OBJ)/deps/lazyusf2/%.o) $(OBJ)/deps/psflib/psflib.o
OBJECTS  := $(SOURCES:%.c=$(OBJ)/%.o) $(GME_OBJ) $(USF_OBJ)
DSP_OBJ  := $(DSP_SRC:%.c=$(OBJ)/%.o) $(GME_OBJ) $(USF_OBJ)
HEADERS  := $(wildcard src/*.h)

all: $(CORE) $(DSP)

$(CORE): $(OBJECTS)
	$(CXX) $(SHARED) -o $@ $(OBJECTS) $(LDLIBS)

$(DSP): $(DSP_OBJ)
	$(CXX) $(SHARED) -o $@ $(DSP_OBJ) $(LDLIBS) $(DSP_LIBS)

$(OBJ)/src/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/deps/gme/%.o: deps/gme/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Old C: typedef'd bool, a void function returning 0.
USF_CFLAGS := -std=gnu11 -fpermissive -O2 -w $(PIC) -Ideps/lazyusf2
ifneq ($(findstring x86_64,$(shell $(CC) -dumpmachine)),)
  USF_CFLAGS += -DARCH_MIN_SSE2 -msse2
endif

$(OBJ)/deps/lazyusf2/%.o: deps/lazyusf2/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USF_CFLAGS) -c -o $@ $<

$(OBJ)/deps/psflib/psflib.o: deps/psflib/psflib.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -w $(PIC) -c -o $@ $<

$(OBJ)/deps/gme/ext/emu2413.o: deps/gme/ext/emu2413.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -w $(PIC) -c -o $@ $<

# Proteus Studio: the song mapping tool (SDL2 + Dear ImGui).
STUDIO     := $(BUILD)/ProteusStudio$(EXE)
IMGUI_SRC  := deps/imgui/imgui.cpp deps/imgui/imgui_draw.cpp deps/imgui/imgui_tables.cpp \
              deps/imgui/imgui_widgets.cpp deps/imgui/backends/imgui_impl_sdl2.cpp \
              deps/imgui/backends/imgui_impl_sdlrenderer2.cpp
STUDIO_SRC := $(wildcard studio/*.cpp)
STUDIO_OBJ := $(STUDIO_SRC:%.cpp=$(OBJ)/%.o) $(IMGUI_SRC:%.cpp=$(OBJ)/%.o) \
              $(filter-out $(OBJ)/src/proteus.o $(OBJ)/src/options.o,$(OBJECTS))
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --static --libs sdl2 2>/dev/null || sdl2-config --static-libs 2>/dev/null)
STUDIO_FLAGS := -std=gnu++17 -O2 -Wall -Wextra -Ideps -Ideps/imgui -Ideps/gme -Isrc -Istudio $(SDL_CFLAGS)

studio: $(STUDIO)

$(STUDIO): $(STUDIO_OBJ)
	$(CXX) -static -o $@ $(STUDIO_OBJ) $(SDL_LIBS) -lz -lcomdlg32 -lole32 -lshell32 -lwininet

$(OBJ)/studio/%.o: studio/%.cpp $(wildcard studio/*.h) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(STUDIO_FLAGS) -c -o $@ $<

# proteus-cli: Studio's scans, references and song tables without the window.
CLI     := $(BUILD)/proteus-cli$(EXE)
CLI_OBJ := $(OBJ)/studio/cli/proteus_cli.o            $(filter-out $(OBJ)/studio/main.o $(OBJ)/studio/audio_out.o,$(STUDIO_SRC:%.cpp=$(OBJ)/%.o))            $(filter-out $(OBJ)/src/proteus.o $(OBJ)/src/options.o,$(OBJECTS))

cli: $(CLI)

$(CLI): $(CLI_OBJ)
	$(CXX) -static -o $@ $(CLI_OBJ) -lz -lcomdlg32 -lole32 -lshell32 -lwininet

$(OBJ)/studio/cli/%.o: studio/cli/%.cpp $(wildcard studio/*.h) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(filter-out -Dmain=SDL_main,$(STUDIO_FLAGS)) -c -o $@ $<

$(OBJ)/deps/imgui/%.o: deps/imgui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=gnu++17 -O2 -w -Ideps/imgui $(SDL_CFLAGS) -c -o $@ $<

$(TESTDIR):
	mkdir -p $@

$(TESTDIR)/testcore_libretro.$(EXT): test/testcore.c | $(TESTDIR)
	$(CC) $(CFLAGS) $(SHARED) -o $@ $< -lm

$(TESTDIR)/harness$(EXE): test/harness.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# The wrapper picks its inner core from its own file name.
$(TESTDIR)/proteus_testcore_libretro.$(EXT): $(CORE) | $(TESTDIR)
	cp $< $@

$(TESTDIR)/assets.stamp: $(TESTDIR)/harness$(EXE) test/game.proteus.ini test/latch.proteus.ini test/pattern.proteus.ini test/stop.proteus.ini test/requests.proteus.ini test/patch.proteus.ini test/tap.proteus.ini test/hold.proteus.ini
	$(TESTDIR)/harness$(EXE) gen $(TESTDIR)
	sox $(TESTDIR)/tone_330.wav $(TESTDIR)/tone_330.ogg
	sox $(TESTDIR)/tone_550.wav $(TESTDIR)/tone_550.mp3
	rm $(TESTDIR)/tone_330.wav $(TESTDIR)/tone_550.wav
	gzip -9 -n -f $(TESTDIR)/tone_500.vgm
	mv $(TESTDIR)/tone_500.vgm.gz $(TESTDIR)/tone_500.vgz
	cp test/game.proteus.ini test/latch.proteus.ini test/pattern.proteus.ini test/stop.proteus.ini test/requests.proteus.ini test/patch.proteus.ini test/tap.proteus.ini test/hold.proteus.ini $(TESTDIR)/
	touch $(TESTDIR)/game.tst $(TESTDIR)/other.tst $(TESTDIR)/latch.tst $(TESTDIR)/pattern.tst $(TESTDIR)/stop.tst \
	      $(TESTDIR)/requests.tst $(TESTDIR)/patch.tst $(TESTDIR)/tap.tst $(TESTDIR)/hold.tst $@

$(TESTDIR)/test_ra$(EXE): test/test_ra.cpp studio/md5.cpp studio/ra_client.cpp studio/http.cpp | $(TESTDIR)
	$(CXX) -static -std=gnu++17 -O2 -Istudio -o $@ $^ -lwininet

$(TESTDIR)/test_apu$(EXE): test/test_apu.cpp studio/apu_analyzer.cpp studio/snes_rom.cpp studio/md5.cpp studio/game_db.cpp studio/platform.cpp src/util.c | $(TESTDIR)
	$(CXX) -static -std=gnu++17 -O2 -Istudio -Isrc -o $@ $^ -lz -lshell32 -lole32 -lcomdlg32

$(TESTDIR)/test_reference$(EXE): test/test_reference.cpp studio/reference.cpp studio/nsf_init.cpp studio/nes_tap.cpp studio/zip_read.cpp studio/http.cpp studio/snes_rom.cpp studio/md5.cpp studio/platform.cpp src/util.c | $(TESTDIR)
	$(CXX) -static -std=gnu++17 -O2 -Istudio -Isrc -o $@ $^ -lz -lshell32 -lole32 -lcomdlg32 -lwininet

test: $(TESTDIR)/proteus_testcore_libretro.$(EXT) $(TESTDIR)/testcore_libretro.$(EXT) \
      $(TESTDIR)/harness$(EXE) $(TESTDIR)/assets.stamp $(DSP) $(TESTDIR)/test_ra$(EXE) $(TESTDIR)/test_apu$(EXE) \
      $(TESTDIR)/test_reference$(EXE)
	$(TESTDIR)/test_ra$(EXE)
	$(TESTDIR)/test_apu$(EXE)
	$(TESTDIR)/test_reference$(EXE) $(TESTDIR)/reference
	$(TESTDIR)/harness$(EXE) run $(TESTDIR)/proteus_testcore_libretro.$(EXT) $(TESTDIR) $(TESTDIR)/mixed.wav
	$(TESTDIR)/harness$(EXE) dsp $(DSP) $(TESTDIR)/testcore_libretro.$(EXT) $(TESTDIR)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean studio cli
