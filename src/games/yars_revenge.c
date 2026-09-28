/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Yars' Revenge (Atari, 1982).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   playfield      two things in turns, one a frame: the neutral zone, a band of the game's
 *                  own code shown as colours in columns 52..80 (rows 14..206), and the shield
 *                  of cells around the Qotile in columns 128..160, in one colour. Neither is
 *                  there in the frame the other is: the game flickers them at 30 Hz.
 *   player 0       the Qotile at column 150, 8 by 18, its colour going round the palette;
 *                  it becomes the Swirl, 8 by 16 in three shapes, which spins and flies
 *   player 1       Yar, 8 by 16 in several shapes (its wings), and smaller while it dies
 *   missile 0      the Destroyer Missile, 4 by 2, which follows Yar everywhere
 *   missile 1      Yar's energy missile, 1 by 2
 *   ball           the Zorlon Cannon, 8 by 8 (4 wide in flight), in stripes of changing
 *                  colours; it waits at the left edge in Yar's row, drawn in the frames of
 *                  the neutral zone
 *   between lives  the score and the lives in digits of 8 by 7, copies of both players in
 *                  rows 59 and 85; nothing else is drawn
 *   the Qotile destroyed: the background and the playfield fill the screen with bands of
 *                  colour, the whole picture shakes, then shrinks to a band
 *
 * Of its memory ($80 is 0): 1..16 hold the shield's cells, 31 Yar's row and 32 its column,
 * 42 the Qotile's row, 47 the Destroyer Missile's column. 34 is 3 while Yar lives and 0 from
 * the moment it is hit until the next life begins (and for one frame now and then, when
 * memory is read in the middle of something). 40 is 0, and counts from $C0 for as long as
 * the Qotile's destruction is shown. 99 has the lives in its upper four bits.
 *
 * Found with the tools of tools/2600 from runs of the game, the Qotile's destruction staged
 * by clearing the shield (bytes 1..16) and moving Yar out of the Cannon's way.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "c5930d0e8cdae3e037349bfa08e871be",   /* Yars' Revenge (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "60",
   "reverb", "room",
   NULL
};

#define OPT_COLORS   "proteus_yr_colors"
#define OPT_STEADY   "proteus_yr_steady"
#define OPT_BACKDROP "proteus_yr_backdrop"
#define OPT_SPARKS   "proteus_yr_sparks"
#define OPT_SOUND    "proteus_yr_sound"
#define OPT_HUM      "proteus_yr_hum"

static const char *const colors[] = { "proteus", "Proteus's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "nebula", "Nebula", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_PROTEUS = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Embers for the shield, a glow for the neutral zone, hot colours for the missiles and the Cannon, or the game's own colours.",
     "proteus", colors },
   { OPT_STEADY, "Steady shield and zone",
     "The game shows the shield and the neutral zone in turns, one a frame. Show both in every frame.",
     "enabled", px_kit_toggle },
   { OPT_BACKDROP, "Backdrop",
     "What is behind the game where its background is black.", "nebula", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks where a cell of the shield is eaten or shot, where Yar is hit, and a blast when the Qotile is destroyed.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the cells, the Swirl, Yar's death and the Qotile's destruction, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_HUM, "Hum",
     "The game's hum and its beat played by Proteus, deeper. Needs the sounds to be Proteus's.",
     "proteus", px_kit_sounds },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define RAM_SHIELD    1      /* 16 bytes: the cells, a bit each */
#define RAM_ALIVE     34     /* 3 while Yar lives, 0 while it dies */
#define RAM_DESTROYED 40     /* not 0 while the Qotile's destruction is shown */

#define ZONE_FROM   32       /* columns of the neutral zone: it is at 52..80 */
#define SHIELD_FROM 104      /* columns of the shield: it is at 128..160 */
#define QOTILE_X    140      /* the Qotile is at 150; the Swirl leaves from there */

#define STARS 200

/* The two things the game draws with the playfield, in turns. */
enum { PART_ZONE = 0, PART_SHIELD, PARTS };

/* Which Yar's things are, in the group of their instance. */
enum { GROUP_QOTILE = 0, GROUP_SWIRL = 1, GROUP_MISSILE = 0, GROUP_CANNON = 1 };

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   uint16_t x, y;
   uint8_t size, phase, pace;
   uint32_t rgb;
} star;

typedef struct
{
   /* The options. */
   unsigned colors;
   bool steady, backdrop, sparks, own_sound, hum;

   uint32_t frame;          /* counts the frames that advance */

   /* The playfield of the part the frame did not draw, as the frame before drew it:
    * PX_KEY()s, 0 where there was none. */
   uint32_t held[PXC_W * PXC_MAX_H];
   unsigned held_height;
   unsigned age[PARTS];     /* frames since each part was drawn; 0: in this one */

   int alive, destroyed;    /* bytes 34 and 40 as the frame before had them; -1: not known */
   unsigned dying;          /* frames byte 34 has been 0 */
   int cells;               /* of the shield, as memory had them; -1: not known */
   unsigned cell_due;       /* frames left to find where a cell memory lost was */
   int yar_x, yar_y;        /* where Yar was last seen; -1: not */
   int qotile_x, qotile_y;

   /* The sounds. */
   px_kit_tia tia;
   int yar_at, enemy_at;    /* columns, for where a sound is; -1: not known */
   unsigned charge;         /* the Swirl's charge at the synth; 0: none */
   unsigned hum_low, hum_high;

   px_kit_canvas sky;       /* the backdrop with its stars */
   star stars[STARS];
   uint32_t seed;
} yars;

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

/* Deep space: a violet nebula about the neutral zone and a teal one to the right, faint,
 * and stars, some of which twinkle. */
static void paint_sky(yars *g)
{
   static const struct { int x, y, r; uint32_t rgb; } clouds[4] = {
      { 30, 40, 34, 0x1C0A30 }, { 70, 65, 40, 0x062026 }, { 50, 15, 26, 0x14081E },
      { 88, 28, 22, 0x0A1426 }
   };
   const unsigned w = g->sky.w, h = g->sky.h;
   uint32_t *p = g->sky.pixels;

   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++)
      {
         uint32_t rgb = 0x020308;
         for (unsigned k = 0; k < 4; k++)
         {
            /* In hundredths of the picture, wide as they are high on a screen. */
            int cx = (int)(x * 100 / w) - clouds[k].x, cy = ((int)(y * 100 / h) - clouds[k].y) * 3 / 4;
            int d2 = cx * cx + cy * cy, r2 = clouds[k].r * clouds[k].r;
            if (d2 < r2)
            {
               unsigned t = (unsigned)((r2 - d2) * 256 / r2);
               rgb = px_rgb_add(rgb, px_rgb_scale(clouds[k].rgb, t * t >> 8));
            }
         }
         p[(size_t)y * w + x] = rgb;
      }

   g->seed = 0x7A25u;
   for (unsigned i = 0; i < STARS; i++)
   {
      star *s = &g->stars[i];
      unsigned kind = px_kit_chance(&g->seed) % 16;
      s->size  = (uint8_t)(kind < 11 ? 1 : 2);
      s->x     = (uint16_t)(px_kit_chance(&g->seed) % (w > 2 ? w - 2 : 1));
      s->y     = (uint16_t)(px_kit_chance(&g->seed) % (h > 2 ? h - 2 : 1));
      s->phase = (uint8_t)px_kit_chance(&g->seed);
      s->pace  = (uint8_t)(1 + px_kit_chance(&g->seed) % 4);
      switch (px_kit_chance(&g->seed) % 5)
      {
         case 0:  s->rgb = 0xFFD0E8; break;
         case 1:  s->rgb = 0xB8E8FF; break;
         default: s->rgb = 0xE8ECFF; break;
      }
   }
}

static void put_star(const yars *g, uint32_t *out, const star *s, uint32_t rgb)
{
   for (unsigned t = 0; t < s->size; t++)
      for (unsigned u = 0; u < s->size; u++)
      {
         size_t i = (size_t)(s->y + t) * g->sky.w + s->x + u;
         out[i] = px_rgb_add(g->sky.pixels[i], rgb);
      }
}

static void paint_backdrop(yars *g, px_scene *s)
{
   if (px_kit_canvas_fit(&g->sky, s))
   {
      paint_sky(g);
      memcpy(s->backdrop, g->sky.pixels, (size_t)s->w * s->h * sizeof(uint32_t));
   }
   else if (!g->sky.pixels)
      return;
   s->backdrop_on = true;
   if (!s->advance)
      return;
   for (unsigned i = 0; i < STARS; i++)
   {
      const star *st = &g->stars[i];
      unsigned wave = px_kit_wave(st->phase + g->frame * st->pace);
      unsigned bright = st->size == 2 ? 110 + (wave * 120 >> 8) : 40 + (wave * 70 >> 8);
      put_star(g, s->backdrop, st, px_rgb_scale(st->rgb, bright));
   }
}

/* ---------------------------------------------------------------------------
 * The playfield: the neutral zone and the shield
 * ------------------------------------------------------------------------- */

static unsigned part_of(unsigned x)
{
   return x >= SHIELD_FROM ? PART_SHIELD : PART_ZONE;
}

/* Whether the shield had a cell at a row and column when it was drawn before. */
static bool held_at(const yars *g, int y, int h, unsigned x)
{
   return y >= 0 && y < h && g->held[(size_t)y * PXC_W + x];
}

/* A cell of the shield that was there when it was drawn before and is not now: eaten or
 * shot. The shield moves up and down with the Qotile, so it is compared where it matches
 * best, a few rows up or down. */
static void lost_cell(const yars *g, const px_scene *s, int *lost_x, int *lost_y)
{
   const int h = (int)s->frame->height;
   int best = -1, best_dy = 0;
   unsigned lost = 0, gained = 0, sx = 0, sy = 0;

   for (int dy = -4; dy <= 4; dy++)
   {
      int miss = 0;
      for (int y = 0; y < h; y++)
         for (unsigned x = SHIELD_FROM; x < PXC_W; x++)
            miss += held_at(g, y - dy, h, x)
                  != (PX_KEY_CLS(s->top[(size_t)y * PXC_W + x]) == PX_CLS_PF);
      if (best < 0 || miss < best)
      {
         best    = miss;
         best_dy = dy;
      }
   }
   for (int y = 0; y < h; y++)
      for (unsigned x = SHIELD_FROM; x < PXC_W; x++)
      {
         const bool was = held_at(g, y - best_dy, h, x);
         const unsigned cls = PX_KEY_CLS(s->top[(size_t)y * PXC_W + x]);
         const bool now = cls == PX_CLS_PF;
         /* What an object covers is not known to be gone. */
         if (was && cls == PX_CLS_BK)
         {
            lost++;
            sx += x;
            sy += (unsigned)y;
         }
         else if (now && !was)
            gained++;
      }
   /* A cell is 4 columns by 8 rows. */
   if (lost >= 8 && lost <= 48 && gained <= 8)
   {
      *lost_x = (int)(sx / lost);
      *lost_y = (int)(sy / lost);
   }
}

/* Keeps what the frame drew of each part, and draws from what was kept the part it did
 * not. A cell of the shield that was there and is not was eaten or shot: where it was is
 * given back in *lost_x, *lost_y, else -1. */
static void steady_playfield(yars *g, px_scene *s, int *lost_x, int *lost_y)
{
   const unsigned h = s->frame->height;
   unsigned drawn[PARTS] = { 0, 0 };
   *lost_x = *lost_y = -1;

   for (unsigned y = 0; y < h; y++)
      for (unsigned x = ZONE_FROM; x < PXC_W; x++)
         if (PX_KEY_CLS(s->top[(size_t)y * PXC_W + x]) == PX_CLS_PF)
            drawn[part_of(x)]++;

   if (h != g->held_height)
   {
      memset(g->held, 0, sizeof(g->held));
      g->held_height = h;
      g->age[PART_ZONE] = g->age[PART_SHIELD] = 99;
   }

   if (s->advance)
      for (unsigned p = 0; p < PARTS; p++)
      {
         const unsigned from = p == PART_ZONE ? ZONE_FROM : SHIELD_FROM;
         const unsigned to   = p == PART_ZONE ? SHIELD_FROM : PXC_W;
         if (!drawn[p])
         {
            if (g->age[p] < 99)
               g->age[p]++;
            continue;
         }
         if (p == PART_SHIELD && g->age[p] <= 2)
            lost_cell(g, s, lost_x, lost_y);
         for (unsigned y = 0; y < h; y++)
            for (unsigned x = from; x < to; x++)
            {
               const size_t i = (size_t)y * PXC_W + x;
               g->held[i] = PX_KEY_CLS(s->top[i]) == PX_CLS_PF ? s->top[i] : 0;
            }
         g->age[p] = 0;
      }

   if (!g->steady)
      return;
   /* The part not drawn now, as it was a frame ago: only while the game draws the two in
    * turns. */
   for (unsigned p = 0; p < PARTS; p++)
   {
      const unsigned from = p == PART_ZONE ? ZONE_FROM : SHIELD_FROM;
      const unsigned to   = p == PART_ZONE ? SHIELD_FROM : PXC_W;
      if (drawn[p] || g->age[p] != 1 || !drawn[1 - p])
         continue;
      for (unsigned y = 0; y < h; y++)
         for (unsigned x = from; x < to; x++)
         {
            const size_t i = (size_t)y * PXC_W + x;
            if (g->held[i] && PX_KEY_CLS(s->top[i]) == PX_CLS_BK)
               s->top[i] = g->held[i];
         }
   }
}

/* The shield as embers, darker at its edges; the neutral zone glows in its own colours. */
static void color_playfield(yars *g, px_scene *s)
{
   const unsigned h = s->frame->height;
   if (g->colors == COLORS_ORIGINAL)
      return;
   for (unsigned y = 0; y < h; y++)
   {
      /* Of 256: how near the middle of the picture, where the Qotile mostly is. */
      int d = (int)y * 512 / (int)h - 256;
      unsigned mid = (unsigned)(256 - (d < 0 ? -d : d));
      uint32_t ember = px_rgb_mix(0x8A200E, 0xE86424, mid);
      for (unsigned x = ZONE_FROM; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         if (PX_KEY_CLS(s->top[i]) != PX_CLS_PF)
            continue;
         if (part_of(x) == PART_SHIELD)
         {
            /* Every cell a little apart from its neighbours. */
            uint32_t rgb = ((x / 4 + y / 8) & 1) ? ember : px_rgb_scale(ember, 216);
            s->top[i] = PX_KEY(PX_CLS_PF, rgb);
            s->light[i] = 0xFF000000u | px_rgb_scale(s->top[i] & 0xFFFFFFu, 80);
         }
         else
            s->light[i] = 0xFF000000u | px_rgb_scale(s->top[i] & 0xFFFFFFu, 90);
      }
   }
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

/* What an object is in the game, from what it is drawn with and its size. */
static unsigned role_of(const px_instance *in, uint8_t *group)
{
   *group = 0;
   switch (in->cls)
   {
      case PXC_L_P0:
         if (in->h <= 8)
            return PX_ROLE_HUD;
         *group = in->h >= 18 && in->x >= QOTILE_X ? GROUP_QOTILE : GROUP_SWIRL;
         return PX_ROLE_ENEMY;
      case PXC_L_P1:
         return in->h <= 8 && in->copy > 1 ? PX_ROLE_HUD : PX_ROLE_PLAYER;
      case PXC_L_M0:
         return PX_ROLE_BOMB;
      case PXC_L_M1:
         return PX_ROLE_SHOT;
      case PXC_L_BL:
         *group = GROUP_CANNON;
         return PX_ROLE_SHOT;
      default:
         return PX_ROLE_NONE;
   }
}

static void color_objects(yars *g, px_scene *s, bool exploding)
{
   px_objects *o = s->objects;
   const uint32_t *palette = s->frame->palette;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      const uint32_t original = palette[in->color] & 0xFFFFFFu;
      uint8_t group;
      in->role  = (uint8_t)role_of(in, &group);
      in->group = group;
      if (in->role == PX_ROLE_PLAYER && !in->ghost)
      {
         g->yar_x = in->x + 4;
         g->yar_y = in->y + in->h / 2;
      }
      else if (in->role == PX_ROLE_ENEMY && in->group == GROUP_QOTILE && !in->ghost)
      {
         g->qotile_x = in->x + 4;
         g->qotile_y = in->y + in->h / 2;
      }
      /* The game's own colours are the game's own picture: nothing glows more either. */
      if (exploding || g->colors == COLORS_ORIGINAL)
         continue;
      switch (in->role)
      {
         case PX_ROLE_ENEMY:
            if (in->group == GROUP_SWIRL)
            {
               /* The Swirl spins through hot colours. */
               static const uint32_t hot[4] = { 0xFF3070, 0xFFB020, 0xFFF0F0, 0xFF60D0 };
               px_scene_tint(s, in, hot[(g->frame / 3) & 3]);
               px_scene_energy(s, in, true);
            }
            else
               px_scene_tint(s, in, px_rgb_mix(original, 0xFFFFFF, 64));
            break;
         case PX_ROLE_PLAYER:
            {
               /* Its wings catch the light as they beat. */
               unsigned wave = px_kit_wave(g->frame * 16);
               px_scene_tint(s, in, px_rgb_mix(0xA878FF, 0x78D8FF, wave * 3 >> 3));
            }
            break;
         case PX_ROLE_BOMB:
            px_scene_tint(s, in, 0xFF5030);
            px_scene_energy(s, in, true);
            break;
         case PX_ROLE_SHOT:
            if (in->group == GROUP_CANNON)
            {
               /* It glows when it flies, not while it waits at the edge. */
               const px_obj_track *t = px_objects_track(o, in->track);
               unsigned wave = px_kit_wave(g->frame * 20);
               px_scene_tint(s, in, px_rgb_mix(0xE06A18, 0xFFE890, wave));
               px_scene_energy(s, in, t && t->vx != 0);
            }
            else
            {
               px_scene_tint(s, in, 0xE0FFFF);
               px_scene_energy(s, in, true);
            }
            break;
         default:
            break;
      }
   }
}

/* The cells of the shield, as memory has them; -1 without memory. */
static int count_cells(const px_scene *s)
{
   int n = 0;
   for (unsigned i = RAM_SHIELD; i < RAM_SHIELD + 16; i++)
   {
      int b = px_kit_ram(s->ram, s->ram_size, i);
      if (b < 0)
         return -1;
      for (; b; b &= b - 1)
         n++;
   }
   return n;
}

static void frame(void *state, px_scene *s)
{
   yars *g = (yars*)state;
   const int alive = px_kit_ram(s->ram, s->ram_size, RAM_ALIVE);
   const int destroyed = px_kit_ram(s->ram, s->ram_size, RAM_DESTROYED);
   const bool exploding = destroyed > 0;
   int lost_x, lost_y, cells;

   if (s->advance)
      g->frame++;

   color_objects(g, s, exploding);
   if (!exploding)
   {
      steady_playfield(g, s, &lost_x, &lost_y);
      color_playfield(g, s);
   }
   else
   {
      lost_x = lost_y = -1;
      g->age[PART_ZONE] = g->age[PART_SHIELD] = 99;
   }

   cells = count_cells(s);
   if (s->advance && g->sparks)
   {
      /* Memory says that a cell is gone; the picture, drawn a frame later, where. Without
       * memory, the picture alone says it. */
      if (cells >= 0 && g->cells >= 0 && cells < g->cells)
         g->cell_due = 4;
      if (lost_x >= 0 && (cells < 0 || g->cell_due))
      {
         px_scene_burst(s, lost_x, lost_y, 0xFF9A40, 16, 260);
         g->cell_due = 0;
      }
      else if (g->cell_due && --g->cell_due == 0 && g->yar_x >= 0)
         px_scene_burst(s, g->yar_x, g->yar_y, 0xFF9A40, 16, 260);
      /* Yar is hit: the byte stays 0 for more than one frame. */
      if (alive == 0 && g->dying == 1 && g->yar_x >= 0)
      {
         px_scene_burst(s, g->yar_x, g->yar_y, 0xC8A0FF, 36, 380);
         px_scene_flash(s, 0xFF3060, 90);
      }
      if (destroyed > 0 && g->destroyed == 0)
      {
         int x = g->qotile_x >= 0 ? g->qotile_x : 154, y = g->qotile_y >= 0 ? g->qotile_y : 110;
         px_scene_burst(s, x, y, 0xFFE0A0, 120, 560);
         px_scene_burst(s, x, y, 0xFF4080, 60, 300);
         px_scene_flash(s, 0xFFF0D0, 180);
      }
   }

   if (s->advance)
   {
      g->dying     = alive == 0 ? g->dying + 1 : 0;
      g->alive     = alive;
      g->destroyed = destroyed;
      g->cells     = cells;
   }

   if (g->backdrop)
      paint_backdrop(g, s);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * What the game plays, as the frame leaves the voices:
 *
 *   voice 1   the hum under everything: waveform 3 at pitch 10, volume 3; pitch 4 and 12
 *             now and then. Every 60 frames a beat: waveform 12 at pitch 12, volume 12, for
 *             one frame.
 *   voice 0   a cell of the shield eaten or shot: waveform 10 at pitch 3, volume 10, one
 *             frame
 *   voice 0   the Swirl gathers itself: waveform 5, the pitch rising from 17 through 31 and
 *             round again, volume 5, some three seconds; it flies: waveform 8, the pitch
 *             rising the same way, volume 12, a second
 *   voice 1   Yar hit: waveform 5, pitch 28..31 in turns, volume 13; then waveform 13 at
 *             pitch 3, volume 5 and 15 in turns, and waveform 8, the pitch from 13 or 15 up
 *             through 26, as it goes
 *   voice 0   the Qotile destroyed: waveform 13, the pitch 4, 8 .. 28 round and round,
 *             volume 13, half a second; then waveform 8 in long rising sweeps, volume 15
 *             falling, while the picture shakes
 *   voice 0   waveform 14 at pitch 7, the volume rising 2..14 over and over: what it tells
 *             was not found out, and it is heard as the game plays it
 *   voice 1   waveform 4 at pitch 4 between the hum while Yar touches the shield, and
 *             waveform 13 at pitch 13 while the Zorlon Cannon waits: heard as the game
 *             plays them
 *
 * What Proteus plays itself, the game's voice is silent for; the rest is the game's.
 * ------------------------------------------------------------------------- */

enum { V0_NONE = 0, V0_CELL, V0_CHARGE, V0_LAUNCH, V0_BLAST, V0_OTHER };
enum { V1_NONE = 0, V1_HUM, V1_BEAT, V1_HIT, V1_DIE, V1_OTHER };

static unsigned voice0_plays(const px_kit_tia *t, bool exploding)
{
   if (!t->volume[0])
      return V0_NONE;
   switch (t->wave[0])
   {
      case 10: return t->pitch[0] == 3 ? V0_CELL : V0_OTHER;
      case 5:  return exploding ? V0_OTHER : V0_CHARGE;
      case 8:  return exploding ? V0_BLAST : V0_LAUNCH;
      case 13: return V0_BLAST;
      default: return V0_OTHER;
   }
}

static unsigned voice1_plays(const px_kit_tia *t)
{
   if (!t->volume[1])
      return V1_NONE;
   switch (t->wave[1])
   {
      case 3:  return t->volume[1] <= 3 ? V1_HUM : V1_OTHER;
      case 12: return t->pitch[1] == 12 ? V1_BEAT : V1_OTHER;
      case 5:  return t->pitch[1] >= 28 ? V1_HIT : V1_OTHER;
      case 8:  return V1_DIE;
      default: return V1_OTHER;
   }
}

static unsigned was0(const px_kit_tia *t, bool exploding)
{
   px_kit_tia before = *t;
   before.wave[0]   = t->was_wave[0];
   before.pitch[0]  = t->was_pitch[0];
   before.volume[0] = t->was_volume[0];
   return voice0_plays(&before, exploding);
}

static unsigned was1(const px_kit_tia *t)
{
   px_kit_tia before = *t;
   before.wave[1]   = t->was_wave[1];
   before.pitch[1]  = t->was_pitch[1];
   before.volume[1] = t->was_volume[1];
   return voice1_plays(&before);
}

static void play_cell(yars *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave          freq  to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_NOISE, 5200, 1400, 0.10f, 0,      0.01f, 0.16f, 0.36f, 4800, 900, 0, 0 },
      { PX_WAVE_SQUARE, 330,  150, 0.08f, 0.001f, 0.01f, 0.12f, 0.22f, 2600, 500, 0, 0 },
      { PX_WAVE_SINE,   120,   60, 0.08f, 0.001f, 0.01f, 0.14f, 0.55f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->yar_at >= 0 ? g->yar_at : 140));
   px_sound_rumble(s, 6000, 14000, 4);
}

static void play_charge(yars *g, px_sound *s)
{
   static const px_tone p = {
      PX_WAVE_SAW, 90, 520, 2.9f, 0.05f, 3.2f, 0.4f, 0.26f, 600, 4200, 7.0f, 0.02f
   };
   px_synth_stop(s->synth, g->charge, 0.05f);
   g->charge = px_synth_play(s->synth, &p, px_kit_pan(g->enemy_at >= 0 ? g->enemy_at : 150), 1.0f);
}

static void play_launch(yars *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE, 1500, 9000, 0.9f, 0.01f, 0.1f, 0.6f, 0.30f, 1200, 8000, 0, 0 },
      { PX_WAVE_SAW,    260, 1300, 0.9f, 0.01f, 0.1f, 0.5f, 0.24f, 5000, 0, 9.0f, 0.03f },
      { PX_WAVE_SINE,    70,   45, 0.4f, 0.005f, 0.1f, 0.5f, 0.6f, 0, 0, 0, 0 }
   };
   px_synth_stop(s->synth, g->charge, 0.05f);
   g->charge = 0;
   px_kit_play(s, p, 3, px_kit_pan(g->enemy_at >= 0 ? g->enemy_at : 150));
   px_sound_rumble(s, 20000, 26000, 14);
}

static void play_blast(yars *g, px_sound *s, bool first)
{
   static const px_tone big[4] = {
      { PX_WAVE_NOISE, 6000,  200, 2.5f, 0,      0.20f, 3.0f, 0.45f, 6000, 120, 0, 0 },
      { PX_WAVE_SINE,    90,   24, 2.0f, 0.002f, 0.20f, 2.6f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    600,   40, 1.8f, 0.002f, 0.10f, 2.2f, 0.34f, 3000, 150, 6.0f, 0.05f },
      { PX_WAVE_SQUARE, 1200, 300, 0.4f, 0.001f, 0.05f, 0.6f, 0.20f, 5000, 800, 0, 0 }
   };
   static const px_tone rumble[2] = {
      { PX_WAVE_NOISE, 2400, 300, 1.6f, 0.02f, 0.10f, 1.8f, 0.40f, 2200, 150, 0, 0 },
      { PX_WAVE_SINE,    55,  30, 1.4f, 0.01f, 0.10f, 1.6f, 0.60f, 0, 0, 0, 0 }
   };
   const float pan = px_kit_pan(g->enemy_at >= 0 ? g->enemy_at : 150);
   if (first)
   {
      px_kit_play(s, big, 4, pan);
      px_sound_rumble(s, 65535, 65535, 60);
   }
   else
   {
      px_kit_play(s, rumble, 2, pan * 0.5f);
      px_sound_rumble(s, 40000, 30000, 30);
   }
}

static void play_hit(yars *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SQUARE, 1800, 90, 0.5f, 0.001f, 0.05f, 0.7f, 0.30f, 6000, 400, 18.0f, 0.08f },
      { PX_WAVE_NOISE,  8000, 600, 0.6f, 0,     0.05f, 0.8f, 0.40f, 7000, 300, 0, 0 },
      { PX_WAVE_SINE,    160,  40, 0.4f, 0.002f, 0.05f, 0.6f, 0.75f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->yar_at));
   px_sound_rumble(s, 50000, 40000, 30);
}

static void play_die(yars *g, px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_TRIANGLE, 180, 1400, 0.25f, 0.005f, 0.05f, 0.35f, 0.40f, 0, 0, 12.0f, 0.04f },
      { PX_WAVE_NOISE,   2000, 9000, 0.25f, 0.005f, 0.05f, 0.30f, 0.18f, 3000, 9000, 0, 0 }
   };
   px_kit_play(s, p, 2, px_kit_pan(g->yar_at));
   px_sound_rumble(s, 0, 20000, 10);
}

static void play_beat(px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_SINE,  78, 38, 0.14f, 0.002f, 0.02f, 0.22f, 0.70f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 900, 0, 0,    0,      0,     0.05f, 0.10f, 700, 200, 0, 0 }
   };
   px_kit_play(s, p, 2, 0.0f);
}

/* The hum, an octave and more below the game's, with its flutter. It follows the game's
 * pitch: higher where the game's is. */
static void play_hum(yars *g, px_sound *s, bool on)
{
   static const px_tone low  = { PX_WAVE_SAW,    41.0f, 0, 0, 0.3f, 0, 0, 1.0f, 240, 0, 6.1f, 0.012f };
   static const px_tone high = { PX_WAVE_SQUARE, 82.0f, 0, 0, 0.3f, 0, 0, 0.5f, 320, 0, 6.1f, 0.010f };
   float up;
   if (!on)
   {
      px_synth_stop(s->synth, g->hum_low, 0.3f);
      px_synth_stop(s->synth, g->hum_high, 0.3f);
      g->hum_low = g->hum_high = 0;
      return;
   }
   /* The game's pitch 10 is the hum's own; 4 is higher, 12 lower. */
   up = g->tia.pitch[1] < 10 ? 1.0f + (float)(10 - g->tia.pitch[1]) * 0.06f
      : 1.0f - (float)(g->tia.pitch[1] - 10) * 0.04f;
   if (!px_synth_move(s->synth, g->hum_low, -0.25f, 0.11f, 41.0f * up))
      g->hum_low = px_synth_play(s->synth, &low, -0.25f, 0.11f);
   if (!px_synth_move(s->synth, g->hum_high, 0.25f, 0.08f, 82.0f * up))
      g->hum_high = px_synth_play(s->synth, &high, 0.25f, 0.08f);
}

static void sound(void *state, px_sound *s)
{
   yars *g = (yars*)state;
   const bool exploding = px_kit_ram(s->ram, s->ram_size, RAM_DESTROYED) > 0;
   unsigned now0, now1, before0, before1;

   /* Where things are, from the picture before. */
   g->yar_at = g->enemy_at = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_PLAYER && !in->ghost)
         g->yar_at = in->x + 4;
      else if (in->role == PX_ROLE_ENEMY && (g->enemy_at < 0 || in->group == GROUP_SWIRL))
         g->enemy_at = in->x + 4;
   }

   px_kit_tia_hear(&g->tia, s->frame);
   now0 = voice0_plays(&g->tia, exploding);
   now1 = voice1_plays(&g->tia);
   before0 = was0(&g->tia, exploding);
   before1 = was1(&g->tia);

   /* Voice 0. */
   if (now0 == V0_CELL && (before0 != V0_CELL || px_kit_tia_louder(&g->tia, 0)))
   {
      if (g->own_sound) play_cell(g, s);
      else              px_sound_rumble(s, 6000, 14000, 4);
   }
   else if (now0 == V0_CHARGE && before0 != V0_CHARGE)
   {
      if (g->own_sound) play_charge(g, s);
   }
   else if (now0 == V0_LAUNCH && before0 != V0_LAUNCH)
   {
      if (g->own_sound) play_launch(g, s);
      else              px_sound_rumble(s, 20000, 26000, 14);
   }
   else if (now0 == V0_BLAST && before0 != V0_BLAST)
   {
      /* The first of the destruction's sounds is the blast; the sweeps after it rumble. */
      if (g->own_sound) play_blast(g, s, g->tia.wave[0] == 13);
      else              px_sound_rumble(s, 65535, 65535, g->tia.wave[0] == 13 ? 60 : 30);
   }
   else if (now0 == V0_BLAST && g->tia.wave[0] == 8 && g->tia.was_wave[0] == 8
         && g->tia.pitch[0] < g->tia.was_pitch[0] && g->tia.volume[0] >= 14)
   {
      /* A sweep of the destruction begins again. */
      if (g->own_sound) play_blast(g, s, false);
      else              px_sound_rumble(s, 40000, 30000, 30);
   }
   if (now0 != V0_CHARGE && g->charge)
   {
      px_synth_stop(s->synth, g->charge, 0.1f);
      g->charge = 0;
   }

   /* Voice 1. */
   if (now1 == V1_HIT && before1 != V1_HIT)
   {
      if (g->own_sound) play_hit(g, s);
      else              px_sound_rumble(s, 50000, 40000, 30);
   }
   else if (now1 == V1_DIE && before1 != V1_DIE)
   {
      if (g->own_sound) play_die(g, s);
      else              px_sound_rumble(s, 0, 20000, 10);
   }
   else if (now1 == V1_BEAT && before1 != V1_BEAT && g->own_sound && g->hum)
      play_beat(s);

   if (g->own_sound)
   {
      if (now0 == V0_CELL || now0 == V0_CHARGE || now0 == V0_LAUNCH || now0 == V0_BLAST)
         s->voice[0] = 0.0f;
      if (now1 == V1_HIT || now1 == V1_DIE || (g->hum && (now1 == V1_HUM || now1 == V1_BEAT)))
         s->voice[1] = 0.0f;
   }
   play_hum(g, s, g->own_sound && g->hum && (now1 == V1_HUM || now1 == V1_BEAT));
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

/* The game was reset, or a state was loaded: what was known is not so any more. */
static void reset(void *state)
{
   yars *g = (yars*)state;
   g->alive = g->destroyed = -1;
   g->dying = 0;
   g->cells = -1;
   g->cell_due = 0;
   g->yar_x = g->yar_y = g->qotile_x = g->qotile_y = -1;
   g->held_height = 0;
   g->age[PART_ZONE] = g->age[PART_SHIELD] = 99;
   px_kit_tia_reset(&g->tia);
   g->charge = g->hum_low = g->hum_high = 0;
}

static void *create(void)
{
   yars *g = (yars*)calloc(1, sizeof(yars));
   if (g)
   {
      g->steady = g->backdrop = g->sparks = g->own_sound = g->hum = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   yars *g = (yars*)state;
   if (g)
      px_kit_canvas_free(&g->sky);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   yars *g = (yars*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->steady    = px_kit_on(get, OPT_STEADY);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->hum       = px_kit_on(get, OPT_HUM);
}

const px_game px_game_yars_revenge = {
   "Yars' Revenge", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
