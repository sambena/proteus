/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Missile Command (Atari, 1981).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  18..25   the score: both players, three copies each
 *   rows  30..198  the sky, which is the background, black:
 *                  the incoming missiles: missile 0, set again on every line, so that each
 *                    trail is a slanted line of short strokes in red ($48); the warhead at
 *                    its lower end is white ($0E) in the frames it is drawn
 *                  the counter-missile on its way up: missile 1, a dot whose colour changes
 *                    every frame
 *                  the blasts: player 1, set again for each, wide and tall as the blast
 *                    grows, its colour changing every frame; several share a frame
 *                  the cross-hair: the ball, 4 by 2, grey
 *   rows 199..208  the mounds of the ground, the launcher's in the middle: the playfield,
 *                  brown
 *   rows 209..228  the flat ground: the background, brown; black marks on it at the left
 *                  are the playfield (what they count was not found out)
 *   rows 200..208  six cities: players 0 and 1, three copies each, at columns 21, 37, 53,
 *                  102, 118 and 134; a city that is lost is rubble two rows high at 207
 *   rows 210..213  the counter-missiles left: player 1 in black, at column 73
 *
 * Of its memory ($80 is 0): 41, 43, 45, 47, 49 and 51 each point to the picture of one of
 * the cities, in the order of their columns: $38 is a city that stands, $43 up to $67 one
 * that is hit, $70 rubble (a poke of $70 to 41 turns the first city to rubble). 93 has the
 * counter-missiles left (10 at the start of a wave), 111 the score's last two digits in BCD
 * and 112 and 113 the digits before them. The random stick of the harness scores little:
 * what a score does was seen in the first few waves only.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define SKY_TOP     28
#define GROUND      199       /* the mounds begin */
#define FLAT        209       /* the flat ground begins */
#define CITIES      6
#define CITY_UP     0x38      /* the picture of a city that stands */
#define RAM_CITY    41        /* and 43, 45, ... */
#define RAM_SCORE   111       /* and 112, 113 */
#define TRAIL_RED   0x48
#define STARS       180

static const uint8_t city_x[CITIES] = { 21, 37, 53, 102, 118, 134 };

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "3a2e2d0c6892aa14544083dfb7762782",   /* Missile Command (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "60",
   "reverb", "hall",
   NULL
};

#define OPT_COLORS   "proteus_mc_colors"
#define OPT_BACKDROP "proteus_mc_backdrop"
#define OPT_SPARKS   "proteus_mc_sparks"
#define OPT_SOUND    "proteus_mc_sound"
#define OPT_DRONE    "proteus_mc_drone"

static const char *const colors[] = { "arcade", "The arcade's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "night", "Night sky", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_ARCADE = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Cyan cities on golden ground, red-hot trails with white warheads and glowing blasts, as the arcade had them, or the game's own colours.",
     "arcade", colors },
   { OPT_BACKDROP, "Backdrop",
     "What is behind the game where its sky is black.", "night", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks where a missile is shot down, and fire and a red flash where a city is lost.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the launch, the blasts and the missiles that hit the ground, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_DRONE, "Hum",
     "A low rumble under the attack that grows as the missiles come nearer the ground. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

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
   bool backdrop, sparks, own_sound, drone;

   uint32_t frame;               /* counts the frames that advance */
   int city[CITIES];             /* the picture of each, as memory had it; -1: not known */
   int score;                    /* the score's six digits as one number; -1: not known */

   px_kit_tia tia;               /* the game's two voices */
   unsigned heard0;              /* what voice 0 played in the frame before: SOUND_* */
   int blast_at, launch_at;      /* columns, for where a sound is; -1: not known */
   unsigned hum;                 /* the hum's voice at the synth */

   px_kit_canvas sky;            /* the backdrop without its stars */
   star stars[STARS];
} game;

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

/* A night sky over a burning horizon: deep blue above, a dull red glow down at the ground. */
static void paint_base(game *g, unsigned sy)
{
   const unsigned w = g->sky.w, h = g->sky.h, ground = GROUND * sy;
   uint32_t *base = g->sky.pixels;
   uint32_t seed = 0x3C11u;

   for (unsigned y = 0; y < h; y++)
   {
      /* Of 256: how far down to the ground. */
      unsigned down = ground ? (y < ground ? y * 256u / ground : 256u) : 0;
      unsigned glow = down > 150 ? (down - 150) * (down - 150) / 44 : 0;
      unsigned r  = 2 + (down * 6 >> 8) + (glow * 70 >> 8);
      unsigned gr = 3 + (down * 3 >> 8) + (glow * 18 >> 8);
      unsigned bl = 14 + (down * 10 >> 8) + (glow * 8 >> 8);
      uint32_t rgb = (r << 16) | (gr << 8) | bl;
      for (unsigned x = 0; x < w; x++)
         base[(size_t)y * w + x] = rgb;
   }

   for (unsigned i = 0; i < STARS; i++)
   {
      star *s = &g->stars[i];
      unsigned kind = px_kit_chance(&seed) % 16;
      s->size  = (uint8_t)(kind < 11 ? 1 : 2);
      s->x     = (uint16_t)(px_kit_chance(&seed) % (w ? w : 1));
      /* Fewer of them near the glow. */
      s->y     = (uint16_t)(px_kit_chance(&seed) % (ground > 8 ? ground * 3 / 4 : 1));
      s->phase = (uint8_t)px_kit_chance(&seed);
      s->pace  = (uint8_t)(1 + px_kit_chance(&seed) % 4);
      s->rgb   = px_kit_chance(&seed) % 5 ? 0xE8ECFF : 0xFFD8B8;
   }
}

static void paint_backdrop(game *g, px_scene *s)
{
   bool fresh = px_kit_canvas_fit(&g->sky, s);
   if (fresh)
      paint_base(g, s->sy);
   else if (!g->sky.pixels)
      return;
   if (!s->advance && !s->backdrop_stale && !fresh)
   {
      s->backdrop_on = true;
      return;
   }

   /* The stars stay where they are: only they are painted again, unless it is all new. */
   if (fresh || s->backdrop_stale)
      memcpy(s->backdrop, g->sky.pixels, (size_t)s->w * s->h * sizeof(uint32_t));
   for (unsigned i = 0; i < STARS; i++)
   {
      const star *st = &g->stars[i];
      unsigned wave = px_kit_wave(st->phase + g->frame * st->pace);
      uint32_t rgb = px_rgb_scale(st->rgb, (st->size > 1 ? 110 : 50) + (wave * 70 >> 8));
      for (unsigned t = 0; t < st->size; t++)
         for (unsigned u = 0; u < st->size; u++)
         {
            unsigned X = st->x + u, Y = st->y + t;
            if (X < g->sky.w && Y < g->sky.h)
            {
               size_t k = (size_t)Y * g->sky.w + X;
               s->backdrop[k] = px_rgb_add(g->sky.pixels[k], rgb);
            }
         }
   }
   s->backdrop_on = true;
}

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

/* What an object is in the game, from where it is and what it is drawn with. */
static unsigned role_of(const px_instance *in)
{
   if (in->y < SKY_TOP)
      return px_kit_is_player(in) ? PX_ROLE_HUD : PX_ROLE_NONE;
   switch (in->cls)
   {
      case PXC_L_P0:
         return in->y >= GROUND ? PX_ROLE_SHIELD : PX_ROLE_NONE;
      case PXC_L_P1:
         if (in->y >= 209)
            return PX_ROLE_HUD;
         return in->y >= GROUND ? PX_ROLE_SHIELD : PX_ROLE_NONE;
      case PXC_L_M0:
         return in->y < GROUND ? PX_ROLE_BOMB : PX_ROLE_NONE;
      case PXC_L_M1:
         return in->y < GROUND - 4 ? PX_ROLE_SHOT : PX_ROLE_NONE;
      case PXC_L_BL:
         return PX_ROLE_PLAYER;
      default:
         return PX_ROLE_NONE;
   }
}

/* A blast is player 1 in the sky. */
static bool is_blast(const px_instance *in)
{
   return in->cls == PXC_L_P1 && in->y >= SKY_TOP && in->y < GROUND;
}

/* Where a missile was shot down: the blast that touches a trail, else the largest. */
static const px_instance *blast_of_kill(const px_objects *o)
{
   const px_instance *best = NULL;
   unsigned best_size = 0;
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      unsigned size = (unsigned)in->w * in->h;
      if (!is_blast(in) || in->ghost)
         continue;
      for (unsigned j = 0; j < o->count; j++)
      {
         const px_instance *m = &o->inst[j];
         if (m->cls == PXC_L_M0 && m->x + m->w >= in->x && m->x <= in->x + in->w
               && m->y + m->h >= in->y && m->y <= in->y + in->h)
            size += 1u << 16;
      }
      if (size > best_size)
      {
         best = in;
         best_size = size;
      }
   }
   return best;
}

static int read_score(const px_scene *s)
{
   int lo = px_kit_ram(s->ram, s->ram_size, RAM_SCORE);
   int mid = px_kit_ram(s->ram, s->ram_size, RAM_SCORE + 1);
   int hi = px_kit_ram(s->ram, s->ram_size, RAM_SCORE + 2);
   if (lo < 0 || mid < 0 || hi < 0)
      return -1;
   return (hi << 16) | (mid << 8) | lo;
}

static void frame(void *state, px_scene *s)
{
   game *g = (game*)state;
   px_objects *o = s->objects;
   const bool own = g->colors != COLORS_ORIGINAL;
   int score;

   /* A frame that is drawn again (the game is paused) is to look as it did: nothing moves
    * on, nothing is counted, nothing bursts. */
   if (s->advance)
      g->frame++;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role = (uint8_t)role_of(in);
      if (!own)
         continue;
      switch (in->role)
      {
         case PX_ROLE_SHIELD:
            /* Cities that stand are cyan; the game's flash when one is hit is its own. */
            if (in->color == 0x84)
               px_scene_tint(s, in, in->h >= 8 ? 0x38D8FF : 0x2C6C88);
            break;
         case PX_ROLE_BOMB:
            if (in->color == TRAIL_RED)
               px_scene_tint(s, in, 0xFF3A28);
            else if (in->color == 0x0E)
            {
               px_scene_tint(s, in, 0xFFF6D8);
               px_scene_energy(s, in, true);
            }
            break;
         case PX_ROLE_SHOT:
            px_scene_tint(s, in, 0x9CE4FF);
            px_scene_energy(s, in, true);
            break;
         case PX_ROLE_PLAYER:
         {
            /* The cross-hair beats slowly. */
            unsigned wave = px_kit_wave(g->frame * 6);
            px_scene_tint(s, in, px_rgb_add(0xB0B8C8, px_rgb_scale(0x4F4737, wave)));
            break;
         }
         default:
            /* The blasts keep the game's changing colours, and glow. */
            if (is_blast(in))
               px_scene_energy(s, in, true);
            break;
      }
   }

   /* The ground is golden: the mounds are the playfield, the flat ground below them the
    * background. What the playfield draws in the ground (black marks) keeps its colour. */
   if (own)
   {
      px_kit_playfield(s, GROUND - 4, FLAT, 0xD8A838);
      px_kit_background(s, FLAT, s->frame->height, 0xC8962E);
   }

   score = read_score(s);
   if (s->advance)
   {
      for (unsigned c = 0; c < CITIES; c++)
      {
         int now = px_kit_ram(s->ram, s->ram_size, RAM_CITY + 2 * c);
         /* A city that stood and does not: fire where it was, and the sky goes red. */
         if (g->sparks && now >= 0 && g->city[c] == CITY_UP && now != CITY_UP)
         {
            px_scene_burst(s, city_x[c] + 4, GROUND + 4, 0xFFB050, 44, 380);
            px_scene_burst(s, city_x[c] + 4, GROUND + 6, 0xFF4020, 20, 200);
            px_scene_flash(s, 0xFF3018, 120);
         }
         g->city[c] = now;
      }
      /* The score went up: a missile was shot down, in a blast. A score that falls is a
       * new game. */
      if (g->sparks && score > g->score && g->score >= 0)
      {
         const px_instance *b = blast_of_kill(o);
         if (b)
            px_scene_burst(s, b->x + b->w / 2, b->y + b->h / 2, 0xFFE8A0, 26, 320);
      }
      g->score = score;
   }

   if (g->backdrop)
      paint_backdrop(g, s);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has these, and the TIA's two voices for them:
 *
 *   voice 1   the counter-missile's launch: noise (waveform 8) at volume 6, the pitch from
 *             10 down to 1, four frames each
 *   voice 1   a missile shot down: noise at pitch 31, from volume 15 down through 12, 10,
 *             8; the score goes up in the frame before
 *   voice 1   a missile that reaches the ground, city or not: noise at pitch 31, from
 *             volume 14 down through 8, 6, 4, 2, over more than a second
 *   voice 0   a short buzz: waveform 5 at volume 8, the pitch from 12 down by 2 to 0, a
 *             frame each. It ends as a counter-missile's blast is gone (bytes 53 to 55 go
 *             to 0), and follows a missile that is shot down
 *   voice 0   when a game begins: waveform 12, the pitch rising from 4, for 128 frames
 *
 * The launch and the blasts share voice 1, and a blast cuts the launch off. Here each is a
 * sound of several voices of its own, where it happens between left and right. What is not
 * known (the tune at the start, what is heard as a wave is counted up) is heard as the game
 * plays it.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_BUZZ, SOUND_OTHER };

static bool voice1_launches(const px_kit_tia *t)
{
   return t->wave[1] == 8 && t->volume[1] == 6 && t->pitch[1] >= 1 && t->pitch[1] <= 10;
}

static bool voice1_blasts(const px_kit_tia *t)
{
   return t->wave[1] == 8 && t->pitch[1] == 31 && t->volume[1];
}

static unsigned voice0_plays(const px_kit_tia *t)
{
   if (!t->volume[0])
      return SOUND_NONE;
   if (t->wave[0] == 5 && t->volume[0] == 8 && t->pitch[0] <= 12 && !(t->pitch[0] & 1))
      return SOUND_BUZZ;
   return SOUND_OTHER;
}

static void play_launch(game *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave          freq  to     glide  attack  hold   decay  gain   cutoff  to */
      { PX_WAVE_NOISE, 2500, 9000,  0.55f, 0.02f,  0.10f, 0.55f, 0.30f, 1200, 7000, 0, 0 },
      { PX_WAVE_SAW,    180,  900,  0.60f, 0.01f,  0.05f, 0.50f, 0.12f, 1500, 5000, 0, 0 },
      { PX_WAVE_SINE,    70,   45,  0.20f, 0.002f, 0.02f, 0.25f, 0.50f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->launch_at));
   px_sound_rumble(s, 0, 12000, 5);
}

static void play_kill(game *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 8000, 600, 0.45f, 0,      0.03f, 0.80f, 0.40f, 6000, 300, 0, 0 },
      { PX_WAVE_SINE,   110,  38, 0.35f, 0.001f, 0.04f, 0.60f, 0.55f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE, 620,  90, 0.30f, 0.001f, 0.01f, 0.30f, 0.12f, 3000, 400, 0, 0 },
      { PX_WAVE_NOISE, 3000, 200, 1.20f, 0.05f,  0.10f, 1.40f, 0.16f, 1400, 120, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->blast_at));
   px_sound_rumble(s, 26000, 34000, 10);
}

static void play_impact(game *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 5000, 200, 1.40f, 0,      0.20f, 1.80f, 0.32f, 4000, 100, 0, 0 },
      { PX_WAVE_SINE,    62,  24, 1.20f, 0.002f, 0.15f, 1.60f, 0.40f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    300,  35, 1.00f, 0.002f, 0.05f, 1.10f, 0.14f, 2200, 150, 9.0f, 0.05f },
      { PX_WAVE_NOISE, 1200,  80, 2.00f, 0.10f,  0.30f, 2.40f, 0.15f, 700, 60, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->blast_at));
   px_sound_rumble(s, 65535, 45000, 40);
}

static void play_buzz(game *g, px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_NOISE,  4000, 900, 0.12f, 0.001f, 0.01f, 0.16f, 0.16f, 3500, 600, 0, 0 },
      { PX_WAVE_TRIANGLE, 330, 110, 0.12f, 0.001f, 0.01f, 0.14f, 0.12f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 2, px_kit_pan(g->blast_at));
}

/* A low rumble under the attack, louder and higher as the missiles come nearer the ground. */
static void play_hum(game *g, px_sound *s, int lowest)
{
   static const px_tone hum = { PX_WAVE_SAW, 41.0f, 0, 0, 1.0f, 0, 0, 1.0f, 180, 0, 0.21f, 0.006f };
   float near, gain;

   if (!g->drone || !g->own_sound || lowest < 0)
   {
      px_synth_stop(s->synth, g->hum, 0.8f);
      g->hum = 0;
      return;
   }
   near = ((float)lowest - SKY_TOP) / (float)(GROUND - SKY_TOP);
   if (near < 0.0f) near = 0.0f;
   if (near > 1.0f) near = 1.0f;
   gain = 0.04f + 0.14f * near * near;
   if (!px_synth_move(s->synth, g->hum, 0.0f, gain, 41.0f * (1.0f + 0.4f * near)))
      g->hum = px_synth_play(s->synth, &hum, 0.0f, gain);
}

static void sound(void *state, px_sound *s)
{
   game *g = (game*)state;
   unsigned heard;
   int lowest = -1, blast_size = -1;

   /* Where things are, from the picture before. */
   g->launch_at = 80;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_BOMB && in->color == TRAIL_RED && in->y + in->h > lowest)
         lowest = in->y + in->h;
      else if (is_blast(in) && !in->ghost && (int)(in->w * in->h) > blast_size)
      {
         blast_size = in->w * in->h;
         g->blast_at = in->x + in->w / 2;
      }
   }

   px_kit_tia_hear(&g->tia, s->frame);

   if (voice1_launches(&g->tia) && g->tia.pitch[1] == 10 && px_kit_tia_louder(&g->tia, 1))
   {
      if (g->own_sound) play_launch(g, s);
      else              px_sound_rumble(s, 0, 12000, 5);
   }
   if (voice1_blasts(&g->tia) && px_kit_tia_louder(&g->tia, 1))
   {
      if (g->tia.volume[1] == 15)
      {
         if (g->own_sound) play_kill(g, s);
         else              px_sound_rumble(s, 26000, 34000, 10);
      }
      else if (g->tia.volume[1] == 14)
      {
         if (g->own_sound) play_impact(g, s);
         else              px_sound_rumble(s, 65535, 45000, 40);
      }
   }

   heard = voice0_plays(&g->tia);
   if (heard == SOUND_BUZZ && g->heard0 != SOUND_BUZZ && g->own_sound)
      play_buzz(g, s);
   g->heard0 = heard;

   if (g->own_sound)
   {
      if (heard == SOUND_BUZZ)
         s->voice[0] = 0.0f;
      if (voice1_launches(&g->tia) || voice1_blasts(&g->tia))
         s->voice[1] = 0.0f;
   }
   play_hum(g, s, lowest);
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

/* The game was reset, or a state was loaded: what was known is not so any more. */
static void reset(void *state)
{
   game *g = (game*)state;
   for (unsigned c = 0; c < CITIES; c++)
      g->city[c] = -1;
   g->score = -1;
   px_kit_tia_reset(&g->tia);
   g->heard0 = SOUND_NONE;
   g->blast_at = g->launch_at = -1;
   g->hum = 0;
}

static void *create(void)
{
   game *g = (game*)calloc(1, sizeof(game));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = g->drone = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   game *g = (game*)state;
   if (g)
      px_kit_canvas_free(&g->sky);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   game *g = (game*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->drone     = px_kit_on(get, OPT_DRONE);
}

const px_game px_game_missile_command = {
   "Missile Command", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
