# vgmstream in Proteus Retune

Plays streamed game music from the GameCube onward (src/vgm_play.c): Nintendo's DSP-ADPCM streams
and containers (DSP, ADP, AST, HPS, THP, BRSTM, BFSTM, BCSTM, BWAV...) and CRI's ADX and HCA; the
extensions px_source hands it are listed in src/vgm_play.c.

- Source: https://github.com/vgmstream/vgmstream, tag `r2117` (commit
  `71e2361042531fe767fb98300cf8c1ee95e539a0`, 2026-05-20), unmodified.
- Kept: `src/` (its `.c` and `.h` files: the top level, `base/`, `coding/`, `coding/libs/`,
  `layout/`, `meta/`, `util/`), `ext_includes/` and `COPYING`.
- Left out: the players and plugins (`cli/`, `fb2k/`, `winamp/`, `xmplay/`, `audacious/`),
  `ext_libs/`, `doc/`, and the CMake, autotools, MSVC and Makefile builds (the Proteus Makefile
  builds every `.c` under `src/` itself).
- Built without any of vgmstream's optional external codecs: no `VGM_USE_*` is defined, so no
  FFmpeg, mpg123, Vorbis, ATRAC9, CELT, Speex, G.719 or G.722.1. Streams that need one of those
  (Switch Opus, Ogg, MP3, AT9...) fail to open; their extensions are not claimed by px_source.
  `ext_includes/` is kept for builds that do turn codecs on; this build does not read it.
- Built with `-std=gnu11 -O2 -w`, as Hyrule Chronicle's WebAssembly engine builds it, into an
  archive (its ~650 objects overflow a Windows command line). On Windows it adds
  `-DVGM_STDIO_UNICODE`, so UTF-8 paths open through `_wfopen`.

## License

ISC-style permissive license (`COPYING`), which is compatible with Proteus Retune's
GPL-3.0-or-later. Some files carry their own permissive notices, kept with them.
