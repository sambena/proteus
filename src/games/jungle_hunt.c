/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Jungle Hunt (Atari, 1983).
 *
 * The explorer goes to the left through four scenes, which byte 9 of memory counts: 0 the
 * vines, 1 the river, 2 the boulders, 3 the cannibals and at the end of them the captive. A
 * rescue counts the time left into the score and begins the vines again.
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  15..73   the canopy: playfield, its lower edge in steps of four rows from row 58. The
 *                  background under it is the jungle's blue (the river has neither: the
 *                  playfield there is the colour of the sky, and shows nothing)
 *   rows  29..51   the score (both players' copies), the time left for a rescue, and the lives
 *                  as a star and a digit
 *   rows  58..77   the branches of six trees: both players, three copies each, in the colour
 *                  of wood. They move a pixel at a time as the explorer goes on
 *   the vines      rows 78..141: both missiles, set again on every row so that they slant.
 *                  Missile 0 has player 0's colour, so the rows of the vine the explorer hangs
 *                  on have his colours
 *   the explorer   player 0, 19 or 20 rows: a white helmet, a red face, a white shirt, the
 *                  belt as a gap, white shorts, red legs
 *   the ground     of the vines: rows 179..190 grass as playfield over the blue, rows 191..206
 *                  earth as playfield on a background of grass. Of the boulders and the
 *                  cannibals: rows 150..206, background alone
 *   the river      the sky to row 102, the waves the playfield in rows 95..102, the water the
 *                  background from row 103, the bed red playfield from row 181. Above the
 *                  water in rows 76..86 "DIVING" and the air left, both players. The swimmer
 *                  is two players side by side (a red head and arm, a white body); a crocodile
 *                  is two players too, a tail and a head with its eye and jaw at the right.
 *                  Swimmer and crocodiles take turns, a frame each
 *   boulders       player 1, 7 or 11 rows, rolling and bouncing along the ground
 *   cannibals      player 1, 22 rows: a skull and a spear, a brown body, a red cloth. The
 *                  captive after them is player 1 too, 17 rows: yellow hair, a green top, a
 *                  blue skirt
 *
 * Of its memory ($80 is 0):
 *
 *    3, 4     the score, in decimal digits (BCD): 4 has the hundreds
 *    6        the air, in its upper four bits: 9 is full, 0 is none; one less every 64
 *             frames under water
 *    7        the time left for a rescue, in tens
 *    8        the lives: 4 at the start, $FF when the game is over
 *    9        the scene, 0..3
 *   10        $80 for a frame when a scene begins
 *
 * What surprised:
 *
 *   - Voice 1 is never used: everything is on voice 0, and told by waveform and volume.
 *   - The scene can be chosen by writing 9 and $80 to 10 in the same frame, which is how the
 *     river, the boulders and the cannibals were studied.
 *   - Stella does not run the game the same way twice from power-on: runs are compared from
 *     a saved state.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_jh_colors"
#define OPT_BACKDROP "proteus_jh_backdrop"
#define OPT_SPARKS   "proteus_jh_sparks"
#define OPT_SOUND    "proteus_jh_sound"
#define OPT_AMBIENCE "proteus_jh_ambience"

#define RAM_SCORE   4
#define RAM_AIR     6
#define RAM_LIVES   8
#define RAM_SCENE   9

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define ROW_TOP       15     /* the first row of the picture the game draws */
#define ROW_HUD       52     /* the score, the time and the lives are above it */
#define ROW_BRANCHES  56
#define ROW_VINES     78     /* the branches end and the vines begin */
#define ROW_DIVING    74     /* "DIVING" and the air, to row 87 */
#define ROW_SWIM      88
#define ROW_SKY_FAR   150    /* the jungle's depth ends at about the ground of the boulders */
#define ROWS          207    /* what the module needs of a frame to know the game in it */

enum { SCENE_VINES = 0, SCENE_RIVER, SCENE_BOULDERS, SCENE_CANNIBALS, SCENE_NONE };

/* What an object is: a px_instance's group. */
enum
{
   KIND_NONE = 0, KIND_EXPLORER, KIND_SWIMMER_HEAD, KIND_SWIMMER_BODY, KIND_VINE, KIND_BRANCH,
   KIND_CROC_TAIL, KIND_CROC_HEAD, KIND_BOULDER, KIND_CANNIBAL, KIND_CAPTIVE, KIND_SCORE,
   KIND_DIVING, KIND_AIR
};

#define BUBBLES 40
#define MOTES   24

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "2bb9f4686f7e08c5fcc69ec1a1c66fe7",   /* Jungle Hunt (USA) */
   NULL
};

static const char *const fx[] = {
   "width", "50",
   "reverb", "room",
   NULL
};

static const char *const colors[] = { "jungle", "Jungle", "original", "The game's own", NULL };
static const char *const backdrop[] = { "jungle", "Jungle and river", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_JUNGLE = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Leaves, bark, vines, earth and water in shades, green crocodiles, boulders of lit stone, the cannibals and the captive in colours of their own, or the game's own colours.",
     "jungle", colors },
   { OPT_BACKDROP, "Backdrop",
     "The depth of the jungle with light coming through, a hillside of earth and stone, the river's far bank and water that is deeper the lower it is, with bubbles.",
     "jungle", backdrop },
   { OPT_SPARKS, "Explosions",
     "Leaves where a vine is caught, a flash where a crocodile is stabbed, dust where a boulder lands, a splash where the explorer dives, and light at the rescue.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the swing, the catch, the swimming, the knife, the boulders, a crocodile killed, the air running low and the rescue, where each happens between left and right, and the game's two tunes played with other voices. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_AMBIENCE, "Jungle sounds",
     "Birds and crickets in the jungle, and the hush of the river under water. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

/* What voice 0 plays. */
enum
{
   HEARD_NONE = 0, HEARD_TUNE, HEARD_CATCH, HEARD_JUMP, HEARD_SWIM, HEARD_DIVE, HEARD_STAB,
   HEARD_DROWN, HEARD_BOUNCE, HEARD_DRUM, HEARD_TALLY, HEARD_OTHER
};

/* Where the rows of a scene are, found in every frame. */
typedef struct
{
   unsigned ground;     /* of the jungle: the ground's first row */
   unsigned surface;    /* of the river: the waves' first row */
   unsigned water;      /* the water's first row */
   unsigned bed;        /* the river bed's first row */
   uint8_t sky;         /* the sky's colour, an index into the palette */
   bool river;
} layout;

typedef struct
{
   uint16_t x, y;       /* in the picture, in 16ths of a pixel */
   uint8_t size, phase;
   bool alive;
} bubble;

typedef struct
{
   uint16_t x, y;
   uint8_t phase, pace, size;
} mote;

typedef struct
{
   /* The options. */
   unsigned colors;
   bool backdrop, sparks, own_sound, ambience;

   uint32_t frame;               /* counts the frames that advance */
   unsigned scene;               /* SCENE_* of the frame at hand */
   layout at;

   /* How far the scenery has moved to the right since the reset, in captured pixels, and
    * where the backdrop that follows it is. */
   int32_t scroll;
   int32_t shown;                /* in 16ths: the water's backdrop comes after the bed's steps */
   uint8_t bed_bits[16][PXC_W];  /* the bed's playfield in the frame before */
   bool bed_known;

   /* The picture. */
   px_kit_tia seen;              /* the game's voices as the picture follows them */
   unsigned seen0;
   int lives, score, air;        /* as memory had them; -1: not known */
   int hero_x, hero_y, hero_h;   /* the explorer or the swimmer; -1: not seen */
   bool under;                   /* the swimmer is under water */
   unsigned since_tally;         /* frames since the time was counted into the score */
   unsigned since_drown;         /* and since the air ran out */
   int captive_x, captive_y;

   px_kit_canvas jungle, ground, bank, water;
   bool painted_jungle, painted_ground, painted_bank, painted_water;
   bubble bubbles[BUBBLES];
   mote motes[MOTES];
   uint32_t seed;

   /* The sound. */
   px_kit_tia tia;
   unsigned heard0;
   unsigned tune_low, tune_high; /* the voices at the synth that play the game's tune */
   unsigned hush;                /* the river's hush under water */
   int hero_at;                  /* the explorer's column, for where a sound is; -1: not known */
   int boulder_at, croc_at;
   bool hero_under;
   int s_lives, s_score, s_air;  /* memory as the sound saw it */
   unsigned s_scene, s_since_tally, s_since_drown, s_air_wait, strokes;
   uint32_t beat;                /* counts the frames that are heard */
   uint32_t chance;
   unsigned wait_bird, wait_cricket, wait_drip;
   unsigned call, call_left, call_wait;
   float call_pan, call_pitch;
} jh;

/* ---------------------------------------------------------------------------
 * Chance that is smooth, and goes round the picture's width
 * ------------------------------------------------------------------------- */

static unsigned hash(int x, int y, uint32_t seed)
{
   uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
   h = (h ^ (h >> 13)) * 1274126177u;
   return (h ^ (h >> 16)) & 255;
}

/* 0..255, smooth: x and y in 256ths of cells, `across` cells to the width of the picture, so
 * that what leaves it at one side fits what comes in at the other. */
static unsigned noise(int x, int y, uint32_t seed, int across)
{
   const int xi = x >> 8, yi = y >> 8;
   const int x0 = ((xi % across) + across) % across, x1 = (x0 + 1) % across;
   unsigned fx = (unsigned)x & 255, fy = (unsigned)y & 255;
   unsigned top, bottom;
   fx     = fx * fx * (768 - 2 * fx) >> 16;
   fy     = fy * fy * (768 - 2 * fy) >> 16;
   top    = hash(x0, yi, seed) * (256 - fx) + hash(x1, yi, seed) * fx;
   bottom = hash(x0, yi + 1, seed) * (256 - fx) + hash(x1, yi + 1, seed) * fx;
   return (top * (256 - fy) + bottom * fy) >> 16;
}

/* A column of the game's 160 put back into them. */
static int wrap(int x)
{
   return ((x % PXC_W) + PXC_W) % PXC_W;
}

static void blend(uint32_t *p, uint32_t rgb, unsigned alpha)
{
   *p = px_rgb_mix(*p, rgb, alpha > 256 ? 256 : alpha);
}

/* ---------------------------------------------------------------------------
 * The scenes' rows
 * ------------------------------------------------------------------------- */

static uint8_t bk_at(const px_scene *s, unsigned x, unsigned y)
{
   return s->frame->color[PXC_L_BK][(size_t)y * PXC_W + x];
}

static bool pf_at(const px_scene *s, unsigned x, unsigned y)
{
   return (s->frame->tags[(size_t)y * PXC_W + x] & (PXC_PF | PXC_BLANK)) == PXC_PF;
}

static bool row_has_pf(const px_scene *s, unsigned y)
{
   for (unsigned x = 8; x < PXC_W; x += 2)
      if (pf_at(s, x, y))
         return true;
   return false;
}

static void find_layout(const px_scene *s, unsigned scene, layout *l)
{
   const unsigned h = s->frame->height < ROWS ? s->frame->height : ROWS;
   memset(l, 0, sizeof(*l));
   l->ground = l->surface = l->water = l->bed = h;
   if (scene == SCENE_RIVER)
   {
      l->river = true;
      l->sky   = bk_at(s, 80, 60);
      for (unsigned y = 60; y < h; y++)
         if (bk_at(s, 80, y) != l->sky && bk_at(s, 20, y) != l->sky)
         {
            l->water = y;
            break;
         }
      for (unsigned y = ROW_SWIM; y < l->water; y++)
         if (row_has_pf(s, y))
         {
            l->surface = y;
            break;
         }
      for (unsigned y = l->water + 20; y < h; y++)
         if (row_has_pf(s, y))
         {
            l->bed = y;
            break;
         }
      return;
   }
   l->sky = bk_at(s, 80, 100);
   for (unsigned y = 100; y < h; y++)
      if (row_has_pf(s, y) || bk_at(s, 80, y) != l->sky)
      {
         l->ground = y;
         break;
      }
}

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

/* The jungle behind the trees: haze that is lighter below, trunks in three depths, lianas, the
 * brush on the ground, and the light that comes through the canopy. */
static void paint_jungle(jh *g, const px_scene *s)
{
   static const struct
   {
      int cell, wide, foot;
      unsigned alpha;
      uint32_t trunk;
   } depth[3] = {
      { 16, 2, 150, 70,  0x2C5244 },
      { 32, 3, 160, 120, 0x223C2C },
      { 40, 6, 176, 170, 0x16261A }
   };
   const unsigned w = g->jungle.w, h = g->jungle.h;
   uint32_t *p = g->jungle.pixels;

   for (unsigned Y = 0; Y < h; Y++)
   {
      const int y = (int)(Y * 256u / s->sy), row = y >> 8;
      const unsigned down = row < ROW_BRANCHES ? 0 : row >= ROW_SKY_FAR ? 256
            : (unsigned)(y - ROW_BRANCHES * 256) / (ROW_SKY_FAR - ROW_BRANCHES);
      const uint32_t haze = px_rgb_mix(0x0E2E26, 0x6C9C66, down);
      for (unsigned X = 0; X < w; X++)
      {
         const int x = (int)(X * 256u / s->sx);
         const unsigned leaves = noise(x / 20, y / 12, 1, 8);
         p[(size_t)Y * w + X] = px_rgb_scale(haze, 204 + (leaves * 52 >> 8));
      }
   }

   /* Lianas that hang far off. */
   for (unsigned X = 0; X < w; X++)
   {
      const int x = (int)(X * 256u / s->sx), col = x >> 8;
      const unsigned k = hash(col / 3, 0, 40);
      if (col % 3 || k % 5 || (x & 255) >= 128)
         continue;
      for (unsigned Y = ROW_BRANCHES * s->sy; Y < (ROW_BRANCHES + 30 + k % 60) * s->sy && Y < h; Y++)
         blend(p + (size_t)Y * w + X, 0x1C3E24, 90);
   }

   for (unsigned d = 0; d < 3; d++)
   {
      const int cells = PXC_W / depth[d].cell;
      for (unsigned X = 0; X < w; X++)
      {
         const int x = (int)(X * 256u / s->sx);
         const int cell = (x >> 8) / depth[d].cell;
         const int from = (cell * depth[d].cell + (int)(hash(cell % cells, (int)d, 7)
               % (unsigned)(depth[d].cell - depth[d].wide))) * 256;
         const int in = x - from, wide = depth[d].wide * 256;
         const unsigned foot = (depth[d].foot * 256u - noise(x / 8, (int)d, 8, PXC_W / 8) * 6u) * s->sy >> 8;
         if (in < 0 || in >= wide || !(hash(cell % cells, (int)d, 10) % 4))
            continue;
         {
            /* Round, and lit from the right where the light comes from. */
            const int edge = in < wide - in ? in : wide - in;
            const unsigned alpha = edge >= 160 ? depth[d].alpha : depth[d].alpha * (unsigned)(edge + 96) / 256;
            const uint32_t rgb = in > wide * 2 / 3 ? px_rgb_add(depth[d].trunk, 0x0C0E06) : depth[d].trunk;
            for (unsigned Y = ROW_BRANCHES * s->sy; Y < foot && Y < h; Y++)
               blend(p + (size_t)Y * w + X, rgb, alpha);
         }
      }
   }

   /* Ferns at the foot of the trees: fronds that rise from the ground, darker than the haze. */
   for (unsigned X = 0; X < w; X++)
   {
      const int x = (int)(X * 256u / s->sx);
      const unsigned tall = noise(x / 5, 0, 30, PXC_W / 5) * 18u + noise(x, 0, 31, PXC_W) * 6u;
      const unsigned top = (ROW_SKY_FAR * 256u - tall) * s->sy >> 8;
      for (unsigned Y = top; Y < h; Y++)
      {
         const unsigned into = (Y - top) * 256u / (s->sy * 8u);
         const unsigned frond = noise(x * 2, (int)(Y * 256u / s->sy) / 3, 32, PXC_W * 2);
         blend(p + (size_t)Y * w + X, frond > 128 ? 0x2A5A28 : 0x1C4020, into > 90 ? 110 : into + 20);
      }
   }

   /* The light that falls through the canopy from the upper right. */
   for (unsigned Y = ROW_BRANCHES * s->sy; Y < h; Y++)
   {
      const int y = (int)(Y * 256u / s->sy) - ROW_BRANCHES * 256;
      const unsigned fade = y < 0 ? 0 : y > 110 * 256 ? 40 : 256 - (unsigned)y / 128;
      for (unsigned X = 0; X < w; X++)
      {
         const int x = (int)(X * 256u / s->sx);
         const unsigned beam = noise((x + y * 5 / 8) / 10, 0, 2, 16);
         if (beam > 150)
            p[(size_t)Y * w + X] = px_rgb_add(p[(size_t)Y * w + X],
                  px_rgb_scale(0x4C4620, (beam - 150) * 2 * fade >> 8));
      }
   }
}

/* The ground of the boulders and the cannibals: grass at the top, then earth in layers with
 * stones in it. It is painted for rows from ROW_SKY_FAR on. */
static void paint_ground(jh *g, const px_scene *s)
{
   const unsigned w = g->ground.w, h = g->ground.h;
   uint32_t *p = g->ground.pixels;

   memset(p, 0, (size_t)w * h * sizeof(uint32_t));
   for (unsigned Y = ROW_SKY_FAR * s->sy; Y < h; Y++)
   {
      const int y = (int)(Y * 256u / s->sy), d = y - ROW_SKY_FAR * 256;
      for (unsigned X = 0; X < w; X++)
      {
         const int x = (int)(X * 256u / s->sx);
         const int tuft = (int)(noise(x / 4, 0, 20, PXC_W / 4) * 5u) - 256 + (int)(hash((int)X, 0, 21) % 3) * 64;
         uint32_t rgb;
         if (d < 4 * 256 + tuft)
         {
            /* Grass, in blades. */
            const unsigned blade = hash((int)(X * 2 / (s->sx ? s->sx : 1)), 1, 22);
            rgb = px_rgb_scale(px_rgb_mix(0x5CA83A, 0x2E6A22, d > 0 ? (unsigned)d / 20 : 0), 190 + (blade * 66 >> 8));
         }
         else
         {
            const unsigned deep = d > 56 * 256 ? 256 : (unsigned)d / 56;
            const unsigned layers = noise(x / 20, y / 3, 23, PXC_W / 20);
            const unsigned grain = hash((int)X / 2, (int)Y / 2, 24);
            /* Game pixels are twice as wide as high: cells of 2 by 4 rows are round. */
            const unsigned stone = noise(x / 2, y / 4, 25, PXC_W / 2);
            rgb = px_rgb_scale(px_rgb_mix(0x8A6036, 0x3A2412, deep), 180 + (layers * 60 >> 8) + (grain * 20 >> 8));
            if (stone > 200)
            {
               /* A stone, lit from above, its foot in shadow. */
               const unsigned above = noise(x / 2, (y - 128) / 4, 25, PXC_W / 2);
               const uint32_t face = above + 4 < stone ? 0x9A8C78 : above > stone + 4 ? 0x4E4438 : 0x76695A;
               rgb = px_rgb_mix(rgb, face, (stone - 200) * 6 > 230 ? 230 : (stone - 200) * 6);
            }
            else if (stone > 186)
               rgb = px_rgb_scale(rgb, 200);
         }
         p[(size_t)Y * w + X] = rgb;
      }
   }
}

/* Above the river: the sky, a cloud or two, and the jungle on the far bank. */
static void paint_bank(jh *g, const px_scene *s)
{
   const unsigned w = g->bank.w, h = g->bank.h;
   uint32_t *p = g->bank.pixels;

   for (unsigned Y = 0; Y < h; Y++)
   {
      const int y = (int)(Y * 256u / s->sy), row = y >> 8;
      const unsigned down = row < ROW_TOP ? 0 : row >= 104 ? 256 : (unsigned)(y - ROW_TOP * 256) / (104 - ROW_TOP);
      const uint32_t sky = px_rgb_mix(0x1E4C8E, 0xB6D2DC, down * down >> 8);
      for (unsigned X = 0; X < w; X++)
      {
         const int x = (int)(X * 256u / s->sx);
         const unsigned cloud = noise(x / 16, y / 6, 60, PXC_W / 16);
         const int crest = (70 * 256 + (int)noise(x / 10, 0, 61, PXC_W / 10) * 14
               + (int)noise(x / 4, 0, 62, PXC_W / 4) * 4);
         uint32_t rgb = sky;
         if (cloud > 170 && row < 60)
            rgb = px_rgb_mix(rgb, 0xEEF2F4, (cloud - 170) * 2 * (unsigned)(60 - row) / 60);
         if (y >= crest)
         {
            /* The far trees, hazy, lit at their tops. */
            const unsigned into = (unsigned)(y - crest) >> 8;
            const unsigned leaf = noise(x / 4, y / 4, 63, PXC_W / 4);
            rgb = px_rgb_mix(sky, into < 2 ? 0x5C8A58 : 0x2A5436, 190);
            rgb = px_rgb_scale(rgb, 200 + (leaf * 56 >> 8));
         }
         p[(size_t)Y * w + X] = rgb;
      }
   }
}

/* The river under water: deeper and darker below, light that plays on it near the top, and
 * shafts of it going down. */
static void paint_water(jh *g, const px_scene *s)
{
   const unsigned w = g->water.w, h = g->water.h, top = 100;
   uint32_t *p = g->water.pixels;

   for (unsigned Y = 0; Y < h; Y++)
   {
      const int y = (int)(Y * 256u / s->sy), row = y >> 8;
      const unsigned deep = row <= (int)top ? 0 : row >= 206 ? 256 : (unsigned)(y - (int)top * 256) / (206 - top);
      const uint32_t base = px_rgb_mix(0x2A8494, 0x04182E, deep < 256 ? (deep * 2 - (deep * deep >> 8)) : 256);
      for (unsigned X = 0; X < w; X++)
      {
         const int x = (int)(X * 256u / s->sx);
         const unsigned a = noise(x / 8, y / 5, 50, PXC_W / 8), b = noise(x / 16, y / 7, 51, PXC_W / 16);
         const unsigned ridge = a > b ? 255 - (a - b) * 4 : 255 - (b - a) * 4;
         const unsigned beam = noise((x - (y - (int)top * 256) * 3 / 8) / 10, 0, 52, PXC_W / 10);
         uint32_t rgb = base;
         if (ridge < 256 && ridge > 196)
            rgb = px_rgb_add(rgb, px_rgb_scale(0x2C5E60, (ridge - 196) * 3 * (256 - deep) >> 8));
         if (beam > 140)
            rgb = px_rgb_add(rgb, px_rgb_scale(0x183640, (beam - 140) * 2 * (256 - deep) >> 8));
         p[(size_t)Y * w + X] = rgb;
      }
   }
}

static void put_dot(px_scene *s, unsigned X, unsigned Y, unsigned size, uint32_t rgb)
{
   for (unsigned v = 0; v < size; v++)
      for (unsigned u = 0; u < size; u++)
         if (X + u < s->w && Y + v < s->h)
         {
            uint32_t *o = s->backdrop + (size_t)(Y + v) * s->w + X + u;
            /* Round: the corners less. */
            const bool corner = size > 2 && (u == 0 || u == size - 1) && (v == 0 || v == size - 1);
            *o = px_rgb_add(*o, corner ? px_rgb_scale(rgb, 90) : rgb);
         }
}

static void add_bubbles(jh *g, const px_scene *s, int x, int y, unsigned count)
{
   for (unsigned i = 0; i < BUBBLES && count; i++)
   {
      bubble *b = &g->bubbles[i];
      if (b->alive)
         continue;
      b->alive = true;
      b->x     = (uint16_t)(((unsigned)wrap(x) * s->sx + px_kit_chance(&g->seed) % (s->sx * 3 + 1)) * 16u);
      b->y     = (uint16_t)(((unsigned)(y < 0 ? 0 : y) * s->sy) * 16u);
      b->size  = (uint8_t)(1 + px_kit_chance(&g->seed) % (s->sy > 2 ? 3 : 2));
      b->phase = (uint8_t)px_kit_chance(&g->seed);
      count--;
   }
}

/* Bubbles rise to the surface, wavering; motes drift in the light of the jungle. */
static void move_bits(jh *g, px_scene *s)
{
   if (g->at.river)
   {
      const unsigned surface = g->at.water * s->sy * 16u;
      for (unsigned i = 0; i < BUBBLES; i++)
      {
         bubble *b = &g->bubbles[i];
         unsigned X;
         if (!b->alive)
            continue;
         if (s->advance)
         {
            b->y = (uint16_t)(b->y > (8u + b->size * 6u) * s->sy / 2 ? b->y - (8u + b->size * 6u) * s->sy / 2 : 0);
            b->phase += 9;
         }
         if (b->y <= surface)
         {
            b->alive = false;
            continue;
         }
         X = (b->x / 16u + (px_kit_wave(b->phase) * s->sx >> 8)) % s->w;
         put_dot(s, X, b->y / 16u, b->size * (s->sx > 3 ? 2 : 1), 0x3A6A70);
      }
      return;
   }
   for (unsigned i = 0; i < MOTES; i++)
   {
      mote *m = &g->motes[i];
      const unsigned wave = px_kit_wave(m->phase + g->frame * m->pace);
      const unsigned top = ROW_VINES * s->sy * 16u, span = (g->at.ground - ROW_VINES - 4) * s->sy * 16u;
      if (!m->size)
      {
         m->size  = (uint8_t)(1 + px_kit_chance(&g->seed) % (s->sy > 2 ? 3 : 2));
         m->x     = (uint16_t)(px_kit_chance(&g->seed) % (s->w * 16u));
         m->y     = (uint16_t)(px_kit_chance(&g->seed) % (span ? span : 1));
         m->phase = (uint8_t)px_kit_chance(&g->seed);
         m->pace  = (uint8_t)(1 + px_kit_chance(&g->seed) % 3);
      }
      if (s->advance && span)
      {
         m->x = (uint16_t)((m->x + s->w * 16u + (wave >> 6) - 1u) % (s->w * 16u));
         m->y = (uint16_t)((m->y + 1u + (m->pace & 1)) % span);
      }
      if (span)
         put_dot(s, m->x / 16u, (top + m->y) / 16u, m->size, px_rgb_scale(0xFFF0B0, 30 + (wave * 70 >> 8)));
   }
}

static bool fit(px_kit_canvas *c, bool *painted, const px_scene *s)
{
   if (px_kit_canvas_fit(c, s))
      *painted = false;
   return c->pixels != NULL;
}

static void paint_backdrop(jh *g, px_scene *s)
{
   const unsigned w = s->w, sy = s->sy;
   const int32_t near = g->scroll * (int32_t)s->sx, far = near / 3;

   /* All four are painted at once, when the picture gets its size: a scene that begins is
    * not held up. */
   if (!fit(&g->jungle, &g->painted_jungle, s) || !fit(&g->ground, &g->painted_ground, s)
         || !fit(&g->bank, &g->painted_bank, s) || !fit(&g->water, &g->painted_water, s))
      return;
   if (!g->painted_jungle)
      paint_jungle(g, s);
   if (!g->painted_ground)
      paint_ground(g, s);
   if (!g->painted_bank)
      paint_bank(g, s);
   if (!g->painted_water)
      paint_water(g, s);
   g->painted_jungle = g->painted_ground = g->painted_bank = g->painted_water = true;

   if (g->at.river)
   {
      int32_t water;
      /* The water drifts with the river as well as moving with the bed. */
      water = g->shown * (int32_t)s->sx / 16 + (int32_t)(g->frame * s->sx / 5);
      for (unsigned Y = 0; Y < s->h; Y++)
      {
         if (Y < ROW_TOP * sy || Y >= ROWS * sy)
            memset(s->backdrop + (size_t)Y * w, 0, w * sizeof(uint32_t));
         else if (Y < g->at.water * sy)
            px_kit_canvas_roll(&g->bank, s->backdrop + (size_t)Y * w, Y, 0, w, g->shown * (int32_t)s->sx / 64, 0);
         else
            px_kit_canvas_roll(&g->water, s->backdrop + (size_t)Y * w, Y, 0, w, water, 0);
      }
   }
   else
   {
      for (unsigned Y = 0; Y < s->h; Y++)
      {
         /* The ground of the boulders and the cannibals begins at row ROW_SKY_FAR, as the
          * canvas does; that of the vines is the game's own. */
         if (Y < ROW_TOP * sy || Y >= ROWS * sy)
            memset(s->backdrop + (size_t)Y * w, 0, w * sizeof(uint32_t));
         else if (Y >= g->at.ground * sy && g->scene != SCENE_VINES)
            px_kit_canvas_roll(&g->ground, s->backdrop + (size_t)Y * w, Y + (ROW_SKY_FAR - g->at.ground) * sy,
                  0, w, near, 0);
         else
            px_kit_canvas_roll(&g->jungle, s->backdrop + (size_t)Y * w, Y, 0, w, far, 0);
      }
   }
   move_bits(g, s);
   s->backdrop_on = true;
}

/* ---------------------------------------------------------------------------
 * The scenery
 * ------------------------------------------------------------------------- */

/* The canopy's colour at a pixel: darker at the top, in clumps of leaves, lit at its lower
 * edge. */
static uint32_t leaves_at(int x, unsigned y, bool edge)
{
   const unsigned down = y < ROW_TOP ? 0 : (y - ROW_TOP) * 256u / (ROW_VINES - ROW_TOP);
   const unsigned clump = noise(wrap(x) * 256 / 8, (int)y * 256 / 4, 5, PXC_W / 8);
   const unsigned much = y < ROW_HUD ? 48 : 80;
   uint32_t rgb = px_rgb_scale(px_rgb_mix(0x0E2C10, 0x2E7424, down > 256 ? 256 : down), 176 + (clump * much >> 8));
   if (edge)
      rgb = px_rgb_add(rgb, 0x1E3A0C);
   return rgb;
}

/* The ground of the vines: grass and the earth under it. */
static uint32_t grass_at(int x, unsigned y)
{
   const unsigned blade = hash(wrap(x), (int)y / 3, 70);
   return px_rgb_scale(px_rgb_mix(0x58A436, 0x2E6420, (y & 3) * 64), 200 + (blade * 56 >> 8));
}

static uint32_t earth_at(int x, unsigned y)
{
   const unsigned grain = noise(wrap(x) * 256 / 4, (int)y * 256 / 2, 71, PXC_W / 4);
   return px_rgb_scale(px_rgb_mix(0x7A5230, 0x46301A, y > 190 ? (y - 190) * 16 : 0), 190 + (grain * 66 >> 8));
}

/* The river bed: stones and sand, blue with the water above them, the tops of the stones lit. */
static uint32_t bed_at(int x, unsigned y, unsigned bed, bool edge)
{
   const unsigned stone = noise(wrap(x) * 256 / 4, (int)y * 256 / 3, 72, PXC_W / 4);
   const unsigned deep = y > bed ? (y - bed) * 10 : 0;
   uint32_t rgb = px_rgb_mix(0xB88E5E, 0x6A4C34, stone);
   rgb = px_rgb_mix(rgb, 0x163A48, 36 + (deep > 90 ? 90 : deep));
   if (edge)
      rgb = px_rgb_add(rgb, 0x302818);   /* lit from above */
   return px_rgb_scale(rgb, 216 + (hash(wrap(x), (int)y, 73) * 40 >> 8));
}

/* The waves: the surface of the river, light moving along it. */
static uint32_t wave_at(const jh *g, unsigned x, unsigned y, bool crest)
{
   const unsigned play = px_kit_wave(x * 9 + g->frame * 3 + y * 40 + px_kit_wave(x * 5 + g->frame) / 3);
   uint32_t rgb = px_rgb_mix(0x2A7890, 0x5AAAC0, play);
   if (crest)
      rgb = px_rgb_add(rgb, 0x284848);
   if (play > 200)
      rgb = px_rgb_add(rgb, px_rgb_scale(0x80C8D0, (play - 200) * 4));
   return rgb;
}

static void paint_scenery(jh *g, px_scene *s)
{
   const unsigned h = s->frame->height < ROWS ? s->frame->height : ROWS;
   const int shift = g->scroll;

   for (unsigned y = ROW_TOP; y < h; y++)
   {
      uint32_t *top = s->top + (size_t)y * PXC_W;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         const bool pf = PX_KEY_CLS(top[x]) == PX_CLS_PF;
         const bool bk = PX_KEY_CLS(top[x]) == PX_CLS_BK;
         if (!pf && !bk)
            continue;
         if (g->at.river)
         {
            if (pf && y >= g->at.surface && y < g->at.water)
            {
               const bool crest = y == 0 || !(s->frame->tags[i - PXC_W] & PXC_PF);
               top[x] = PX_KEY(PX_CLS_PF, wave_at(g, x, y, crest));
               s->light[i] = 0xFF000000u | 0x0E2C34;
            }
            else if (pf && y >= g->at.bed)
               top[x] = PX_KEY(PX_CLS_PF, bed_at((int)x - g->shown / 16, y, g->at.bed,
                     !(s->frame->tags[i - PXC_W] & PXC_PF)));
            continue;
         }
         if (pf && y < ROW_VINES)
         {
            const bool edge = y + 1 < h && !(s->frame->tags[i + PXC_W] & PXC_PF);
            top[x] = PX_KEY(PX_CLS_PF, leaves_at((int)x - shift, y, edge));
         }
         else if (y >= g->at.ground && g->scene == SCENE_VINES)
         {
            /* Grass is the playfield over the blue and the background under the earth; the
             * earth is the playfield of the other colour. */
            const uint8_t c = pf ? s->frame->color[PXC_L_PF][i] : bk_at(s, x, y);
            if (bk && c == g->at.sky)
               continue;
            if (pf && y > g->at.ground + 11 && c != bk_at(s, x, y))
               top[x] = PX_KEY(PX_CLS_PF, earth_at((int)x - shift, y));
            else if (pf && y >= g->at.ground + 11 && c == bk_at(s, x, y))
               top[x] = PX_KEY(PX_CLS_PF, earth_at((int)x - shift, y));
            else
               top[x] = PX_KEY(PX_CLS_PF, grass_at((int)x - shift, y));
         }
      }
   }
}

/* The background without a backdrop: the jungle's blue made green and deeper, the ground
 * earth, the river a sky and water. */
static void paint_background(jh *g, px_scene *s)
{
   const unsigned h = s->frame->height < ROWS ? s->frame->height : ROWS;
   for (unsigned y = ROW_TOP; y < h; y++)
   {
      uint32_t rgb;
      if (g->at.river)
         rgb = y < g->at.water ? px_rgb_mix(0x1E4C8E, 0xB6D2DC, (y - ROW_TOP) * 256u / (g->at.water - ROW_TOP + 1))
               : px_rgb_mix(0x2A8494, 0x04182E, (y - g->at.water) * 256u / (h - g->at.water + 1));
      else if (y < g->at.ground)
         rgb = px_rgb_mix(0x1A3E30, 0x6C9C66, y < ROW_BRANCHES ? 0 : (y - ROW_BRANCHES) * 256u / (g->at.ground - ROW_BRANCHES + 1));
      else if (g->scene == SCENE_VINES)
         continue;
      else
         rgb = y < g->at.ground + 4 ? 0x4C9430 : px_rgb_mix(0x7A5230, 0x3A2412, (y - g->at.ground) * 256u / (h - g->at.ground + 1));
      px_kit_background(s, y, y + 1, rgb);
   }
}

/* The bars HMOVE leaves at the left were given the game's colours of what is next to them
 * before the module saw the frame; they get what it has made of that. */
static void refill_bars(px_scene *s)
{
   const unsigned h = s->frame->height < ROWS ? s->frame->height : ROWS;
   if (!s->cfg->bars)
      return;
   for (unsigned y = ROW_TOP; y < h; y++)
   {
      const uint8_t *tags = s->frame->tags + (size_t)y * PXC_W;
      uint32_t *top = s->top + (size_t)y * PXC_W, *bk = s->bk + (size_t)y * PXC_W;
      unsigned n = 0;
      while (n < PXC_W && (tags[n] & PXC_BLANK))
         n++;
      if (!n || n > 16 || n >= PXC_W)
         continue;
      for (unsigned x = 0; x < n; x++)
      {
         top[x] = PX_KEY_CLS(top[n]) == PX_CLS_SPRITE ? PX_KEY(PX_CLS_BK, bk[n]) : top[n];
         bk[x]  = bk[n];
      }
   }
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

static bool has_row_color(const px_objects *o, const px_instance *in, unsigned color)
{
   for (unsigned r = 0; r < in->h && r < PX_OBJ_ROWS; r++)
      if ((o->colors[in->rows + r] & 0xFE) == color)
         return true;
   return false;
}

static unsigned kind_of(const jh *g, const px_objects *o, const px_instance *in)
{
   const unsigned color = in->color & 0xFE;

   if (!px_kit_is_player(in))
      return g->scene == SCENE_VINES && in->y >= ROW_VINES - 4 && in->cls != PXC_L_BL ? KIND_VINE : KIND_NONE;
   if (in->y + (int)in->h <= ROW_HUD)
      return KIND_SCORE;
   if (g->scene == SCENE_RIVER)
   {
      if (in->y < ROW_SWIM)
         return in->y >= ROW_DIVING + 6 ? KIND_AIR : KIND_DIVING;
      switch (color)
      {
         case 0xF8: return in->cls == PXC_L_P0 ? KIND_CROC_TAIL : KIND_CROC_HEAD;
         case 0x4A: return KIND_SWIMMER_HEAD;
         case 0x0E: return KIND_SWIMMER_BODY;
         default:   return KIND_NONE;
      }
   }
   if (in->y >= ROW_BRANCHES && in->y + (int)in->h <= ROW_VINES + 1)
      return KIND_BRANCH;
   if (in->y < ROW_VINES)
      return KIND_NONE;
   if (in->cls == PXC_L_P0)
      return in->h >= 12 ? KIND_EXPLORER : KIND_NONE;
   switch (g->scene)
   {
      case SCENE_BOULDERS:
         return KIND_BOULDER;
      case SCENE_CANNIBALS:
         if (color == 0x1A || has_row_color(o, in, 0xD8) || has_row_color(o, in, 0x88))
            return KIND_CAPTIVE;
         return KIND_CANNIBAL;
      default:
         return KIND_NONE;
   }
}

static unsigned role_of(unsigned kind)
{
   switch (kind)
   {
      case KIND_EXPLORER:
      case KIND_SWIMMER_HEAD:
      case KIND_SWIMMER_BODY: return PX_ROLE_PLAYER;
      case KIND_CROC_TAIL:
      case KIND_CROC_HEAD:
      case KIND_BOULDER:
      case KIND_CANNIBAL:     return PX_ROLE_ENEMY;
      case KIND_CAPTIVE:      return PX_ROLE_BONUS;
      case KIND_SCORE:
      case KIND_DIVING:
      case KIND_AIR:          return PX_ROLE_HUD;
      default:                return PX_ROLE_NONE;
   }
}

/* The colour of pixel (b, r) of an object, which has the game's colour `color` on the row;
 * `original` where the module has none for it. */
static uint32_t color_of(const jh *g, const px_instance *in, unsigned r, unsigned b, unsigned color,
      uint32_t original)
{
   const unsigned down = in->h > 1 ? r * 256u / (in->h - 1u) : 0;
   color &= 0xFE;
   switch (in->group)
   {
      case KIND_EXPLORER:
         if (color == 0x0E)
            return r < 2 ? 0xF4ECD4                 /* the helmet */
                  : r < 10 ? 0xD8C48E               /* the shirt */
                  : 0xB49A64;                       /* the shorts */
         if (color == 0x48)
            return r < 10 ? 0xE8A878                /* the face and the arms */
                  : r + 2 >= in->h ? 0x5A3A1E       /* the boots */
                  : 0xE0A070;
         return original;
      case KIND_SWIMMER_HEAD:
         return r == 0 ? 0xF4ECD4 : 0xE8A878;
      case KIND_SWIMMER_BODY:
         return px_rgb_mix(0xE0CE98, 0xA88E5C, down);
      case KIND_CROC_TAIL:
      case KIND_CROC_HEAD:
      {
         uint32_t rgb = px_rgb_mix(0x3C8A34, 0x1E4E1C, down);
         if (r + 1 >= in->h)
            rgb = 0x9CB05A;                          /* the belly */
         if (in->group == KIND_CROC_HEAD && r == 0)
            return 0xF0D840;                         /* the eye */
         /* A glint of teeth at the end of the jaw, now and then. */
         if (in->group == KIND_CROC_HEAD && r == 1 && b >= 3
               && px_kit_wave(g->frame * 5 + (unsigned)in->x * 13) > 170)
            return 0xFFFFF0;
         return rgb;
      }
      case KIND_BOULDER:
      {
         /* Round and lit from the upper left; its specks turn with it as it rolls. */
         const int cx = (int)b * 256 / 8 - 128, cy = (int)down - 128;
         const int lit = 128 - (cx + cy) / 2;
         const unsigned speck = hash(((int)b - in->x / 2) & 7, (int)r, 80);
         uint32_t rgb = px_rgb_mix(0x4C443A, 0xC8BCA6, lit < 0 ? 0 : lit > 256 ? 256 : (unsigned)lit);
         return speck > 220 ? px_rgb_scale(rgb, 180) : rgb;
      }
      case KIND_CANNIBAL:
         switch (color)
         {
            case 0x0A: return 0xEEE6CC;               /* the skull and the spear */
            case 0x46: return 0xE89020;               /* the cloth */
            default:   return r < 6 ? 0xEEE6CC : 0x6A3A1C;   /* the body */
         }
      case KIND_CAPTIVE:
         switch (color)
         {
            case 0x1A: return 0xFFD85A;               /* her hair */
            case 0x4A: return 0xF4BE94;
            case 0xD8: return 0xF6F2EA;               /* a white blouse */
            case 0x88: return px_rgb_mix(0xF05C8C, 0xB0305E, down);
            default:   return original;
         }
      case KIND_BRANCH:
      {
         /* Bark, lighter where it is higher, and moss on it here and there. */
         const unsigned down_the_tree = (unsigned)(in->y + (int)r - ROW_BRANCHES) * 12;
         const uint32_t bark = px_rgb_mix(0xB47E4A, 0x6A4424, down_the_tree > 256 ? 256 : down_the_tree);
         return hash((in->x + (int)b) / 2, in->y + (int)r, 82) > 200 ? px_rgb_mix(bark, 0x78A840, 150) : bark;
      }
      case KIND_VINE:
      {
         /* A vine that twists, with a leaf now and then. */
         const int y = in->y + (int)r;
         if (hash(in->x / 4, y / 3, 81) > 236)
            return 0x7CC84A;
         return (y / 2) % 3 ? 0x3E8A2A : 0x2A5E1E;
      }
      case KIND_SCORE:
         return color == 0xFA ? 0xF4C850 : 0xFFF0D0;
      case KIND_DIVING:
         return 0xB8F0F8;
      case KIND_AIR:
         if (g->air >= 5 || g->air < 0)
            return 0x58D068;
         if (g->air >= 3)
            return 0xF0B830;
         return px_rgb_mix(0xC01810, 0xFF6040, px_kit_wave(g->frame * 8));
      default:
         return original;
   }
}

/* Gives an object its colours, pixel by pixel where they are the object's. */
static void paint_object(const jh *g, px_scene *s, const px_instance *in)
{
   const px_objects *o = s->objects;
   /* An object drawn from its track has the rows the track keeps. */
   const unsigned rows = in->ghost && in->h > PX_OBJ_ROWS ? PX_OBJ_ROWS : in->h;
   for (unsigned r = 0; r < rows; r++)
   {
      const int y = in->y + (int)r;
      const uint32_t bits = o->bits[in->rows + r];
      const unsigned color = o->colors[in->rows + r];
      if (y < 0 || y >= (int)s->frame->height)
         continue;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         const int x = in->x + (int)b;
         uint32_t rgb;
         size_t i;
         if (!((bits >> b) & 1) || x < 0 || x >= PXC_W)
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         /* Another object may be in front of this one. */
         if (PX_KEY_CLS(s->top[i]) != PX_CLS_SPRITE || (!in->ghost && s->frame->winner[i] != color))
            continue;
         rgb = color_of(g, in, r, b, color, s->frame->palette[color] & 0xFFFFFFu) & 0xFFFFFFu;
         s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
         if (s->sprite[i])
            s->sprite[i] = 0xFF000000u | rgb;
         /* A vine is a plant, not a beam: it does not glow as missiles do. */
         if (in->group == KIND_VINE)
            s->energy[i] = 0;
      }
   }
}

/* How far the scenery moved: the trees in the jungle, a pixel at a time, as most of the
 * branches that were seen before have it; the bed of the river as its playfield has moved. */
static int follow_trees(const px_scene *s)
{
   int count[9] = { 0 }, best = 4;
   const px_objects *o = s->objects;
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      const px_obj_track *t;
      if (in->group != KIND_BRANCH || in->ghost || in->w < 8 || !(t = px_objects_track(o, in->track)))
         continue;
      if (t->vx >= -4 && t->vx <= 4 && (t->seen & 2))
         count[t->vx + 4]++;
   }
   for (int k = 0; k < 9; k++)
      if (count[k] > count[best])
         best = k;
   return count[best] >= 2 ? best - 4 : 0;
}

static int follow_bed(jh *g, const px_scene *s)
{
   const unsigned from = g->at.bed;
   int best = 0, best_score = -1;
   uint8_t now[16][PXC_W];
   unsigned rows = 0;

   for (unsigned r = 0; r < 16 && from + r < s->frame->height && from + r < ROWS; r++, rows++)
      for (unsigned x = 0; x < PXC_W; x++)
         now[r][x] = pf_at(s, x, from + r);
   if (g->bed_known && rows)
      for (int d = -8; d <= 8; d++)
      {
         int score = 0;
         for (unsigned r = 0; r < rows; r++)
            for (int x = 16; x < PXC_W - 16; x++)
               score += now[r][x] == g->bed_bits[r][x - d];
         /* No move unless a move fits better. */
         if (score > best_score || (score == best_score && abs(d) < abs(best)))
         {
            best = d;
            best_score = score;
         }
      }
   memcpy(g->bed_bits, now, sizeof(now));
   g->bed_known = rows == 16;
   return best;
}

/* ---------------------------------------------------------------------------
 * What the voices tell: the picture and the sound both go by it
 *
 * The game has voice 0 for everything, and tells its sounds apart by waveform and volume:
 *
 *   the tune        waveform 4 at volume 8, as a scene begins and after a life lost: pitch
 *                   18, 16; 18, 15, 16, 18, 21; 18, 21, 20, 21, 24, 21, 24, 27; 13, eight
 *                   frames to a step
 *   a vine caught   waveform 4 at volume 15, pitch 5 and 9 in turns, two frames each
 *   a jump          waveform 4 at volume 7, pitch 13, 11, 8, 11, 13, 17, two frames each: off
 *                   a vine, over a boulder or a cannibal
 *   swimming        waveform 8 at volume 3, pitch 10 and 4, a frame every three
 *   under water     waveform 13 at volume 3, pitch 31 and 20, seven frames each
 *   the knife       waveform 8 at volume 12, pitch 1 up to 8, a frame each
 *   out of air      waveform 4, pitch 31 down to 24 as the volume goes from 15 down to 1
 *   a boulder lands waveform 15 at pitch 27, from volume 12 down to 4, a frame each
 *   the cannibals   a tune: waveform 6 at volume 7, pitch 13 for 24 frames, 6, 6, 13, 18, 18,
 *                   six frames each with six between
 *   the rescue      waveform 4 at volume 10, pitch 8 for three frames every seven, as the time
 *                   left is counted into the score
 *
 * A crocodile killed, a life lost and the rescue itself have no sound of their own.
 * ------------------------------------------------------------------------- */

static unsigned voice0_plays(const px_kit_tia *t)
{
   const unsigned w = t->wave[0], p = t->pitch[0], v = t->volume[0];
   if (!v || !w)
      return HEARD_NONE;
   switch (w)
   {
      case 4:
         if (v == 8)
            return HEARD_TUNE;
         if (v == 10 && p == 8)
            return HEARD_TALLY;
         if (p >= 24)
            return HEARD_DROWN;
         if (v == 15 && (p == 5 || p == 9))
            return HEARD_CATCH;
         if (p == 8 || p == 11 || p == 13 || p == 17)
            return HEARD_JUMP;
         return HEARD_OTHER;
      case 6:  return v == 7 ? HEARD_DRUM : HEARD_OTHER;
      case 8:
         if (v == 3 && (p == 10 || p == 4))
            return HEARD_SWIM;
         return v == 12 && p >= 1 && p <= 8 ? HEARD_STAB : HEARD_OTHER;
      case 13: return v == 3 && (p == 31 || p == 20) ? HEARD_DIVE : HEARD_OTHER;
      case 15: return p == 27 ? HEARD_BOUNCE : HEARD_OTHER;
      default: return HEARD_OTHER;
   }
}

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static int score_of(const uint8_t *ram, size_t size)
{
   const int hi = px_kit_ram(ram, size, RAM_SCORE - 1), lo = px_kit_ram(ram, size, RAM_SCORE);
   if (hi < 0 || lo < 0)
      return -1;
   return ((hi >> 4) * 10 + (hi & 15)) * 100 + (lo >> 4) * 10 + (lo & 15);
}

static void events(jh *g, px_scene *s, int lives, int score)
{
   const px_objects *o = s->objects;
   const unsigned heard = voice0_plays(&g->seen);
   const bool fresh = heard != g->seen0 || (heard != HEARD_NONE && px_kit_tia_louder(&g->seen, 0));

   if (!g->sparks)
      return;

   /* A vine caught: leaves fly from it. */
   if (fresh && heard == HEARD_CATCH && heard != g->seen0 && g->hero_x >= 0)
   {
      px_scene_burst(s, g->hero_x, g->hero_y, 0x6CC840, 14, 220);
      px_scene_burst(s, g->hero_x, g->hero_y, 0xB0E070, 6, 140);
   }
   /* The knife: a glint where it is. */
   if (fresh && heard == HEARD_STAB && heard != g->seen0 && g->hero_x >= 0)
      px_scene_burst(s, g->hero_x - 6, g->hero_y + 3, 0xE8F4FF, 8, 160);
   /* A boulder lands: dust. */
   if (heard == HEARD_BOUNCE && px_kit_tia_louder(&g->seen, 0))
   {
      int best = -1;
      for (unsigned i = 0; i < o->count; i++)
      {
         const px_instance *in = &o->inst[i];
         if (in->group == KIND_BOULDER && (best < 0 || in->y + in->h > o->inst[best].y + o->inst[best].h))
            best = (int)i;
      }
      if (best >= 0)
      {
         const px_instance *in = &o->inst[best];
         px_scene_burst(s, in->x + 4, in->y + (int)in->h, 0xB89868, 16, 200);
         px_scene_burst(s, in->x + 4, in->y + (int)in->h, 0x7A6448, 8, 120);
      }
   }
   /* The time counted into the score: the captive is saved. */
   if (heard == HEARD_TALLY && g->seen0 != HEARD_TALLY && g->since_tally > 90 && g->captive_x >= 0)
   {
      px_scene_burst(s, g->captive_x, g->captive_y, 0xFFD860, 40, 360);
      px_scene_burst(s, g->captive_x, g->captive_y, 0xFF80B0, 24, 260);
      px_scene_flash(s, 0xFFE0A0, 90);
   }
   if (heard == HEARD_TALLY)
      g->since_tally = 0;
   else if (g->since_tally < 1000)
      g->since_tally++;

   /* A crocodile killed: the score goes up in the river. */
   if (g->scene == SCENE_RIVER && score > g->score && g->score >= 0 && score - g->score <= 5)
   {
      int best = -1, far = 1 << 20;
      for (unsigned i = 0; i < o->count; i++)
      {
         const px_instance *in = &o->inst[i];
         const int d = abs(in->x - g->hero_x) + abs(in->y - g->hero_y);
         if ((in->group == KIND_CROC_HEAD || in->group == KIND_CROC_TAIL) && d < far)
         {
            far  = d;
            best = (int)i;
         }
      }
      {
         const int x = best >= 0 ? o->inst[best].x + 4 : g->hero_x - 8;
         const int y = best >= 0 ? o->inst[best].y + 2 : g->hero_y + 3;
         px_scene_burst(s, x, y, 0xFF4830, 30, 320);
         px_scene_burst(s, x, y, 0xFFFFFF, 10, 200);
         px_scene_flash(s, 0xFF6040, 70);
         add_bubbles(g, s, x, y, 8);
      }
   }
   /* A life lost. */
   if (lives >= 0 && g->lives >= 0 && lives < g->lives && lives != 0xFF && g->hero_x >= 0)
   {
      px_scene_burst(s, g->hero_x, g->hero_y + g->hero_h / 2, 0xFF5030, 34, 340);
      px_scene_flash(s, 0xFF2010, 100);
      if (g->scene == SCENE_RIVER)
         add_bubbles(g, s, g->hero_x, g->hero_y, 10);
   }
   /* Out of air: the last of it goes up. It is heard in steps with silence between. */
   if (heard == HEARD_DROWN && g->since_drown > 40 && g->hero_x >= 0)
      add_bubbles(g, s, g->hero_x, g->hero_y, 12);
   if (heard == HEARD_DROWN)
      g->since_drown = 0;
   else if (g->since_drown < 1000)
      g->since_drown++;
}

static void frame(void *state, px_scene *s)
{
   jh *g = (jh*)state;
   px_objects *o = s->objects;
   const int scene = px_kit_ram(s->ram, s->ram_size, RAM_SCENE);
   int lives, score, air;
   bool was_under = g->under;

   if (s->frame->height < ROWS || !s->frame->tags || scene < 0 || scene > SCENE_CANNIBALS)
   {
      g->scene = SCENE_NONE;
      return;
   }
   if (s->advance)
      g->frame++;
   if ((unsigned)scene != g->scene)
   {
      g->bed_known = false;
      for (unsigned i = 0; i < BUBBLES; i++)
         g->bubbles[i].alive = false;
   }
   g->scene = (unsigned)scene;
   find_layout(s, g->scene, &g->at);

   lives = px_kit_ram(s->ram, s->ram_size, RAM_LIVES);
   score = score_of(s->ram, s->ram_size);
   air   = px_kit_ram(s->ram, s->ram_size, RAM_AIR);
   if (air >= 0)
      air >>= 4;
   g->air = g->scene == SCENE_RIVER ? air : -1;

   g->hero_x = g->hero_y = -1;
   g->hero_h = 0;
   g->captive_x = g->captive_y = -1;
   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->group = (uint8_t)kind_of(g, o, in);
      in->role  = (uint8_t)role_of(in->group);
      if ((in->group == KIND_EXPLORER || in->group == KIND_SWIMMER_HEAD) && (g->hero_x < 0 || !in->ghost))
      {
         g->hero_x = in->x + 4;
         g->hero_y = in->y;
         g->hero_h = (int)in->h;
      }
      else if (in->group == KIND_CAPTIVE)
      {
         g->captive_x = in->x + 4;
         g->captive_y = in->y + (int)in->h / 2;
      }
   }
   /* Under water once the head is well below the surface, above it once it is up again. */
   if (g->scene != SCENE_RIVER)
      g->under = false;
   else if (g->hero_x >= 0)
      g->under = g->hero_y > (int)g->at.water + (g->under ? -2 : 5);

   if (s->advance)
   {
      if (g->at.river)
      {
         g->scroll += follow_bed(g, s);
         /* The water's backdrop goes after the bed's steps a little at a time. */
         g->shown += (g->scroll * 16 - g->shown) / 8;
      }
      else
      {
         g->scroll += follow_trees(s);
         g->shown = g->scroll * 16;
      }
   }

   if (g->colors != COLORS_ORIGINAL)
   {
      paint_scenery(g, s);
      for (unsigned i = 0; i < o->count; i++)
         if (o->inst[i].group != KIND_NONE)
            paint_object(g, s, &o->inst[i]);
      if (!g->backdrop)
         paint_background(g, s);
   }

   if (g->backdrop)
   {
      /* The backdrop shows where the background is black; in the river the playfield that
       * has the sky's colour above the water is sky too. */
      const unsigned h = s->frame->height < ROWS ? s->frame->height : ROWS;
      if (g->scene == SCENE_VINES)
         px_kit_background(s, ROW_TOP, g->at.ground + 12 < h ? g->at.ground + 12 : h, 0x000000);
      else
         px_kit_background(s, ROW_TOP, h, 0x000000);
      if (g->at.river)
         for (size_t i = (size_t)ROW_TOP * PXC_W; i < (size_t)g->at.water * PXC_W; i++)
            if (PX_KEY_CLS(s->top[i]) == PX_CLS_PF && s->frame->color[PXC_L_PF][i] == g->at.sky
                  && s->frame->color[PXC_L_BK][i] == g->at.sky)
               s->top[i] = PX_KEY(PX_CLS_BK, 0);
      if (g->scene == SCENE_VINES)
         for (size_t i = (size_t)g->at.ground * PXC_W; i < (size_t)(g->at.ground + 12 < h ? g->at.ground + 12 : h) * PXC_W; i++)
            if (PX_KEY_CLS(s->top[i]) == PX_CLS_BK && s->frame->color[PXC_L_BK][i] != g->at.sky)
               s->top[i] = PX_KEY(PX_CLS_BK, s->frame->palette[s->frame->color[PXC_L_BK][i]] & 0xFFFFFFu);
      if (s->advance && g->scene == SCENE_RIVER && g->under && g->frame % 14 == 0 && g->hero_x >= 0)
         add_bubbles(g, s, g->hero_x - 4, g->hero_y, 1);
      paint_backdrop(g, s);
   }
   if (g->backdrop || g->colors != COLORS_ORIGINAL)
      refill_bars(s);

   if (!s->advance)
      return;

   px_kit_tia_hear(&g->seen, s->frame);
   events(g, s, lives, score);
   /* Into the water, and out of it. */
   if (g->sparks && g->scene == SCENE_RIVER && g->hero_x >= 0 && g->under != was_under)
   {
      px_scene_burst(s, g->hero_x, (int)g->at.water - 1, 0xC8ECFF, g->under ? 22 : 10, g->under ? 260 : 180);
      if (g->under)
         add_bubbles(g, s, g->hero_x, (int)g->at.water + 4, 6);
   }
   g->seen0 = voice0_plays(&g->seen);
   g->lives = lives;
   g->score = score;
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * What the game's are is said above, where they are told apart. Each is a sound of several
 * voices here, where it happens between left and right. The game's two tunes are its own,
 * note for note and as long: the one a scene begins with an octave lower on a wooden flute,
 * the cannibals' on log drums. Proteus adds what the game is silent for: a crocodile killed,
 * the splash of a dive, the air running low, a life lost and the rescue.
 * ------------------------------------------------------------------------- */

static float pan_of(const jh *g)
{
   return px_kit_pan(g->hero_at);
}

static void play_catch(jh *g, px_sound *s)
{
   static const px_tone p[4] = {
      /* wave             freq  to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SAW,      190,  140,  0.25f, 0.005f, 0.04f, 0.30f, 0.20f, 900, 400, 7.0f, 0.03f },
      { PX_WAVE_SINE,     110,  55,   0.10f, 0.001f, 0.02f, 0.20f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE,    7000, 0,    0.10f, 0.002f, 0.03f, 0.18f, 0.16f, 5000, 1500, 0, 0 },
      { PX_WAVE_TRIANGLE, 660,  880,  0.08f, 0.004f, 0.05f, 0.12f, 0.12f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 4, pan_of(g));
   px_sound_rumble(s, 14000, 18000, 6);
}

static void play_jump(jh *g, px_sound *s)
{
   /* Off a vine it is a long swing through the air; on the ground a hop. */
   static const px_tone swing[3] = {
      { PX_WAVE_NOISE,    900,  0,    0.30f, 0.08f,  0.10f, 0.30f, 0.30f, 500, 2600, 0, 0 },
      { PX_WAVE_SINE,     240,  520,  0.25f, 0.01f,  0.08f, 0.20f, 0.18f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 120,  260,  0.25f, 0.01f,  0.06f, 0.18f, 0.16f, 0, 0, 0, 0 }
   };
   static const px_tone hop[3] = {
      { PX_WAVE_SINE,     260,  620,  0.16f, 0.004f, 0.06f, 0.14f, 0.28f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 130,  310,  0.16f, 0.004f, 0.05f, 0.12f, 0.20f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE,    3000, 0,    0.08f, 0.01f,  0.02f, 0.10f, 0.12f, 1200, 2800, 0, 0 }
   };
   px_kit_play(s, g->s_scene == SCENE_VINES ? swing : hop, 3, pan_of(g));
   px_sound_rumble(s, 0, 9000, 4);
}

static void play_stroke(jh *g, px_sound *s, bool under)
{
   static const px_tone splash[2] = {
      { PX_WAVE_NOISE, 4000, 0,   0.10f, 0.005f, 0.01f, 0.12f, 0.10f, 2600, 700, 0, 0 },
      { PX_WAVE_SINE,  380,  620, 0.05f, 0.002f, 0,     0.06f, 0.05f, 0, 0, 0, 0 }
   };
   static const px_tone bubbles[2] = {
      { PX_WAVE_SINE,  260,  520, 0.06f, 0.004f, 0.01f, 0.10f, 0.10f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 900,  0,   0.10f, 0.01f,  0.02f, 0.14f, 0.07f, 500, 250, 0, 0 }
   };
   /* Now one arm, now the other. */
   px_kit_play(s, under ? bubbles : splash, 2, pan_of(g) + (++g->strokes & 1 ? 0.1f : -0.1f));
}

static void play_stab(jh *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE,  8000, 0,    0.10f, 0.003f, 0.02f, 0.12f, 0.30f, 6500, 1500, 0, 0 },
      { PX_WAVE_SINE,   2600, 2300, 0.15f, 0.001f, 0.01f, 0.22f, 0.10f, 0, 0, 6.0f, 0.01f },
      { PX_WAVE_SINE,   3900, 3500, 0.15f, 0.001f, 0,     0.12f, 0.05f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, pan_of(g) - 0.05f);
   px_sound_rumble(s, 0, 8000, 3);
}

static void play_croc(jh *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 3500, 500, 0.40f, 0.002f, 0.05f, 0.45f, 0.40f, 3000, 400, 0, 0 },
      { PX_WAVE_SINE,  120,  40,  0.20f, 0.001f, 0.03f, 0.30f, 0.70f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,   95,   60,  0.30f, 0.01f,  0.05f, 0.35f, 0.22f, 700, 200, 14.0f, 0.05f },
      { PX_WAVE_SINE,  420,  900, 0.20f, 0.01f,  0.02f, 0.20f, 0.08f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->croc_at >= 0 ? g->croc_at : g->hero_at));
   px_sound_rumble(s, 26000, 30000, 10);
}

static void play_splash(jh *g, px_sound *s, bool in)
{
   static const px_tone dive[3] = {
      { PX_WAVE_NOISE, 6000, 700, 0.30f, 0.002f, 0.04f, 0.35f, 0.40f, 5000, 600, 0, 0 },
      { PX_WAVE_SINE,  160,  70,  0.12f, 0.001f, 0.02f, 0.18f, 0.40f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,  500,  1100, 0.18f, 0.01f, 0.02f, 0.16f, 0.08f, 0, 0, 0, 0 }
   };
   static const px_tone out[2] = {
      { PX_WAVE_NOISE, 5000, 1500, 0.20f, 0.004f, 0.02f, 0.20f, 0.20f, 4000, 1200, 0, 0 },
      { PX_WAVE_NOISE, 2500, 0,    0.15f, 0.05f,  0.05f, 0.15f, 0.08f, 2000, 3500, 0, 0 }
   };
   if (in)
      px_kit_play(s, dive, 3, pan_of(g));
   else
      px_kit_play(s, out, 2, pan_of(g));
   px_sound_rumble(s, in ? 12000 : 0, 14000, in ? 8 : 4);
}

static void play_drown(jh *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SINE,     620, 140, 1.10f, 0.01f, 0.60f, 0.50f, 0.34f, 0, 0, 7.0f, 0.03f },
      { PX_WAVE_TRIANGLE, 310, 70,  1.10f, 0.01f, 0.60f, 0.50f, 0.20f, 0, 0, 7.0f, 0.03f },
      { PX_WAVE_NOISE,    800, 0,   0.50f, 0.05f, 0.60f, 0.60f, 0.14f, 600, 200, 0, 0 }
   };
   px_kit_play(s, p, 3, pan_of(g));
   px_sound_rumble(s, 20000, 30000, 30);
}

static void play_bounce(jh *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SINE,  78,   38,  0.18f, 0.001f, 0.02f, 0.40f, 0.90f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 1200, 200, 0.20f, 0.001f, 0.02f, 0.30f, 0.40f, 900, 150, 0, 0 },
      { PX_WAVE_NOISE, 5000, 0,   0.06f, 0.001f, 0,     0.10f, 0.12f, 3500, 1500, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->boulder_at >= 0 ? g->boulder_at : g->hero_at));
   px_sound_rumble(s, 30000, 16000, 8);
}

static void play_lost(jh *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE, 4500, 400, 0.30f, 0,      0.04f, 0.45f, 0.36f, 3000, 260, 0, 0 },
      { PX_WAVE_SINE,  170,  45,  0.35f, 0.002f, 0.04f, 0.55f, 0.66f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,   340,  80,  0.40f, 0.002f, 0.04f, 0.40f, 0.18f, 1800, 240, 0, 0 }
   };
   px_kit_play(s, p, 3, pan_of(g));
   px_sound_rumble(s, 65535, 36000, 36);
}

static void play_tally(jh *g, px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_SINE, 1760, 0, 0, 0.001f, 0.01f, 0.14f, 0.16f, 0, 0, 0, 0 },
      { PX_WAVE_SINE, 2637, 0, 0, 0.001f, 0,     0.10f, 0.08f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 2, pan_of(g) * 0.5f);
}

/* The rescue: a chord of bells, struck once. */
static void play_rescue(jh *g, px_sound *s)
{
   static const float chord[4] = { 523.25f, 659.26f, 783.99f, 1046.5f };
   for (unsigned n = 0; n < 4; n++)
   {
      px_tone p = { PX_WAVE_SINE, chord[n], 0, 0, 0.002f + 0.03f * (float)n, 0.10f, 1.4f, 0.16f, 0, 0, 5.0f, 0.004f };
      px_tone q = { PX_WAVE_TRIANGLE, chord[n] * 2.0f, 0, 0, 0.002f + 0.03f * (float)n, 0.05f, 0.8f, 0.06f, 0, 0, 0, 0 };
      px_synth_play(s->synth, &p, pan_of(g) * 0.5f + 0.15f * ((float)n - 1.5f), 1.0f);
      px_synth_play(s->synth, &q, pan_of(g) * 0.5f, 1.0f);
   }
   px_sound_rumble(s, 12000, 24000, 12);
}

/* The air running low: a soft beat that comes faster as there is less of it. */
static void play_air(jh *g, px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_SINE, 70, 50, 0.08f, 0.004f, 0.02f, 0.16f, 0.50f, 0, 0, 0, 0 },
      { PX_WAVE_SINE, 440, 330, 0.10f, 0.004f, 0.02f, 0.10f, 0.05f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 2, pan_of(g));
   px_sound_rumble(s, 9000, 0, 3);
}

/* The tune a scene begins with: the note voice 0 has, an octave lower on a wooden flute. */
static void play_note(jh *g, px_sound *s, bool begins)
{
   static const px_tone low  = { PX_WAVE_TRIANGLE, 1, 0, 0, 0.02f, 0, 0, 0.34f, 0, 0, 5.0f, 0.006f };
   static const px_tone high = { PX_WAVE_SINE,     1, 0, 0, 0.03f, 0, 0, 0.14f, 0, 0, 5.0f, 0.006f };
   const float hz = px_kit_tune(px_kit_tia_hz(g->tia.wave[0], g->tia.pitch[0])) * 0.5f;
   px_tone p;

   if (hz <= 0.0f)
      return;
   if (begins || !px_synth_move(s->synth, g->tune_low, 0.0f, 1.0f, hz))
   {
      p = low;
      p.freq = hz;
      px_synth_stop(s->synth, g->tune_low, 0.02f);
      g->tune_low = px_synth_play(s->synth, &p, 0.0f, 1.0f);
   }
   if (begins || !px_synth_move(s->synth, g->tune_high, 0.0f, 1.0f, hz * 2.0f))
   {
      p = high;
      p.freq = hz * 2.0f;
      px_synth_stop(s->synth, g->tune_high, 0.02f);
      g->tune_high = px_synth_play(s->synth, &p, 0.0f, 1.0f);
   }
}

static void stop_tune(jh *g, px_sound *s)
{
   px_synth_stop(s->synth, g->tune_low, 0.06f);
   px_synth_stop(s->synth, g->tune_high, 0.06f);
   g->tune_low = g->tune_high = 0;
}

/* The cannibals' tune: every note of it struck on a log drum. */
static void play_drum(jh *g, px_sound *s)
{
   const float hz = px_kit_tune(px_kit_tia_hz(g->tia.wave[0], g->tia.pitch[0])) * 2.0f;
   px_tone p[3] = {
      { PX_WAVE_SINE,     0, 0, 0.12f, 0.001f, 0.01f, 0.32f, 0.50f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 0, 0, 0.10f, 0.001f, 0,     0.16f, 0.20f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 2400, 0, 0,     0,      0,     0.03f, 0.10f, 1800, 600, 0, 0 }
   };
   if (hz <= 0.0f)
      return;
   p[0].freq = hz;        p[0].freq_end = hz * 0.82f;
   p[1].freq = hz * 2.7f; p[1].freq_end = hz * 2.4f;
   px_kit_play(s, p, 3, 0.25f);
}

/* A number of frames between two, by chance. */
static unsigned wait_for(jh *g, unsigned least, unsigned most)
{
   return least + px_kit_chance(&g->chance) % (most - least + 1);
}

static void play_bird(jh *g, px_sound *s)
{
   px_tone p = { PX_WAVE_SINE, 0, 0, 0, 0.004f, 0.02f, 0.10f, 0.050f, 0, 0, 0, 0 };
   const float f = g->call_pitch;
   switch (g->call)
   {
      case 0:    /* notes that rise */
         p.freq = f; p.freq_end = f * 1.4f; p.glide = 0.07f;
         g->call_wait = 8;
         break;
      case 1:    /* a trill */
         p.freq = f * 1.1f; p.hold = 0.25f; p.decay = 0.12f; p.vibrato_hz = 24.0f; p.vibrato = 0.08f;
         g->call_wait = 24;
         break;
      case 2:    /* a parrot's squawk, low and rough */
         p.wave = PX_WAVE_SAW; p.freq = f * 0.35f; p.freq_end = f * 0.30f; p.glide = 0.12f;
         p.hold = 0.06f; p.decay = 0.10f; p.gain = 0.030f; p.cutoff = 2400; p.cutoff_end = 1400;
         p.vibrato_hz = 40.0f; p.vibrato = 0.05f;
         g->call_wait = 14;
         break;
      default:   /* notes that fall, quickly */
         p.freq = f * 1.5f; p.freq_end = f * 0.9f; p.glide = 0.05f; p.decay = 0.07f;
         g->call_wait = 6;
         break;
   }
   px_synth_play(s->synth, &p, g->call_pan, 1.0f);
}

/* What is heard with nothing happening: the jungle, or the river under water. */
static void play_ambience(jh *g, px_sound *s)
{
   static const px_tone hush  = { PX_WAVE_NOISE, 700, 0, 0, 0.8f, 0, 0, 0.10f, 380, 0, 0.09f, 0.25f };
   static const px_tone chirp = { PX_WAVE_SINE, 4700, 4900, 0.02f, 0.003f, 0.012f, 0.03f, 0.026f, 0, 0, 0, 0 };
   static const px_tone lap[2] = {
      { PX_WAVE_NOISE, 1800, 0,   0.20f, 0.05f, 0.04f, 0.25f, 0.06f, 900, 1600, 0, 0 },
      { PX_WAVE_SINE,  300,  420, 0.10f, 0.01f, 0.01f, 0.10f, 0.03f, 0, 0, 0, 0 }
   };
   const bool on = g->ambience && g->own_sound && g->s_scene != SCENE_NONE;
   const bool under = g->s_scene == SCENE_RIVER && g->hero_under;

   if (!on || !under)
   {
      px_synth_stop(s->synth, g->hush, 0.6f);
      g->hush = 0;
   }
   if (!on)
      return;
   if (under)
   {
      if (!px_synth_move(s->synth, g->hush, 0.0f, 1.0f, 0))
         g->hush = px_synth_play(s->synth, &hush, 0.0f, 1.0f);
      return;
   }
   if (g->s_scene == SCENE_RIVER)
   {
      /* The river laps at the explorer as he swims on top. */
      if (g->wait_drip)
         g->wait_drip--;
      else
      {
         px_kit_play(s, lap, 2, (float)(px_kit_chance(&g->chance) % 160) / 100.0f - 0.8f);
         g->wait_drip = wait_for(g, 40, 120);
      }
      return;
   }

   if (g->call_left)
   {
      if (g->call_wait)
         g->call_wait--;
      else
      {
         play_bird(g, s);
         g->call_left--;
      }
   }
   else if (g->wait_bird)
      g->wait_bird--;
   else
   {
      g->call       = px_kit_chance(&g->chance) % 4;
      g->call_left  = g->call == 1 || g->call == 2 ? 1 + px_kit_chance(&g->chance) % 2 : 2 + px_kit_chance(&g->chance) % 3;
      g->call_wait  = 0;
      g->call_pan   = (float)(px_kit_chance(&g->chance) % 180) / 100.0f - 0.9f;
      g->call_pitch = 2000.0f + (float)(px_kit_chance(&g->chance) % 1600);
      g->wait_bird  = wait_for(g, 80, 300);
   }
   if (g->wait_cricket)
      g->wait_cricket--;
   else
   {
      const unsigned n = px_kit_chance(&g->chance);
      px_synth_play(s->synth, &chirp, n & 64 ? -0.7f : 0.6f, 1.0f);
      g->wait_cricket = (n & 3) ? 4 : wait_for(g, 40, 140);
   }
}

static void sound(void *state, px_sound *s)
{
   jh *g = (jh*)state;
   const int scene = px_kit_ram(s->ram, s->ram_size, RAM_SCENE);
   const int lives = px_kit_ram(s->ram, s->ram_size, RAM_LIVES);
   const int score = score_of(s->ram, s->ram_size);
   int air = px_kit_ram(s->ram, s->ram_size, RAM_AIR);
   unsigned heard;
   bool began, under = g->hero_under;

   g->s_scene = scene >= 0 && scene <= SCENE_CANNIBALS ? (unsigned)scene : SCENE_NONE;
   if (air >= 0)
      air >>= 4;

   /* Where things are, from the picture before. */
   if (s->objects)
   {
      /* Where the explorer is not seen (the crocodiles' turn to be drawn), he is where he
       * was. Under water once his head is well below the surface, above it once it is up. */
      int hero = -1;
      g->boulder_at = g->croc_at = -1;
      for (unsigned i = 0; i < s->objects->count; i++)
      {
         const px_instance *in = &s->objects->inst[i];
         if ((in->group == KIND_EXPLORER || in->group == KIND_SWIMMER_HEAD) && (hero < 0 || !in->ghost))
         {
            hero  = in->x + 4;
            under = in->group == KIND_SWIMMER_HEAD && in->y > (g->hero_under ? 101 : 108);
         }
      }
      if (hero >= 0)
         g->hero_at = hero;
      for (unsigned i = 0; i < s->objects->count; i++)
      {
         const px_instance *in = &s->objects->inst[i];
         if (in->group == KIND_BOULDER && (g->boulder_at < 0 || abs(in->x + 4 - g->hero_at) < abs(g->boulder_at - g->hero_at)))
            g->boulder_at = in->x + 4;
         else if ((in->group == KIND_CROC_HEAD || in->group == KIND_CROC_TAIL)
               && (g->croc_at < 0 || abs(in->x + 4 - g->hero_at) < abs(g->croc_at - g->hero_at)))
            g->croc_at = in->x + 4;
      }
      if (g->s_scene != SCENE_RIVER)
         under = false;
   }

   px_kit_tia_hear(&g->tia, s->frame);
   heard = voice0_plays(&g->tia);
   began = heard != g->heard0 || (heard != HEARD_NONE && px_kit_tia_began(&g->tia, 0));

   if (heard != g->heard0 && g->heard0 == HEARD_TUNE)
      stop_tune(g, s);
   switch (heard)
   {
      case HEARD_TUNE:
         if (g->own_sound && (heard != g->heard0 || g->tia.pitch[0] != g->tia.was_pitch[0]))
            play_note(g, s, heard != g->heard0);
         break;
      case HEARD_DRUM:
         /* A note begins after a rest, or goes straight on to another pitch. It shakes
          * nothing: the tune goes on for the whole scene. */
         if (g->own_sound && (px_kit_tia_louder(&g->tia, 0) || g->tia.pitch[0] != g->tia.was_pitch[0]))
            play_drum(g, s);
         break;
      case HEARD_CATCH:
         if (heard != g->heard0)
         {
            if (g->own_sound) play_catch(g, s);
            else              px_sound_rumble(s, 14000, 18000, 6);
         }
         break;
      case HEARD_JUMP:
         if (heard != g->heard0)
         {
            if (g->own_sound) play_jump(g, s);
            else              px_sound_rumble(s, 0, 9000, 4);
         }
         break;
      case HEARD_SWIM:
      case HEARD_DIVE:
         if (began && g->own_sound)
            play_stroke(g, s, heard == HEARD_DIVE);
         break;
      case HEARD_STAB:
         if (heard != g->heard0)
         {
            if (g->own_sound) play_stab(g, s);
            else              px_sound_rumble(s, 0, 8000, 3);
         }
         break;
      case HEARD_DROWN:
         /* It comes in steps with silence between: once for all of them. */
         if (g->s_since_drown > 40)
         {
            if (g->own_sound) play_drown(g, s);
            else              px_sound_rumble(s, 20000, 30000, 30);
         }
         break;
      case HEARD_BOUNCE:
         if (px_kit_tia_louder(&g->tia, 0))
         {
            if (g->own_sound) play_bounce(g, s);
            else              px_sound_rumble(s, 30000, 16000, 8);
         }
         break;
      case HEARD_TALLY:
         if (px_kit_tia_began(&g->tia, 0) || heard != g->heard0)
         {
            if (g->s_since_tally > 90)
            {
               if (g->own_sound) play_rescue(g, s);
               else              px_sound_rumble(s, 12000, 24000, 12);
            }
            if (g->own_sound)
               play_tally(g, s);
         }
         break;
      default:
         break;
   }
   if (heard == HEARD_TALLY)
      g->s_since_tally = 0;
   else if (g->s_since_tally < 1000)
      g->s_since_tally++;
   if (heard == HEARD_DROWN)
      g->s_since_drown = 0;
   else if (g->s_since_drown < 1000)
      g->s_since_drown++;

   /* What the game is silent for. */
   if (g->s_scene == SCENE_RIVER && score > g->s_score && g->s_score >= 0 && score - g->s_score <= 5)
   {
      if (g->own_sound) play_croc(g, s);
      else              px_sound_rumble(s, 26000, 30000, 10);
   }
   if (lives >= 0 && g->s_lives >= 0 && lives < g->s_lives && lives != 0xFF && g->s_since_drown > 600)
   {
      if (g->own_sound) play_lost(g, s);
      else              px_sound_rumble(s, 65535, 36000, 36);
   }
   if (g->s_scene == SCENE_RIVER && s->objects && under != g->hero_under && g->hero_at >= 0)
   {
      if (g->own_sound) play_splash(g, s, under);
      else              px_sound_rumble(s, under ? 12000 : 0, 14000, under ? 8 : 4);
   }
   if (g->s_scene == SCENE_RIVER && under && air >= 1 && air <= 2)
   {
      if (g->s_air_wait)
         g->s_air_wait--;
      else
      {
         if (g->own_sound) play_air(g, s);
         g->s_air_wait = air == 1 ? 24 : 40;
      }
   }
   else
      g->s_air_wait = 0;
   g->heard0 = heard;
   g->hero_under = under;
   g->s_lives = lives;
   g->s_score = score;
   g->s_air = air;

   /* What is not known is heard as the game plays it. */
   if (g->own_sound && heard != HEARD_OTHER)
      s->voice[0] = 0.0f;
   play_ambience(g, s);
   g->beat++;
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   jh *g = (jh*)state;
   px_kit_tia_reset(&g->seen);
   px_kit_tia_reset(&g->tia);
   g->seen0 = g->heard0 = HEARD_NONE;
   g->scene = g->s_scene = SCENE_NONE;
   g->lives = g->score = g->air = -1;
   g->s_lives = g->s_score = g->s_air = -1;
   g->hero_x = g->hero_y = -1;
   g->hero_h = 0;
   g->captive_x = g->captive_y = -1;
   g->under = g->hero_under = false;
   g->since_tally = g->s_since_tally = 1000;
   g->since_drown = g->s_since_drown = 1000;
   g->scroll = g->shown = 0;
   g->bed_known = false;
   g->tune_low = g->tune_high = g->hush = 0;
   g->hero_at = g->boulder_at = g->croc_at = -1;
   g->chance = 0x4A7C15u;
   g->wait_bird = 90;
   g->wait_cricket = 40;
   g->wait_drip = 30;
   g->call_left = g->call_wait = 0;
   g->s_air_wait = 0;
   for (unsigned i = 0; i < BUBBLES; i++)
      g->bubbles[i].alive = false;
}

static void *create(void)
{
   jh *g = (jh*)calloc(1, sizeof(jh));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = g->ambience = true;
      g->seed = 0x1A6B7Eu;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   jh *g = (jh*)state;
   if (g)
   {
      px_kit_canvas_free(&g->jungle);
      px_kit_canvas_free(&g->ground);
      px_kit_canvas_free(&g->bank);
      px_kit_canvas_free(&g->water);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   jh *g = (jh*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->ambience  = px_kit_on(get, OPT_AMBIENCE);
}

const px_game px_game_jungle_hunt = {
   "Jungle Hunt", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
