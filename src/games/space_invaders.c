/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Space Invaders (Atari, 1980).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  20..32   the two scores: playfield, on every other line
 *   rows  33..40   the saucer: a player
 *   rows  42..160  six rows of six invaders, 18 rows apart: both players in turns, three
 *                  copies each, set again for every row; the rows step down by ten
 *   rows 168..186  three shields: copies of player 0
 *   rows 196..206  the cannon: a player
 *   rows 206..     the ground, which is the background's colour, and what is left of the
 *                  cannons on it
 *   shots          the ball, which the game shares between the cannon's and the invaders'
 *                  from frame to frame
 *
 * Of its memory ($80 is 0): 17 has the invaders that are left, 73 the cannons.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_si_colors"
#define OPT_BACKDROP "proteus_si_backdrop"
#define OPT_SCORE    "proteus_si_score"
#define OPT_SPARKS   "proteus_si_sparks"
#define OPT_SOUND    "proteus_si_sound"
#define OPT_DRONE    "proteus_si_drone"

#define RAM_INVADERS 17
#define RAM_CANNONS  73

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define ROW_FIRST  42     /* the top row of invaders before they step down */
#define ROW_PITCH  18
#define ROWS       6
#define GROUND     206

#define STARS   260

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "72ffbef6504b75e69ee1045af9075f66",   /* Space Invaders (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "50",
   "reverb", "room",
   NULL
};

static const char *const colors[] = { "rows", "A colour a row", "arcade", "The arcade's gels",
   "original", "The game's own", NULL };
static const char *const backdrop[] = { "stars", "Night sky", "off", "Off", NULL };

static const px_game_option options[] = {
   { OPT_COLORS, "Invader colours",
     "A colour for every row of invaders, the white, red and green of the arcade cabinet's gels, or the game's own colours.",
     "rows", colors },
   { OPT_BACKDROP, "Backdrop",
     "What is behind the game where its background is black.", "stars", backdrop },
   { OPT_SCORE, "Solid score",
     "The game draws its score on every other line. Fill the lines between.", "enabled", px_kit_toggle },
   { OPT_SPARKS, "Explosions",
     "Sparks where an invader or the cannon is hit.", "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the invaders' step, the shot and the hits, each where it happens between left and right, and the march in four notes as the arcade had it. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_DRONE, "Hum",
     "A low hum that rises as the invaders get fewer and nearer. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* In the order of the option's values. */
enum { COLORS_ROWS = 0, COLORS_ARCADE, COLORS_ORIGINAL };

typedef struct
{
   uint16_t x, y;       /* in the picture, x in 16ths of a pixel */
   uint16_t drawn_x;
   uint8_t size, layer, phase, pace;
   uint32_t rgb;
   bool drawn;
} star;

typedef struct
{
   unsigned colors;
   bool backdrop, score, sparks;
   bool own_sound, drone;

   px_kit_tia tia;                 /* the game's two voices */
   unsigned heard0;                /* what voice 0 played in the frame before: SOUND_* */
   unsigned step;                  /* which of the march's four notes is next */
   unsigned drone_low, drone_high; /* the drone's two voices at the synth */
   int cannon_at, shot_at;         /* columns, for where a sound is; -1: not known */

   uint32_t frame;
   int invaders, cannons;          /* as memory had them; -1 before the first frame */
   px_kit_tags enemies;            /* tagged with the row each began in */

   px_kit_canvas sky;              /* the backdrop without its stars */
   star stars[STARS];
   uint32_t seed;
} si;

static const uint32_t row_colors[ROWS] = {
   0xFF5FD2, 0xB87BFF, 0x5F9BFF, 0x3FDDE6, 0x58E883, 0xF0E45A
};

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

/* A night sky: darker at the top, two faint clouds, and light above the ground. */
static void paint_base(si *g, unsigned sy)
{
   static const struct { int x, y, r; uint32_t rgb; } clouds[3] = {
      { 27, 30, 42, 0x180830 }, { 78, 58, 38, 0x061A28 }, { 55, 12, 30, 0x100818 }
   };
   const unsigned w = g->sky.w, h = g->sky.h, ground = GROUND * sy;
   uint32_t *base = g->sky.pixels;

   for (unsigned y = 0; y < h; y++)
   {
      /* Of 256: how far down to the ground. */
      unsigned down = ground ? (y < ground ? y * 256u / ground : 256u) : 0;
      unsigned haze = down > 176 ? (down - 176) * (down - 176) / 25 : 0;
      int dy = ((int)(2 * y + 1) - (int)h) * 256 / (int)h;
      for (unsigned x = 0; x < w; x++)
      {
         int dx = ((int)(2 * x + 1) - (int)w) * 256 / (int)w;
         unsigned dim = 256 - ((((unsigned)(dx * dx) >> 8) + ((unsigned)(dy * dy) >> 8)) * 90u >> 8);
         unsigned r = 3 + (down * 5 >> 8) + (haze * 40 >> 8);
         unsigned gr = 4 + (down * 4 >> 8) + (haze * 18 >> 8);
         unsigned bl = 12 + (down * 16 >> 8) + (haze * 30 >> 8);
         uint32_t rgb = (r << 16) | (gr << 8) | bl;
         for (unsigned k = 0; k < 3; k++)
         {
            /* Clouds are in hundredths of the picture; wide as they are high on a screen. */
            int cx = (int)(x * 100 / w) - clouds[k].x, cy = ((int)(y * 100 / h) - clouds[k].y) * 3 / 4;
            int d2 = cx * cx + cy * cy, r2 = clouds[k].r * clouds[k].r;
            if (d2 < r2)
               rgb = px_rgb_add(rgb, px_rgb_scale(clouds[k].rgb, (unsigned)((r2 - d2) * 256 / r2)));
         }
         base[(size_t)y * w + x] = px_rgb_scale(rgb, dim);
      }
   }

   g->seed = 0x51A7u;
   for (unsigned i = 0; i < STARS; i++)
   {
      star *s = &g->stars[i];
      unsigned kind = px_kit_chance(&g->seed) % 16;
      s->layer = kind < 9 ? 0 : kind < 14 ? 1 : 2;
      s->size  = (uint8_t)((s->layer == 2 ? 3 : s->layer == 1 ? 2 : 1) * (w >= 1200 ? 1 : 1));
      s->x     = (uint16_t)(px_kit_chance(&g->seed) % (w * 16u));
      s->y     = (uint16_t)(px_kit_chance(&g->seed) % (ground > 8 ? ground - 8 : 1));
      s->phase = (uint8_t)px_kit_chance(&g->seed);
      s->pace  = (uint8_t)(1 + px_kit_chance(&g->seed) % 5);
      switch (px_kit_chance(&g->seed) % 6)
      {
         case 0:  s->rgb = 0xFFD8B0; break;
         case 1:  s->rgb = 0xB0D0FF; break;
         default: s->rgb = 0xF0F0FF; break;
      }
      s->drawn = false;
   }
}

static void put_star(const si *g, uint32_t *out, const star *s, unsigned x, uint32_t rgb)
{
   for (unsigned t = 0; t < s->size; t++)
      for (unsigned u = 0; u < s->size; u++)
      {
         unsigned X = x + u, Y = s->y + t;
         if (X < g->sky.w && Y < g->sky.h)
         {
            size_t i = (size_t)Y * g->sky.w + X;
            out[i] = rgb ? px_rgb_add(g->sky.pixels[i], rgb) : g->sky.pixels[i];
         }
      }
}

static void paint_backdrop(si *g, px_scene *s)
{
   if (px_kit_canvas_fit(&g->sky, s))
   {
      paint_base(g, s->sy);
      memcpy(s->backdrop, g->sky.pixels, (size_t)s->w * s->h * sizeof(uint32_t));
   }
   else if (!g->sky.pixels)
      return;
   else if (!s->advance)
   {
      s->backdrop_on = true;
      return;
   }

   for (unsigned i = 0; i < STARS; i++)
   {
      star *st = &g->stars[i];
      /* A triangle wave: stars near by twinkle more than those far off. */
      unsigned wave = px_kit_wave(st->phase + g->frame * st->pace);
      unsigned bright = st->layer == 2 ? 150 + (wave * 105 >> 8)
            : st->layer == 1 ? 90 + (wave * 90 >> 8) : 50 + (wave * 50 >> 8);
      unsigned x;

      if (st->drawn)
         put_star(g, s->backdrop, st, st->drawn_x, 0);
      /* They drift to the left, the near ones faster. */
      st->x = (uint16_t)((st->x + s->w * 16u - (st->layer + 1u)) % (s->w * 16u));
      x = st->x / 16u;
      put_star(g, s->backdrop, st, x, px_rgb_scale(st->rgb, bright));
      st->drawn_x = (uint16_t)x;
      st->drawn   = true;
   }
   s->backdrop_on = true;
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

/* The rows of invaders: a track keeps the row it had when it was first seen, and rows of
 * tracks not seen before are told from how far those seen before have stepped down. */
static void find_rows(si *g, px_scene *s)
{
   px_objects *o = s->objects;
   int known = 0, stepped = 0, top = 1 << 20;

   px_kit_tags_begin(&g->enemies);

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      const px_kit_tagged *seen;
      if (in->role != PX_ROLE_ENEMY)
         continue;
      if (in->y < top)
         top = in->y;
      if ((seen = px_kit_tags_find(&g->enemies, in)) != NULL)
      {
         stepped += in->y - (ROW_FIRST + ROW_PITCH * seen->tag);
         known++;
      }
   }
   /* Nothing to go by: the top row is there when a wave begins. */
   stepped = known ? (stepped + known / 2) / known : top - ROW_FIRST;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      const px_kit_tagged *slot;
      int row;
      if (in->role != PX_ROLE_ENEMY)
         continue;
      row  = (in->y - ROW_FIRST - stepped + ROW_PITCH / 2) / ROW_PITCH;
      slot = px_kit_tags_keep(&g->enemies, in, row < 0 ? 0 : row >= ROWS ? ROWS - 1 : row);
      if (slot)
         in->group = slot->tag;
   }
}

static uint32_t enemy_color(const si *g, unsigned group, uint32_t original)
{
   switch (g->colors)
   {
      case COLORS_ROWS:   return row_colors[group < ROWS ? group : ROWS - 1];
      case COLORS_ARCADE: return 0xF4F4F4;
      default:            return original;
   }
}

/* The score is drawn on every other line: the lines between get what is above them. */
static void fill_score(px_scene *s)
{
   for (unsigned y = 12; y + 2 < 40 && y + 2 < s->frame->height; y++)
   {
      uint32_t *row = s->top + (size_t)y * PXC_W;
      for (unsigned x = 0; x < PXC_W; x++)
         if (PX_KEY_CLS(row[x]) == PX_CLS_PF && PX_KEY_CLS(row[x + PXC_W]) == PX_CLS_BK
               && row[x + 2 * PXC_W] == row[x])
            row[x + PXC_W] = row[x];
   }
}

static void frame(void *state, px_scene *s)
{
   si *g = (si*)state;
   px_objects *o = s->objects;
   const uint32_t *palette = s->frame->palette;
   int invaders = -1, cannons = -1;
   int cannon_x = -1, cannon_y = 0;

   if (s->advance)
      g->frame++;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role = PX_ROLE_NONE;
      if (px_kit_is_player(in))
      {
         if (in->y >= GROUND)
            in->role = PX_ROLE_HUD;
         else if (in->y >= 190)
            in->role = PX_ROLE_PLAYER;
         else if (in->y >= 160 && in->h >= 12)
            in->role = PX_ROLE_SHIELD;
         else if (in->y >= 41 && in->y < 160)
            in->role = PX_ROLE_ENEMY;
         else if (in->y >= 24)
            in->role = PX_ROLE_BONUS;
      }
      else if (in->y >= 30 && in->y < GROUND)
      {
         const px_obj_track *t = px_objects_track(o, in->track);
         int vy = t ? t->vy : 0;
         in->role = vy < 0 || (!vy && in->y > 180) ? PX_ROLE_SHOT : PX_ROLE_BOMB;
      }
   }
   find_rows(g, s);

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      uint32_t original = palette[in->color] & 0xFFFFFFu;
      switch (in->role)
      {
         case PX_ROLE_ENEMY:
            if (g->colors != COLORS_ORIGINAL)
               px_scene_tint(s, in, enemy_color(g, in->group, original));
            break;
         case PX_ROLE_BONUS:
         {
            /* The saucer beats. */
            unsigned wave = px_kit_wave(g->frame * 12);
            px_scene_tint(s, in, px_rgb_add(0xE02030, px_rgb_scale(0x603020, wave)));
            px_scene_energy(s, in, true);
            break;
         }
         case PX_ROLE_SHOT:
            px_scene_tint(s, in, g->colors == COLORS_ORIGINAL ? original : 0xC8F4FF);
            break;
         case PX_ROLE_BOMB:
            px_scene_tint(s, in, g->colors == COLORS_ORIGINAL ? original : 0xFF7A3A);
            break;
         case PX_ROLE_SHIELD:
         case PX_ROLE_PLAYER:
            if (g->colors == COLORS_ARCADE)
               px_scene_tint(s, in, 0x38F060);
            if (in->role == PX_ROLE_PLAYER && !in->ghost)
            {
               cannon_x = in->x + 4;
               cannon_y = in->y + 4;
            }
            break;
         default:
            break;
      }
   }

   if (g->score)
      fill_score(s);
   if (g->colors == COLORS_ARCADE)
      px_kit_playfield(s, 0, 40, 0xF4F4F4);

   invaders = px_kit_ram(s->ram, s->ram_size, RAM_INVADERS);
   cannons  = px_kit_ram(s->ram, s->ram_size, RAM_CANNONS);

   if (s->advance && g->sparks)
   {
      /* An invader that was there and is not was hit, if it is one or two of them; more
       * are a wave that ended or a game that began. */
      const unsigned gone = px_kit_tags_gone(&g->enemies);
      for (unsigned e = 0; e < PX_KIT_TAGS; e++)
      {
         const px_kit_tagged *en = &g->enemies.slot[e];
         if (!en->id || en->kept)
            continue;
         if (gone <= 2 && (invaders < 0 || g->invaders < 0 || invaders <= g->invaders))
            px_scene_burst(s, en->x + 4, en->y + 5, enemy_color(g, en->tag, 0xD0D040), 18, 300);
      }
      if (cannons >= 0 && g->cannons >= 0 && cannons < g->cannons && cannon_x >= 0)
      {
         px_scene_burst(s, cannon_x, cannon_y, 0xFFE0A0, 40, 420);
         px_scene_flash(s, 0xFF3020, 110);
      }
   }

   if (s->advance)
   {
      px_kit_tags_end(&g->enemies);
      g->invaders = invaders;
      g->cannons  = cannons;
   }

   if (g->backdrop)
      paint_backdrop(g, s);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has four, and the TIA's two voices for them:
 *
 *   voice 0   the invaders' step: waveform 9 at pitch 22, 8 at 25, 12 at 31 and 14 at 22, a
 *             frame or two each. Every step sounds the same, where the arcade's had four
 *             notes in turns.
 *   voice 0   an invader hit: waveform 12, the pitch from 16 up through 13, 10, 8, 7, 6, 5
 *   voice 0   the cannon hit: waveform 3 at pitch 24, 10 at 16, then 14 at 18, 22, 26, 29
 *   voice 1   the cannon's shot: noise (waveform 8) at 24, 25, 28, 30, from volume 7 down
 *
 * Voice 0 has one thing to say at a time: an invader's step is not heard while another is
 * hit. Here each is a sound of several voices of its own, where it happens between left and
 * right, and the game's voice is silent while it plays what is known. What is not known
 * (the saucer) is heard as the game plays it.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_STEP, SOUND_HIT, SOUND_LOST, SOUND_KNOWN, SOUND_OTHER };

static unsigned voice0_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   if (!volume)
      return SOUND_NONE;
   switch (wave)
   {
      case 9:  return pitch == 22 ? SOUND_STEP : SOUND_OTHER;
      case 8:  return pitch == 25 ? SOUND_KNOWN : SOUND_OTHER;
      case 3:  return pitch == 24 ? SOUND_LOST : SOUND_OTHER;
      case 10: return pitch == 16 ? SOUND_KNOWN : SOUND_OTHER;
      case 12:
         if (pitch == 16)
            return SOUND_HIT;
         return pitch == 31 || pitch == 13 || pitch == 10 || (pitch >= 5 && pitch <= 8)
               ? SOUND_KNOWN : SOUND_OTHER;
      case 14:
         return pitch == 22 || pitch == 18 || pitch == 26 || pitch == 29
               ? SOUND_KNOWN : SOUND_OTHER;
      default: return SOUND_OTHER;
   }
}

static bool voice1_shoots(unsigned wave, unsigned pitch)
{
   return wave == 8 && (pitch == 24 || pitch == 25 || pitch == 28 || pitch == 30);
}

/* The arcade's march: four notes going down, one a step. */
static void play_step(si *g, px_sound *s)
{
   static const float notes[4] = { 98.00f, 87.31f, 82.41f, 73.42f };
   const float f = notes[g->step++ & 3];
   px_tone p[3] = {
      /* wave              freq      to    glide  attack hold   decay  gain   cutoff  to */
      { PX_WAVE_SINE,      f * 1.7f, f,    0.04f, 0.002f, 0.03f, 0.30f, 0.80f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE,    f * 2.0f, 0,    0,     0.002f, 0.01f, 0.16f, 0.26f, 900, 240, 0, 0 },
      { PX_WAVE_NOISE,     6000,     0,    0,     0,      0,     0.03f, 0.20f, 3000, 800, 0, 0 }
   };
   p[1].glide = 0.12f;
   p[2].glide = 0.03f;
   px_kit_play(s, p, 3, 0.0f);
   px_sound_rumble(s, 9000, 0, 3);
}

static void play_shot(si *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SAW,    1900, 260, 0.16f, 0.001f, 0.02f, 0.20f, 0.34f, 7000, 1200, 0, 0 },
      { PX_WAVE_SQUARE,  950, 130, 0.16f, 0.001f, 0.01f, 0.16f, 0.18f, 3000, 700, 0, 0 },
      { PX_WAVE_NOISE,  9000,   0, 0.07f, 0,      0,     0.07f, 0.16f, 6000, 1500, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->cannon_at));
   px_sound_rumble(s, 0, 14000, 4);
}

static void play_hit(si *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE,  7000, 900, 0.25f, 0,      0.02f, 0.30f, 0.46f, 5000, 500, 0, 0 },
      { PX_WAVE_SQUARE,  880, 110, 0.22f, 0.001f, 0.01f, 0.25f, 0.26f, 4000, 600, 0, 0 },
      { PX_WAVE_SINE,    140,  45, 0.12f, 0.001f, 0.02f, 0.25f, 0.70f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->shot_at >= 0 ? g->shot_at : g->cannon_at));
   px_sound_rumble(s, 22000, 30000, 8);
}

static void play_lost(si *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE, 5000, 300, 1.0f, 0,      0.15f, 1.3f, 0.62f, 3500, 150, 0, 0 },
      { PX_WAVE_SINE,    75,  28, 0.9f, 0.002f, 0.10f, 1.2f, 0.85f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    420,  40, 0.8f, 0.002f, 0.05f, 0.9f, 0.30f, 2500, 200, 11.0f, 0.04f }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->cannon_at));
   px_sound_rumble(s, 65535, 40000, 45);
}

/* A low hum under the game that rises and grows as the invaders get fewer and nearer. */
static void play_drone(si *g, px_sound *s, int invaders, int lowest)
{
   static const px_tone low  = { PX_WAVE_SAW, 55.0f, 0, 0, 0.8f, 0, 0, 1.0f, 210, 0, 0.13f, 0.004f };
   static const px_tone high = { PX_WAVE_SAW, 82.7f, 0, 0, 0.8f, 0, 0, 0.6f, 260, 0, 0.17f, 0.004f };
   float few, near, tense, gain, pitch;

   if (!g->drone || !g->own_sound || invaders <= 0 || lowest < 0)
   {
      px_synth_stop(s->synth, g->drone_low, 0.6f);
      px_synth_stop(s->synth, g->drone_high, 0.6f);
      g->drone_low = g->drone_high = 0;
      return;
   }
   few   = (36.0f - (float)invaders) / 36.0f;
   near  = ((float)lowest - 132.0f) / 60.0f;
   if (few < 0.0f)  few = 0.0f;
   if (near < 0.0f) near = 0.0f;
   tense = few > near ? few : near;
   if (tense > 1.0f)
      tense = 1.0f;
   gain  = 0.05f + 0.16f * tense;
   pitch = 1.0f + 0.5f * tense;

   if (!px_synth_move(s->synth, g->drone_low, -0.35f, gain, 55.0f * pitch))
      g->drone_low = px_synth_play(s->synth, &low, -0.35f, gain);
   if (!px_synth_move(s->synth, g->drone_high, 0.35f, gain, 82.7f * pitch))
      g->drone_high = px_synth_play(s->synth, &high, 0.35f, gain);
}

static void sound(void *state, px_sound *s)
{
   si *g = (si*)state;
   unsigned heard;
   int lowest = -1, invaders = -1;

   /* Where things are, from the picture before. */
   g->cannon_at = g->shot_at = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_PLAYER && !in->ghost)
         g->cannon_at = in->x + 4;
      else if (in->role == PX_ROLE_SHOT)
         g->shot_at = in->x;
      else if (in->role == PX_ROLE_ENEMY && in->y > lowest)
         lowest = in->y;
   }
   invaders = px_kit_ram(s->ram, s->ram_size, RAM_INVADERS);

   px_kit_tia_hear(&g->tia, s->frame);
   heard = voice0_plays(g->tia.wave[0], g->tia.pitch[0], g->tia.volume[0]);
   if (heard != g->heard0)
   {
      if (heard == SOUND_STEP)
      {
         if (g->own_sound) play_step(g, s);
         else              px_sound_rumble(s, 9000, 0, 3);
      }
      else if (heard == SOUND_HIT)
      {
         if (g->own_sound) play_hit(g, s);
         else              px_sound_rumble(s, 22000, 30000, 8);
      }
      else if (heard == SOUND_LOST)
      {
         if (g->own_sound) play_lost(g, s);
         else              px_sound_rumble(s, 65535, 40000, 45);
      }
   }
   g->heard0 = heard;

   if (voice1_shoots(g->tia.wave[1], g->tia.pitch[1]) && px_kit_tia_louder(&g->tia, 1)
         && g->tia.volume[1] >= 6)
   {
      if (g->own_sound) play_shot(g, s);
      else              px_sound_rumble(s, 0, 14000, 4);
   }

   if (g->own_sound)
   {
      if (heard != SOUND_OTHER)
         s->voice[0] = 0.0f;
      if (!g->tia.volume[1] || voice1_shoots(g->tia.wave[1], g->tia.pitch[1]))
         s->voice[1] = 0.0f;
   }
   play_drone(g, s, invaders, lowest);
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   si *g = (si*)state;
   g->invaders = g->cannons = -1;
   px_kit_tags_reset(&g->enemies);
   px_kit_tia_reset(&g->tia);
   g->heard0 = SOUND_NONE;
   g->step   = 0;
   g->drone_low = g->drone_high = 0;
}

static void *create(void)
{
   si *g = (si*)calloc(1, sizeof(si));
   if (g)
   {
      g->backdrop = g->score = g->sparks = true;
      g->own_sound = g->drone = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   si *g = (si*)state;
   if (g)
      px_kit_canvas_free(&g->sky);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   si *g = (si*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->score     = px_kit_on(get, OPT_SCORE);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->drone     = px_kit_on(get, OPT_DRONE);
}

const px_game px_game_space_invaders = {
   "Space Invaders", md5, fx, options, create, destroy, reset, configure, frame, sound
};
