# Proteus Retune

A RetroArch audio plugin and libretro core that swap a game's soundtrack while keeping
its sound effects.

Proteus works alongside an existing core (Snes9x, Genesis Plus GX, Mesen, ...). Per game it:

1. watches a RAM address to learn which song the game is playing,
2. mutes the original music through the core's per-channel volume options,
3. mixes in replacement music, with crossfades and loop points. The replacement can be
   - a recording: WAV, MP3 or Ogg Vorbis, or
   - another game's music on its original sound chip, emulated by libgme: SNES `.spc`,
     NES `.nsf`/`.nsfe`, Genesis/Master System `.vgm`/`.vgz`/`.gym`, Game Boy `.gbs`,
     PC Engine `.hes`, MSX `.kss`, ZX Spectrum `.ay` and Atari `.sap`.

Proteus comes in two forms that share profiles and music files:

| | DSP plugin | Wrapper core |
| --- | --- | --- |
| Use with | any core, unchanged — keep choosing Snes9x | a separate core entry, e.g. "Proteus Retune + Snes9x" |
| Turn on | Settings → Audio → DSP Plugin → `Proteus.dsp` (or a per-core/per-game override) | load the game with the wrapper core |
| Muting the original music | for the whole game, via the core's channel volumes saved as game options | song by song, automatically |
| Settings | the profile; song changes are logged to `logs/proteus.log` | Quick Menu → Core Options → Proteus Retune, with per-song pickers and on-screen song values |
| Save states | music follows the song detected after loading | music position is saved in the state |

When the wrapper core is running, the plugin stands aside. Games without a profile
play exactly as they would without Proteus.

## Build

From an MSYS2 **UCRT64** shell (`pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2`):

```sh
make          # build/proteus_dsp.dll (plugin) and build/proteus_libretro.dll (wrapper core)
make studio   # build/ProteusStudio.exe (song discovery and profile mapping GUI)
make test     # runs the headless test suite (needs sox for the test assets)
make ZLIB=0   # without compressed .vgz support
```

`ProteusStudio.exe` and both DLLs link their C++ runtime, zlib, and dependencies statically,
so they only need Windows' own runtime libraries.

## Install

```powershell
.\tools\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Dsp            # plugin
.\tools\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Core snes9x    # wrapper core
```

**Plugin:** copies `proteus_dsp.dll` and a `Proteus.dsp` preset to `filters\audio`.
Select it under Settings → Audio → DSP Plugin. It finds the running core inside
RetroArch and the running game from RetroArch's content history, so history must be
enabled (the default).

**Wrapper core:** Proteus chooses the core to wrap from its own file name:
`proteus_snes9x_libretro.dll` wraps `snes9x_libretro.dll` in the same folder. The
script also creates a `.info` file, so RetroArch lists
**Nintendo - SNES / SFC (Proteus Retune + Snes9x)**. Load a game with that core
instead of Snes9x itself.

## Core options (wrapper core)

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

## Proteus Studio

Proteus Studio (`make studio`) is a companion GUI tool (SDL2 + Dear ImGui) for creating
and testing game profiles interactively:

1. **Setup**: Select your RetroArch folder, choose an installed core, and open a ROM
   or zip archive.
2. **Find address**: Play the game directly in Studio. Press **M** right after the music
   changes; press **N** periodically while walking around with unchanged music. The candidate
   list quickly narrows down the RAM addresses holding the song value or command register.
3. **Songs**: Discovered songs collect automatically with a screenshot thumbnail and a
   12-second clip of the original music. Assign replacement files, silence, or original music,
   and test them live inside the game with **Hear replacements in the game**.
4. **Channels**: Toggle emulator channels live to discover which voices carry music vs sound
   effects. Checked channels are automatically saved into the profile's `[mute]` section.
5. **Analyze**: If the game uses a command register, automatically sweep song numbers from a
   save state to audition and catalog every song in the game without playing through it.
6. **Profile**: Adjust mix volume and crossfades, save the profile (`.proteus.ini` next to the ROM
   or in RetroArch's `system/proteus/`), and optionally export channel mutes as RetroArch per-game
   core options for the DSP plugin.

Controls:
- **D-pad**: Arrow keys (or left stick / D-pad on controller)
- **Face buttons**: `Z` (B), `X` (A), `A` (Y), `S` (X)
- **Shoulders**: `Q` (L), `W` (R)
- **Menu**: `Enter` (Start), `Right Shift` (Select)
- **Shortcuts**: `P` (pause), `Tab` (fast forward), `F2`/`F4` (save/load state), `M` (mark music changed), `N` (mark music same)

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
latch    = 1            ; optional: address is a command register (ignores 0, keeps playing last song)
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

The easiest method is using **Proteus Studio** (`make studio`), which narrows down
candidates automatically as you mark song changes while playing, or sweeps command
registers across a save state.

Alternatively, use a RAM search tool (RetroArch's cheat search, or the memory viewer in Mesen,
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
| `src/engine.c` | song detection, choosing what plays, mixing, save state data (shared) |
| `src/proteus.c` | the wrapper core: libretro API passthrough, option overrides, save states |
| `src/dsp.c` | the DSP plugin: finds the running core and game inside RetroArch |
| `src/options.c` | merges Proteus's core options with the wrapped core's, for every libretro option API |
| `src/profile.c` | profile parser |
| `src/music.c` | resampling mixer with crossfades and loops |
| `src/decoders.c` | WAV / MP3 / Ogg decoders and libgme sources |
| `studio/` | Proteus Studio: Dear ImGui + SDL2 frontend for finding songs and creating profiles |
| `deps/imgui/` | Dear ImGui bundled library |
| `test/` | a fake game core and a headless frontend that checks the mixed audio and the options |
| `tools/install-core.ps1` | installs the plugin and wrapper cores into a RetroArch folder |

## License

Proteus Retune is licensed under the GNU Lesser General Public License v2.1 or later
(`LGPL-2.1-or-later`); see [LICENSE](LICENSE). Bundled libraries in `deps/` keep their
own licenses:

| Library | License |
| --- | --- |
| `deps/gme` — libgme 0.6.5 | LGPL-2.1-or-later (`deps/gme/LICENSE`); `ext/emu2413` is MIT |
| `deps/imgui` — Dear ImGui 1.91.x | MIT (`deps/imgui/LICENSE.txt`) |
| `deps/libretro.h`, `deps/libretro_dspfilter.h` | MIT |
| `deps/dr_wav.h`, `deps/dr_mp3.h` | public domain / MIT-0 |
| `deps/stb_vorbis.c` | public domain / MIT |

## Roadmap

- Muting music by patching the game's play-song routine instead of muting channels
