# Proteus Retune

A libretro core for RetroArch that swaps a game's soundtrack while keeping its
sound effects.

Proteus wraps an existing core (snes9x, Genesis Plus GX, Mesen, ...). Per game it:

1. watches a RAM address to learn which song the game is playing,
2. mutes the original music by forcing the core's per-channel volume options,
3. mixes in replacement music, with crossfades and loop points. The replacement can be
   - a recording: WAV, MP3 or Ogg Vorbis, or
   - another game's music on its original sound chip, emulated by libgme: SNES `.spc`,
     NES `.nsf`/`.nsfe`, Genesis/Master System `.vgm`/`.vgz`/`.gym`, Game Boy `.gbs`,
     PC Engine `.hes`, MSX `.kss`, ZX Spectrum `.ay` and Atari `.sap`.

Settings and per-song choices are available in RetroArch under
**Quick Menu → Core Options → Proteus Retune**. Save states, rewind and run-ahead
keep the replacement music in step with the game. Games without a profile play
exactly as they would on the wrapped core.

## Build

From an MSYS2 **UCRT64** shell (`pacman -S make mingw-w64-ucrt-x86_64-gcc`):

```sh
make          # build/proteus_libretro.dll
make test     # runs the headless test suite (needs sox for the test assets)
make ZLIB=0   # without compressed .vgz support
```

The DLL links its C++ runtime and zlib statically, so it only needs Windows' own
runtime libraries.

## Install

Proteus chooses the core to wrap from its own file name:
`proteus_snes9x_libretro.dll` wraps `snes9x_libretro.dll` in the same folder.

```powershell
.\tools\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Core snes9x
```

This copies the DLL and creates a `.info` file, so RetroArch lists
**Proteus Retune (Snes9x)**. Load a game with that core instead of snes9x itself.

## Core options

Proteus adds a **Proteus Retune** category next to the wrapped core's own options:

| Option | Values |
| --- | --- |
| Music replacement | Enabled / Disabled |
| Replacement music volume | Profile default, 0–200% |
| Game audio volume | Profile default, 0–200% (everything the game plays, including sound effects) |
| Crossfade | Profile default, Off, 100–2000 ms |
| Song change notifications | Profile default, Enabled / Disabled; shows each song value on screen |
| Song 0x05, Song 0x06, ... | Profile default, Original music, Silence, or any playable file |

The song pickers appear for every song listed in the game's profile. They list the
files in the profile's `[library]` folders and in the folders its tracks come from;
multi-song files such as NSF appear once per song (`dungeon.nsf #3`).

Changes apply immediately. Use RetroArch's **Manage Core Options → Save Game Options**
to keep them for one game only.

## Game profiles

Proteus looks for a profile in this order:

1. next to the ROM: `Super Game (USA).proteus.ini`
2. in RetroArch's system folder: `system/proteus/Super Game (USA).ini`

Paths are relative to the profile.

```ini
[song]
memory   = system_ram   ; system_ram | save_ram | video_ram | rtc
address  = 0x1234       ; offset of the "current song" value
size     = 1            ; 1, 2 or 4 bytes, little endian
mask     = 0xFF         ; optional
debounce = 2            ; frames a new value must hold before it counts
unmapped = original     ; what unlisted values do: original | silence | keep

[mute]
; Core options forced while replacement music (or silence) is active.
snes9x_sndchan_volume_1 = 0
snes9x_sndchan_volume_2 = 0

[mix]
music_volume = 100      ; percent, up to 200
game_volume  = 100
crossfade_ms = 400

[library]
; Folders offered in the song pickers (tracks' own folders are always included).
dir = music
dir = ../Other Game/spc

[tracks]
; song value = file | options
0x01 = music/title.ogg
0x05 = music/overworld.ogg | loop_start=264600
0x06 = ../Other Game/spc/boss.spc
0x07 = music/fanfare.wav | loop=0
0x08 = music/soundtrack.nsf | track=4
0x09 = silence
0x0A = original

[debug]
log_songs = 1           ; log and show every song change on screen
```

Track options:

| Option | Meaning |
| --- | --- |
| `loop` | `1` (default) loops the track; `0` plays it once. Emulated tracks without a tagged length keep playing. |
| `loop_start` | sample frame the loop returns to (recordings) |
| `volume` | percent, up to 200 |
| `track` | song number inside a multi-song file (NSF, NSFE, GBS, HES, KSS, AY, SAP), starting at 1 |

Emulated music plays at the wrapped core's sample rate, so an SPC under snes9x needs no
resampling.

### Finding the song address

Use a RAM search tool (RetroArch's cheat search, or the memory viewer in Mesen,
bsnes-plus or RetroAchievements' RAIntegration): note values in one music area,
move to an area with different music, and keep the addresses that changed. Then put
the candidate in `[song] address`, turn on **Song change notifications**, and watch
the on-screen values while walking between areas.

For SNES, `system_ram` is the 128 KB work RAM, so address `$7E0ABC` becomes
`0x0ABC`.

### Muting the original music

The mute options are core-specific:

| Core | Options | Mute value |
| --- | --- | --- |
| snes9x | `snes9x_sndchan_volume_1` .. `_8` | `0` |
| Genesis Plus GX | `genesis_plus_gx_md_channel_0_volume` .. `_5` (FM, MAME FM emulators only), `genesis_plus_gx_psg_channel_0_volume` .. `_3` | `0` |

Sound effects in most SNES games use the upper voices (often 7 and 8), so muting
voices 1–6 usually keeps them. Games that steal music voices for effects will
lose some effects; mute fewer channels for those games.

## Layout

| Path | Purpose |
| --- | --- |
| `src/proteus.c` | libretro API passthrough, song detection, option overrides, save states |
| `src/options.c` | merges Proteus's core options with the wrapped core's, for every libretro option API |
| `src/profile.c` | profile parser |
| `src/music.c` | resampling mixer with crossfades and loops |
| `src/decoders.c` | WAV / MP3 / Ogg decoders and libgme sources |
| `test/` | a fake game core and a headless frontend that checks the mixed audio and the options |
| `tools/install-core.ps1` | installs a wrapper into a RetroArch folder |

## License

Proteus Retune is licensed under the GNU Lesser General Public License v2.1 or later
(`LGPL-2.1-or-later`); see [LICENSE](LICENSE). Bundled libraries in `deps/` keep their
own licenses:

| Library | License |
| --- | --- |
| `deps/gme` — libgme 0.6.5 | LGPL-2.1-or-later (`deps/gme/LICENSE`); `ext/emu2413` is MIT |
| `deps/libretro.h` | MIT |
| `deps/dr_wav.h`, `deps/dr_mp3.h` | public domain / MIT-0 |
| `deps/stb_vorbis.c` | public domain / MIT |

## Roadmap

- Muting music by patching the game's play-song routine instead of muting channels
- A discovery mode that helps find the song address
