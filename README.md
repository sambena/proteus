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
| Muting the original music | for the whole game: the profile's `[mute]` options are saved as RetroArch game options (by **Generate INI**, or by the plugin the first time the game runs, which then needs the game loaded again) | song by song, automatically |
| Settings | the profile; song changes are logged to `logs/proteus.log` | Quick Menu → Core Options → Proteus Retune, with per-song pickers and on-screen song values |
| Save states | music follows the song detected after loading | music position is saved in the state |

When the wrapper core is running, the plugin stands aside. Games without a profile
play exactly as they would without Proteus.

## Download

Windows builds are on the [Releases page](https://github.com/sambena/proteus/releases). Unzip,
close RetroArch, and run the installer from the unzipped folder (see [Install](#install));
`READ ME FIRST.txt` has the commands. No games, ROMs or music are included.

## Build

From an MSYS2 **UCRT64** shell (`pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2`):

```sh
make          # build/proteus_dsp.dll (plugin) and build/proteus_libretro.dll (wrapper core)
make studio   # build/ProteusStudio.exe (song discovery and profile mapping GUI)
make cli      # build/proteus-cli.exe (Studio's scans and reference songs from the command line)
make test     # runs the headless test suite (needs sox for the test assets)
make ZLIB=0   # without compressed .vgz support
```

`ProteusStudio.exe` and both DLLs link their C++ runtime, zlib, and dependencies statically,
so they only need Windows' own runtime libraries.

## Install

Close RetroArch, then from the release folder (or `tools\` in a source checkout):

```powershell
powershell -ExecutionPolicy Bypass -File .\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Dsp            # plugin
powershell -ExecutionPolicy Bypass -File .\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Core snes9x    # wrapper core
powershell -ExecutionPolicy Bypass -File .\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Core fceumm    # wrapper core for NES
```

`tools/package-release.sh <version>` builds everything and packs the release zip.

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

Proteus Studio (`make studio`, SDL2 + Dear ImGui) builds a profile from two SNES or NES ROMs: the
game to change and a game to take music from. It needs RetroArch's snes9x core for SNES games and
its FCEUmm core for NES games (see [NES games](#nes-games)).

1. Open the game to change on the left and the music source on the right (or drop ROMs on
   either side).
2. **Scan songs** on each side. Studio starts the game, sends every song number through the
   game's music command from that moment, and rips whatever the sound chip plays to an `.spc`
   file. Values that change nothing, repeat an earlier song (even shifted in time), or stay
   silent are dropped; short songs are marked as jingles. Rips are kept in
   `%APPDATA%\ProteusStudio\library`, so a ROM opens with its songs next time.

   When it is not known how the game starts songs, or the known way starts none, the scan finds
   out first. It watches the game boot and, each time a command goes to the sound CPU, notes the
   RAM bytes copied to its ports and the calls left on the CPU stack. It then tries, with a few
   song numbers each:
   - writing each command it saw back to RAM for the game to send (A Link to the Past:
     `$012C = song`), and
   - calling each routine it saw with the command in RAM or the song number in A, by pointing
     the CPU at a few instructions in spare RAM (Chrono Trigger: `JSL $C70004` with
     `$1E00 = 10 song FF 05`),

   and keeps the one that starts the most different songs.
   With **reference songs** for the game (below), each rip is named after the song it matches,
   only real songs are kept, and a way of starting songs counts only when it starts at least two
   different reference songs.
3. Click play on any song, from either game, to listen. Rips play from the sound chip state,
   with their own loops.
4. For each song of the game to change, pick a replacement (or drag one from the right), or
   Silence, or any music file.
5. **Generate INI** writes the profile to the game's existing profile or
   `system/proteus/<game>.ini`, and copies the chosen songs to `system/proteus/music/<game>/`.
   A profile that Studio did not write is first saved as `<game>.ini.bak`.

Songs are numbered by the **song address** Proteus follows while the game runs. Before ripping,
a scan starts a few songs and reads all of work RAM back: a byte that holds exactly the song
started, and keeps it, is where the game records it (A Link to the Past: `$0130`). Addresses
from the game database, RetroAchievements notes and static analysis are preferred when they pass
this check; one that fails is not used. When no byte keeps the song number, the command itself
becomes the song address (Super Mario World: `$1DFB`; Chrono Trigger: `$1E00 = 10 song .. ..`,
where `..` matches the bytes that change from song to song).

Rips are taken at a song's first notes when the game goes quiet while it loads the song, so they
play from the start.

### Reference songs

A game's soundtrack as `.spc` files, from an archive such as [SNESmusic.org](https://www.snesmusic.org) or
[Zophar's Domain](https://www.zophar.net/music/nintendo-snes-spc), makes Studio's results reliable. Each
`.spc` holds the sound CPU's memory while its song plays, including the song's data. Add them per game
with **Reference songs**:

- **Download** finds the game's set on Zophar's Domain, or else on SNESmusic.org, by the ROM's file name and
  the game's title. Names are compared by their words, so "Legend of Zelda - A Link to the Past, The (USA)"
  finds "Legend of Zelda: A Link to the Past".
- **Import folder...** or **Import archive or .spc...** copies a set you downloaded. Dropping a folder, an
  archive or `.spc` files on a panel does the same. SNESmusic.org's `.rsn` sets, and `.rar` and `.7z` archives,
  are opened with [7-Zip](https://www.7-zip.org), which must be installed.
- **List reference songs** adds every reference to the list without playing the game. That is all a
  music source needs.

Sets are kept in `%APPDATA%\ProteusStudio\reference\<ROM CRC32>`.

**Naming rips.** A rip is compared with every reference, counting only the sound CPU memory that changed
since the song was started, so what an earlier song left behind does not count. Data that many
references hold (instrument samples, which load wherever there is room) counts for little; data only
one song holds counts for much.

Some sound drivers (Super Mario World's) load a whole group of songs at once and start one by moving a
pointer, so little memory tells those songs apart. Studio also listens: it measures how strongly each of
the 12 notes sounds over a song's first 30 seconds, and lines the rip up with each reference wherever
they match best (sets are often dumped seconds into a song). A weak memory match is kept only when the
rip's notes rank that song among their closest three, and a rip no memory matches is named when its
notes match one reference almost exactly, well ahead of any other song. A rip that is the music already
playing, exactly as far along as the time that passed, changed nothing and is not named; the same song
started over lines up differently and is. Super Mario World's overworld songs are named this way.
A scan names each song after its reference and uses the reference's
`.spc`, which plays the song from its start. Songs that match no reference are marked **no match**.

**Song tables.** Many games keep their songs uncompressed in the ROM, with a table of pointers to them.
When the references' song data is found in the ROM and a table points to most of it, Studio reads the
song numbers from the table (entry 0 is where the game's code reads it). Scan songs then lists every
song of the table by name at once, plays the table's song numbers, and checks each one against its
reference. Chrono Trigger's table is at `$C70D18`: 83 songs, all named by the SNESmusic set.

### NES games

NES games work the same way in Studio, with these differences:

- **Rips are recordings.** The NES plays its music on the main CPU, so there is no sound chip state to
  save: a rip is 20 seconds of what the game plays, kept as a `.wav` file.
- **Reference songs are `.nsf` sets.** Download finds the game's set on Zophar's Domain; its `.m3u`
  playlist names the songs (a set without one lists every song of the `.nsf` by number). Rips are named by
  their notes alone, and a song named by a reference plays from the `.nsf`, looping as the game does.
- **Finding how songs start.** An `.nsf` holds the game's own music code, and many rips start a song
  the way the game does, by writing a request to RAM. Studio runs the `.nsf`'s init routine for every song
  on a small 6502 and notes the RAM each writes (The Legend of Zelda: `$0600 = 80` for the title, `01` for
  the overworld). Those bytes are tried first; when one starts songs in the game, the values the `.nsf`
  writes are the game's songs: they are listed by name at once, and only they are played to check them.
  Otherwise the scan watches the RAM while the game starts up, and writes each byte that takes a few values
  from a moment music plays (many games play none on their title screen), keeping the bytes after which the
  game sounds different. Super Mario Bros. asks for songs through `$FB`, which it reads as bits, and keeps
  the song playing at `$F4`; a song it plays under many numbers is listed under its first number and single
  bits.
- **Watching the `.nsf`'s song RAM in the game.** Writing a request from outside does not always start a
  song (the game may read it only at certain moments), but the game still uses the same RAM. From power-on,
  with Start tapped now and then, the scan watches every byte the `.nsf` writes with its songs' values. A
  byte that takes them, each either cleared at once (a request: Metroid `$0684`, Punch-Out!! `$00F0`) or
  held while the song plays (Castlevania `$0082`), and never swaps one for another within a second,
  becomes the song address, and the set's songs are listed by those values. Requests are used before a
  tap; a held byte, which may be channel state that sound effects also change, only when no tap works.
- **A tap on the sound routine.** Much music code keeps no song number anywhere: Mega Man 3's `.nsf`
  starts a song by calling the game's "play this sound" routine (`$8106`) with the song in A. The rip's
  code is the ROM's own, so Studio taps that routine with cheat codes: its first instructions move to a
  stub in blank ROM, which writes each request + 1 to a RAM byte no code names and the game leaves alone,
  then carries on into the routine. The scan checks the tap reports one of the `.nsf`'s songs in the game;
  that byte becomes the song address (`latch = 1`), every song of the set is listed by the value the game
  requests it with, and sound effects, which the tap reports too, are left unlisted (`unmapped = keep`).
  Profiles carry the tap in a `[tap]` section, on for the whole game. Taps are FCEUmm cheat codes, so
  they need FCEUmm. Games found neither way can be played with **Play & rip** (`R` rips the music
  playing), which names the songs heard and, after three or more, can learn the song address as it does
  for SNES games.
- **Stopping the music, keeping the sound effects.** Most NES games play sound effects on the music's
  channels, so muting channels loses them. After a scan, Studio writes each value of the song request
  while music plays and keeps one that stops the music (Super Mario Bros.: `$FB = 80`); profiles then
  carry a `[silence]` section instead of channel mutes, and Proteus writes that request whenever
  replacement music starts. The RAM that holds the song playing then reads the silence, so profiles
  follow the requests instead: the song request (`$FB`, `latch = 1`) and the register the `.nsf` starts
  its other songs with, as jingles (`events = 0x00FC`: the death jingle is song 0x101), so the game's own
  jingles stop the replacement. The `.nsf` also shows how to stop the music code itself: Studio skips each
  call and flips each branch its play routine runs, and keeps a change that silences the music songs while
  the `.nsf`'s sound effects keep sounding, when the ROM holds that code (Mega Man 3: `809D` `D0` → `F0`).
  A patch that silences the `.nsf` can still leave a note hanging in the game, so each is heard there too,
  from a moment a song starts over another, and kept only when the game goes quiet (Mega Man 2's first
  candidate left 83% of the sound; `8269` leaves none). Profiles carry it as a `[patch]` cheat code,
  applied only while replacement music plays. Games with
  neither (The Legend of Zelda so far) fall back to muting channels:
  FCEUmm switches each of the NES's five channels on or off (`fceumm_apu_1` .. `_5`: two squares,
  triangle, noise and samples), and Studio mutes the squares and triangle by default. The DSP plugin
  saves those mutes as game options, which stay off for the whole game.
- TAS movies are SNES only for now.

**Progress.** Scan folder over 100 popular NES games, with FCEUmm and Zophar's `.nsf` sets:

| | Games |
| --- | --- |
| Song address found and at least 3 songs named | 78 (17 before the watch, taps and 6502 fixes) |
| ... through the `.nsf`'s song RAM watched in the game | 41 |
| ... through a tap on the sound routine | 21 |
| ... through RAM that starts songs, or the song finder | 16 |
| Music stopped with a code patch, sound effects kept | 23 |
| Music stopped with a RAM request, sound effects kept | 12 |
| Partly (no song address, or fewer than 3 songs named) | 17 |
| Skipped (no `.nsf` set, or the ROM did not open) | 5 |

A found song address is not a game heard working: Super Mario Bros., Mega Man 2, Mega Man 3 and
Castlevania have been played through the wrapper core with replacements, the game's music stopped and
its sound effects kept. Still partly: Contra, Zelda II, Tetris, Gradius, Dragon Warrior, Faxanadu,
Life Force, Kung Fu, Pac-Man, Paperboy, Rad Racer, R.B.I. Baseball, Kickle Cubicle and Top Gun (1943,
Duck Tales 2 and Shadow of the Ninja have no set on Zophar's Domain).

### TAS movies

Games whose songs a scan cannot start from outside (ActRaiser 2 uploads each song itself) can still
be worked out by playing them through. [TASVideos](https://tasvideos.org) publishes movies that play
whole games, and **TAS movie** on either panel (Advanced > **TAS movie**) uses them:

**Play movie and find songs** does every step that is missing, in order; the steps can also be done
one at a time:

1. **Download BizHawk** installs the latest [BizHawk](https://github.com/TASEmulators/BizHawk) release
   into `%APPDATA%\ProteusStudio\tools\BizHawk`. Movies only stay in sync on the emulator they
   were made with: on snes9x, or on libretro's bsnes cores, a movie falls out of step within minutes.
   BizHawk needs .NET Framework 4.8 and the Microsoft Visual C++ runtime.
2. **Find movies on TASVideos** lists the game's publications; **Download** saves a BizHawk movie
   (`.bk2`) to `%APPDATA%\ProteusStudio\movies\<ROM CRC32>`. **Open movie file...** adds one you have.
   The movie must be for the same version of the ROM.
3. **Play movie and find songs** plays it in BizHawk, as fast as the computer allows up to the speed
   chosen (most SNES movies run 5 to 8 times faster than real time). A Lua script saves the sound CPU's
   memory and work RAM every 2 seconds. Each moment is named against the reference songs, and a moment
   counts only when the moments on both sides of it name the same song. The work RAM byte that holds one
   value for each song, and a different value for every song, becomes the song address (saved to the
   game database), and the songs heard join the list with their numbers.

ActRaiser 2's movie hears all 15 reference songs in six and a half minutes and numbers 13 of them
by `$0028`. Games that pass songs through a command and keep no song number (Chrono Trigger) have
their songs named, but no address is found this way.

### Scan folder

**Scan folder** in the header scans every ROM in a folder, one after another: it downloads each game's
reference songs, runs Scan songs, and, when asked and BizHawk is installed, plays a TAS movie for games the
scan could not number. Each game is rated **easy** (a song address and at least three songs named by
reference songs), **partly** (songs or reference songs, but not both working) or **skip**. Results go to
the song libraries and the game database as a scan in the window would, and to a report in
`%APPDATA%\ProteusStudio\folder-scans`; a stopped scan carries on where it left off. Double-click a game
to open it.

### Game database

What Studio learns about a game is kept in `%APPDATA%\ProteusStudio\games.ini`, one section per
ROM CRC32 (without a copier header), so each game is worked out once:

```ini
[2D206BF7]
name = Chrono Trigger
song_address = system_ram 0x1E00 bytes=10 xx .. .. latch=1 debounce=1
start = routine jsl 0xC70004 block=0x1E00 bytes=10 xx FF 05 settle=300
note = confirmed by a scan 2026-09-13

[B19ED489]
name = Super Mario World
song_address = system_ram 0x1DFB size=1 latch=1 debounce=1
start = ram 0x1DFB bytes=xx settle=150
```

`start` says how songs are started: `ram <address> bytes=<command>` writes a command the game
sends itself; `routine <jsl|jsr> <address> [block=<address> bytes=<command>] [a=song]` calls the
game's music routine. `xx` marks the song number; `settle` is how many frames a song gets before
it is ripped. Scans and the song finder write the file; it can also be edited by hand or under
**Advanced > Game info** (which also includes **RetroAchievements lookup** to query documented BGM
addresses by ROM hash). Super Mario World, A Link to the Past and Chrono Trigger are built in.

The **Advanced** tabs:

| Advanced tab | Use |
| --- | --- |
| Play & rip | Play either game. Whenever the game sends the sound CPU a command, or the song address changes, the new song is ripped once it has started (repeats are skipped); **Rip current song** (`R`) rips whatever plays. **Scan from this moment** makes later scans start there, for games that load music per world or level. With reference songs, play also learns the song address: every 2 seconds the song playing is named, and RAM that does not hold one value per song is ruled out; when one byte is left after three songs or more, it becomes the song address and the rips are numbered by it (A Link to the Past: `$0130` is among the last bytes left after three songs). For games that choose their music in their own logic and upload each song (ActRaiser 2), which no scan can start, this or **TAS movie** is the way. |
| TAS movie | Play a TASVideos movie of the whole game in BizHawk to hear its songs and learn the song address (see [TAS movies](#tas-movies)). **Play movie and find songs** does every missing step. |
| Find song address | Press **Music changed** (`M`) right after the music changes and **Same music** (`N`) when it does not; the song and command bytes remain. |
| Game info | The song address, how songs start, and the scan range, saved to the game database. |
| Channels & mix | The channels muted while replacements play, and the mix volumes. |
| INI preview | The profile Generate INI will write. |

Game controls in Advanced: arrow keys or a controller; `Z`/`X` B/A, `A`/`S` Y/X, `Q`/`W` L/R,
`Enter` Start, `Right Shift` Select; `P` pause, `Tab` fast forward, `F2`/`F4` save/load state.

A RAM command only finds songs whose music data is loaded at the scan's starting moment; a music
routine loads each song itself. Games whose songs start some other way (Super Metroid and
ActRaiser queue them; Mega Man X was not found either) cannot be scanned yet: play them with
**Play & rip** or **TAS movie**, or add a `start` line to the game database once it is known.

## Game profiles

Proteus looks for a profile in this order:

1. next to the ROM: `Super Game (USA).proteus.ini`
2. in RetroArch's system folder: `system/proteus/Super Game (USA).ini`

Paths are relative to the profile. Lines starting with `;` or `#` are comments; after a value,
` ;` starts one (a `#` there is part of the value, as in `music/Stage #1.ogg`). A file name holding
`;` or `|` goes in double quotes: `0x05 = "music/Act 1; Part 2.ogg" | loop=0`.

```ini
[song]
memory   = system_ram   ; system_ram | save_ram | video_ram | rtc
address  = 0x1234       ; offset of the "current song" value
size     = 1            ; 1, 2 or 4 bytes, little endian (ignored when bytes is set)
bytes    = 10 xx .. ..  ; optional: multi-byte command block pattern; xx is the song, .. any value
mask     = 0xFF         ; optional
debounce = 2            ; frames a new value must hold before it counts
latch    = 1            ; optional: address is a command register (ignores 0, keeps playing last song)
events   = 0x00FC       ; optional: a second command register for jingles, read as song 0x100 + command
unmapped = original     ; what unlisted values do: original | silence | keep
byte_order = n64        ; optional: N64 addresses in RDRAM kept as host-order 32-bit words (Mupen64Plus)
active   = 0x80128B60 & 0x80 ; optional: while these bits are clear no song plays (a sequence player's flag)
stopped  = original     ; what a clear `active` flag does: original | silence | keep

[mute]
; Core options forced while replacement music (or silence) is active.
snes9x_sndchan_volume_1 = 0
snes9x_sndchan_volume_2 = 0

[silence]
; Optional: a request written to RAM that stops the game's own music while replacement music
; (or silence) is active, instead of muting channels, so sound effects keep playing.
memory   = system_ram
address  = 0x00FB       ; Super Mario Bros.
value    = 0x80

[hold]
; Optional: RAM written before every frame while replacement music (or silence) is active, silencing
; the game's music player; sound effects play on. Values are hex bytes (big endian with
; byte_order = n64). After a comma, what is written back when the mute lifts (by default, what was
; there); |= sets bits, and sets them once more after the mute (a "recalculate volume" flag).
0x80128B8C = 00000000, 3F800000   ; Ocarina of Time: the BGM player's volume scale
0x80128B60 |= 04

[patch]
; Optional: cheat codes, per core, that stop the game's music code while replacement music (or
; silence) plays, keeping its sound effects. A core's lines are joined with '+'.
fceumm = 809D?D0:F0     ; Mega Man 3

[tap]
; Optional: cheat codes, per core, on for the whole game, that make it report what it asks its
; sound routine to play at the song address (see NES games).
fceumm = 8106?C9:4C+8107?F0:1E+8108?90:8A+8A1E?00:08+8A1F?00:48+8A20?00:18+8A21?00:69+8A22?00:01
fceumm = 8A23?00:8D+8A24?00:FC+8A25?00:07+8A26?00:68+8A27?00:28+8A28?00:C9+8A29?00:F0+8A2A?00:B0
fceumm = 8A2B?00:03+8A2C?00:4C+8A2D?00:0D+8A2E?00:81+8A2F?00:4C+8A30?00:0A+8A31?00:81

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
| `name` | what the game's song is, shown in the Quick Menu song pickers (`0x02 = original \| name=Hyrule Field`) |

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
`0x0ABC`. For NES it is the 2 KB of RAM at `$0000`-`$07FF`. For N64 (Mupen64Plus-Next) it is the
8 MB of RDRAM; with `byte_order = n64`, write addresses as the game sees them (`0x80222618`).

### N64 games (Nintendo EAD sound engine)

Super Mario 64 and Ocarina of Time play music with sequence players: structures in RAM with the
sequence (song) number, a playing flag and volumes, one player for background music and others for
fanfares and sound effects. The profile follows the BGM player's sequence number and holds its
volume at zero while replacing, so the sound effects on the other players are untouched.

| Game | Player | Song | Playing flag | Silence it |
| --- | --- | --- | --- | --- |
| Super Mario 64 (USA) | `0x80222618` (size 0x140) | `+0x05` | `+0x00 & 0x80` | fade volume `+0x18 = 00000000, 3F800000` |
| Ocarina of Time (USA 1.0) | `0x80128B60` (size 0x160) | `+0x04` | `+0x00 & 0x80` | volume scale `+0x2C = 00000000, 3F800000` and `+0x00 \|= 04` (recalculate) |

The addresses move between revisions (Ocarina of Time Rev 1: `0x80128D20`, Rev 2: `0x80129430`), so
let proteus-cli find them and write the profile, every song listed by name:

```
proteus-cli n64 <cores\mupen64plus_next_libretro.dll> <ROM or ROM folder> <system\proteus> [--force]
```

Sequence numbers are the decompilations' (`seq_ids.h` in n64decomp/sm64, `sequence_table.h` in
zeldaret/oot): Ocarina of Time's title is `0x1E`, file select `0x57`, Hyrule Field `0x02`.

### Muting the original music

When the game's music can be stopped through its RAM, a `[silence]` section does that and no
channel needs muting. Otherwise the mute options are core-specific:

| Core | Options | Mute value |
| --- | --- | --- |
| snes9x | `snes9x_sndchan_volume_1` .. `_8` | `0` |
| FCEUmm (NES) | `fceumm_apu_1` .. `_5` (square 1, square 2, triangle, noise, samples) | `disabled` |
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
| `studio/main.cpp` | Proteus Studio's window: the two song lists, replacements, Advanced tabs |
| `studio/rom_session.cpp` | one open ROM: its emulator thread, song scans, finding how songs start, live ripping, song library |
| `studio/game_db.cpp` | the game database (`games.ini`) |
| `studio/snes_rom.cpp` | SNES ROM header, CRC32, and CPU address mapping |
| `studio/reference.cpp` | reference songs: matching rips, finding song tables in the ROM, importing and downloading sets |
| `studio/nsf_init.cpp` | a small 6502 that runs an `.nsf`'s init and play routines: the RAM requests and routine calls that start its songs, and code patches that silence its music |
| `studio/nes_tap.cpp` | taps on an NES game's sound routine: a stub in blank ROM, as cheat codes, reporting the game's requests |
| `studio/song_notes.cpp` | a song's notes over time, for matching songs that memory cannot tell apart |
| `studio/tas_runner.cpp` | TASVideos lookups and downloads, installing BizHawk, playing a movie in BizHawk with a RAM-saving Lua script |
| `studio/movie_learner.cpp` | names the songs of a movie's moments and learns the song address from them |
| `studio/folder_scan.cpp` | Scan folder: every ROM of a folder in turn, with a report |
| `studio/zip_read.cpp`, `studio/http.cpp` | reading zip archives; HTTPS requests (RetroAchievements, Zophar's Domain, SNESmusic.org, TASVideos, GitHub) |
| `studio/cli/proteus_cli.cpp` | `proteus-cli`: song tables, matching, scans (`scan --profile` exports one), TAS movies, folder scans and downloads without the window; `nsftrace`, `nsfcalls`, `nsfpatch`, `nsfsurvey` and `nestap` look into `.nsf` rips and design taps |
| `studio/spc_rip.cpp` | turns a snes9x save state into an `.spc` file |
| `studio/profile_export.cpp` | writes the profile and copies the music |
| `studio/core_host.cpp` | minimal libretro frontend; runs up to four cores at once |
| `studio/audio_out.cpp` | plays songs and game audio |
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
| `deps/imgui` — Dear ImGui 1.92.x | MIT (`deps/imgui/LICENSE.txt`) |
| `deps/libretro.h`, `deps/libretro_dspfilter.h` | MIT |
| `deps/dr_wav.h`, `deps/dr_mp3.h` | public domain / MIT-0 |
| `deps/stb_vorbis.c` | public domain / MIT |
| `studio/md5.cpp` | follows RFC 1321; derived from the RSA Data Security, Inc. MD5 Message-Digest Algorithm |

Proteus Retune includes no games, ROMs, soundtracks or movies. Studio downloads, only when asked,
SPC sets from Zophar's Domain and SNESmusic.org, input movies from TASVideos, and BizHawk (MIT) from
its GitHub releases; they keep their own terms, and are kept in your `%APPDATA%\ProteusStudio`
folder, not in this repository. Use ROMs you own.

Proteus Retune is not affiliated with Nintendo, Libretro/RetroArch, TASVideos, BizHawk, Zophar's
Domain, SNESmusic.org or RetroAchievements. Game titles are trademarks of their owners.

## Roadmap

- Muting music by patching the game's play-song routine instead of muting channels
