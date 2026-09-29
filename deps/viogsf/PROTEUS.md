# viogsf in Proteus Retune

Plays Game Boy Advance GSF/miniGSF rips (`src/gsf_play.cpp`); `deps/psflib` reads the files.

- Source: https://github.com/kode54/viogsf (Christopher Snowhill), commit
  `6c43a9926a6a85fbb736ea8f5f7f6c4f59ed3d64` (2018-01-30). It is VisualBoyAdvance-M's GBA core
  cut down to the ARM7 interpreter, memory, timers, DMA, a high-level BIOS and the sound hardware,
  with its state held in one `GBASystem` struct so several songs can play at once.
- Left out: `viogsf.pro` and the Visual Studio project files. `apu/Effects_Buffer.cpp` is kept but
  not built (nothing uses it, and it would add a second `blargg_vector<int>` next to libgme's).
- One change, marked `Proteus:` in `gba/GBA.h`: `soundFinalWave` grows from 1600 to 6400 samples.
  `flush_samples` writes a sixtieth of a second into it at a time, so 1600 only held 48 kHz; 6400
  holds 192 kHz, the top of the rates Proteus asks for.
- Portable C++98/11: no threads, no recompiler, no assembly outside a PowerPC big-endian path
  that other builds never see, no OS calls. Emscripten builds it as is.

## License

`License.txt` gives the terms: the VBA/VBA-M files are licensed under the GNU General Public
License version 2 or (at your option) any later version, and `gpl.txt` is that license's text.
The `apu/` files are Shay Green's Gb_Snd_Emu/Blip_Buffer, LGPL 2.1 or later (the notice is in
each file). Proteus Retune, which includes them, is GPL-3.0-or-later.
