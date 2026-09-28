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
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define MAZE_TOP   11
#define MAZE_END   178    /* the row after its last */
#define LIVES_TOP  193

#define GHOSTS 4

#define RAM_FRAME   0
#define RAM_PAC_X   49
#define RAM_GHOST_X 50
#define RAM_PAC_Y   54
#define RAM_GHOST_Y 55
#define RAM_WAFERS  76
#define WAFERS      126    /* in a maze, by the count of them on the screen */

#define SHAPE_EYES 0x88DEB1F3u

/* What the ghosts are about, by the colour of the one drawn. */
enum { MODE_IDLE = 0, MODE_CHASE, MODE_SCARED, MODE_WARNING };

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
   uint32_t ticks;                  /* counts the frames heard */

   /* The picture. */
   unsigned mode;                   /* MODE_* */
   bool eyes[GHOSTS];               /* a ghost was a pair of eyes when it was drawn last */
   unsigned dying;                  /* frames since Pac-Man began to die; 0: he does not */
   uint8_t *small;                  /* where the wafers are: an entry a captured pixel */

   /* The sounds. */
   px_kit_tia tia;                  /* the game's two voices */
   unsigned heard0;                 /* what voice 0 played in the frame before: SOUND_* */
   unsigned sweep;                  /* SOUND_CAUGHT or SOUND_EATEN while voice 0 plays it */
   unsigned silent0;                /* frames for which voice 0 has been silent */
   bool waka;                       /* which of the two sounds of eating is next */
   unsigned caught_for;             /* frames since Pac-Man was caught; 0: he was not */
   unsigned warble, hum;            /* the voices at the synth that go on: 0 for none */
   unsigned moved;                  /* frames since a ghost last moved */
   uint8_t ghost_at[2 * GHOSTS];    /* where memory had the ghosts */
   int pac_at;                      /* Pac-Man's column, for where a sound is; -1: not known */
} pm;

static const uint32_t ghost_colors[GHOSTS] = { 0xFF2A1A, 0xFFA8E0, 0x2AE8F0, 0xFFA030 };

#define RGB_PAC     0xFFE81Au
#define RGB_SCARED  0x2438FFu
#define RGB_PALE    0xF2F2F8u
#define RGB_EYES    0xE4EAFFu
#define RGB_WAFER   0xFFB897u
#define RGB_WALL    0x2A48FFu
#define RGB_WITHIN  0x05082Cu
#define RGB_SCORE   0xF0F0F0u

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
      return MODE_SCARED;
   if (in->color == 0x5A)
      return MODE_WARNING;
   return light == 0x0C && hue >= 0x0C ? MODE_CHASE : MODE_IDLE;
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

/* The wafers as dots: two pixels by two in the middle of each. Two wafers side by side, as
 * in the middle of the maze where its halves meet, are one. */
static void draw_wafers(pm *g, px_scene *s)
{
   const uint32_t *palette = s->frame->palette;
   const struct pxc_frame *f = s->frame;

   for (unsigned y = MAZE_TOP; y < MAZE_END && y < f->height; y++)
   {
      const uint8_t *row = g->small + (size_t)y * PXC_W;
      for (unsigned x = 0; x < PXC_W; )
      {
         unsigned end = x;
         uint32_t rgb;
         while (end < PXC_W && row[end])
            end++;
         if (end == x)
         {
            x++;
            continue;
         }
         rgb = g->colors == COLORS_ARCADE ? RGB_WAFER
               : palette[f->color[PXC_L_PF][(size_t)y * PXC_W + x]] & 0xFFFFFFu;

         for (unsigned u = x; u < end; u++)
         {
            const size_t i = (size_t)y * PXC_W + u;
            /* In the middle of what there is of it, or of each four pixels of it. */
            const unsigned len = end - x, cell = len == 8 ? 8 : 4, at = (u - x) % cell;
            const bool dot = at == cell / 2 - 1 || at == cell / 2;
            if (PX_KEY_CLS(s->top[i]) != PX_CLS_PF)
               continue;
            if (!g->dots)
               s->top[i] = PX_KEY(PX_CLS_PF, rgb);
            else if (dot)
            {
               s->top[i]    = PX_KEY(PX_CLS_SPRITE, rgb);
               s->sprite[i] = 0xFF000000u | rgb;
            }
            else
               s->top[i] = PX_KEY(PX_CLS_BK, s->bk[i]);
         }
         x = end;
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
         px_kit_outline(s, MAZE_TOP, MAZE_END, RGB_WALL, RGB_WITHIN, px_rgb_scale(RGB_WALL, 120),
               g->small);
      else
         px_kit_outline(s, MAZE_TOP, MAZE_END, RGB_WALL, RGB_WALL, 0, g->small);
      px_kit_playfield(s, LIVES_TOP, s->frame->height, RGB_PAC);
   }
   if (g->dots || g->colors == COLORS_ARCADE)
      draw_wafers(g, s);
}

static uint32_t ghost_color(const pm *g, const px_instance *in, uint32_t *eyes)
{
   if (is_eyes(in))
   {
      *eyes = 0;
      return RGB_EYES;
   }
   switch (g->mode)
   {
      case MODE_SCARED:
         *eyes = 0xFFC4A8;
         return RGB_SCARED;
      case MODE_WARNING:
         /* Pale and blue in turns, faster than the eye follows the game's purple. */
         if ((g->frame >> 3) & 1)
         {
            *eyes = 0xFF3030;
            return RGB_PALE;
         }
         *eyes = 0xFFC4A8;
         return RGB_SCARED;
      default:
         *eyes = 0xF8F8FF;
         return ghost_colors[in->group & 3];
   }
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
            px_scene_tint(s, in, RGB_SCORE);
         continue;
      }
      if (g->colors != COLORS_ARCADE)
         continue;
      switch (in->role)
      {
         case PX_ROLE_ENEMY:
         {
            uint32_t eyes = 0;
            px_scene_tint(s, in, ghost_color(g, in, &eyes));
            if (eyes)
               px_kit_fill_holes(s, in, eyes);
            break;
         }
         case PX_ROLE_PLAYER:
            px_scene_tint(s, in, RGB_PAC);
            break;
         case PX_ROLE_BONUS:
            /* The power pills beat. */
            px_scene_tint(s, in, px_rgb_scale(RGB_WAFER, 150 + (px_kit_wave(g->frame * 10) * 105 >> 8)));
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
            px_scene_burst(s, in->x + 4, in->y + 3, ghost_colors[which], 26, 340);
            px_scene_burst(s, in->x + 4, in->y + 3, RGB_SCARED, 14, 220);
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
            px_scene_burst(s, in->x + 4, in->y, RGB_PAC, 36, 400);
            g->dying = 1000;
         }
      }
   }
   g->dying = dies ? g->dying + 1 : 0;

   if (g->mode == MODE_SCARED && mode_before == MODE_CHASE && g->sparks)
      px_scene_flash(s, RGB_SCARED, 80);
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

/* A note of the game's tune: the melody two octaves down, where it is a tune and not a
 * whistle, and the second voice below it. */
static void play_note(px_sound *s, unsigned voice, float hz)
{
   const float f = px_kit_tune(hz / 4.0f);
   px_tone lead[2] = {
      /* wave            freq  to  glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SQUARE,   0,   0,  0,     0.004f, 0.10f, 0.30f, 0.20f, 2600, 900, 5.5f, 0.004f },
      { PX_WAVE_TRIANGLE, 0,   0,  0,     0.004f, 0.12f, 0.36f, 0.42f, 0,    0,   0,    0 }
   };
   px_tone bass[2] = {
      { PX_WAVE_TRIANGLE, 0,   0,  0,     0.006f, 0.14f, 0.34f, 0.50f, 0,    0,   0,    0 },
      { PX_WAVE_SAW,      0,   0,  0,     0.006f, 0.10f, 0.26f, 0.12f, 700,  300, 0,    0 }
   };
   if (f <= 0.0f)
      return;
   if (voice == 0)
   {
      lead[0].freq = f;
      lead[1].freq = f * 2.0f;
      px_kit_play(s, lead, 2, 0.25f);
   }
   else
   {
      bass[0].freq = f;
      bass[1].freq = f;
      px_kit_play(s, bass, 2, -0.25f);
   }
}

/* Eating: up and down in turns. */
static void play_wafer(pm *g, px_sound *s)
{
   static const px_tone up[2] = {
      { PX_WAVE_SQUARE,   300, 640, 0.07f, 0.002f, 0.03f, 0.08f, 0.26f, 2400, 1200, 0, 0 },
      { PX_WAVE_TRIANGLE, 150, 320, 0.07f, 0.002f, 0.03f, 0.08f, 0.40f, 0, 0, 0, 0 }
   };
   static const px_tone down[2] = {
      { PX_WAVE_SQUARE,   640, 300, 0.07f, 0.002f, 0.03f, 0.08f, 0.26f, 2400, 1200, 0, 0 },
      { PX_WAVE_TRIANGLE, 320, 150, 0.07f, 0.002f, 0.03f, 0.08f, 0.40f, 0, 0, 0, 0 }
   };
   px_kit_play(s, g->waka ? down : up, 2, px_kit_pan(g->pac_at));
   g->waka = !g->waka;
   px_sound_rumble(s, 0, 7000, 2);
}

static void play_pill(pm *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SAW,      196, 784, 0.22f, 0.003f, 0.10f, 0.30f, 0.30f, 1200, 5000, 0, 0 },
      { PX_WAVE_SQUARE,   392, 1568, 0.22f, 0.003f, 0.08f, 0.24f, 0.14f, 2000, 6000, 0, 0 },
      { PX_WAVE_SINE,      98,  49, 0.30f, 0.002f, 0.10f, 0.40f, 0.70f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->pac_at));
   px_sound_rumble(s, 26000, 20000, 14);
}

static void play_eaten(pm *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SQUARE,   330, 1760, 0.40f, 0.002f, 0.30f, 0.22f, 0.24f, 1500, 6000, 14.0f, 0.03f },
      { PX_WAVE_TRIANGLE, 165,  880, 0.40f, 0.002f, 0.30f, 0.22f, 0.44f, 0, 0, 14.0f, 0.03f },
      { PX_WAVE_NOISE,   6000,  900, 0.10f, 0,      0.01f, 0.12f, 0.24f, 5000, 800, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->pac_at));
   px_sound_rumble(s, 30000, 36000, 12);
}

/* Pac-Man caught: down and away, and the burst at the end of it. */
static void play_caught(pm *g, px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_TRIANGLE, 988, 110, 1.45f, 0.004f, 1.30f, 0.30f, 0.60f, 0, 0, 7.0f, 0.06f },
      { PX_WAVE_SQUARE,   494,  55, 1.45f, 0.004f, 1.30f, 0.30f, 0.16f, 1800, 300, 7.0f, 0.06f }
   };
   px_kit_play(s, p, 2, px_kit_pan(g->pac_at));
   px_sound_rumble(s, 52000, 30000, 30);
}

static void play_burst(pm *g, px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_NOISE, 5000, 500, 0.20f, 0,      0.02f, 0.28f, 0.40f, 4000, 400, 0, 0 },
      { PX_WAVE_SINE,   160,  50, 0.14f, 0.001f, 0.02f, 0.24f, 0.70f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 2, px_kit_pan(g->pac_at));
   px_sound_rumble(s, 65535, 40000, 16);
}

static void stop(px_sound *s, unsigned *voice, float seconds)
{
   if (*voice)
      px_synth_stop(s->synth, *voice, seconds);
   *voice = 0;
}

/* While the ghosts can be eaten: a note that climbs with the game's own, over and over, and
 * faster when that is about to end, which the game passes over in silence. */
static void play_warble(pm *g, px_sound *s, bool on)
{
   static const px_tone p = { PX_WAVE_SQUARE, 220, 0, 0, 0.02f, 0, 0, 1.0f, 1500, 0, 0, 0 };
   float climb;

   if (!on)
   {
      stop(s, &g->warble, 0.08f);
      return;
   }
   if (voice1_warbles(&g->tia))
      climb = (float)g->tia.pitch[1] / 13.0f;
   else
      climb = (float)(g->ticks % 12) / 12.0f;
   if (!px_synth_move(s->synth, g->warble, 0.0f, 0.13f, 196.0f * (1.0f + climb)))
      g->warble = px_synth_play(s->synth, &p, 0.0f, 0.13f);
}

/* The chase: a siren that goes up and down, higher and faster as the wafers get fewer. */
static void play_siren(pm *g, px_sound *s, bool on)
{
   static const px_tone p = { PX_WAVE_TRIANGLE, 330, 0, 0, 0.25f, 0, 0, 1.0f, 1400, 0, 0, 0 };
   const int eaten = px_kit_ram(s->ram, s->ram_size, RAM_WAFERS);
   float few, swing, hz;

   if (!on)
   {
      stop(s, &g->hum, 0.25f);
      return;
   }
   few   = eaten < 0 ? 0.0f : eaten >= WAFERS ? 1.0f : (float)eaten / (float)WAFERS;
   swing = (float)px_kit_wave(g->ticks * (unsigned)(5 + (int)(few * 6.0f))) / 254.0f;
   hz    = (300.0f + 220.0f * few) * (1.0f + 0.45f * swing);
   if (!px_synth_move(s->synth, g->hum, 0.0f, 0.085f, hz))
      g->hum = px_synth_play(s->synth, &p, 0.0f, 0.085f);
}

/* Whether the game is on: the ghosts move. They stand while the tune plays and while
 * Pac-Man dies. */
static bool ghosts_move(pm *g, const px_sound *s)
{
   bool moved = false;
   for (unsigned i = 0; i < 2 * GHOSTS; i++)
   {
      const unsigned at = i < GHOSTS ? RAM_GHOST_X + i : RAM_GHOST_Y + i - GHOSTS;
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

   g->ticks++;
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
         if (g->own_sound) play_caught(g, s);
         else              px_sound_rumble(s, 52000, 30000, 30);
      }
      else if (!g->sweep && g->tia.volume[0] == 15 && g->tia.pitch[0] == 16)
      {
         g->sweep = SOUND_EATEN;
         if (g->own_sound) play_eaten(g, s);
         else              px_sound_rumble(s, 30000, 36000, 12);
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
            play_note(s, 0, px_kit_tia_hz(4, g->tia.pitch[0]));
         if (g->heard0 != SOUND_TUNE || g->tia.pitch[1] != g->tia.was_pitch[1])
            play_note(s, 1, px_kit_tia_hz(4, g->tia.pitch[1]));
         break;
      case SOUND_WAFER:
         if (g->heard0 != SOUND_WAFER || px_kit_tia_louder(&g->tia, 0))
         {
            if (g->own_sound) play_wafer(g, s);
            else              px_sound_rumble(s, 0, 7000, 2);
         }
         break;
      case SOUND_PILL:
         if (g->heard0 != SOUND_PILL)
         {
            if (g->own_sound) play_pill(g, s);
            else              px_sound_rumble(s, 26000, 20000, 14);
         }
         break;
      default:
         break;
   }
   g->heard0 = heard;

   /* The burst when the last of Pac-Man is gone: 118 frames after he was caught. */
   if (g->caught_for && ++g->caught_for == 118)
   {
      if (g->own_sound) play_burst(g, s);
      else              px_sound_rumble(s, 65535, 40000, 16);
   }
   if (g->caught_for > 118 || (g->caught_for && heard != SOUND_CAUGHT && g->silent0 > 6))
      g->caught_for = 0;

   running = ghosts_move(g, s) && heard != SOUND_TUNE && heard != SOUND_CAUGHT;
   scared  = voice1_warbles(&g->tia) || (running && g->mode == MODE_WARNING);
   play_warble(g, s, g->own_sound && scared);
   play_siren(g, s, g->own_sound && g->siren && running && !scared && g->mode == MODE_CHASE);

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
   g->mode   = MODE_IDLE;
   g->dying  = 0;
   g->heard0 = g->sweep = SOUND_NONE;
   g->silent0 = g->caught_for = 0;
   g->moved  = 1000;
   g->warble = g->hum = 0;
   g->waka   = false;
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
