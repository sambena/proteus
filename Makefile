# Build from an MSYS2 UCRT64 shell (or any gcc toolchain on Linux/macOS).
#   make          build build/proteus_libretro.<ext>
#   make test     build the test core + harness and run the scenarios

CC      ?= gcc
BUILD   := build
TESTDIR := $(BUILD)/test

ifeq ($(OS),Windows_NT)
  EXT     := dll
  EXE     := .exe
  LDLIBS  :=
  SHARED  := -shared -static-libgcc -Wl,--no-undefined
else
  EXT     := so
  EXE     :=
  LDLIBS  := -ldl
  SHARED  := -shared -fPIC -Wl,--no-undefined
  CFLAGS  += -fPIC
endif

CFLAGS  += -std=gnu11 -O2 -Wall -Wextra -Ideps -Isrc
LDLIBS  += -lm

CORE    := $(BUILD)/proteus_libretro.$(EXT)
SOURCES := src/proteus.c src/profile.c src/music.c src/decoders.c src/util.c
HEADERS := $(wildcard src/*.h)

all: $(CORE)

$(CORE): $(SOURCES) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(SHARED) -o $@ $(SOURCES) $(LDLIBS)

$(BUILD) $(TESTDIR):
	mkdir -p $@

$(TESTDIR)/testcore_libretro.$(EXT): test/testcore.c | $(TESTDIR)
	$(CC) $(CFLAGS) $(SHARED) -o $@ $< $(LDLIBS)

$(TESTDIR)/harness$(EXE): test/harness.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# The wrapper picks its inner core from its own file name.
$(TESTDIR)/proteus_testcore_libretro.$(EXT): $(CORE) | $(TESTDIR)
	cp $< $@

$(TESTDIR)/assets.stamp: $(TESTDIR)/harness$(EXE) test/game.proteus.ini
	$(TESTDIR)/harness$(EXE) gen $(TESTDIR)
	sox $(TESTDIR)/tone_330.wav $(TESTDIR)/tone_330.ogg
	sox $(TESTDIR)/tone_550.wav $(TESTDIR)/tone_550.mp3
	rm $(TESTDIR)/tone_330.wav $(TESTDIR)/tone_550.wav
	cp test/game.proteus.ini $(TESTDIR)/
	touch $(TESTDIR)/game.tst $(TESTDIR)/other.tst $@

test: $(TESTDIR)/proteus_testcore_libretro.$(EXT) $(TESTDIR)/testcore_libretro.$(EXT) \
      $(TESTDIR)/harness$(EXE) $(TESTDIR)/assets.stamp
	$(TESTDIR)/harness$(EXE) run $(TESTDIR)/proteus_testcore_libretro.$(EXT) $(TESTDIR) $(TESTDIR)/mixed.wav

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
