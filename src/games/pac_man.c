/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pac-Man (Atari, 1982).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  11..177  the maze: the background is its floor, the playfield its walls (four
 *                  rows high, or a playfield bit wide) and its wafers (two rows high)
 *   ghosts         player 0, one of the four a frame, in turns: each is drawn in one frame
 *                  of four. They have one shape, and hues of their own so alike that they
 *                  pass for one colour: FC, EC, DC and CC, in the order of their turns.
 *                  All are 7A while they can be eaten, and 5A while that is about to end.
 *                  Eaten, a ghost is a pair of eyes six rows high.
 *   Pac-Man        player 1, 14 rows high; smaller shapes while he dies
 *   power pills    missile 0, which takes the colour of the ghost of the frame; the two at
 *                  the left and the two at the right in turns
 *   rows 182..192  the score: both players, in black on a bar of the background's colour
 *   rows 194..199  the lives: playfield
 *
 * Things leave the maze at the top and come back at the bottom, and are cut off at either
 * while they do: a ghost six rows high is not for that a pair of eyes.
 *
 * When no game is played the colours change every few seconds, the ghosts' too.
 *
 * Of its memory ($80 is 0):
 *
 *   0        counts the frames; the ghost drawn is the one of its low two bits, less one
 *   49       Pac-Man's column          54       his row: he is drawn at 17 + 2 * it
 *   50..53   the ghosts' columns       55..58   their rows: drawn at 16 + 2 * it
 *   76       the wafers eaten
 *
 * Not known: what the vitamin is drawn with. It shows as the game draws it.
 */
#include "pac_family.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define MAZE_TOP   11
#define MAZE_END   178    /* the row after its last */
#define LIVES_TOP  193

#define RAM_FRAME   0
#define RAM_PAC_X   49
#define RAM_GHOST_X 50
#define RAM_PAC_Y   54
#define RAM_GHOST_Y 55
#define RAM_WAFERS  76
#define WAFERS      126    /* in a maze, by the count of them on the screen */

#define SHAPE_EYES 0x88DEB1F3u

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "6e372f076fb9586aff416144f5cfe1cb",   /* Pac-Man (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "50",
   "reverb", "small",
   NULL
};

#define OPT_COLORS "proteus_pm_colors"
#define OPT_MAZE   "proteus_pm_maze"
#define OPT_WAFERS "proteus_pm_wafers"
#define OPT_SPARKS "proteus_pm_sparks"
#define OPT_SOUND  "proteus_pm_sound"
#define OPT_SIREN  "proteus_pm_siren"

static const char *const colors[] = { "arcade", "Red, pink, cyan, orange",
   "original", "The game's own", NULL };
static const char *const mazes[] = { "neon", "Blue outlines on black", "solid", "Blue on black",
   "original", "The game's own", NULL };
static const char *const wafers[] = { "dots", "Round dots", "original", "The game's dashes", NULL };
/* In the order of the options' values. */
enum { COLORS_ARCADE = 0, COLORS_ORIGINAL };
enum { MAZE_NEON = 0, MAZE_SOLID, MAZE_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Ghost colours",
     "The four ghosts in the red, pink, cyan and orange of the arcade, with eyes, blue while they can be eaten and flashing before that ends; Pac-Man in yellow. Or the game's own colours, in which the ghosts look alike.",
     "arcade", colors },
   { OPT_MAZE, "Maze",
     "The maze as glowing blue outlines on black, as solid blue walls on black, or in the game's own colours.",
     "neon", mazes },
   { OPT_WAFERS, "Wafers",
     "What Pac-Man eats as round dots, or as the dashes the game draws.", "dots", wafers },
   { OPT_SPARKS, "Explosions",
     "Sparks where a ghost is eaten and where Pac-Man is caught, and a flash when he eats a power pill.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for eating, the power pill, a ghost eaten and Pac-Man caught, each where it happens between left and right, and the game's tune in softer voices. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_SIREN, "Siren",
     "A siren under the chase that rises as the wafers get fewer, where the game is silent. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   /* The options. */
   unsigned colors, maze;
   bool dots, sparks, own_sound, siren;

   uint32_t frame;                  /* counts the pictures that advance */

   /* The picture. */
   unsigned mode;                   /* PAC_IDLE and the like */
   bool eyes[PAC_GHOSTS];               /* a ghost was a pair of eyes when it was drawn last */
   unsigned dying;                  /* frames since Pac-Man began to die; 0: he does not */
   uint8_t *small;                  /* where the wafers are: an entry a captured pixel */

   /* The sounds. */
   px_kit_tia tia;                  /* the game's two voices */
   unsigned heard0;                 /* what voice 0 played in the frame before: SOUND_* */
   unsigned sweep;                  /* SOUND_CAUGHT or SOUND_EATEN while voice 0 plays it */
   unsigned silent0;                /* frames for which voice 0 has been silent */
   unsigned caught_for;             /* frames since Pac-Man was caught; 0: he was not */
   pac_voices voices;               /* what of Proteus's sounds goes on */
   unsigned moved;                  /* frames since a ghost last moved */
   uint8_t ghost_at[2 * PAC_GHOSTS];    /* where memory had the ghosts */
   int pac_at;                      /* Pac-Man's column, for where a sound is; -1: not known */
} pm;

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static bool in_maze(const px_instance *in)
{
   return in->y + (int)in->h > MAZE_TOP && in->y < MAZE_END;
}

static bool is_eyes(const px_instance *in)
{
   return in->h == 6 && in->hash == SHAPE_EYES;
}

/* Pac-Man caught by a ghost: he closes up and bursts. Not these shapes, he is whole, or cut
 * off where he leaves the maze. */
static bool is_dying(const px_instance *in)
{
   static const uint32_t shapes[] = {
      0xC7501779u, 0x81E07BEBu, 0x3FFD031Du, 0x0CE134DDu, 0x5320259Du, 0x3CCE7EF5u, 0xDE3D91C5u
   };
   for (unsigned i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++)
      if (in->hash == shapes[i])
         return true;
   return false;
}

/* Which ghost the one drawn in this frame is: by its hue in the chase, by the frame's count
 * when all have one colour. -1: not to be told. */
static int ghost_drawn(const px_glance *s, const px_instance *in)
{
   const unsigned hue = in->color >> 4, light = in->color & 0x0F;
   const int count = px_kit_ram(s->ram, s->ram_size, RAM_FRAME);
   if (light == 0x0C && hue >= 0x0C)
      return (int)(0x0F - hue);
   return count < 0 ? -1 : (count + 3) & 3;
}

static unsigned mode_of(const px_instance *in)
{
   const unsigned hue = in->color >> 4, light = in->color & 0x0F;
   if (in->color == 0x7A)
      return PAC_SCARED;
   if (in->color == 0x5A)
      return PAC_WARNING;
   return light == 0x0C && hue >= 0x0C ? PAC_CHASE : PAC_IDLE;
}

/* The ghosts are often too near each other to be told apart by where they are, and leave
 * the maze at the top to come back at the bottom: each is who the game says, wherever it
 * is. The power pills are away for eight frames at a time, which is longer than what is not
 * told apart may be. */
static unsigned who(void *state, const px_glance *g, const px_instance *in)
{
   (void)state;
   if (!in_maze(in))
      return 0;
   if (in->cls == PXC_L_P0)
   {
      const int which = ghost_drawn(g, in);
      return which < 0 ? 0 : PX_WHO_ANYWHERE | (unsigned)(which + 1);
   }
   return in->cls == PXC_L_M0 ? 1 : 0;
}

/* Roles, and which ghost is which. */
static void find_things(pm *g, px_scene *s)
{
   px_objects *o = s->objects;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role = PX_ROLE_NONE;
      if (!in_maze(in))
      {
         if (in->y >= MAZE_END)
            in->role = PX_ROLE_HUD;
         continue;
      }
      if (in->cls == PXC_L_P1)
         in->role = PX_ROLE_PLAYER;
      else if (in->cls == PXC_L_M0)
         in->role = PX_ROLE_BONUS;
      else if (in->cls == PXC_L_P0)
      {
         in->role  = PX_ROLE_ENEMY;
         in->group = in->who & 0x7F ? (uint8_t)((in->who & 0x7F) - 1) : 0;
         if (!in->ghost && !is_eyes(in))
            g->mode = mode_of(in);
      }
   }
}

static void draw_maze(pm *g, px_scene *s)
{
   if (!g->small)
      return;
   px_kit_small_playfield(s, MAZE_TOP, MAZE_END, 3, g->small);

   if (g->maze != MAZE_ORIGINAL)
   {
      px_kit_background(s, 0, s->frame->height, 0x000000);
      if (g->maze == MAZE_NEON)
         px_kit_outline(s, MAZE_TOP, MAZE_END, PAC_RGB_WALL, PAC_RGB_WITHIN, px_rgb_scale(PAC_RGB_WALL, 120),
               g->small);
      else
         px_kit_outline(s, MAZE_TOP, MAZE_END, PAC_RGB_WALL, PAC_RGB_WALL, 0, g->small);
      px_kit_playfield(s, LIVES_TOP, s->frame->height, PAC_RGB_PAC);
   }
   if (g->dots || g->colors == COLORS_ARCADE)
      px_kit_dots(s, g->small, MAZE_TOP, MAZE_END, g->colors == COLORS_ARCADE, PAC_RGB_WAFER, g->dots);
}

static void color_things(pm *g, px_scene *s)
{
   px_objects *o = s->objects;

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      if (in->role == PX_ROLE_HUD)
      {
         /* The score is black, as the maze is then. */
         if (g->maze != MAZE_ORIGINAL)
            px_scene_tint(s, in, PAC_RGB_SCORE);
         continue;
      }
      if (g->colors != COLORS_ARCADE)
         continue;
      switch (in->role)
      {
         case PX_ROLE_ENEMY:
         {
            uint32_t eyes = 0;
            px_scene_tint(s, in, pac_ghost_color(g->mode, g->frame, in->group, is_eyes(in), &eyes));
            if (eyes)
               px_kit_fill_holes(s, in, eyes);
            break;
         }
         case PX_ROLE_PLAYER:
            px_scene_tint(s, in, PAC_RGB_PAC);
            break;
         case PX_ROLE_BONUS:
            /* The power pills beat. */
            px_scene_tint(s, in, pac_pill_color(g->frame));
            px_scene_energy(s, in, true);
            break;
         default:
            break;
      }
   }
}

/* What happened since the frame before. */
static void find_events(pm *g, px_scene *s, unsigned mode_before)
{
   px_objects *o = s->objects;
   bool dies = false;

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      if (in->ghost)
         continue;
      if (in->role == PX_ROLE_ENEMY)
      {
         const unsigned which = in->group & 3;
         const bool eyes = is_eyes(in);
         if (eyes && !g->eyes[which] && g->sparks)
         {
            px_scene_burst(s, in->x + 4, in->y + 3, pac_ghost_colors[which], 26, 340);
            px_scene_burst(s, in->x + 4, in->y + 3, PAC_RGB_SCARED, 14, 220);
         }
         g->eyes[which] = eyes;
      }
      else if (in->role == PX_ROLE_PLAYER && is_dying(in))
      {
         dies = true;
         if (!g->dying && g->sparks)
            px_scene_flash(s, 0xFF3020, 90);
         /* The last of him. */
         if (in->h == 2 && g->dying < 1000 && g->sparks)
         {
            px_scene_burst(s, in->x + 4, in->y, PAC_RGB_PAC, 36, 400);
            g->dying = 1000;
         }
      }
   }
   g->dying = dies ? g->dying + 1 : 0;

   if (g->mode == PAC_SCARED && mode_before == PAC_CHASE && g->sparks)
      px_scene_flash(s, PAC_RGB_SCARED, 80);
}

static void frame(void *state, px_scene *s)
{
   pm *g = (pm*)state;
   const unsigned mode_before = g->mode;

   if (s->advance)
      g->frame++;

   find_things(g, s);
   draw_maze(g, s);
   color_things(g, s);
   if (s->advance)
   {
      find_events(g, s, mode_before);
   }
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has six, all but one on voice 0:
 *
 *   both      the tune before a game and after Pac-Man is caught: four notes of 16 frames,
 *             waveform 4 at volume 8 on both voices; voice 0 at pitch 4, 12, 4 and 8,
 *             voice 1 at 24, 20, 24 and 20
 *   voice 0   a wafer eaten: waveform 9 at pitch 9, the volume from 9 down to 2
 *   voice 0   a power pill eaten: waveform 13 at pitch 13 and volume 13, for 15 frames
 *   voice 0   a ghost eaten: waveform 4 at pitch 16 and volume 15 for a frame, then three
 *             times the pitch down through fifteen steps (from 14, 30 and 14) while the
 *             volume rises from 1
 *   voice 0   Pac-Man caught: waveform 4, the pitch from 31 down to 0 in two frames a step,
 *             the volume from 15 down to 1 four times over
 *   voice 1   while the ghosts can be eaten: waveform 4, the pitch from 0 up to 13 and the
 *             volume from 7 down to 1, every 32 frames. Not while that is about to end.
 *
 * The chase itself is silent, where the arcade had its siren.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_TUNE, SOUND_WAFER, SOUND_PILL, SOUND_EATEN, SOUND_CAUGHT,
   SOUND_OTHER };

static bool plays_tune(const px_kit_tia *t)
{
   return t->wave[0] == 4 && t->wave[1] == 4 && t->volume[0] == 8 && t->volume[1] == 8
         && (t->pitch[1] == 24 || t->pitch[1] == 20);
}

static bool voice1_warbles(const px_kit_tia *t)
{
   return t->wave[1] == 4 && t->volume[1] && t->volume[1] <= 7 && t->pitch[1] <= 13
         && !plays_tune(t);
}

/* What voice 0 plays, of what is told by one frame. */
static unsigned voice0_plays(const px_kit_tia *t)
{
   if (!t->volume[0])
      return SOUND_NONE;
   if (plays_tune(t))
      return SOUND_TUNE;
   if (t->wave[0] == 9 && t->pitch[0] == 9)
      return SOUND_WAFER;
   if (t->wave[0] == 13 && t->pitch[0] == 13)
      return SOUND_PILL;
   return SOUND_OTHER;
}

/* While the ghosts can be eaten: a note that climbs with the game's own, and by itself
 * when that is about to end, which the game passes over in silence. */
static void play_warble(pm *g, px_sound *s, bool on)
{
   pac_warble(&g->voices, s, on, voice1_warbles(&g->tia) ? (float)g->tia.pitch[1] / 13.0f
         : (float)(g->voices.ticks % 12) / 12.0f);
}

static void play_siren(pm *g, px_sound *s, bool on)
{
   const int eaten = px_kit_ram(s->ram, s->ram_size, RAM_WAFERS);
   pac_siren(&g->voices, s, on,
         eaten < 0 ? 0.0f : eaten >= WAFERS ? 1.0f : (float)eaten / (float)WAFERS);
}

/* Whether the game is on: the ghosts move. They stand while the tune plays and while
 * Pac-Man dies. */
static bool ghosts_move(pm *g, const px_sound *s)
{
   bool moved = false;
   for (unsigned i = 0; i < 2 * PAC_GHOSTS; i++)
   {
      const unsigned at = i < PAC_GHOSTS ? RAM_GHOST_X + i : RAM_GHOST_Y + i - PAC_GHOSTS;
      const int v = px_kit_ram(s->ram, s->ram_size, at);
      if (v < 0)
         return false;
      if (g->ghost_at[i] != (uint8_t)v)
         moved = true;
      g->ghost_at[i] = (uint8_t)v;
   }
   g->moved = moved ? 0 : g->moved + 1;
   return g->moved < 12;
}

static void sound(void *state, px_sound *s)
{
   pm *g = (pm*)state;
   unsigned heard;
   bool running, scared;

   /* Where Pac-Man is: from the picture before, or from memory. */
   g->pac_at = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
      if (s->objects->inst[i].role == PX_ROLE_PLAYER && !s->objects->inst[i].ghost)
         g->pac_at = s->objects->inst[i].x + 4;
   if (g->pac_at < 0 && px_kit_ram(s->ram, s->ram_size, RAM_PAC_X) >= 0)
      g->pac_at = px_kit_ram(s->ram, s->ram_size, RAM_PAC_X) + 4;

   g->voices.ticks++;
   px_kit_tia_hear(&g->tia, s->frame);
   heard = voice0_plays(&g->tia);

   /* The two sounds that are a waveform 4 going through its pitches are told by how they
    * begin, and go on until the voice has been silent for a while: both have gaps. */
   g->silent0 = g->tia.volume[0] ? 0 : g->silent0 + 1;
   if (g->silent0 > 6 || heard == SOUND_TUNE || heard == SOUND_WAFER || heard == SOUND_PILL)
      g->sweep = SOUND_NONE;
   if (heard == SOUND_OTHER && g->tia.wave[0] == 4)
   {
      if (!g->sweep && g->tia.volume[0] == 15 && g->tia.pitch[0] == 31)
      {
         g->sweep      = SOUND_CAUGHT;
         g->caught_for = 1;
         if (g->own_sound)
            pac_play_caught(s, g->pac_at);
         PAC_RUMBLE_CAUGHT(s);
      }
      else if (!g->sweep && g->tia.volume[0] == 15 && g->tia.pitch[0] == 16)
      {
         g->sweep = SOUND_EATEN;
         if (g->own_sound)
            pac_play_eaten(s, g->pac_at);
         PAC_RUMBLE_EATEN(s);
      }
   }
   if (g->sweep)
      heard = g->sweep;

   switch (heard)
   {
      case SOUND_TUNE:
         if (!g->own_sound)
            break;
         if (g->heard0 != SOUND_TUNE || g->tia.pitch[0] != g->tia.was_pitch[0])
            pac_play_note(s, true, px_kit_tia_hz(4, g->tia.pitch[0]) / 4.0f);
         if (g->heard0 != SOUND_TUNE || g->tia.pitch[1] != g->tia.was_pitch[1])
            pac_play_note(s, false, px_kit_tia_hz(4, g->tia.pitch[1]) / 4.0f);
         break;
      case SOUND_WAFER:
         if (g->heard0 != SOUND_WAFER || px_kit_tia_louder(&g->tia, 0))
         {
            if (g->own_sound)
               pac_play_wafer(&g->voices, s, g->pac_at);
            PAC_RUMBLE_WAFER(s);
         }
         break;
      case SOUND_PILL:
         if (g->heard0 != SOUND_PILL)
         {
            if (g->own_sound)
               pac_play_pill(s, g->pac_at);
            PAC_RUMBLE_PILL(s);
         }
         break;
      default:
         break;
   }
   g->heard0 = heard;

   /* The burst when the last of Pac-Man is gone: 118 frames after he was caught. */
   if (g->caught_for && ++g->caught_for == 118)
   {
      if (g->own_sound)
         pac_play_burst(s, g->pac_at);
      PAC_RUMBLE_BURST(s);
   }
   if (g->caught_for > 118 || (g->caught_for && heard != SOUND_CAUGHT && g->silent0 > 6))
      g->caught_for = 0;

   running = ghosts_move(g, s) && heard != SOUND_TUNE && heard != SOUND_CAUGHT;
   scared  = voice1_warbles(&g->tia) || (running && g->mode == PAC_WARNING);
   play_warble(g, s, g->own_sound && scared);
   play_siren(g, s, g->own_sound && g->siren && running && !scared && g->mode == PAC_CHASE);

   if (g->own_sound)
   {
      if (heard != SOUND_OTHER)
         s->voice[0] = 0.0f;
      if (!g->tia.volume[1] || plays_tune(&g->tia) || voice1_warbles(&g->tia))
         s->voice[1] = 0.0f;
   }
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   pm *g = (pm*)state;
   px_kit_tia_reset(&g->tia);
   memset(g->eyes, 0, sizeof(g->eyes));
   memset(g->ghost_at, 0, sizeof(g->ghost_at));
   g->mode   = PAC_IDLE;
   g->dying  = 0;
   g->heard0 = g->sweep = SOUND_NONE;
   g->silent0 = g->caught_for = 0;
   g->moved  = 1000;
   pac_voices_reset(&g->voices);
   g->pac_at = -1;
}

static void *create(void)
{
   pm *g = (pm*)calloc(1, sizeof(pm));
   if (g)
   {
      g->small = (uint8_t*)calloc((size_t)PXC_W * PXC_MAX_H, 1);
      g->dots = g->sparks = g->own_sound = g->siren = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   pm *g = (pm*)state;
   if (g)
      free(g->small);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   pm *g = (pm*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->maze      = px_kit_pick(get, OPT_MAZE, mazes);
   g->dots      = px_kit_on(get, OPT_WAFERS);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->siren     = px_kit_on(get, OPT_SIREN);
}

const px_game px_game_pac_man = {
   "Pac-Man", md5, fx, options, create, destroy, reset, configure, frame, sound, who
};
