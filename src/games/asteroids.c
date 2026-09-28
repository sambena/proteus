/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Asteroids (Atari, 1981).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  16..25   the score, and at the right the ships that are left: playfield
 *   rows  28..205  space, from column 8 to 159; what leaves at one edge comes in at the other,
 *                  and is drawn in two pieces while it does
 *   rocks          both players, one copy each, placed again for every rock down the screen.
 *                  A large rock is a player of double width, 16 by 28, and the game moves it
 *                  by a pixel between its rows: it is found as three parts of 16, 6 and 6
 *                  rows, one below the other. A medium one is 8 by 14, a small one 8 by 8.
 *                  Every rock has a colour of its own, and its pieces get new ones.
 *   the ship       player 0, 8 wide and 8 or 10 rows, colour 4C; in sixteen directions
 *   its shots      the two missiles, a pixel by two rows
 *   the saucer     and the satellite: player 1, with the left difficulty switch at A only
 *   its shot       the ball
 *   the title      both players in three copies: the only time there is more than one
 *
 * The game shows these in turns: the rocks in one frame, the ship, the shots and the saucer
 * in the next, each thirty times a second. All of it flickers, and all of it is drawn from
 * its tracks in the frames between. In 3690 frames of play 98 of 100 objects were; the rest
 * are shots and pieces in the first frame after they appeared, and rocks that came in at
 * another edge, which begin a new track.
 *
 * A rock that leaves at the right comes in at the left, behind the eight columns the game
 * leaves black, and may be at both edges for seconds. The object finder finds what is at
 * the right edge as an instance of its own (fx_track.c: the same copy twice on a line).
 *
 * Of its memory ($80 is 0):
 *
 *   57   counts the frames: the rocks are drawn when it is odd, the ship when it is even
 *   60   the ships that are left in the high nibble; in the low one where the ship points,
 *        in sixteenths of a turn to the left: 0 up, 4 left, 8 down, 12 right
 *   61   the score's thousands, 62 its hundreds and tens, as decimal digits. A large rock
 *        is 20, a medium one 50, a small one 100.
 *   74   the ship's row, two lines to the count; E0 while there is none
 *   94   bit 7 while the ship explodes, then a count down to the next ship from 3F;
 *        a count down from 1F without the bit while the ship is in hyperspace
 *   3..20 and 21..38   the rocks' rows and columns, a list for each player; 86, 87 and 89,
 *        90 those of the ship's shots
 *
 * Stella does not play this game the same way twice when the harness presses the buttons
 * by --input, not even from a saved state; with --press alone it does.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_as_colors"
#define OPT_BACKDROP "proteus_as_backdrop"
#define OPT_FLAME    "proteus_as_flame"
#define OPT_SPARKS   "proteus_as_sparks"
#define OPT_SOUND    "proteus_as_sound"
#define OPT_WARP     "proteus_as_warp"

#define RAM_FRAMES   57
#define RAM_SHIP     60
#define RAM_SCORE_HI 61
#define RAM_SCORE    62
#define RAM_ROW      74
#define RAM_FATE     94

#define NO_SHIP      0xE0   /* in RAM_ROW */

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define SPACE_TOP    28
#define SPACE_BOTTOM 206    /* the row past the last */
#define SPACE_LEFT   8

#define BODIES  64
#define SHOTS   4
#define STARS   240
#define DUST    170

/* What a body is; of rocks, in->group too, for the sound. */
enum { KIND_SMALL = 0, KIND_MEDIUM, KIND_LARGE, KIND_SAUCER, KIND_SHIP };

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "dd7884b4f93cab423ac471aa1935e3df",   /* Asteroids (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "50",
   "reverb", "room",
   NULL
};

static const char *const colors[] = { "stone", "Rocks of stone", "vector", "The arcade's lines",
   "original", "The game's own", NULL };
static const char *const backdrop[] = { "space", "Deep space", "stars", "Stars alone", "off", "Off", NULL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Rocks of stone, lit from the upper left and tinted by their size; the bright outlines of the arcade's screen; or the game's own colours.",
     "stone", colors },
   { OPT_BACKDROP, "Backdrop",
     "What is behind the game where its background is black: a nebula and stars that drift, or the stars alone.",
     "space", backdrop },
   { OPT_FLAME, "Thrust flame",
     "A flame behind the ship while it thrusts.", "enabled", px_kit_toggle },
   { OPT_SPARKS, "Explosions",
     "Sparks where a rock, the saucer or the ship is hit, as many as it was large, and a flash when the ship is lost.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the shot, the thrust, the saucer and the explosions, each where it happens between left and right, and the game's heartbeat played deeper. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_WARP, "Hyperspace sound",
     "The game is silent when the ship leaves for hyperspace and when it comes back. This gives both a sound. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* In the order of the options' values. */
enum { COLORS_STONE = 0, COLORS_VECTOR, COLORS_ORIGINAL };
enum { BACKDROP_SPACE = 0, BACKDROP_STARS, BACKDROP_OFF };

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

/* A rock, the ship or the saucer: the parts it was found in, together. */
typedef struct
{
   int16_t x0, y0, x1, y1;   /* the box around it; x1 and y1 are past it */
   int16_t last_x;           /* of the part that was added last */
   uint16_t rows;            /* of its parts, and of what there is of it at the other edge */
   uint8_t kind;             /* KIND_* */
   uint8_t color;            /* the game's, of the palette */
   uint8_t cls, role;
   bool ghost;               /* drawn from its tracks */
   bool wide;                /* a part is wider than 8: a large rock */
} body;

typedef struct
{
   uint16_t x, y;            /* in the picture, in 16ths of a pixel */
   uint16_t drawn_x, drawn_y;
   uint8_t size, layer, phase, pace;
   uint32_t rgb;
   bool drawn;
} star;

typedef struct
{
   /* The options. */
   unsigned colors, backdrop;
   bool flame, sparks, own_sound, warp;

   /* The picture. */
   uint32_t frame;                 /* counts the frames that advance */
   px_kit_tia seen;                /* the game's voices as the picture follows them */
   px_kit_tags known;              /* the players' tracks, tagged with their roles */
   bool turn_known;                /* without memory: whose turn a frame is was seen */
   uint8_t ship_turn;              /* and the frames that are the ship's are those odd or even */
   uint8_t *mask;                  /* for every pixel the body there, counted from 1 */
   body bodies[BODIES];            /* of the frame at hand */
   unsigned body_count;
   body rocks[BODIES];             /* as the last frame of theirs had them, and the saucer */
   unsigned rock_count;
   body saucer;
   bool saucer_there;
   struct { int16_t x, y; } shots[SHOTS];   /* the ship's, as its last frame had them */
   unsigned shot_count;
   int ship_x, ship_y;             /* the middle of the ship when it was seen last; -1: never */
   int fate, row;                  /* RAM_FATE and RAM_ROW as the frame before had them */
   bool away;                      /* the ship is in hyperspace */

   px_kit_canvas sky;              /* the backdrop without its stars */
   unsigned sky_kind;              /* BACKDROP_* it was painted as */
   star stars[STARS];
   uint32_t seed;

   /* The sound. */
   px_kit_tia tia;                 /* the game's voices as the sound follows them */
   unsigned heard0, heard1;        /* what they played in the frame before */
   unsigned thrust[3], hum[2];     /* the voices at the synth of what goes on */
   uint32_t clock;                 /* counts the frames heard */
   uint32_t beat_at, beat_true;    /* when the heart beat last, and when the game's did */
   uint32_t beat_every;            /* frames from one beat to the next; 0: not known */
   uint32_t shot_heard;            /* when the game's voice last played the shot */
   uint8_t beat_pitch;             /* of the beat that is next */
   uint8_t true_pitch;             /* of the game's own beat that was heard last */
   int score;                      /* in tens, as the frame before had it; -1: not known */
   int heard_fate, heard_row;
   bool heard_away;
   int ship_at, saucer_at;         /* columns, for where a sound is; -1: not known */
} as;

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

static unsigned hash2(unsigned x, unsigned y, unsigned salt)
{
   uint32_t h = x * 374761393u + y * 668265263u + salt * 2246822519u;
   h = (h ^ (h >> 13)) * 1274126177u;
   return (h ^ (h >> 16)) & 255u;
}

/* Clouds of chance, 0..255: the values at the corners of cells of `cell` pixels, blended. */
static unsigned cloud(unsigned x, unsigned y, unsigned cell, unsigned salt)
{
   const unsigned ix = x / cell, iy = y / cell;
   unsigned fx = (x % cell) * 256u / cell, fy = (y % cell) * 256u / cell;
   unsigned a, b;
   fx = fx * fx * (768u - 2u * fx) >> 16;
   fy = fy * fy * (768u - 2u * fy) >> 16;
   a = hash2(ix, iy, salt) * (256u - fx) + hash2(ix + 1, iy, salt) * fx;
   b = hash2(ix, iy + 1, salt) * (256u - fx) + hash2(ix + 1, iy + 1, salt) * fx;
   return (a * (256u - fy) + b * fy) >> 16;
}

/* Deep space: all but black, a nebula in three colours across it, a band of dust from the
 * lower left to the upper right, and stars too far off to move. */
static void paint_base(as *g, unsigned sx)
{
   static const struct { int x, y, r; uint32_t rgb; } nebula[5] = {
      { 24, 30, 40, 0x30103C }, { 40, 52, 30, 0x0A2438 }, { 78, 70, 44, 0x3A1420 },
      { 88, 22, 26, 0x0C1C3C }, { 60, 40, 22, 0x14302C }
   };
   const unsigned w = g->sky.w, h = g->sky.h;
   const unsigned cell = 12 * (sx ? sx : 1);
   uint32_t *base = g->sky.pixels;

   for (unsigned y = 0; y < h; y++)
   {
      int dy = ((int)(2 * y + 1) - (int)h) * 256 / (int)h;
      for (unsigned x = 0; x < w; x++)
      {
         int dx = ((int)(2 * x + 1) - (int)w) * 256 / (int)w;
         unsigned dim = 256 - ((((unsigned)(dx * dx) >> 8) + ((unsigned)(dy * dy) >> 8)) * 80u >> 8);
         uint32_t rgb = 0x020309;

         if (g->backdrop == BACKDROP_SPACE)
         {
            const unsigned coarse = cloud(x, y, cell * 4, 1), fine = cloud(x, y, cell, 2);
            const unsigned wisps = (coarse * 3 + fine) / 4;
            /* How far from the band, in 256ths of the picture's height. */
            int off = ((int)(x * 100 / w) + (int)(y * 100 / h) - 100) * 256 / 100;
            if (off < 0)
               off = -off;
            for (unsigned k = 0; k < 5; k++)
            {
               /* In hundredths of the picture; as wide as high on a screen. */
               int cx = (int)(x * 100 / w) - nebula[k].x, cy = ((int)(y * 100 / h) - nebula[k].y) * 3 / 4;
               int d2 = cx * cx + cy * cy, r2 = nebula[k].r * nebula[k].r;
               if (d2 < r2)
               {
                  unsigned f = (unsigned)((r2 - d2) * 256 / r2);
                  f = f * f >> 8;
                  rgb = px_rgb_add(rgb, px_rgb_scale(nebula[k].rgb, f * (40 + wisps) / 232));
               }
            }
            if (off < 60)
               rgb = px_rgb_add(rgb, px_rgb_scale(0x120E16, (unsigned)(60 - off) * fine / 60));
         }
         base[(size_t)y * w + x] = px_rgb_scale(rgb, dim);
      }
   }

   g->seed = 0xA57E401Du;
   for (unsigned i = 0; i < DUST; i++)
   {
      const unsigned x = px_kit_chance(&g->seed) % w, y = px_kit_chance(&g->seed) % h;
      const unsigned bright = 30 + px_kit_chance(&g->seed) % 60;
      const uint32_t rgb = px_kit_chance(&g->seed) % 4 ? 0xD0D8FF : 0xFFE0C0;
      base[(size_t)y * w + x] = px_rgb_add(base[(size_t)y * w + x], px_rgb_scale(rgb, bright));
   }

   for (unsigned i = 0; i < STARS; i++)
   {
      star *s = &g->stars[i];
      unsigned kind = px_kit_chance(&g->seed) % 16;
      s->layer = kind < 10 ? 0 : kind < 14 ? 1 : 2;
      s->size  = (uint8_t)(s->layer == 2 ? 3 : s->layer == 1 ? 2 : 1);
      s->x     = (uint16_t)(px_kit_chance(&g->seed) % (w * 16u));
      s->y     = (uint16_t)(px_kit_chance(&g->seed) % (h * 16u));
      s->phase = (uint8_t)px_kit_chance(&g->seed);
      s->pace  = (uint8_t)(1 + px_kit_chance(&g->seed) % 4);
      switch (px_kit_chance(&g->seed) % 7)
      {
         case 0:  s->rgb = 0xFFD0A0; break;
         case 1:  s->rgb = 0xA8C8FF; break;
         case 2:  s->rgb = 0xFFB0B0; break;
         default: s->rgb = 0xF0F0FF; break;
      }
      s->drawn = false;
   }
}

static void put_star(const as *g, uint32_t *out, const star *s, unsigned x, unsigned y, uint32_t rgb)
{
   for (unsigned t = 0; t < s->size; t++)
      for (unsigned u = 0; u < s->size; u++)
      {
         unsigned X = x + u, Y = y + t;
         if (X < g->sky.w && Y < g->sky.h)
         {
            size_t i = (size_t)Y * g->sky.w + X;
            out[i] = rgb ? px_rgb_add(g->sky.pixels[i], rgb) : g->sky.pixels[i];
         }
      }
}

static void paint_backdrop(as *g, px_scene *s)
{
   if (px_kit_canvas_fit(&g->sky, s) || (g->sky.pixels && g->sky_kind != g->backdrop))
   {
      g->sky_kind = g->backdrop;
      paint_base(g, s->sx);
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
      unsigned wave = px_kit_wave(st->phase + g->frame * st->pace);
      unsigned bright = st->layer == 2 ? 140 + (wave * 115 >> 8)
            : st->layer == 1 ? 80 + (wave * 90 >> 8) : 45 + (wave * 45 >> 8);

      if (st->drawn)
         put_star(g, s->backdrop, st, st->drawn_x, st->drawn_y, 0);
      /* They drift down and to the left, the near ones faster: the ship is on its way. */
      if (s->advance)
      {
         st->x = (uint16_t)((st->x + s->w * 16u - (st->layer + 1u)) % (s->w * 16u));
         if (!(g->frame & 1))
            st->y = (uint16_t)((st->y + st->layer + 1u) % (s->h * 16u));
      }
      st->drawn_x = (uint16_t)(st->x / 16u);
      st->drawn_y = (uint16_t)(st->y / 16u);
      put_star(g, s->backdrop, st, st->drawn_x, st->drawn_y, px_rgb_scale(st->rgb, bright));
      st->drawn = true;
   }
   s->backdrop_on = true;
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

/* Whether a frame is the ship's or the rocks'. Memory says; without it, what is in the
 * frame does where that is plain, and the frames between take turns. */
static bool ships_turn(as *g, const px_objects *o, int counter)
{
   bool shots = false, rocks = false;

   if (counter >= 0)
      return !(counter & 1);
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      if (in->ghost || in->y < SPACE_TOP)
         continue;
      if (!px_kit_is_player(in))
         shots = true;
      else if (in->w > 8 || in->h > 10)
         rocks = true;
   }
   if (shots != rocks)
   {
      g->turn_known = true;
      g->ship_turn  = (uint8_t)((g->frame ^ (shots ? 0u : 1u)) & 1u);
   }
   return g->turn_known && (g->frame & 1u) == g->ship_turn;
}

/* Gives every object its role. False if the frame is not of the game: the title. */
static bool find_roles(as *g, px_scene *s, bool ships)
{
   px_objects *o = s->objects;

   for (unsigned i = 0; i < o->count; i++)
   {
      o->inst[i].role  = PX_ROLE_NONE;
      o->inst[i].group = 0;
   }
   for (unsigned i = 0; i < o->count; i++)
      if (px_kit_is_player(&o->inst[i]) && o->inst[i].copy > 1)
         return false;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      unsigned own, role;
      px_kit_tagged *t;

      if (in->y < SPACE_TOP - 2)
         continue;
      if (!px_kit_is_player(in))
      {
         in->role = in->cls == PXC_L_BL ? PX_ROLE_BOMB : PX_ROLE_SHOT;
         continue;
      }
      own  = in->cls == PXC_L_P0 ? PX_ROLE_PLAYER : PX_ROLE_BONUS;
      /* What is drawn from its track was seen in the other's turn. */
      role = ships != (in->ghost != 0) ? own : PX_ROLE_ENEMY;
      t    = px_kit_tags_keep(&g->known, in, role);
      if (t && in->ghost)
         role = t->tag;
      else if (t)
         t->tag = (uint8_t)role;
      in->role = (uint8_t)role;
   }
   return true;
}

/* The parts of rocks, from the top down, joined to bodies; and the ship and the saucer. */
static void find_bodies(as *g, px_scene *s, uint8_t *of)
{
   px_objects *o = s->objects;
   uint8_t order[PX_MAX_INSTANCES];
   unsigned count = 0;

   g->body_count = 0;
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      unsigned at = count;
      of[i] = 0xFF;
      if (in->role != PX_ROLE_ENEMY && in->role != PX_ROLE_PLAYER && in->role != PX_ROLE_BONUS)
         continue;
      while (at && o->inst[order[at - 1]].y > in->y)
      {
         order[at] = order[at - 1];
         at--;
      }
      order[at] = (uint8_t)i;
      count++;
   }

   for (unsigned k = 0; k < count; k++)
   {
      const px_instance *in = &o->inst[order[k]];
      body *b = NULL;

      for (unsigned n = 0; n < g->body_count && !b; n++)
      {
         body *c = &g->bodies[n];
         if (c->role == in->role && c->cls == in->cls && c->color == in->color
               && c->ghost == (in->ghost != 0) && c->y1 == in->y
               && in->x - c->last_x <= 1 && c->last_x - in->x <= 1)
            b = c;
      }
      if (!b)
      {
         if (g->body_count >= BODIES)
            continue;
         b = &g->bodies[g->body_count++];
         memset(b, 0, sizeof(*b));
         b->x0    = in->x;
         b->y0    = in->y;
         b->x1    = (int16_t)(in->x + in->w);
         b->cls   = in->cls;
         b->role  = in->role;
         b->color = in->color;
         b->ghost = in->ghost != 0;
      }
      if (in->x < b->x0)
         b->x0 = in->x;
      if (in->x + (int)in->w > b->x1)
         b->x1 = (int16_t)(in->x + in->w);
      b->y1     = (int16_t)(in->y + in->h);
      b->last_x = in->x;
      b->rows   = (uint16_t)(b->rows + in->h);
      b->wide   = b->wide || in->w > 8;
      of[order[k]] = (uint8_t)(b - g->bodies);
   }

   /* What leaves at the bottom comes in at the top, and what leaves at the right comes in
    * at the left, behind the eight columns the game leaves black: the two pieces are one
    * rock. */
   for (unsigned n = 0; n < g->body_count; n++)
      for (unsigned m = 0; m < g->body_count; m++)
      {
         body *far = &g->bodies[n], *near = &g->bodies[m];
         if (n == m || far->role != PX_ROLE_ENEMY || near->role != PX_ROLE_ENEMY
               || far->cls != near->cls || far->color != near->color || far->ghost != near->ghost)
            continue;
         if (far->y1 >= SPACE_BOTTOM - 1 && near->y0 <= SPACE_TOP + 2 && far->y0 > near->y0
               && far->x0 - near->x0 <= 1 && near->x0 - far->x0 <= 1)
         {
            far->rows = near->rows = (uint16_t)(far->rows + near->rows);
            far->wide = near->wide = far->wide || near->wide;
         }
         else if (far->x1 >= PXC_W && near->x0 <= SPACE_LEFT && far->x0 > near->x0
               && far->y0 < near->y1 && near->y0 < far->y1)
         {
            far->rows = near->rows = far->rows > near->rows ? far->rows : near->rows;
            far->wide = near->wide = far->wide || near->wide || PXC_W - far->x0 + near->x1 > 9;
         }
      }

   for (unsigned n = 0; n < g->body_count; n++)
   {
      body *b = &g->bodies[n];
      b->kind = b->role == PX_ROLE_PLAYER ? KIND_SHIP : b->role == PX_ROLE_BONUS ? KIND_SAUCER
            : b->wide || b->rows > 16 ? KIND_LARGE : b->rows > 9 ? KIND_MEDIUM : KIND_SMALL;
   }
   for (unsigned i = 0; i < o->count; i++)
      if (of[i] != 0xFF && o->inst[i].role == PX_ROLE_ENEMY)
         o->inst[i].group = g->bodies[of[i]].kind;
}

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static const uint32_t stone[3] = { 0xA89C86, 0x8C94A4, 0x9A8672 };   /* small, medium, large */

/* Sixteenths of a turn to the left from straight up: where the ship's tail is from its
 * middle, in 256ths. */
static const int16_t turn_x[16] = { 0, 98, 181, 237, 256, 237, 181, 98, 0, -98, -181, -237, -256, -237, -181, -98 };
static const int16_t turn_y[16] = { 256, 237, 181, 98, 0, -98, -181, -237, -256, -237, -181, -98, 0, 98, 181, 237 };

static void put(px_scene *s, size_t i, uint32_t rgb, uint32_t light)
{
   if (s->sprite[i])
      s->sprite[i] = 0xFF000000u | (light & 0xFFFFFFu);
   if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE)
      s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
}

/* Whether a pixel is of a body. What is past the edges of space counts as of it: a body
 * the screen cuts off has no rim there. */
static bool same(const as *g, const px_scene *s, int x, int y, unsigned id)
{
   if (x < SPACE_LEFT || x >= PXC_W || y <= SPACE_TOP + 2 || y >= SPACE_BOTTOM - 1
         || y >= (int)s->frame->height)
      return true;
   return g->mask[(size_t)y * PXC_W + (size_t)x] == id;
}

/* What a body of stone or of metal has for a colour at a pixel: light from the upper left
 * on its rim, shade at the lower right, and dents that go where it goes. The game's blocks
 * are a pixel by two rows, and a pixel is nearly as wide as two rows are high. */
static uint32_t shade(const as *g, const px_scene *s, const body *b, unsigned id, int x, int y,
      uint32_t base)
{
   const bool lit  = !same(g, s, x - 1, y, id) || !same(g, s, x, y - 1, id) || !same(g, s, x, y - 2, id);
   const bool dark = !same(g, s, x + 1, y, id) || !same(g, s, x, y + 1, id) || !same(g, s, x, y + 2, id);
   /* Dents are counted from an edge that is there: not from one the screen's cuts off. */
   const int ax = b->x0 <= SPACE_LEFT ? b->x1 - x : x - b->x0;
   const int ay = b->y0 <= SPACE_TOP + 2 ? b->y1 - y : y - b->y0;

   if (lit && !dark)
      return px_rgb_mix(base, 0xFFF6E4, 120);
   if (dark && !lit)
      return px_rgb_scale(base, 110);
   if (lit)
      return base;
   if (!same(g, s, x - 2, y, id) || !same(g, s, x, y - 4, id))
      return px_rgb_mix(base, 0xFFF6E4, 44);
   if (!same(g, s, x + 2, y, id) || !same(g, s, x, y + 4, id))
      return px_rgb_scale(base, 176);
   if (b->kind <= KIND_LARGE && hash2((unsigned)ax, (unsigned)ay / 2u, b->color) < 40)
      return px_rgb_scale(base, 168);
   return base;
}

static void paint_bodies(as *g, px_scene *s, const uint8_t *of, int fate)
{
   const px_objects *o = s->objects;
   const uint32_t *palette = s->frame->palette;
   const unsigned beat = px_kit_wave(g->frame * 10);

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      const body *b;
      unsigned id;
      uint32_t base;

      if (of[i] == 0xFF)
         continue;
      b    = &g->bodies[of[i]];
      id   = of[i] + 1u;
      base = palette[in->color] & 0xFFFFFFu;
      switch (b->kind)
      {
         case KIND_SHIP:
            /* What is left of it while it explodes is on fire. */
            base = fate >= 0 && (fate & 0x80) ? 0xFFB050 : 0xB4C6E6;
            break;
         case KIND_SAUCER:
            base = px_rgb_add(0x38C060, px_rgb_scale(0x306030, beat));
            break;
         default:
            /* Of the size's stone, and a little of what the game made it. */
            base = px_rgb_mix(stone[b->kind], base, 56);
            break;
      }

      for (unsigned r = 0; r < in->h; r++)
      {
         const int y = in->y + (int)r;
         const uint32_t bits = o->bits[in->rows + r];
         if (y < 0 || y >= (int)s->frame->height)
            continue;
         for (unsigned c = 0; c < 32 && bits >> c; c++)
         {
            const int x = in->x + (int)c;
            size_t at;
            if (!((bits >> c) & 1) || x < 0 || x >= PXC_W)
               continue;
            at = (size_t)y * PXC_W + (size_t)x;
            if (g->mask[at] != id)
               continue;

            if (g->colors == COLORS_VECTOR)
            {
               /* The arcade drew lines on a black screen: what is within is left out. */
               if (!same(g, s, x - 1, y, id) || !same(g, s, x + 1, y, id)
                     || !same(g, s, x, y - 1, id) || !same(g, s, x, y + 1, id))
               {
                  put(s, at, 0xE4F0FF, 0xE4F0FF);
                  s->energy[at] = b->kind == KIND_SAUCER;
               }
               else
               {
                  if (PX_KEY_CLS(s->top[at]) == PX_CLS_SPRITE)
                     s->top[at] = PX_KEY(PX_CLS_BK, s->bk[at]);
                  s->sprite[at] = 0;
                  s->energy[at] = 0;
               }
            }
            else
            {
               const uint32_t rgb = shade(g, s, b, id, x, y, base);
               /* Stone gives little light of its own; the ship and the saucer do. */
               put(s, at, rgb, b->kind <= KIND_LARGE ? px_rgb_scale(rgb, 70) : rgb);
               if (b->kind == KIND_SAUCER || (b->kind == KIND_SHIP && fate >= 0 && (fate & 0x80)))
                  s->energy[at] = 1;
            }
         }
      }
   }
}

/* A flame behind the ship, of a length that flickers. */
static void paint_flame(as *g, px_scene *s, const px_instance *ship, unsigned turn)
{
   static const uint32_t fire[6] = { 0xFFF4C0, 0xFFD060, 0xFF9A30, 0xF06018, 0xC03810, 0x801C08 };
   const px_objects *o = s->objects;
   const unsigned length = 3 + (hash2(g->frame >> 1, 7, 3) % 3);
   int sum_x = 0, sum_y = 0, n = 0;

   for (unsigned r = 0; r < ship->h; r++)
      for (unsigned c = 0; c < 8; c++)
         if ((o->bits[ship->rows + r] >> c) & 1)
         {
            sum_x += (int)c;
            sum_y += (int)r;
            n++;
         }
   if (!n)
      return;

   for (unsigned k = 0; k < length; k++)
   {
      /* From behind the ship on, a pixel a step; a row is half a pixel. */
      const int far = 640 + (int)k * 256;
      const int x = ship->x + (sum_x * 256 / n + turn_x[turn & 15] * far / 256 + 128) / 256;
      const int y = ship->y + (sum_y * 256 / n + turn_y[turn & 15] * far / 128 + 128) / 256;
      const uint32_t rgb = g->colors == COLORS_VECTOR ? px_rgb_scale(0xE4F0FF, 256 - k * 40)
            : fire[k < 6 ? k : 5];

      for (int t = 0; t < 2; t++)
      {
         size_t at;
         if (x < SPACE_LEFT || x >= PXC_W || y + t < SPACE_TOP || y + t >= SPACE_BOTTOM
               || y + t >= (int)s->frame->height)
            continue;
         at = (size_t)(y + t) * PXC_W + (size_t)x;
         if (PX_KEY_CLS(s->top[at]) != PX_CLS_BK)
            continue;
         s->top[at]    = PX_KEY(PX_CLS_SPRITE, rgb);
         s->sprite[at] = 0xFF000000u | rgb;
         s->energy[at] = 1;
      }
   }
}

/* How far a place is from a body: pixels, a row counting half. */
static int apart(const body *b, int x, int y)
{
   const int dx = x < b->x0 ? b->x0 - x : x >= b->x1 ? x - b->x1 + 1 : 0;
   const int dy = y < b->y0 ? b->y0 - y : y >= b->y1 ? y - b->y1 + 1 : 0;
   return dx + dy / 2;
}

static uint32_t dust_of(const as *g, const px_scene *s, const body *b)
{
   const uint32_t own = s->frame->palette[b->color] & 0xFFFFFFu;
   switch (g->colors)
   {
      case COLORS_STONE:  return b->kind <= KIND_LARGE ? px_rgb_mix(own, 0xFFD8A0, 150) : 0xA0FFB0;
      case COLORS_VECTOR: return 0xE4F0FF;
      default:            return own;
   }
}

/* An explosion is heard: sparks where the shot was that hit, as many as what it hit was
 * large; or where the ship was, if it is the ship. */
static void burst(as *g, px_scene *s, int fate)
{
   static const struct { unsigned count, speed; } sparks[4] = {
      { 14, 260 }, { 24, 330 }, { 40, 420 }, { 34, 380 }
   };
   const body *hit = NULL;
   int best = 13, hit_x = 0, hit_y = 0;
   bool lost = fate >= 0 && g->fate >= 0 && (fate & 0x80) && !(g->fate & 0x80);

   for (unsigned k = 0; k < g->shot_count && !lost; k++)
      for (unsigned n = 0; n <= g->rock_count; n++)
      {
         const body *b = n < g->rock_count ? &g->rocks[n] : &g->saucer;
         int d;
         if (n == g->rock_count && !g->saucer_there)
            continue;
         d = apart(b, g->shots[k].x, g->shots[k].y);
         if (d < best)
         {
            best  = d;
            hit   = b;
            hit_x = g->shots[k].x;
            hit_y = g->shots[k].y;
         }
      }
   /* Without memory: no shot was near a rock, and the ship was. */
   if (!hit && !lost && fate < 0 && g->ship_x >= 0)
      for (unsigned n = 0; n < g->rock_count; n++)
         lost = lost || apart(&g->rocks[n], g->ship_x, g->ship_y) < 5;

   if (lost && g->ship_x >= 0)
   {
      px_scene_burst(s, g->ship_x, g->ship_y, g->colors == COLORS_VECTOR ? 0xE4F0FF : 0xFFE0A0, 60, 460);
      px_scene_flash(s, g->colors == COLORS_VECTOR ? 0xC0D0FF : 0xFF3820, 120);
   }
   else if (hit)
      px_scene_burst(s, hit_x, hit_y, dust_of(g, s, hit), sparks[hit->kind & 3].count,
            sparks[hit->kind & 3].speed);
}

/* What the frame has is kept for the frames to come: rocks of the rocks' frame, the ship,
 * its shots and the saucer of the ship's. */
static void keep(as *g, px_scene *s, bool ships)
{
   const px_objects *o = s->objects;

   if (!ships)
   {
      g->rock_count = 0;
      for (unsigned n = 0; n < g->body_count; n++)
         if (g->bodies[n].role == PX_ROLE_ENEMY && !g->bodies[n].ghost)
            g->rocks[g->rock_count++] = g->bodies[n];
      return;
   }

   g->shot_count   = 0;
   g->saucer_there = false;
   for (unsigned n = 0; n < g->body_count; n++)
   {
      const body *b = &g->bodies[n];
      if (b->ghost)
         continue;
      if (b->role == PX_ROLE_PLAYER)
      {
         g->ship_x = (b->x0 + b->x1) / 2;
         g->ship_y = (b->y0 + b->y1) / 2;
      }
      else if (b->role == PX_ROLE_BONUS)
      {
         g->saucer       = *b;
         g->saucer_there = true;
      }
   }
   for (unsigned i = 0; i < o->count; i++)
      if (o->inst[i].role == PX_ROLE_SHOT && !o->inst[i].ghost && g->shot_count < SHOTS)
      {
         g->shots[g->shot_count].x = o->inst[i].x;
         g->shots[g->shot_count].y = o->inst[i].y;
         g->shot_count++;
      }
}

static void frame(void *state, px_scene *s)
{
   as *g = (as*)state;
   px_objects *o = s->objects;
   const int fate = px_kit_ram(s->ram, s->ram_size, RAM_FATE);
   const int row  = px_kit_ram(s->ram, s->ram_size, RAM_ROW);
   const int ship = px_kit_ram(s->ram, s->ram_size, RAM_SHIP);
   uint8_t of[PX_MAX_INSTANCES];
   bool ships, game;

   if (s->advance)
   {
      g->frame++;
      px_kit_tia_hear(&g->seen, s->frame);
      px_kit_tags_begin(&g->known);
   }
   ships = ships_turn(g, o, px_kit_ram(s->ram, s->ram_size, RAM_FRAMES));
   game  = find_roles(g, s, ships);
   find_bodies(g, s, of);

   if (g->mask)
   {
      memset(g->mask, 0, (size_t)PXC_W * PXC_MAX_H);
      for (unsigned i = 0; i < o->count; i++)
      {
         const px_instance *in = &o->inst[i];
         if (of[i] == 0xFF)
            continue;
         for (unsigned r = 0; r < in->h; r++)
         {
            const int y = in->y + (int)r;
            const uint32_t bits = o->bits[in->rows + r];
            if (y < 0 || y >= (int)s->frame->height)
               continue;
            for (unsigned c = 0; c < 32 && bits >> c; c++)
               if (((bits >> c) & 1) && in->x + (int)c >= 0 && in->x + (int)c < PXC_W)
                  g->mask[(size_t)y * PXC_W + (size_t)(in->x + (int)c)] = (uint8_t)(of[i] + 1u);
         }
      }
   }

   if (game && g->colors != COLORS_ORIGINAL)
   {
      if (g->mask)
         paint_bodies(g, s, of, fate);
      for (unsigned i = 0; i < o->count; i++)
      {
         const px_instance *in = &o->inst[i];
         if (in->role == PX_ROLE_SHOT)
            px_scene_tint(s, in, g->colors == COLORS_VECTOR ? 0xFFFFFF : 0xC8F4FF);
         else if (in->role == PX_ROLE_BOMB)
            px_scene_tint(s, in, g->colors == COLORS_VECTOR ? 0xFFFFFF : 0xFF7A3A);
      }
      if (g->colors == COLORS_VECTOR)
         px_kit_playfield(s, 0, SPACE_TOP, 0xE4F0FF);
   }

   /* The thrust is heard for as long as the stick is up. */
   if (game && g->flame && ship >= 0 && !(fate >= 0 && (fate & 0x80)) && g->seen.volume[0]
         && g->seen.wave[0] == 8 && g->seen.pitch[0] == 8)
      for (unsigned i = 0; i < o->count; i++)
         if (o->inst[i].role == PX_ROLE_PLAYER)
         {
            paint_flame(g, s, &o->inst[i], (unsigned)ship & 15u);
            break;
         }

   if (s->advance && game && g->sparks)
   {
      if (g->seen.volume[0] > g->seen.was_volume[0] && g->seen.wave[0] == 8 && g->seen.pitch[0] == 31)
         burst(g, s, fate);
      /* Into hyperspace, and out of it where the ship is seen again. */
      if (row == NO_SHIP && g->row >= 0 && g->row != NO_SHIP && fate > 0 && !(fate & 0x80)
            && g->fate >= 0 && !(g->fate & 0x80) && g->ship_x >= 0)
      {
         px_scene_burst(s, g->ship_x, g->ship_y, g->colors == COLORS_VECTOR ? 0xE4F0FF : 0x80B8FF, 26, 240);
         g->away = true;
      }
      else if (g->away && row >= 0 && row != NO_SHIP)
      {
         for (unsigned n = 0; n < g->body_count; n++)
            if (g->bodies[n].role == PX_ROLE_PLAYER && !g->bodies[n].ghost)
            {
               const body *b = &g->bodies[n];
               px_scene_burst(s, (b->x0 + b->x1) / 2, (b->y0 + b->y1) / 2,
                     g->colors == COLORS_VECTOR ? 0xE4F0FF : 0x80B8FF, 26, 240);
               g->away = false;
            }
      }
   }

   if (s->advance)
   {
      if (game)
         keep(g, s, ships);
      else
         g->rock_count = g->shot_count = 0;
      px_kit_tags_end(&g->known);
      g->fate = fate;
      g->row  = row;
      if (!g->sparks || row < 0)
         g->away = false;
   }

   if (g->backdrop != BACKDROP_OFF)
      paint_backdrop(g, s);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has five, and the TIA's two voices for them:
 *
 *   voice 0   the thrust: noise (waveform 8) at pitch 8 and volume 6, for as long as the
 *             stick is up
 *   voice 0   an explosion: noise at pitch 31, the volume from 15 down by one every other
 *             frame. It is the same for a rock of any size, the saucer and the ship.
 *   voice 1   the ship's shot: waveforms 12 and 8 in turns, a frame each, the pitch from 1
 *             up to 14, at volume 13
 *   voice 1   the saucer: waveform 12 at pitches 14 and 16 in turns, a frame each, at
 *             volume 8, for as long as it is there
 *   voice 1   the heartbeat: waveform 6 at pitch 19, then at pitch 20, at volume 12: 14
 *             frames each and 24 frames apart when a game begins, shorter and closer as
 *             it goes on
 *
 * Voice 1 has one thing to say at a time: a shot cuts the heartbeat and the saucer short,
 * and a beat that would have begun while a shot sounds is heard late or not at all. Here
 * each is a sound of voices of its own, and the heart goes on beating under the shots, at
 * the pace it had. The notes of the heartbeat are the game's, 50.6 and 48.2 Hz, played by
 * voices that have them as their lowest and not, as the TIA's, as a buzz above them.
 *
 * Going to hyperspace and coming back from it the game makes no sound. What else it may
 * play is heard as it plays it.
 * ------------------------------------------------------------------------- */

enum { V0_NONE = 0, V0_THRUST, V0_BOOM, V0_OTHER };
enum { V1_NONE = 0, V1_SHOT, V1_SAUCER, V1_BEAT, V1_OTHER };

static unsigned voice0_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   if (!volume)
      return V0_NONE;
   if (wave == 8 && pitch == 8)
      return V0_THRUST;
   return wave == 8 && pitch == 31 ? V0_BOOM : V0_OTHER;
}

static unsigned voice1_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   if (!volume)
      return V1_NONE;
   if (volume > 10 && ((wave == 12 && pitch >= 1 && pitch <= 14)
         || (wave == 8 && (pitch == 2 || pitch == 4 || pitch == 6))))
      return V1_SHOT;
   if (wave == 12 && (pitch == 14 || pitch == 16))
      return V1_SAUCER;
   return wave == 6 && (pitch == 19 || pitch == 20) ? V1_BEAT : V1_OTHER;
}

static void play_shot(as *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave           freq  to   glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SQUARE, 1500, 190, 0.20f, 0.001f, 0.02f, 0.22f, 0.22f, 5000, 900, 0, 0 },
      { PX_WAVE_SAW,     760,  95, 0.20f, 0.001f, 0.01f, 0.18f, 0.20f, 2600, 500, 0, 0 },
      { PX_WAVE_NOISE,  8000,   0, 0.05f, 0,      0,     0.05f, 0.14f, 6000, 1500, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->ship_at));
   px_sound_rumble(s, 0, 12000, 3);
}

/* A rock breaks: the larger, the lower and the longer. */
static void play_rock(px_sound *s, unsigned kind, float pan)
{
   static const px_tone p[3][3] = {
      { { PX_WAVE_NOISE, 9000, 1500, 0.20f, 0,      0.01f, 0.24f, 0.36f, 7000, 900, 0, 0 },
        { PX_WAVE_SQUARE, 700,  160, 0.16f, 0.001f, 0.01f, 0.18f, 0.16f, 3500, 600, 0, 0 },
        { PX_WAVE_SINE,   190,   70, 0.10f, 0.001f, 0.01f, 0.18f, 0.44f, 0, 0, 0, 0 } },
      { { PX_WAVE_NOISE, 6000,  700, 0.30f, 0,      0.02f, 0.42f, 0.40f, 4500, 450, 0, 0 },
        { PX_WAVE_SQUARE, 420,   90, 0.25f, 0.001f, 0.02f, 0.30f, 0.18f, 2400, 400, 0, 0 },
        { PX_WAVE_SINE,   120,   42, 0.14f, 0.001f, 0.02f, 0.32f, 0.54f, 0, 0, 0, 0 } },
      { { PX_WAVE_NOISE, 3600,  300, 0.50f, 0,      0.04f, 0.80f, 0.42f, 2800, 200, 0, 0 },
        { PX_WAVE_SAW,    240,   45, 0.45f, 0.002f, 0.03f, 0.55f, 0.18f, 1400, 220, 0, 0 },
        { PX_WAVE_SINE,    78,   30, 0.22f, 0.002f, 0.04f, 0.60f, 0.60f, 0, 0, 0, 0 } }
   };
   static const struct { unsigned strong, weak, frames; } shake[3] = {
      { 12000, 24000, 6 }, { 26000, 30000, 10 }, { 44000, 30000, 18 }
   };
   if (kind > KIND_LARGE)
      kind = KIND_MEDIUM;
   if (pan > -2.0f)
      px_kit_play(s, p[kind], 3, pan);
   px_sound_rumble(s, shake[kind].strong, shake[kind].weak, shake[kind].frames);
}

static void play_saucer_hit(px_sound *s, float pan)
{
   static const px_tone p[3] = {
      { PX_WAVE_SAW,   1300,  70, 0.45f, 0.001f, 0.03f, 0.50f, 0.22f, 5000, 400, 23.0f, 0.05f },
      { PX_WAVE_NOISE, 6000, 500, 0.40f, 0,      0.03f, 0.55f, 0.36f, 4500, 350, 0, 0 },
      { PX_WAVE_SINE,   130,  36, 0.20f, 0.001f, 0.03f, 0.45f, 0.54f, 0, 0, 0, 0 }
   };
   if (pan > -2.0f)
      px_kit_play(s, p, 3, pan);
   px_sound_rumble(s, 34000, 36000, 14);
}

static void play_lost(px_sound *s, float pan)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 5000, 250, 1.1f, 0,      0.15f, 1.4f, 0.44f, 3600, 140, 0, 0 },
      { PX_WAVE_SINE,    72,  26, 1.0f, 0.002f, 0.10f, 1.3f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    380,  36, 0.9f, 0.002f, 0.05f, 1.0f, 0.20f, 2400, 180, 9.0f, 0.05f },
      { PX_WAVE_SQUARE, 900, 110, 0.3f, 0.001f, 0.02f, 0.3f, 0.10f, 3000, 500, 0, 0 }
   };
   if (pan > -2.0f)
      px_kit_play(s, p, 4, pan);
   px_sound_rumble(s, 65535, 45000, 50);
}

/* The game's two notes, a beat each: a thump that falls to the note, the note above it for
 * loudspeakers that have no 50 Hz, and the click of its beginning. */
static void play_beat(as *g, px_sound *s, unsigned pitch)
{
   const float f = px_kit_tia_hz(6, pitch);
   px_tone p[3] = {
      { PX_WAVE_SINE,     0, 0, 0.05f, 0.002f, 0.06f, 0.26f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 0, 0, 0,     0.003f, 0.04f, 0.18f, 0.20f, 500, 160, 0, 0 },
      { PX_WAVE_NOISE, 3000, 0, 0.02f, 0,      0,     0.02f, 0.08f, 1400, 300, 0, 0 }
   };
   p[0].freq     = f * 2.2f;
   p[0].freq_end = f;
   p[1].freq     = f * 2.0f;
   p[1].glide    = 0.12f;
   px_kit_play(s, p, 3, 0.0f);
   px_sound_rumble(s, 8000, 0, 3);
   g->beat_at    = g->clock;
   g->beat_pitch = (uint8_t)(pitch == 19 ? 20 : 19);
}

static void play_warp(px_sound *s, bool back, float pan)
{
   static const px_tone out[3] = {
      { PX_WAVE_SINE,   180, 2600, 0.40f, 0.01f, 0.20f, 0.25f, 0.20f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,     90, 1300, 0.40f, 0.01f, 0.20f, 0.25f, 0.10f, 600, 5000, 17.0f, 0.03f },
      { PX_WAVE_NOISE, 2000, 9000, 0.40f, 0.05f, 0.15f, 0.30f, 0.12f, 900, 7000, 0, 0 }
   };
   static const px_tone in[3] = {
      { PX_WAVE_SINE,  2600, 180, 0.35f, 0.01f, 0.15f, 0.30f, 0.20f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,   1300,  90, 0.35f, 0.01f, 0.15f, 0.30f, 0.10f, 5000, 600, 17.0f, 0.03f },
      { PX_WAVE_NOISE, 9000, 2000, 0.35f, 0.02f, 0.10f, 0.30f, 0.12f, 7000, 900, 0, 0 }
   };
   px_kit_play(s, back ? in : out, 3, pan);
   px_sound_rumble(s, 0, 20000, 8);
}

/* What goes on while the game's voice plays it: the thrust, and the saucer's hum. */
static void hold(px_sound *s, unsigned *ids, const px_tone *tones, unsigned count, bool on, float pan)
{
   for (unsigned i = 0; i < count; i++)
   {
      if (!on)
      {
         px_synth_stop(s->synth, ids[i], 0.12f);
         ids[i] = 0;
      }
      else if (!px_synth_move(s->synth, ids[i], pan, 1.0f, 0.0f))
         ids[i] = px_synth_play(s->synth, &tones[i], pan, 1.0f);
   }
}

/* Where between left and right the shot is that is nearest to a rock or the saucer, and
 * what that is: KIND_*. -1 if there is none. */
static int nearest(const px_objects *o, int *column)
{
   int best = 14, kind = -1;
   for (unsigned i = 0; o && i < o->count; i++)
   {
      const px_instance *shot = &o->inst[i];
      if (shot->role != PX_ROLE_SHOT)
         continue;
      for (unsigned k = 0; k < o->count; k++)
      {
         const px_instance *in = &o->inst[k];
         int dx, dy;
         if (in->role != PX_ROLE_ENEMY && in->role != PX_ROLE_BONUS)
            continue;
         dx = shot->x < in->x ? in->x - shot->x : shot->x >= in->x + (int)in->w ? shot->x - in->x - (int)in->w + 1 : 0;
         dy = shot->y < in->y ? in->y - shot->y : shot->y >= in->y + (int)in->h ? shot->y - in->y - (int)in->h + 1 : 0;
         if (dx + dy / 2 < best)
         {
            best    = dx + dy / 2;
            kind    = in->role == PX_ROLE_BONUS ? KIND_SAUCER : in->group;
            *column = shot->x;
         }
      }
   }
   return kind;
}

static void sound(void *state, px_sound *s)
{
   static const px_tone thrust[3] = {
      { PX_WAVE_NOISE, 1100, 0, 0, 0.03f, 0, 0, 0.30f, 520, 0, 0, 0 },
      { PX_WAVE_NOISE, 6000, 0, 0, 0.03f, 0, 0, 0.09f, 2600, 0, 0, 0 },
      { PX_WAVE_SAW,     55, 0, 0, 0.03f, 0, 0, 0.22f, 190, 0, 7.0f, 0.04f }
   };
   /* The saucer's two notes, 349 and 308 Hz, as the ends of one that wavers. */
   static const px_tone hum[2] = {
      { PX_WAVE_SQUARE, 328.0f, 0, 0, 0.02f, 0, 0, 0.17f, 1500, 0, 15.0f, 0.063f },
      { PX_WAVE_SINE,   164.0f, 0, 0, 0.02f, 0, 0, 0.22f, 0, 0, 15.0f, 0.063f }
   };
   as *g = (as*)state;
   const int fate = px_kit_ram(s->ram, s->ram_size, RAM_FATE);
   const int row  = px_kit_ram(s->ram, s->ram_size, RAM_ROW);
   const int tens = px_kit_ram(s->ram, s->ram_size, RAM_SCORE);
   const int more = px_kit_ram(s->ram, s->ram_size, RAM_SCORE_HI);
   const int score = tens < 0 || more < 0 ? -1
         : (more >> 4) * 1000 + (more & 15) * 100 + (tens >> 4) * 10 + (tens & 15);
   unsigned heard0, heard1;
   bool saucer = false;

   /* Where things are, from the picture before. */
   g->ship_at = g->saucer_at = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_PLAYER)
         g->ship_at = in->x + 4;
      else if (in->role == PX_ROLE_BONUS)
      {
         g->saucer_at = in->x + 4;
         saucer       = true;
      }
   }

   g->clock++;
   px_kit_tia_hear(&g->tia, s->frame);
   heard0 = voice0_plays(g->tia.wave[0], g->tia.pitch[0], g->tia.volume[0]);
   heard1 = voice1_plays(g->tia.wave[1], g->tia.pitch[1], g->tia.volume[1]);

   if (heard0 == V0_BOOM && (g->heard0 != V0_BOOM || px_kit_tia_louder(&g->tia, 0)))
   {
      int column = -1;
      int kind = nearest(s->objects, &column);
      const int gain = score >= 0 && g->score >= 0 ? (score - g->score + 10000) % 10000 : 0;
      /* Silent where the sounds are the game's: the controller shakes all the same. */
      const float pan = g->own_sound ? px_kit_pan(column) : -3.0f;

      /* What the score went up by says what was hit, where it is known. */
      if (gain == 2)
         kind = KIND_LARGE;
      else if (gain == 5)
         kind = KIND_MEDIUM;
      else if (gain == 10)
         kind = KIND_SMALL;
      else if (gain >= 20)
         kind = KIND_SAUCER;

      if (fate >= 0 && g->heard_fate >= 0 && (fate & 0x80) && !(g->heard_fate & 0x80))
         play_lost(s, g->own_sound ? px_kit_pan(g->ship_at) : -3.0f);
      else if (kind == KIND_SAUCER)
         play_saucer_hit(s, pan);
      else
         play_rock(s, kind < 0 ? KIND_MEDIUM : (unsigned)kind, pan);
   }

   if (heard1 == V1_SHOT && g->tia.pitch[1] == 1 && (g->heard1 != V1_SHOT || g->tia.was_pitch[1] != 1))
   {
      if (g->own_sound) play_shot(g, s);
      else              px_sound_rumble(s, 0, 12000, 3);
   }

   if (heard1 == V1_SHOT)
      g->shot_heard = g->clock;
   if (heard1 == V1_BEAT && (g->heard1 != V1_BEAT || g->tia.pitch[1] != g->tia.was_pitch[1]))
   {
      /* After a shot the voice is silent for a frame before it goes on with the beat. */
      if (g->clock - g->shot_heard > 2)
      {
         /* The game's own beat, on time. From the one before it is as far as beats are
          * apart; twice as far if it has the same note, as the one between them was not
          * heard for a shot. */
         uint32_t since = g->clock - g->beat_true;
         if (g->true_pitch == g->tia.pitch[1])
            since /= 2;
         if (g->beat_true && since >= 6 && since <= 40)
            g->beat_every = since;
         g->beat_true  = g->clock;
         g->true_pitch = g->tia.pitch[1];
         if (!g->own_sound)
            px_sound_rumble(s, 8000, 0, 3);
         else if (g->clock - g->beat_at >= 6)
            play_beat(g, s, g->tia.pitch[1]);
      }
      else if (g->own_sound && (!g->beat_every || g->clock - g->beat_at >= g->beat_every * 3 / 4))
         /* What is left of one a shot cut short, and it was not played when it was due. */
         play_beat(g, s, g->tia.pitch[1]);
   }
   else if (heard1 == V1_SHOT && g->own_sound && g->beat_every && g->beat_pitch
         && g->clock - g->beat_at >= g->beat_every && g->clock - g->beat_true < 3 * g->beat_every)
      /* Due while a shot has the game's voice. */
      play_beat(g, s, g->beat_pitch);

   if (g->own_sound && g->warp && row >= 0 && g->heard_row >= 0)
   {
      /* The count down without the ship having exploded before it. */
      if (row == NO_SHIP && g->heard_row != NO_SHIP && fate > 0 && !(fate & 0x80)
            && g->heard_fate >= 0 && !(g->heard_fate & 0x80))
      {
         play_warp(s, false, px_kit_pan(g->ship_at));
         g->heard_away = true;
      }
      else if (row != NO_SHIP && g->heard_row == NO_SHIP && g->heard_away)
      {
         play_warp(s, true, px_kit_pan(g->ship_at));
         g->heard_away = false;
      }
   }
   if (row == NO_SHIP && fate >= 0 && (fate & 0x80))
      g->heard_away = false;

   hold(s, g->thrust, thrust, 3, g->own_sound && heard0 == V0_THRUST, px_kit_pan(g->ship_at));
   /* A shot takes the saucer's voice, and the saucer is there all the same. */
   hold(s, g->hum, hum, 2, g->own_sound && (heard1 == V1_SAUCER || (heard1 == V1_SHOT && saucer && g->hum[0])),
         px_kit_pan(g->saucer_at));
   if (heard0 == V0_THRUST)
      px_sound_rumble(s, 0, 9000, 2);

   if (g->own_sound)
   {
      if (heard0 != V0_OTHER)
         s->voice[0] = 0.0f;
      if (heard1 != V1_OTHER)
         s->voice[1] = 0.0f;
   }

   g->heard0     = heard0;
   g->heard1     = heard1;
   g->score      = score;
   g->heard_fate = fate;
   g->heard_row  = row;
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   as *g = (as*)state;
   px_kit_tags_reset(&g->known);
   px_kit_tia_reset(&g->seen);
   px_kit_tia_reset(&g->tia);
   g->turn_known = false;
   g->body_count = g->rock_count = g->shot_count = 0;
   g->saucer_there = false;
   g->ship_x = g->ship_y = -1;
   g->fate = g->row = -1;
   g->away = false;

   g->heard0 = V0_NONE;
   g->heard1 = V1_NONE;
   memset(g->thrust, 0, sizeof(g->thrust));
   memset(g->hum, 0, sizeof(g->hum));
   g->clock = g->beat_at = g->beat_true = g->beat_every = g->shot_heard = 0;
   g->beat_pitch = g->true_pitch = 0;
   g->score = g->heard_fate = g->heard_row = -1;
   g->heard_away = false;
   g->ship_at = g->saucer_at = -1;
}

static void *create(void)
{
   as *g = (as*)calloc(1, sizeof(as));
   if (g)
   {
      g->mask  = (uint8_t*)calloc((size_t)PXC_W * PXC_MAX_H, 1);
      g->flame = g->sparks = g->own_sound = g->warp = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   as *g = (as*)state;
   if (g)
   {
      px_kit_canvas_free(&g->sky);
      free(g->mask);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   as *g = (as*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_pick(get, OPT_BACKDROP, backdrop);
   g->flame     = px_kit_on(get, OPT_FLAME);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->warp      = px_kit_on(get, OPT_WARP);
}

const px_game px_game_asteroids = {
   "Asteroids", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
