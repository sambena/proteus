# LakeSnes in Proteus Retune

Plays SNES SNSF/miniSNSF rips (src/snsf_play.c), which carry a SNES ROM patched to play one song:
the whole console (65816, SPC700, S-DSP) is emulated, the picture is not drawn.

- Source: https://github.com/dinkc64/LakeSnes (the maintained fork of angelo-wf/LakeSnes),
  commit `048a0d72568668f74e0cdf371dcd9e8efd614965` (2025-04-11). Kept: `snes/` and
  `LICENSE.txt`. Left out: the SDL frontend (`main.c`), `zip/`, `tracing.*`, `resources/`.
- `snes/snes_other.c` (ROM loading with console messages, save states) is kept but not built:
  src/snsf_play.c maps the ROM itself, because SNSF tags can force the mapping and region.
- Portable C99 with no threads, assembly or OS calls, so it also builds with Emscripten.

## Changes

Only `snes/snes.c`, so each player owns all of its state:

- The CPU memory access-time table was a process-wide 16 MB table rebuilt by every instance
  (two open players would share it, and closing one freed it under the other). The timing is now
  computed per access from the instance's own `fastMem` setting.
- `-DLAKESNES_NO_RENDER` skips drawing each scanline (the PPU registers still work), which is
  what a music player wants.

## License

MIT (`LICENSE.txt`), which is compatible with Proteus Retune's GPL-3.0-or-later.
