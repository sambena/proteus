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
  LDLIBS  := -ldl -lpthread
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
# The effects and the systems they draw from, the kit for game modules, and every game
# module there is. A file whose name begins with an underscore is not a game
# (src/games/_template.c).
FX_SRC   := src/fx_video.c src/fx_track.c src/fx_audio.c src/fx_pool.c src/fx_panel.c src/fx_synth.c \
            src/fx_game.c src/kit.c src/sys_tia.c
GAME_SRC := $(sort $(filter-out src/games/_%,$(wildcard src/games/*.c)))
SOURCES  := src/proteus.c src/engine.c src/profile.c src/music.c src/decoders.c src/usf_play.c src/options.c src/util.c src/game_options.c \
            $(FX_SRC) $(GAME_SRC)
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
# RSN soundtracks (RAR archives of SNES .spc files): dmc_unrar unpacks them for libgme.
RSN_OBJ  := $(OBJ)/src/rsn_play.o $(OBJ)/deps/dmc_unrar/dmc_unrar.o
OBJECTS  += $(RSN_OBJ)
DSP_OBJ  += $(RSN_OBJ)
# viogsf (GBA GSF rips): VBA-M's GBA interpreter and sound, no video; psflib (in USF_OBJ) reads
# the files. The player is C++, so it rides in GSF_OBJ rather than SOURCES.
GSF_SRC  := apu/Blip_Buffer.cpp apu/Gb_Apu.cpp apu/Gb_Oscs.cpp apu/Multi_Buffer.cpp \
            gba/GBA.cpp gba/GBA-arm.cpp gba/GBA-thumb.cpp gba/Sound.cpp gba/bios.cpp
GSF_OBJ  := $(GSF_SRC:%.cpp=$(OBJ)/deps/viogsf/vbam/%.o) $(OBJ)/src/gsf_play.o
OBJECTS  += $(GSF_OBJ)
DSP_OBJ  += $(GSF_OBJ)
# Nintendo DS rips. 2SF: vio2sf (DeSmuME's CPU interpreters and SPU). NCSF: SSEQPlayer, which plays
# the sequences in a game's SDAT directly. psflib reads both.
NDS_SRC  := armcpu.c arm_instructions.c barray.c bios.c cp15.c FIFO.c GPU.c isqrt.c matrix.c mc.c MMU.c \
            NDSSystem.c resampler.c state.c thumb_instructions.c SPU.cpp
SSEQ_SRC := Channel.cpp FATSection.cpp INFOEntry.cpp INFOSection.cpp NDSStdHeader.cpp Player.cpp SBNK.cpp \
            SDAT.cpp SSEQ.cpp SWAR.cpp SWAV.cpp SYMBSection.cpp Track.cpp
NDS_OBJ  := $(OBJ)/src/twosf_play.o $(OBJ)/src/ncsf_play.o $(OBJ)/deps/psflib/psflib.o \
            $(addprefix $(OBJ)/deps/vio2sf/desmume/,$(addsuffix .o,$(basename $(NDS_SRC)))) \
            $(SSEQ_SRC:%.cpp=$(OBJ)/deps/sseqplayer/%.o)
OBJECTS  += $(filter-out $(OBJECTS),$(NDS_OBJ))
DSP_OBJ  += $(filter-out $(DSP_OBJ),$(NDS_OBJ))
# LakeSnes (SNES SNSF rips): the console without drawing its picture; psflib (above) reads the files.
SNSF_SRC := apu.c cart.c cpu.c cx4.c dma.c dsp.c input.c ppu.c snes.c spc.c statehandler.c
SNSF_OBJ := $(OBJ)/src/snsf_play.o $(SNSF_SRC:%.c=$(OBJ)/deps/lakesnes/snes/%.o)
OBJECTS  += $(SNSF_OBJ)
DSP_OBJ  += $(SNSF_OBJ)
HEADERS  := $(wildcard src/*.h) $(wildcard src/games/*.h)

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

$(OBJ)/deps/viogsf/%.o: deps/viogsf/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=gnu++11 -O2 -w $(PIC) -c -o $@ $<

$(OBJ)/src/gsf_play.o: src/gsf_play.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) -std=gnu++11 -O2 $(WARN) $(PIC) -Ideps -Isrc -c -o $@ $<

$(OBJ)/deps/psflib/psflib.o: deps/psflib/psflib.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -w $(PIC) -c -o $@ $<

# Built without stdio or the Win32 API: Proteus hands it the archive in memory.
$(OBJ)/deps/dmc_unrar/dmc_unrar.o: deps/dmc_unrar/dmc_unrar.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -w $(PIC) -DDMC_UNRAR_DISABLE_STDIO=1 -DDMC_UNRAR_DISABLE_WIN32=1 -c -o $@ $<
# vio2sf's symbols are renamed (vio2sf_names.h) so they cannot clash with other cores' resamplers.
NDS_FLAGS := -O2 -w $(PIC) -include deps/vio2sf/vio2sf_names.h
# SSEQPlayer defines two std::codecvt ids for old libc++; libstdc++ has them (as its Kodi build notes).
SSEQ_FLAGS := -std=gnu++11 -O2 -w $(PIC) -D_LIBCPP_VERSION

$(OBJ)/deps/vio2sf/%.o: deps/vio2sf/%.c deps/vio2sf/vio2sf_names.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 $(NDS_FLAGS) -c -o $@ $<

$(OBJ)/deps/vio2sf/%.o: deps/vio2sf/%.cpp deps/vio2sf/vio2sf_names.h
	@mkdir -p $(dir $@)
	$(CXX) -std=gnu++11 $(NDS_FLAGS) -c -o $@ $<

$(OBJ)/deps/sseqplayer/%.o: deps/sseqplayer/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SSEQ_FLAGS) -c -o $@ $<

$(OBJ)/src/ncsf_play.o: src/ncsf_play.cpp src/ncsf_play.h
	@mkdir -p $(dir $@)
	$(CXX) $(SSEQ_FLAGS) -Ideps -Isrc -c -o $@ $<
$(OBJ)/deps/lakesnes/%.o: deps/lakesnes/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -w $(PIC) -DLAKESNES_NO_RENDER -c -o $@ $<

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

# The chip-music tests link px_source and every format it knows: the DSP plugin minus its entry points.
PX_TEST_OBJ := $(filter-out $(OBJ)/src/dsp.o,$(DSP_OBJ))

# RSN soundtracks through px_source, against a real SPC set: make test-rsn [RSN=set.rsn]
RSN ?= $(TESTDIR)/rips/loz3.rsn
$(TESTDIR)/test_rsn$(EXE): test/test_rsn.c $(PX_TEST_OBJ) | $(TESTDIR)
	$(CC) $(CFLAGS) -c -o $(OBJ)/test_rsn.o $<
	$(CXX) -static -o $@ $(OBJ)/test_rsn.o $(filter-out $<,$^) $(LDLIBS) $(DSP_LIBS)

test-rsn: $(TESTDIR)/test_rsn$(EXE)
	$(TESTDIR)/test_rsn$(EXE) $(RSN)
# GBA GSF through px_source. The rip is not in the repository: download one (with its .gsflib)
# and name a song, e.g. make test-gsf GSF_RIP="build/test/rips/gsf/minish/11 Hyrule Field.minigsf"
GSF_RIP ?= build/test/rips/gsf/minish/11 Hyrule Field.minigsf
# Linked against the DSP plugin's objects, so every format decoders.c knows comes along.
GSF_TEST_OBJ := $(OBJ)/test/test_gsf.o $(PX_TEST_OBJ)

$(OBJ)/test/test_gsf.o: test/test_gsf.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TESTDIR)/test_gsf$(EXE): $(GSF_TEST_OBJ) | $(TESTDIR)
	$(CXX) -static -o $@ $(GSF_TEST_OBJ) $(LDLIBS) $(DSP_LIBS)

test-gsf: $(TESTDIR)/test_gsf$(EXE)
	$(TESTDIR)/test_gsf$(EXE) "$(GSF_RIP)"

# Nintendo DS rips through px_source. The rips are not in the repository: unpack Zophar's Domain's
# Phantom Hourglass 2SF set to $(NDS_RIPS)/ph_2sf and the NCSF site's (cyberbotx.com/NCSF) set of the
# same game to $(NDS_RIPS)/ph_ncsf.
NDS_RIPS ?= $(TESTDIR)/rips
$(TESTDIR)/test_nds$(EXE): test/test_nds.c $(PX_TEST_OBJ) | $(TESTDIR)
	$(CXX) -static -o $@ -x c -std=gnu11 -O2 $(WARN) -Isrc $< -x none $(filter-out $<,$^) $(LDLIBS) $(DSP_LIBS)

test-nds: $(TESTDIR)/test_nds$(EXE)
	$(TESTDIR)/test_nds$(EXE) "$(NDS_RIPS)/ph_2sf/001 Title.mini2sf" \
	    "$(NDS_RIPS)/ph_2sf/009 Tetra's Pirates.mini2sf" \
	    "$(NDS_RIPS)/ph_ncsf/001 - Title.minincsf" "$(NDS_RIPS)/ph_ncsf/009 - Tetra's Pirates.minincsf"

test: $(TESTDIR)/proteus_testcore_libretro.$(EXT) $(TESTDIR)/testcore_libretro.$(EXT) \
      $(TESTDIR)/harness$(EXE) $(TESTDIR)/assets.stamp $(DSP) $(TESTDIR)/test_ra$(EXE) $(TESTDIR)/test_apu$(EXE) \
      $(TESTDIR)/test_reference$(EXE)
	$(TESTDIR)/test_ra$(EXE)
	$(TESTDIR)/test_apu$(EXE)
	$(TESTDIR)/test_reference$(EXE) $(TESTDIR)/reference
	$(TESTDIR)/harness$(EXE) run $(TESTDIR)/proteus_testcore_libretro.$(EXT) $(TESTDIR) $(TESTDIR)/mixed.wav
	$(TESTDIR)/harness$(EXE) dsp $(DSP) $(TESTDIR)/testcore_libretro.$(EXT) $(TESTDIR)

# SNSF playback through px_source, on rips downloaded to build/test/rips/snsf (not in the repo):
# Chrono Trigger and Wario's Woods from ftp.modland.com, "Super Nintendo Sound Format".
SNSF_RIPS ?= $(wildcard $(TESTDIR)/rips/snsf/*/*.minisnsf $(TESTDIR)/rips/snsf/*/*.snsf)

$(TESTDIR)/test_snsf$(EXE): test/test_snsf.c $(PX_TEST_OBJ) | $(TESTDIR)
	$(CC) $(CFLAGS) -c -o $(OBJ)/test_snsf.o test/test_snsf.c
	$(CXX) -static -o $@ $(OBJ)/test_snsf.o $(filter %.o,$^) $(LDLIBS) $(DSP_LIBS)

test-snsf: $(TESTDIR)/test_snsf$(EXE)
	$(TESTDIR)/test_snsf$(EXE) $(SNSF_RIPS)

# Atari 2600: Proteus around Stella, checked against Stella itself. STELLA is Stella's
# libretro core as its authors build it, STELLAPX the build with the capture interface.
TEST2600 := $(BUILD)/test2600
STELLA   ?= ../cores/stock/stella_libretro.$(EXT)
STELLAPX ?= ../cores/px/stellapx_libretro.$(EXT)

$(TEST2600):
	mkdir -p $@

$(TEST2600)/rom2600$(EXE): test/rom2600.c | $(TEST2600)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST2600)/harness2600$(EXE): test/harness2600.c src/fx_track.c src/fx.h src/proteus_capture.h | $(TEST2600)
	$(CC) $(CFLAGS) -o $@ test/harness2600.c src/fx_track.c $(LDLIBS)

$(TEST2600)/pxtest.a26: $(TEST2600)/rom2600$(EXE)
	$(TEST2600)/rom2600$(EXE) $@
	$(TEST2600)/rom2600$(EXE) $(TEST2600)/pxtest_pal.a26 pal

# The game modules, held to what docs/GAME_MODULES.md asks of them; and the template, which
# is to compile though it is no game.
$(TEST2600)/lint_games$(EXE): test/lint_games.c $(FX_SRC) $(GAME_SRC) src/games/_template.c $(HEADERS) | $(TEST2600)
	$(CC) $(CFLAGS) -o $@ test/lint_games.c $(FX_SRC) $(GAME_SRC) src/games/_template.c $(LDLIBS)

lint-games: $(TEST2600)/lint_games$(EXE)
	$(TEST2600)/lint_games$(EXE)

test2600: lint-games $(CORE) $(TEST2600)/harness2600$(EXE) $(TEST2600)/pxtest.a26
	sh test/run2600.sh $(abspath $(TEST2600)) $(abspath $(STELLA)) $(abspath $(STELLAPX)) $(abspath $(CORE))

clean:
	rm -rf $(BUILD)

.PHONY: all test test2600 lint-games clean studio cli test-rsn test-gsf test-nds test-snsf
