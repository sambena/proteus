# vio2sf in Proteus Retune

Plays Nintendo DS 2SF/mini2SF rips (`src/twosf_play.c`).

- Source: https://gitlab.com/kode54/vio2sf (Christopher Snowhill), commit
  `1d68801f5fd370c3275affd51505353e2b366a7e` (2019-05-02): `src/vio2sf/desmume/`, unmodified.
  The same commit is mirrored on GitHub in https://github.com/xbmc/audiodecoder.2sf (`lib/vio2sf`).
- Left out: the Visual Studio projects, the Makefile and `trim2sflib.c`.
- Added here: `vio2sf_names.h`, which the Makefile force-includes into every vio2sf file to give
  its global symbols a `vio2sf_` prefix, so they cannot clash with other emulator cores linked into
  the same program. It was generated from `nm -g --defined-only` of the objects; regenerate it if
  vio2sf is updated.
- Portable C and C++: interpreters only (no recompiler), no threads. `resampler.c` uses `cpuid`
  inline assembly only on x86 hosts with GCC or Clang, to pick its SSE path; other targets
  (WebAssembly) build the plain C path.
- It emulates the DS sound hardware at 44.1 kHz only, so a 2SF source reports that rate and the
  mixer resamples it.

## License

vio2sf is a cut-down DeSmuME. Its DeSmuME files are licensed under the GNU General Public License
version 2 or (at your option) any later version, as their headers say; `desmume/COPYING` is that
license's text. The files vio2sf adds (`state.c`, `barray.c`, `resampler.c`) carry no license of
their own; vio2sf is distributed as a whole under DeSmuME's terms, as GPL-2.0 section 2(b)
requires of a work based on it. Proteus Retune, which includes it, is GPL-3.0-or-later.
