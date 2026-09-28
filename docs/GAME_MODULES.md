# Game modules: the standard

Proteus draws and plays every Atari 2600 game with effects that need to know nothing of the
game. A **game module** adds what is particular to one game: which object is the enemy, what
colour it is to have, what is to be heard when it is hit.

This is how a module is made, what is asked of it, and what is there to make it with. It is
written so that the twentieth game is no harder to add than the second, and so that someone
who opens a module they did not write finds everything where it is in every other.

## Where a module sits

```
Stella (capture build)          what the TIA drew with which object, every register write,
        |                       the two voices apart, the console's memory
        v
src/fx_track.c                  the frame's objects, and the same object over frames
        |
        v
src/games/<game>.c   frame()    roles, colours, light, sparks, a backdrop     <- the module
        |            sound()    which of the game's sounds Proteus plays itself
        v
src/fx_video.c, fx_audio.c      glow, shadows, smoothing, panning, reverb: as for any game
```

A module never draws the picture and never mixes the sound. It changes what the effects are
given, through `px_scene` and `px_sound` (`src/fx.h`), with the help of the kit
(`src/kit.h`).

## Adding a game

1. **Study it.** See [Studying a game](#studying-a-game). An hour with the tools tells what
   the game draws with what, and what its sounds are.
2. **Copy** `src/games/_template.c` to `src/games/<game>.c`. Replace `template` by the
   game's name and `tp` by two letters for it.
3. **List it** in `src/games/games.h`. The Makefile finds the file by itself.
4. **Fill it in**, in the order of the file.
5. **Check it**: `make lint-games`, then `make test2600`, then the game itself with
   `test/games2600.sh`.
6. **Write it down**: a section in the README under "Games Proteus knows", and the game's
   line in `docs/TOP_GAMES.md`.

One game is one branch and one pull request.

## The file

Every module has these parts, in this order, under these headings.

| Part | What is in it |
|---|---|
| The comment at the top | The game, and what was found out about it: what it draws with which object in which rows, what its memory holds, anything that surprised. This is the part a later reader needs most. |
| The ROMs, the defaults and the options | `md5[]`, `fx[]`, the option keys, their values, `options[]`. |
| What the module keeps | One `struct` for everything that lasts from frame to frame. No globals: two games may be open at once. |
| The picture | `frame()` and what it needs. |
| The sounds | A comment that lists the game's sounds as register values, then `sound()` and the sounds Proteus plays instead. |
| The module | `reset`, `create`, `destroy`, `configure`, and the `px_game` itself, last. |

A module has up to three hooks. `frame` gets every picture between the finding of its
objects and its drawing. `sound` gets every frame before its sound is mixed. `who` is for
games whose things are too near each other to be told apart by where they are: it is asked
who an object is before the object is matched to those of the frames before.

## The rules

**The game's own is always to be had.** Every change of colour and every sound of Proteus's
has an option whose last value is the game as it was (`original`). With the option "What
Proteus knows of the game" off, the module is not asked at all.

**The four options a player looks for.** Where a game has a use for them, they have these
names, so that the panel reads alike from game to game:

| Key | Name | Values |
|---|---|---|
| `proteus_<id>_colors` | Colours (or what is coloured: "Ghost colours") | the first is the default, `original` the last |
| `proteus_<id>_backdrop` | Backdrop | what it shows, `off` |
| `proteus_<id>_sparks` | Explosions | `enabled`, `disabled` |
| `proteus_<id>_sound` | Sounds | `proteus`, `original` |

Other options are the module's to name. A name is at most 24 letters, as are the labels of
its values: the panel has a line of 52.

**A frame drawn again looks as it did.** When `px_scene.advance` is false the game stands
still (the player opened the panel). Nothing is counted, moved on or set off then.

**What is not known is left alone.** An object without a role keeps its colour. A sound the
module does not know is heard as the game plays it: a voice is made silent (`voice[n] = 0`)
only while it plays what Proteus plays itself.

**Nothing is taken for granted.** The console's memory may not be there (`px_kit_ram` gives
-1), a frame may have no objects, the sound may come without a picture. `make lint-games`
runs every module through all of these.

**After `reset`, nothing is remembered.** It is called when the game is reset and when a
state is loaded.

**Sounds are told at the end of the frame.** Between the writes of one frame a voice may
have the waveform of one sound and the pitch of another. `px_kit_tia` keeps what was there
when the frame ended, and what was there the frame before.

**Roles.** An object is given one of `PX_ROLE_*` (`src/fx.h`) where one fits. The sound hook
gets the objects of the picture before with their roles, to play a sound where the thing is
that made it.

**Music.** A tune a game plays is the game's: Proteus may play its notes with better
voices, lower or with more of them. It does not put another tune in its place.

## The kit

`src/kit.h` says what each of these does. A module that needs something a second module
would need too adds it there.

| For | What |
|---|---|
| Colours | `px_rgb_add`, `px_rgb_scale`, `px_rgb_mix`, `px_kit_wave` |
| Options | `px_kit_on`, `px_kit_pick`, `px_kit_toggle`, `px_kit_sounds` |
| Memory | `px_kit_ram` |
| What is known of an object | `px_kit_tags`: `begin`, `find`, `keep`, `gone`, `end` |
| The playfield and the background | `px_kit_playfield`, `px_kit_background`, `px_kit_outline` for walls drawn as outlines, `px_kit_small_playfield` for what is to be eaten among them, and `px_scene.light` for scenery that glows |
| Objects | `px_scene_tint`, `px_scene_energy`, `px_kit_fill_holes`, `px_kit_is_player` |
| What happens | `px_scene_burst`, `px_scene_flash`, `px_sound_rumble` |
| A backdrop | `px_scene.backdrop`, `px_kit_canvas` for the part of it that stands still |
| The game's voices | `px_kit_tia`: `hear`, `began`, `louder`; `px_kit_tia_hz`, and `px_kit_tune` for the note a pitch is nearest to |
| Sounds of Proteus's own | `px_tone`, `px_kit_play`, `px_kit_pan`, `px_synth_play`, `px_synth_move`, `px_synth_stop` |

## What games have in common

These come up in game after game. The module named with each is where to look for how it is
done.

| What the game does | What the module does | Where |
|---|---|---|
| Shows several things with one object, in turns (flicker) | Nothing, where they are far enough apart to be told by where they are: they are drawn in every frame as for any game. Where they are not, the module says who each is (`px_game.who`), from its colour or from memory, and they are followed by that. | Pac-Man: the ghosts |
| Draws rows of copies of one object, set again for every row | Tags each with the row it began in, since rows move. | Space Invaders: the invaders |
| Draws its score on every other line, or with the playfield | Fills the lines between; recolours the rows of the score. | Space Invaders |
| Draws a maze or scenery with the playfield | Recolours it, and gives it light so that it glows. Tells what is wall from what is to be eaten by its shape, and draws that as dots (`px_kit_dots`). | Pac-Man, Ms. Pac-Man |
| Is silent by a pitch too high to hear, its volume left on | Takes a voice for silent by that pitch. | Ms. Pac-Man |
| Makes a sound for one thing and none for another like it | One hook tells the other: the picture sees the power pill eaten, the sound hears her caught. | Ms. Pac-Man |
| Has a black background | Paints a backdrop, which shows where the background is dark. | Space Invaders: the night sky |
| Counts lives, enemies, dots in memory | Compares with the frame before: sparks, a flash, the controller shakes. | both |
| Has one voice for two sounds | Plays each with voices of its own, so that neither cuts the other off. | Space Invaders: the step and the hit |
| Has silence where the arcade had a sound | Adds one that follows the game: a hum, a siren. It has an option of its own. | both |
| Gives its things holes for eyes | Fills what an object encloses with a colour. | Pac-Man: the ghosts |
| Plays a tune | Plays its notes with other voices. | Pac-Man: the four notes at the start |

**Games of one family share a file.** What two games have in common and no third would
need (the colours of the arcade's ghosts, the sound of eating) is neither in the kit nor in
both modules: it is in a header next to them, `src/games/pac_family.h` for Pac-Man and
Ms. Pac-Man. Each module still tells by itself what happens in its game.

What no module has had to do yet, and the games that will ask for it, is in
`docs/TOP_GAMES.md`.

## Studying a game

Everything in `tools/2600/` reads what `record.sh` writes. Each says what it takes when it is
run without arguments.

```sh
# Play the game for 3000 frames, the stick in all four directions.
tools/2600/record.sh "C:/roms/Pac-Man (USA).a26" pm 3000 --roam

# What is drawn with which object, in which colours and shapes?
tools/2600/shapes.sh build/study/pm.objects.txt

# What do the voices play?
tools/2600/sounds.sh build/study/pm.sound.txt

# Which bytes of memory say where things are?
tools/2600/ram-where.sh build/study/pm.objects.txt build/study/pm.ram.txt

# Which bytes count what happens when voice 0 plays waveform 9?
tools/2600/ram-events.sh build/study/pm.sound.txt build/study/pm.ram.txt 0 9
```

What does not happen by itself is staged. Once memory is known to hold where things are, a
thing is put where another is:

```sh
# In frame 800, Pac-Man (bytes 49 and 54) goes where the first ghost is (50 and 55).
tools/2600/record.sh "C:/roms/Pac-Man (USA).a26" eaten 1400 --roam --seed 2 \
   --poke 800:49:@50 --poke 800:54:@55
```

To see what Proteus makes of the objects, the option "View" has `instances` (a box around
every object) and `layers` (every class in a colour of its own).

## Checking a module

| Check | What it holds the module to |
|---|---|
| `make lint-games` | The tables (ROMs, defaults, options) are well formed and clash with no other module. The module gets through frames without objects, without memory and without sound, with every option at either end, and through being reset. |
| `make test2600` | Proteus as a whole, with the test program. |
| `test/games2600.sh <test dir> <out dir> <rom>` | With the game itself: Proteus is given what Stella draws, and shows it unchanged with the effects off. Writes pictures with and without what the module does. |

A pull request for a game says what was checked by these, and what was only looked at or
listened to.
