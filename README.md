# Proteus Retune

A libretro core for RetroArch that swaps a game's soundtrack while keeping its
sound effects.

Proteus wraps an existing core (snes9x, Genesis Plus GX, Mesen, ...). Per game it:

1. watches a RAM address to learn which song the game is playing,
2. mutes the original music by forcing the core's per-channel volume options,
3. mixes in a replacement WAV, MP3 or Ogg Vorbis track, with crossfades and loop points.

Save states, rewind and run-ahead keep the replacement music in step with the game.
Games without a profile play exactly as they would on the wrapped core.

## Build

From an MSYS2 **UCRT64** shell (`pacman -S make mingw-w64-ucrt-x86_64-gcc`):

```sh
make          # build/proteus_libretro.dll
make test     # runs the headless test suite (needs sox for the test assets)
```

## Install

Proteus chooses the core to wrap from its own file name:
`proteus_snes9x_libretro.dll` wraps `snes9x_libretro.dll` in the same folder.

```powershell
.\tools\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Core snes9x
```

This copies the DLL and creates a `.info` file, so RetroArch lists
**Proteus Retune (Snes9x)**. Load a game with that core instead of snes9x itself.

## Game profiles

Proteus looks for a profile in this order:

1. next to the ROM: `Super Game (USA).proteus.ini`
2. in RetroArch's system folder: `system/proteus/Super Game (USA).ini`

Track paths are relative to the profile.

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

[tracks]
; song value = file | options
0x01 = music/title.ogg
0x05 = music/overworld.ogg | loop_start=264600
0x06 = music/boss.mp3 | volume=80
0x07 = music/fanfare.wav | loop=0
0x08 = silence
0x09 = original

[debug]
log_songs = 1           ; log and show every song change on screen
```

### Finding the song address

Use a RAM search tool (RetroArch's cheat search, or the memory viewer in Mesen,
bsnes-plus or RetroAchievements' RAIntegration): note values in one music area,
move to an area with different music, and keep the addresses that changed. Then put
the candidate in `[song] address`, set `log_songs = 1`, and check the log
while walking between areas.

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
| `src/profile.c` | profile parser |
| `src/music.c` | resampling mixer with crossfades and loops |
| `src/decoders.c` | WAV / MP3 / Ogg sources |
| `test/` | a fake game core and a headless frontend that checks the mixed audio |
| `deps/` | `libretro.h` (MIT), dr_wav and dr_mp3 (public domain / MIT-0), stb_vorbis (public domain / MIT) |

## Roadmap

- Emulated music sources through libgme: `.spc`, `.nsf`, `.vgm`, `.gbs`, so one game can
  play another game's soundtrack on its original sound chip
- Muting music by patching the game's play-song routine instead of muting channels
- A discovery mode that helps find the song address
