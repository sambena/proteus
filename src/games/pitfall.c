/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pitfall! (Activision, 1982).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  17..75   the canopy: playfield, its lower edge in steps in rows 68..75. Behind it
 *                  and down to row 127 the background is the jungle's light green
 *   rows  20..40   the score above the time, and the lives left as strokes before the time:
 *                  both players in turns, three copies each
 *   rows  68..75   the branches of the four trees: two copies of each player
 *   rows  76..127  the trunks: playfield. Where they stand is one of four patterns
 *   rows  17..127  the vine: the ball, a pixel wide, in the canopy's colour in the canopy
 *   rows 128..143  the path: playfield. Pits and holes are gaps in it, and what shows in them
 *                  is the background, which is blue for water and black for tar and holes
 *   rows 129..142  what is on the path: player 1, up to three copies. Logs, a fire, a cobra,
 *                  crocodiles in the water, or a treasure: a bag of money, a bar of silver or
 *                  of gold, a ring. Every row of an object has a colour of its own
 *   rows 144..158  the earth: playfield, with the shafts below the holes as gaps
 *   rows 147..188  the ladder: the ball, four wide, two rows of every four
 *   rows 159..190  the tunnel: background, black. Player 1 is the scorpion in it, or the
 *                  wall of bricks
 *   rows 191..196  the tunnel's floor: playfield
 *   rows 197..     black playfield, and the publisher's name on it: both players
 *   Harry          player 0, 21 rows high when he stands: hair, face, shirt and trousers
 *
 * The eight pixels at the left are blanked on every line (HMOVE).
 *
 * Of its memory ($80 is 0):
 *
 *    0        the lives: $A0 for three, $80 for two, $00 for the last
 *    1        the scene, of 255. A step to the right shifts it left and puts bits 3, 4, 5
 *             and 7 together (exclusive or) as the new bit 0; a step to the left undoes
 *             that, and the tunnel goes three scenes to the step. The game begins at $C4.
 *             Bits 0..2 are what is on the path (19 has them alone), bits 3..5 the kind of
 *             scene (20), bits 6 and 7 the pattern of the trees, bit 7 the side of the wall
 *   17        the colour in the pits: $A4 for water, 0 for tar and holes
 *   19        on the path: 0..3 logs that roll, 4 and 5 logs that lie, 6 a fire, 7 a cobra;
 *             with a treasure 0 the bag, 1 silver, 2 gold, 3 the ring
 *   20        the scene's kind: 0 a hole with a ladder, 1 three holes, 2 tar and 3 water
 *             with a vine, 4 crocodiles (a vine if bit 1 of 19 is set), 5 a treasure, 6 tar
 *             and 7 water that open and close; 6 has a vine
 *   85..87    the score, in decimal digits (BCD); 88..90 minutes, seconds and frames left
 *   97        Harry's column
 *  105        Harry's row: 32 on the path, less in a jump, 64 to 85 on the ladder, 86 on the
 *             tunnel's floor; his sprite begins 83 or 84 rows on
 *
 * What surprised:
 *
 *   - A voice that is silent has waveform 0 and keeps its volume of 4, which a volume alone
 *     does not tell from a sound.
 *   - The sound of a log that rolls over Harry is on voice 1 only between the lines of a
 *     frame: waveform 7 from line 3 and waveform 0 again from line 258. What the registers
 *     have when the frame ends never shows it.
 *   - Stella does not run the game the same way twice from power-on: runs are compared from
 *     a saved state.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_pf_colors"
#define OPT_BACKDROP "proteus_pf_backdrop"
#define OPT_SPARKS   "proteus_pf_sparks"
#define OPT_SOUND    "proteus_pf_sound"
#define OPT_AMBIENCE "proteus_pf_ambience"

#define RAM_SCENE  1
#define RAM_HARRY_ROW 105

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define ROW_CANOPY  17
#define ROW_HUD     45     /* the score and the time are above it */
#define ROW_EDGE    68     /* the canopy's lower edge, and the branches */
#define ROW_TRUNKS  76
#define ROW_PATH    128
#define ROW_EARTH   144
#define ROW_TUNNEL  159
#define ROW_FLOOR   191
#define ROW_BELOW   197
#define ROWS        208    /* what the module needs of a frame to know the game in it */

#define FIRST_SCENE 0xC4

#define MOTES  30
#define SHAFTS 4

/* What an object is: a px_instance's group. */
enum
{
   KIND_NONE = 0, KIND_HARRY, KIND_LOG, KIND_FIRE, KIND_COBRA, KIND_CROCODILE, KIND_SCORPION,
   KIND_WALL, KIND_BAG, KIND_SILVER, KIND_GOLD, KIND_RING, KIND_BRANCH, KIND_VINE, KIND_LADDER,
   KIND_SCORE, KIND_NAME
};

/* What is in the gaps of the path. */
enum { PIT_NONE = 0, PIT_HOLES, PIT_TAR, PIT_WATER };

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "3e90cf23106f2e08b2781e41299de556",   /* Pitfall! - Pitfall Harry's Jungle Adventure (USA) */
   NULL
};

static const char *const fx[] = {
   "width", "50",
   "reverb", "room",
   NULL
};

static const char *const colors[] = { "jungle", "Jungle", "original", "The game's own", NULL };
static const char *const backdrop[] = { "jungle", "Jungle and cave", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_JUNGLE = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Bark, leaves, sand and earth in shades, treasures that shine and a fire that flickers, or the game's own colours.",
     "jungle", colors },
   { OPT_BACKDROP, "Backdrop",
     "The depth of the jungle behind the trees, which moves on from screen to screen, rock in the tunnel, and water and tar that move.",
     "jungle", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks and a flash where Harry takes a treasure, is rolled over by a log, lands in the tunnel or loses a life.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the jump, the fall, the logs and the loss of a life, where Harry is between left and right, and the game's three tunes played with other voices. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_AMBIENCE, "Jungle sounds",
     "Birds, crickets and the wind in the leaves above ground, water that drips in the tunnel. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

/* What voice 0 plays. */
enum { HEARD_NONE = 0, HEARD_JUMP, HEARD_FALL, HEARD_LOST, HEARD_TUNE, HEARD_OTHER };

/* A speck of dust in the light of the jungle. */
typedef struct
{
   uint16_t x, y;       /* in the picture, in 16ths of a pixel */
   uint16_t at_x, at_y; /* where it was drawn */
   uint8_t phase, pace, size;
   bool drawn;
} mote;

typedef struct
{
   /* The options. */
   unsigned colors;
   bool backdrop, sparks, own_sound, ambience;

   uint32_t frame;               /* counts the frames that advance */
   uint8_t screen[256];          /* how many screens to the right of the first a scene is */

   /* The picture. */
   px_kit_tia seen;              /* the game's voices as the picture follows them */
   unsigned seen0;               /* what voice 0 played in the frame before: HEARD_* */
   int harry_x, harry_y;         /* Harry's middle and his feet; -1: not seen */
   int treasure_x, treasure_y;   /* the treasure's middle */
   unsigned treasure_kind;       /* KIND_* */
   unsigned treasure_gone;       /* frames since it was seen; many: there was none */

   px_kit_canvas base;           /* haze and rock: the same on every screen */
   px_kit_canvas still;          /* with what the screen at hand has: the backdrop but for what moves */
   unsigned painted_screen;      /* what `still` was painted for */
   unsigned painted_shafts;
   mote motes[MOTES];
   uint32_t seed;

   /* The sound. */
   px_kit_tia tia;               /* the game's two voices */
   uint32_t beat;                /* counts the frames that are heard */
   unsigned heard0;              /* HEARD_* */
   bool rolled;                  /* a log rolled over Harry in the frame before */
   bool treasure;                /* the tune that plays is the one for a treasure */
   unsigned tune_low, tune_high; /* the voices at the synth that play the game's tune */
   unsigned roll;                /* the log that rolls over Harry */
   unsigned wind, deep;          /* what goes on above ground, and below */
   unsigned treasure_heard;      /* frames since a treasure was on the screen */
   int at;                       /* Harry's column, for where a sound is; -1: not known */
   bool below;                   /* he is in the tunnel */
   uint32_t chance;
   unsigned wait_bird, wait_cricket, wait_drop;
   unsigned call, call_left, call_wait;   /* a bird's call: its kind, notes to come, frames to the next */
   float call_pan, call_pitch;
} pf;

/* ---------------------------------------------------------------------------
 * The scenes
 * ------------------------------------------------------------------------- */

/* The scene to the right of a scene. */
static unsigned scene_right(unsigned scene)
{
   return ((scene << 1) & 255) | (((scene >> 3) ^ (scene >> 4) ^ (scene >> 5) ^ (scene >> 7)) & 1);
}

/* There is no pixel of the playfield here: the background shows, or an object over it. */
static bool is_open(const px_scene *s, unsigned x, unsigned y)
{
   return !(s->frame->tags[(size_t)y * PXC_W + x] & (PXC_PF | PXC_BLANK));
}

/* What is in the gaps of the path, and from which column to which they are. */
static unsigned find_pit(const px_scene *s, unsigned *left, unsigned *right)
{
   unsigned from = PXC_W, to = 0;
   uint32_t rgb;

   for (unsigned y = ROW_PATH; y < ROW_EARTH; y++)
      for (unsigned x = 0; x < PXC_W; x++)
         if (is_open(s, x, y))
         {
            if (x < from) from = x;
            if (x + 1 > to) to = x + 1;
         }
   *left  = from;
   *right = to;
   if (from >= to)
      return PIT_NONE;
   for (unsigned x = 0; x < PXC_W; x++)
      if (is_open(s, x, ROW_EARTH + 6))
         return PIT_HOLES;
   rgb = s->frame->palette[s->frame->color[PXC_L_BK][(size_t)(ROW_PATH + 8) * PXC_W + 80]];
   return (rgb & 0xFF) > ((rgb >> 16) & 0xFF) + 40 ? PIT_WATER : PIT_TAR;
}

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

static unsigned hash(int x, int y, uint32_t seed)
{
   uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
   h = (h ^ (h >> 13)) * 1274126177u;
   return (h ^ (h >> 16)) & 255;
}

/* Chance that is smooth, 0..255: x and y in 256ths of the cells it has a value for. */
static unsigned noise(int x, int y, uint32_t seed)
{
   const int xi = x >> 8, yi = y >> 8;
   unsigned fx = (unsigned)x & 255, fy = (unsigned)y & 255;
   unsigned top, bottom;
   fx     = fx * fx * (768 - 2 * fx) >> 16;
   fy     = fy * fy * (768 - 2 * fy) >> 16;
   top    = hash(xi, yi, seed) * (256 - fx) + hash(xi + 1, yi, seed) * fx;
   bottom = hash(xi, yi + 1, seed) * (256 - fx) + hash(xi + 1, yi + 1, seed) * fx;
   return (top * (256 - fy) + bottom * fy) >> 16;
}

/* What is the same on every screen: the jungle's haze with the sun in it, darker below the
 * canopy, and the rock the tunnel is cut into. */
static void paint_base(pf *g, const px_scene *s)
{
   const unsigned w = g->base.w, h = g->base.h;

   for (unsigned Y = 0; Y < h; Y++)
   {
      uint32_t *out = g->base.pixels + (size_t)Y * w;
      /* The game's row and column, in 256ths. */
      const int y = (int)(Y * 256u / s->sy), row = y >> 8;

      if (row < ROW_PATH)
      {
         /* Of 256: how far down from the canopy to the path. */
         const unsigned down = row < ROW_TRUNKS ? 0
               : (unsigned)(y - ROW_TRUNKS * 256) / (ROW_PATH - ROW_TRUNKS);
         const uint32_t haze = px_rgb_mix(0x1C4428, 0x8CBC62, down);
         for (unsigned X = 0; X < w; X++)
         {
            const int x = (int)(X * 256u / s->sx);
            const unsigned leaves = noise(x / 23, y / 12, 1);
            /* The sun comes down through the canopy from the right. */
            const unsigned beam = noise((x + (y - ROW_TRUNKS * 256) * 5 / 8) / 15, 0, 2);
            uint32_t rgb = px_rgb_scale(haze, 170 + (leaves * 86 >> 8));
            if (beam > 136 && row >= ROW_EDGE)
               rgb = px_rgb_add(rgb, px_rgb_scale(0x5A5228, ((beam - 136) * 2 * (320 - down)) >> 8));
            out[X] = rgb;
         }
      }
      else
      {
         const uint32_t rock = row < ROW_TUNNEL ? 0xD8A878 : 0xAEB6D8;
         for (unsigned X = 0; X < w; X++)
         {
            const int x = (int)(X * 256u / s->sx);
            const unsigned layers = noise(x / 21, y / 4, 3), grain = noise(x / 3, y / 2, 4);
            out[X] = px_rgb_scale(rock, 12 + (layers * 26 >> 8) + (grain * 10 >> 8));
         }
      }
   }
}

static void blend(uint32_t *p, uint32_t rgb, unsigned alpha)
{
   *p = px_rgb_mix(*p, rgb, alpha);
}

/* The trees further into the jungle, in three depths, and what grows at their feet. A
 * screen to the right they have moved to the left, the near ones further than the far ones. */
static void paint_jungle(pf *g, const px_scene *s, unsigned screen)
{
   static const struct
   {
      unsigned cell, wide, shift, foot, alpha;
      uint32_t trunk, brush;
   } depth[3] = {
      { 19, 2, 13, 110, 70, 0x2E5238, 0x3E7440 },
      { 31, 3, 29, 117, 120, 0x243E24, 0x2C5C2C },
      { 53, 5, 61, 124, 176, 0x1A2A12, 0x1C4218 }
   };
   const unsigned w = g->still.w;

   for (unsigned d = 0; d < 3; d++)
   {
      const int offset = (int)(screen * depth[d].shift) * 256;
      for (unsigned X = 0; X < w; X++)
      {
         const int x = (int)(X * 256u / s->sx) + offset;
         const int cell = x / ((int)depth[d].cell * 256);
         const int from = (cell * (int)depth[d].cell
               + (int)(hash(cell, (int)d, 7) % (depth[d].cell - depth[d].wide))) * 256;
         const int in = x - from, wide = (int)depth[d].wide * 256;
         /* The ground rises and falls, and what grows on it. */
         const unsigned top = (depth[d].foot * 256u - 2 * 256u
               - noise(x / 7, (int)d, 8) * 9u - noise(x / 2, (int)d, 9) * 3u) * s->sy >> 8;

         if (in >= 0 && in < wide && hash(cell, (int)d, 10) % 5)
         {
            /* Round: its edges are soft. */
            const int edge = in < wide - in ? in : wide - in;
            const unsigned alpha = edge >= 160 ? depth[d].alpha : depth[d].alpha * (unsigned)(edge + 96) / 256;
            for (unsigned Y = ROW_CANOPY * s->sy; Y < top && Y < g->still.h; Y++)
               blend(g->still.pixels + (size_t)Y * w + X, depth[d].trunk, alpha);
         }
         for (unsigned Y = top; Y < ROW_PATH * s->sy && Y < g->still.h; Y++)
            blend(g->still.pixels + (size_t)Y * w + X, depth[d].brush,
                  depth[d].alpha + (Y - top) * 40 / (ROW_PATH * s->sy - top + 1));
      }
   }
}

/* The tunnel is a cave: rock hangs from its roof and lies on its floor, which goes on from
 * screen to screen as the jungle does. */
static void paint_cave(pf *g, const px_scene *s, unsigned screen)
{
   const unsigned w = g->still.w, roof = ROW_TUNNEL * s->sy, floor = ROW_FLOOR * s->sy;

   for (unsigned X = 0; X < w; X++)
   {
      const int x = (int)(X * 256u / s->sx) + (int)(screen * PXC_W) * 256;
      const unsigned hangs = (noise(x / 9, 0, 20) * 11u + noise(x / 3, 0, 21) * 5u) * s->sy >> 8;
      const unsigned lies  = (noise(x / 13, 1, 20) * 6u + noise(x / 4, 1, 21) * 3u) * s->sy >> 8;

      for (unsigned Y = roof; Y < roof + hangs && Y < floor && Y < g->still.h; Y++)
         blend(g->still.pixels + (size_t)Y * w + X, 0x4A4038, 150 - (Y - roof) * 90 / (hangs + 1));
      for (unsigned Y = floor > lies ? floor - lies : 0; Y < floor && Y < g->still.h; Y++)
         blend(g->still.pixels + (size_t)Y * w + X, 0x3E3A3A, 70 + (Y + lies - floor) * 80 / (lies + 1));
   }
}

/* The shafts below the holes of a row of the picture: where each begins and ends. */
static unsigned find_shafts(const px_scene *s, unsigned from[SHAFTS], unsigned to[SHAFTS])
{
   unsigned count = 0;
   for (unsigned x = 0; x < PXC_W && count < SHAFTS; x++)
   {
      if (!is_open(s, x, ROW_EARTH + 6))
         continue;
      from[count] = x;
      while (x < PXC_W && is_open(s, x, ROW_EARTH + 6))
         x++;
      to[count++] = x;
   }
   /* The earth is not there at all: not a frame of the game's. */
   return count == 1 && to[0] - from[0] > 40 ? 0 : count;
}

/* Daylight falls down the shafts and into the tunnel. */
static void paint_shafts(pf *g, const px_scene *s)
{
   unsigned from[SHAFTS], to[SHAFTS];
   const unsigned count = find_shafts(s, from, to), w = g->still.w;

   for (unsigned n = 0; n < count; n++)
   {
      const int middle = (int)(from[n] + to[n]) * 128;
      for (unsigned Y = ROW_PATH * s->sy; Y < ROW_FLOOR * s->sy && Y < g->still.h; Y++)
      {
         const int y = (int)(Y * 256u / s->sy) - ROW_PATH * 256;
         /* It widens below the earth, and gets less. */
         const int half = (int)(to[n] - from[n]) * 128 + (y > 31 * 256 ? (y - 31 * 256) * 5 / 8 : 0);
         const unsigned strength = 150 - (unsigned)y * 110u / ((ROW_FLOOR - ROW_PATH) * 256u);
         for (unsigned X = 0; X < w; X++)
         {
            const int x = (int)(X * 256u / s->sx) - middle, away = x < 0 ? -x : x;
            if (away < half)
               g->still.pixels[(size_t)Y * w + X] = px_rgb_add(g->still.pixels[(size_t)Y * w + X],
                     px_rgb_scale(0x56603C, strength * (unsigned)(half - away) / (unsigned)half));
         }
      }
   }
}

/* Water, or tar: what moves in the pit, in the rows of the path from column to column. */
static void paint_pit(pf *g, px_scene *s, unsigned pit, unsigned left, unsigned right)
{
   const unsigned t = g->frame;

   for (unsigned Y = ROW_PATH * s->sy; Y < ROW_EARTH * s->sy && Y < s->h; Y++)
   {
      uint32_t *out = s->backdrop + (size_t)Y * s->w;
      /* The game's row and column, in 16ths. */
      const unsigned y = Y * 16u / s->sy - ROW_PATH * 16u;
      for (unsigned X = left * s->sx; X < right * s->sx && X < s->w; X++)
      {
         const unsigned x = X * 16u / s->sx;
         if (pit == PIT_WATER)
         {
            const unsigned a = px_kit_wave(x / 3 + t * 2 + px_kit_wave(y * 5 + t * 3) / 3);
            const unsigned b = px_kit_wave(x / 5 + y * 7 + 256 - (t & 255));
            const unsigned light = a * b >> 8;
            uint32_t rgb = px_rgb_scale(px_rgb_mix(0x14487C, 0x2C7CB8, y), 190 + (a >> 2));
            if (light > 150)
               rgb = px_rgb_add(rgb, px_rgb_scale(0xA8DCFF, (light - 150) * 2));
            out[X] = rgb;
         }
         else
         {
            const unsigned a = px_kit_wave(x / 6 + t / 2 + px_kit_wave(y * 3 + t / 3) / 2);
            out[X] = a > 190 ? px_rgb_add(0x07070A, px_rgb_scale(0x4A4A60, (a - 190) * 3)) : 0x07070A;
         }
      }
   }
}

static void put_mote(const pf *g, uint32_t *out, unsigned x, unsigned y, unsigned size, uint32_t rgb)
{
   for (unsigned t = 0; t < size; t++)
      for (unsigned u = 0; u < size; u++)
         if (x + u < g->still.w && y + t < g->still.h)
         {
            const size_t i = (size_t)(y + t) * g->still.w + x + u;
            out[i] = rgb ? px_rgb_add(g->still.pixels[i], rgb) : g->still.pixels[i];
         }
}

static void paint_backdrop(pf *g, px_scene *s, unsigned pit, unsigned left, unsigned right)
{
   const int scene = px_kit_ram(s->ram, s->ram_size, RAM_SCENE);
   const unsigned screen = scene < 0 ? 0 : g->screen[scene];
   unsigned from[SHAFTS], to[SHAFTS], shafts = find_shafts(s, from, to);
   bool fit;

   /* What tells one screen's shafts from another's. */
   for (unsigned n = 0, count = shafts; n < count; n++)
      shafts = shafts * 167 + from[n] * 13 + to[n];

   fit = px_kit_canvas_fit(&g->base, s);
   if (!g->base.pixels)
      return;
   if (fit)
      paint_base(g, s);
   if (px_kit_canvas_fit(&g->still, s))
      fit = true;
   if (!g->still.pixels)
      return;

   if (fit || (s->advance && (screen != g->painted_screen || shafts != g->painted_shafts)))
   {
      memcpy(g->still.pixels, g->base.pixels, (size_t)s->w * s->h * sizeof(uint32_t));
      paint_jungle(g, s, screen);
      paint_cave(g, s, screen);
      paint_shafts(g, s);
      memcpy(s->backdrop, g->still.pixels, (size_t)s->w * s->h * sizeof(uint32_t));
      g->painted_screen = screen;
      g->painted_shafts = shafts;
      for (unsigned i = 0; i < MOTES; i++)
         g->motes[i].drawn = false;
   }
   s->backdrop_on = true;
   if (!s->advance)
      return;

   if (pit == PIT_WATER || pit == PIT_TAR)
      paint_pit(g, s, pit, left, right);

   for (unsigned i = 0; i < MOTES; i++)
   {
      mote *m = &g->motes[i];
      const unsigned wave = px_kit_wave(m->phase + g->frame * m->pace);
      const unsigned top = ROW_TRUNKS * s->sy * 16u, span = (ROW_PATH - ROW_TRUNKS - 4) * s->sy * 16u;

      if (m->drawn)
         put_mote(g, s->backdrop, m->at_x, m->at_y, m->size, 0);
      if (!m->size)
      {
         m->size  = (uint8_t)(1 + px_kit_chance(&g->seed) % (s->sy > 2 ? 3 : 2));
         m->x     = (uint16_t)(px_kit_chance(&g->seed) % (s->w * 16u));
         m->y     = (uint16_t)(px_kit_chance(&g->seed) % span);
         m->phase = (uint8_t)px_kit_chance(&g->seed);
         m->pace  = (uint8_t)(1 + px_kit_chance(&g->seed) % 3);
      }
      /* They drift with the air, and sink. */
      m->x = (uint16_t)((m->x + s->w * 16u + (wave >> 6) - 1u) % (s->w * 16u));
      m->y = (uint16_t)((m->y + 1u + (m->pace & 1)) % span);
      m->at_x  = m->x / 16u;
      m->at_y  = (uint16_t)((top + m->y) / 16u);
      m->drawn = true;
      put_mote(g, s->backdrop, m->at_x, m->at_y, m->size, px_rgb_scale(0xFFF0B0, 40 + (wave * 90 >> 8)));
   }
}

/* ---------------------------------------------------------------------------
 * The scenery
 * ------------------------------------------------------------------------- */

/* The canopy's colour at a pixel: darker at the top, in clumps of leaves. */
static uint32_t leaves_at(unsigned x, unsigned y)
{
   const unsigned down = y < ROW_CANOPY ? 0 : (y - ROW_CANOPY) * 256u / (ROW_TRUNKS - ROW_CANOPY);
   const unsigned clump = noise((int)x * 256 / 9, (int)y * 256 / 5, 5);
   /* The score is read against it: less of the clumps there. */
   const unsigned much = y < ROW_HUD ? 24 : y < ROW_HUD + 16 ? 24 + (y - ROW_HUD) * 3 : 72;
   return px_rgb_scale(px_rgb_mix(0x113410, 0x2E7420, down > 256 ? 256 : down),
         184 + (clump * much >> 8));
}

/* The ground's colour at a pixel: the sand of the path, the earth below it, the tunnel's
 * floor. */
static uint32_t ground_at(unsigned x, unsigned y)
{
   if (y < ROW_EARTH)
      return px_rgb_scale(px_rgb_mix(0xD8C46C, 0xB89C48, (y - ROW_PATH) * 16),
            236 + (hash((int)x / 2, (int)y, 11) * 20 >> 8));
   if (y < ROW_TUNNEL)
      return px_rgb_scale(px_rgb_mix(0x86642C, 0x4A3416, (y - ROW_EARTH) * 17),
            216 + (noise((int)x * 256 / 6, (int)y * 256 / 2, 12) * 40 >> 8));
   return px_rgb_scale(px_rgb_mix(0x8A7448, 0x4E3E24, (y - ROW_FLOOR) * 42),
         226 + (hash((int)x / 2, (int)y, 13) * 30 >> 8));
}

/* Shades for the scenery, the playfield's pixel by pixel. */
static void paint_scenery(px_scene *s)
{
   for (unsigned y = ROW_CANOPY; y < ROW_BELOW; y++)
   {
      uint32_t *top = s->top + (size_t)y * PXC_W;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         uint32_t rgb;
         if (PX_KEY_CLS(top[x]) != PX_CLS_PF || (y >= ROW_TUNNEL && y < ROW_FLOOR))
            continue;
         if (y < ROW_TRUNKS)
            rgb = leaves_at(x, y);
         else if (y < ROW_PATH)
         {
            /* A trunk is round: lit from the right, where the sun is. */
            const bool first = !x || PX_KEY_CLS(top[x - 1]) != PX_CLS_PF;
            const bool last  = x + 1 >= PXC_W || PX_KEY_CLS(top[x + 1]) != PX_CLS_PF;
            rgb = px_rgb_scale(first ? 0x3A2612 : last ? 0x7A5630 : 0x5A3C1E,
                  216 + (hash((int)x, (int)y / 3, 6) * 40 >> 8));
         }
         else
            rgb = ground_at(x, y);
         top[x] = PX_KEY(PX_CLS_PF, rgb);
      }
   }
}

/* The background without a backdrop: the jungle's green lighter towards the path. */
static void paint_background(px_scene *s, unsigned pit)
{
   for (unsigned y = ROW_CANOPY; y < ROW_PATH; y++)
      px_kit_background(s, y, y + 1, px_rgb_mix(0x3E7C34, 0x86B85C,
            y < ROW_EDGE ? 0 : (y - ROW_EDGE) * 256u / (ROW_PATH - ROW_EDGE)));
   if (pit == PIT_WATER)
      for (unsigned y = ROW_PATH; y < ROW_EARTH; y++)
         px_kit_background(s, y, y + 1, px_rgb_mix(0x1C5690, 0x2C7CB8, (y - ROW_PATH) * 16));
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

/* What an object is, from what it is drawn with, where, and in which colours. */
static unsigned kind_of(const px_objects *o, const px_instance *in)
{
   const unsigned color = in->color & 0xFE;

   if (in->cls == PXC_L_BL)
      return in->y < ROW_PATH ? KIND_VINE : in->w == 4 && in->y >= ROW_EARTH ? KIND_LADDER : KIND_NONE;
   if (!px_kit_is_player(in))
      return KIND_NONE;
   if (in->y + (int)in->h <= ROW_HUD)
      return KIND_SCORE;
   if (in->y >= ROW_BELOW)
      return KIND_NAME;
   if (color == 0x10 && in->h <= 8 && in->y >= ROW_EDGE - 2 && in->y < ROW_TRUNKS)
      return KIND_BRANCH;
   if (in->cls == PXC_L_P0)
      return KIND_HARRY;
   if (in->y >= ROW_TUNNEL - 1)
      return color == 0x42 && in->h >= 12 ? KIND_WALL : KIND_SCORPION;
   if (in->y < ROW_PATH - 8)
      return KIND_NONE;
   switch (color)
   {
      case 0x12: return KIND_LOG;
      case 0x2E: return KIND_FIRE;
      case 0xD0: return KIND_CROCODILE;
      case 0x42:
      case 0x00: return KIND_COBRA;
      case 0x04: return KIND_BAG;
      default:   break;
   }
   /* A treasure that glitters: by what is below the glitter. */
   for (unsigned r = 0; r < in->h; r++)
      if ((o->colors[in->rows + r] & 0xFE) == 0x1E)
         return in->h <= 11 ? KIND_RING : KIND_GOLD;
   return color == 0x0E ? KIND_SILVER : KIND_NONE;
}

static unsigned role_of(unsigned kind)
{
   switch (kind)
   {
      case KIND_HARRY:     return PX_ROLE_PLAYER;
      case KIND_LOG:
      case KIND_FIRE:
      case KIND_COBRA:
      case KIND_CROCODILE:
      case KIND_SCORPION:  return PX_ROLE_ENEMY;
      case KIND_BAG:
      case KIND_SILVER:
      case KIND_GOLD:
      case KIND_RING:      return PX_ROLE_BONUS;
      case KIND_SCORE:
      case KIND_NAME:      return PX_ROLE_HUD;
      default:             return PX_ROLE_NONE;
   }
}

/* The colour of a row of an object, which has the game's colour `color` there; `original`
 * where the module has none for it. */
static uint32_t color_of(const pf *g, const px_instance *in, unsigned row, unsigned color, uint32_t original)
{
   /* Of 256: from the top of the object to its foot. */
   const unsigned down = in->h > 1 ? row * 256u / (in->h - 1u) : 0;

   color &= 0xFE;
   switch (in->group)
   {
      case KIND_HARRY:
         switch (color)
         {
            case 0x12: return 0x6E421A;   /* his hair */
            case 0x4A: return 0xF6B48A;   /* his face */
            case 0xC8: return 0x66DC70;   /* his shirt */
            case 0xD2: return 0x1E6A40;   /* his trousers */
            default:   return original;
         }
      case KIND_LOG:
         /* Round: light on top. */
         return px_rgb_mix(0xA87438, 0x4E3012, down);
      case KIND_BRANCH:
         return 0x5A3C1E;
      case KIND_FIRE:
         if (color == 0x10)
            return 0x4A2E14;               /* the wood */
         /* The flames: white at the tip to red at the wood, never the same for long. */
         return px_rgb_scale(px_rgb_mix(0xFFF0A0, 0xFF5A14, down * 3 / 2 > 256 ? 256 : down * 3 / 2),
               200 + (px_kit_wave(g->frame * 23 + row * 40) * 56 >> 8));
      case KIND_COBRA:
         switch (color)
         {
            case 0x42: return 0xFF2A2A;   /* its tongue */
            case 0x00: return 0x14200E;
            case 0x04: return 0x9CAA50;   /* the bands of its coils */
            default:   return original;
         }
      case KIND_CROCODILE:
         return px_rgb_mix(0x5CB040, 0x1E5A22, down);
      case KIND_SCORPION:
         return 0xD8D0B4;
      case KIND_WALL:
         return color == 0x42 ? px_rgb_scale(0xC4523A, 210 + (hash((int)row / 4, in->x, 14) * 46 >> 8))
               : 0x8E8A80;
      case KIND_BAG:
         return color == 0x12 ? 0x5A3C1E : px_rgb_mix(0xE0C890, 0x9A7C48, down);
      case KIND_SILVER:
      case KIND_GOLD:
      case KIND_RING:
      {
         /* A light that goes over it. */
         const unsigned shine = px_kit_wave(g->frame * 6 + row * 24);
         if (color == 0x0E)
            return in->group == KIND_RING ? px_rgb_add(0xB8E8FF, px_rgb_scale(0x404040, shine)) : 0xFFFFFF;
         if (color == 0x1E)
            return px_rgb_add(0xE8A818, px_rgb_scale(0x605828, shine));
         return px_rgb_add(0x9CA8B8, px_rgb_scale(0x606060, shine));
      }
      case KIND_VINE:
         return 0x8A7A32;
      case KIND_LADDER:
         return 0xD0A050;
      case KIND_SCORE:
         return 0xF6EED2;
      default:
         return original;
   }
}

/* Gives an object its colours, row by row, and the light it has. The vine and the ladder
 * are scenery, which does not glow. */
static void paint_object(const pf *g, px_scene *s, const px_instance *in)
{
   const px_objects *o = s->objects;
   const bool scenery = in->group == KIND_VINE || in->group == KIND_LADDER;
   const bool glows = in->group == KIND_FIRE;

   for (unsigned r = 0; r < in->h; r++)
   {
      const int y = in->y + (int)r;
      const uint32_t bits = o->bits[in->rows + r];
      const unsigned color = o->colors[in->rows + r];
      uint32_t rgb;
      if (y < 0 || y >= (int)s->frame->height)
         continue;
      rgb = color_of(g, in, r, color, s->frame->palette[color] & 0xFFFFFFu) & 0xFFFFFFu;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         const int x = in->x + (int)b;
         size_t i;
         if (!((bits >> b) & 1) || x < 0 || x >= PXC_W)
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         /* What is behind the playfield (Harry's legs in the tar are behind the earth) is
          * not seen, and has no light or shadow either. */
         if ((scenery && !(s->frame->tags[i] & (PXC_P0 | PXC_P1)))
               || (PX_KEY_CLS(s->top[i]) == PX_CLS_PF && (s->frame->tags[i] & PXC_PF)))
         {
            s->sprite[i] = 0;
            s->energy[i] = 0;
         }
         /* Another object may be in front of this one. */
         if (PX_KEY_CLS(s->top[i]) != PX_CLS_SPRITE || (!in->ghost && s->frame->winner[i] != color))
            continue;
         if (scenery)
         {
            s->top[i] = PX_KEY(PX_CLS_PF, in->group == KIND_VINE && y < ROW_TRUNKS
                  ? leaves_at((unsigned)x, (unsigned)y) : rgb);
            continue;
         }
         s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
         if (s->sprite[i])
            s->sprite[i] = 0xFF000000u | rgb;
         if (glows && !(in->group == KIND_FIRE && (color & 0xFE) == 0x10))
            s->energy[i] = 1;
      }
   }
}

/* ---------------------------------------------------------------------------
 * What the voices tell: the picture and the sound both go by it
 *
 * The game has voice 0 for all that Harry does, always with waveform 1, and tells the
 * sounds apart by their pitches:
 *
 *   the jump        pitch 6, 4, 3 and 2, four frames each, and a frame of 4
 *   the fall        into a hole: pitch 24 to 31, four frames each
 *   a life lost     a tune: pitch 29 for 36 frames, 26 for 8, 25 for 24, 29 for 20, then 20
 *                   and 21 in turns, four frames each, twelve times
 *   a treasure      a tune: pitch 19 for 8 frames, 14 for 4, 11 for 4, 9 for 12, 11 for 4, 9
 *                   for 13
 *   the vine        Harry's call as he swings, a tune: pitch 19 for 27 frames, 9 for 4, 11
 *                   for 48, 9 for 4, 11 for 4, 9 for 4, 11 for 29
 *
 * Voice 1 has one sound, the log that rolls over Harry: waveform 7 at pitch 16, from line 3
 * to line 258 of every frame in which it does.
 *
 * A voice that is silent has waveform 0, at the volume of 4 that both voices always have.
 * ------------------------------------------------------------------------- */

static bool in_tune(unsigned heard, unsigned pitch)
{
   switch (heard)
   {
      case HEARD_JUMP: return pitch == 6 || (pitch >= 2 && pitch <= 4);
      case HEARD_FALL: return pitch >= 24;
      case HEARD_LOST: return pitch == 29 || pitch == 26 || pitch == 25 || pitch == 20 || pitch == 21;
      case HEARD_TUNE: return pitch == 19 || pitch == 14 || pitch == 11 || pitch == 9;
      default:         return false;
   }
}

/* What voice 0 plays, which played `before` in the frame before. */
static unsigned voice0_plays(const px_kit_tia *t, unsigned before)
{
   const unsigned pitch = t->pitch[0];
   if (!t->volume[0] || !t->wave[0])
      return HEARD_NONE;
   if (t->wave[0] != 1)
      return HEARD_OTHER;
   /* It goes on with what it played, */
   if (in_tune(before, pitch))
      return before;
   /* or begins with something else. */
   switch (pitch)
   {
      case 6:  return HEARD_JUMP;
      case 24: return HEARD_FALL;
      case 19: return HEARD_TUNE;
      case 29:
      case 26:
      case 25:
      case 20:
      case 21: return HEARD_LOST;
      default: return HEARD_OTHER;
   }
}

/* A log rolls over Harry in this frame. */
static bool log_rolls(const struct pxc_frame *f)
{
   for (uint32_t i = 0; f && i < f->write_count; i++)
      if (f->writes[i].reg == 0x16 && (f->writes[i].value & 0x0F) == 7)
         return true;
   return false;
}

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static void frame(void *state, px_scene *s)
{
   pf *g = (pf*)state;
   px_objects *o = s->objects;
   unsigned pit, left = 0, right = 0, heard;
   bool treasure_seen = false;

   if (s->frame->height < ROWS || !s->frame->tags)
      return;
   if (s->advance)
      g->frame++;

   pit = find_pit(s, &left, &right);
   if (g->colors != COLORS_ORIGINAL)
   {
      paint_scenery(s);
      if (!g->backdrop)
         paint_background(s, pit);
   }

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->group = (uint8_t)kind_of(o, in);
      in->role  = (uint8_t)role_of(in->group);
      if (in->role == PX_ROLE_PLAYER && !in->ghost)
      {
         g->harry_x = in->x + 4;
         g->harry_y = in->y + (int)in->h;
      }
      else if (in->role == PX_ROLE_BONUS && s->advance)
      {
         g->treasure_x    = in->x + 4;
         g->treasure_y    = in->y + (int)in->h - 4;
         g->treasure_kind = in->group;
         treasure_seen    = true;
      }
      if (g->colors != COLORS_ORIGINAL && in->group != KIND_NONE && in->group != KIND_NAME)
         paint_object(g, s, in);
   }

   if (g->backdrop)
   {
      /* The backdrop shows where the background is black. */
      px_kit_background(s, ROW_CANOPY, ROW_BELOW, 0x000000);
      paint_backdrop(g, s, pit, left, right);
      /* Water has a light of its own. */
      if (pit == PIT_WATER && s->backdrop_on)
         for (unsigned y = ROW_PATH; y < ROW_EARTH; y++)
            for (unsigned x = left; x < right; x++)
               if (PX_KEY_CLS(s->top[(size_t)y * PXC_W + x]) == PX_CLS_BK)
                  s->light[(size_t)y * PXC_W + x] = 0xFF000000u | 0x0C3458;
   }

   if (!s->advance)
      return;

   /* What happens to Harry is heard before it is seen. */
   px_kit_tia_hear(&g->seen, s->frame);
   heard = voice0_plays(&g->seen, g->seen0);
   if (g->sparks && g->harry_x >= 0)
   {
      if (heard != g->seen0)
      {
         if (heard == HEARD_TUNE && g->treasure_gone < 30)
         {
            const uint32_t rgb = g->treasure_kind == KIND_SILVER ? 0xE0ECFF
                  : g->treasure_kind == KIND_RING ? 0xC8F0FF : 0xFFD850;
            px_scene_burst(s, g->treasure_x, g->treasure_y, rgb, 44, 380);
            px_scene_burst(s, g->treasure_x, g->treasure_y, 0xFFFFFF, 12, 200);
            px_scene_flash(s, 0xFFD060, 60);
         }
         else if (heard == HEARD_LOST)
         {
            px_scene_burst(s, g->harry_x, g->harry_y - 10, 0xFF5030, 36, 360);
            px_scene_flash(s, 0xFF2010, 100);
         }
         else if (g->seen0 == HEARD_FALL && heard == HEARD_NONE)
            /* He lands in the tunnel: dust. */
            px_scene_burst(s, g->harry_x, g->harry_y - 1, 0xA89878, 14, 160);
      }
      if (log_rolls(s->frame) && g->frame % 5 == 0)
         px_scene_burst(s, g->harry_x, g->harry_y - 3, 0xC89858, 5, 190);
   }
   g->seen0 = heard;

   if (treasure_seen)
      g->treasure_gone = 0;
   else if (g->treasure_gone < 1000)
      g->treasure_gone++;
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * What the game's are is said above, where they are told apart. The jump, the fall, the
 * log and the loss of a life are sounds of several voices here, where Harry is between left
 * and right. The three tunes are the game's, note for note and as long: for a treasure
 * bells, two octaves up, for the vine a voice that wavers, an octave up, for a life lost
 * low reeds.
 * ------------------------------------------------------------------------- */

static float pan_of(const pf *g)
{
   return px_kit_pan(g->at);
}

static void play_jump(pf *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave             freq  to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SINE,     300,  720,  0.20f, 0.004f, 0.10f, 0.16f, 0.34f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 150,  360,  0.20f, 0.004f, 0.08f, 0.14f, 0.26f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE,    5000, 0,    0.05f, 0,      0,     0.05f, 0.10f, 2400, 600, 0, 0 }
   };
   px_kit_play(s, p, 3, pan_of(g));
   px_sound_rumble(s, 0, 9000, 4);
}

static void play_fall(pf *g, px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_SINE,     980, 170, 0.46f, 0.01f, 0.30f, 0.20f, 0.30f, 0, 0, 9.0f, 0.02f },
      { PX_WAVE_TRIANGLE, 490,  85, 0.46f, 0.01f, 0.30f, 0.20f, 0.20f, 0, 0, 9.0f, 0.02f }
   };
   px_kit_play(s, p, 2, pan_of(g));
   px_sound_rumble(s, 0, 7000, 20);
}

/* He lands in the tunnel. */
static void play_landing(pf *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SINE,   120,  42, 0.10f, 0.001f, 0.02f, 0.24f, 0.80f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 3000, 500, 0.12f, 0,      0.01f, 0.16f, 0.34f, 1400, 220, 0, 0 },
      { PX_WAVE_SQUARE, 180,  70, 0.08f, 0.001f, 0.01f, 0.10f, 0.14f, 900, 260, 0, 0 }
   };
   px_kit_play(s, p, 3, pan_of(g));
   px_sound_rumble(s, 30000, 16000, 8);
}

static void play_lost(pf *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE, 5000, 400, 0.30f, 0,      0.04f, 0.50f, 0.38f, 3000, 260, 0, 0 },
      { PX_WAVE_SINE,   150,  38, 0.30f, 0.002f, 0.04f, 0.60f, 0.66f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    300,  60, 0.40f, 0.002f, 0.04f, 0.40f, 0.18f, 1800, 240, 0, 0 }
   };
   px_kit_play(s, p, 3, pan_of(g));
   px_sound_rumble(s, 65535, 36000, 40);
}

/* A log rolls over Harry, for as long as it does. */
static void play_roll(pf *g, px_sound *s, bool rolls)
{
   static const px_tone rumble = { PX_WAVE_NOISE, 900, 0, 0, 0.02f, 0, 0, 0.46f, 420, 0, 0, 0 };
   static const px_tone knock[2] = {
      { PX_WAVE_SINE,   210,  96, 0.05f, 0.001f, 0.01f, 0.09f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE, 420, 190, 0.04f, 0.001f, 0,     0.05f, 0.16f, 1600, 500, 0, 0 }
   };
   if (rolls)
      px_sound_rumble(s, 12000, 20000, 3);
   if (!rolls || !g->own_sound)
   {
      px_synth_stop(s->synth, g->roll, 0.12f);
      g->roll = 0;
      return;
   }
   if (!px_synth_move(s->synth, g->roll, pan_of(g), 1.0f, 0))
      g->roll = px_synth_play(s->synth, &rumble, pan_of(g), 1.0f);
   /* Wood on bone. */
   if (!g->rolled || g->beat % 7 == 0)
      px_kit_play(s, knock, 2, pan_of(g));
}

/* The game's tunes: the note voice 0 has, with other voices. */
static void play_note(pf *g, px_sound *s, unsigned heard, bool begins)
{
   static const px_tone bell[3] = {
      { PX_WAVE_SINE,     1, 0, 0, 0.002f, 0.02f, 0.50f, 0.34f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 1, 0, 0, 0.002f, 0.02f, 0.30f, 0.22f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,     1, 0, 0, 0.001f, 0,     0.12f, 0.10f, 0, 0, 0, 0 }
   };
   static const px_tone call_low  = { PX_WAVE_SAW,      1, 0, 0, 0.03f, 0, 0, 0.30f, 1500, 0, 5.5f, 0.012f };
   static const px_tone call_high = { PX_WAVE_TRIANGLE, 1, 0, 0, 0.03f, 0, 0, 0.30f, 0, 0, 5.5f, 0.012f };
   static const px_tone reed_low  = { PX_WAVE_SAW,      1, 0, 0, 0.02f, 0, 0, 0.40f, 700, 0, 4.0f, 0.006f };
   static const px_tone reed_high = { PX_WAVE_SQUARE,   1, 0, 0, 0.02f, 0, 0, 0.16f, 1100, 0, 4.0f, 0.006f };
   const float hz = px_kit_tia_hz(g->tia.wave[0], g->tia.pitch[0]), pan = pan_of(g) * 0.5f;
   px_tone p[3];

   if (hz <= 0.0f)
      return;
   if (heard == HEARD_TUNE && g->treasure)
   {
      /* Bells: every note rings out by itself. */
      memcpy(p, bell, sizeof(p));
      p[0].freq = hz * 4.0f;
      p[1].freq = hz * 2.0f;
      p[2].freq = hz * 12.0f;
      px_kit_play(s, p, 3, pan);
      return;
   }
   /* A voice that goes from note to note. */
   if (begins || !px_synth_move(s->synth, g->tune_low, pan, 1.0f, heard == HEARD_TUNE ? hz * 2.0f : hz))
   {
      p[0] = heard == HEARD_TUNE ? call_low : reed_low;
      p[0].freq = heard == HEARD_TUNE ? hz * 2.0f : hz;
      px_synth_stop(s->synth, g->tune_low, 0.03f);
      g->tune_low = px_synth_play(s->synth, &p[0], pan, 1.0f);
   }
   if (begins || !px_synth_move(s->synth, g->tune_high, pan, 1.0f, heard == HEARD_TUNE ? hz * 4.0f : hz * 2.0f))
   {
      p[1] = heard == HEARD_TUNE ? call_high : reed_high;
      p[1].freq = heard == HEARD_TUNE ? hz * 4.0f : hz * 2.0f;
      px_synth_stop(s->synth, g->tune_high, 0.03f);
      g->tune_high = px_synth_play(s->synth, &p[1], pan, 1.0f);
   }
}

static void stop_tune(pf *g, px_sound *s)
{
   px_synth_stop(s->synth, g->tune_low, 0.10f);
   px_synth_stop(s->synth, g->tune_high, 0.10f);
   g->tune_low = g->tune_high = 0;
}

/* A number of frames between two, by chance. */
static unsigned wait_for(pf *g, unsigned least, unsigned most)
{
   return least + px_kit_chance(&g->chance) % (most - least + 1);
}

/* One note of a bird's call. */
static void play_bird(pf *g, px_sound *s)
{
   px_tone p = { PX_WAVE_SINE, 0, 0, 0, 0.004f, 0.02f, 0.10f, 0.060f, 0, 0, 0, 0 };
   const float f = g->call_pitch;

   switch (g->call)
   {
      case 0:    /* two or three notes that rise */
         p.freq = f; p.freq_end = f * 1.35f; p.glide = 0.06f;
         g->call_wait = 7;
         break;
      case 1:    /* a trill */
         p.freq = f * 1.2f; p.hold = 0.22f; p.decay = 0.12f; p.vibrato_hz = 26.0f; p.vibrato = 0.07f;
         g->call_wait = 22;
         break;
      case 2:    /* notes that fall, quickly */
         p.freq = f * 1.5f; p.freq_end = f * 0.9f; p.glide = 0.05f; p.decay = 0.07f;
         g->call_wait = 5;
         break;
      default:   /* a dove, low */
         p.freq = f * 0.22f; p.freq_end = f * 0.19f; p.glide = 0.20f; p.attack = 0.03f;
         p.hold = 0.12f; p.decay = 0.22f; p.gain = 0.09f;
         g->call_wait = 24;
         break;
   }
   px_synth_play(s->synth, &p, g->call_pan, 1.0f);
}

/* What is heard where Harry is, with nothing happening: the jungle, or the tunnel. */
static void play_ambience(pf *g, px_sound *s)
{
   static const px_tone wind  = { PX_WAVE_NOISE, 2600, 0, 0, 1.5f, 0, 0, 0.050f, 520, 0, 0.11f, 0.30f };
   static const px_tone deep  = { PX_WAVE_SAW, 41.2f, 0, 0, 1.2f, 0, 0, 0.09f, 130, 0, 0.13f, 0.006f };
   static const px_tone chirp = { PX_WAVE_SINE, 4300, 4500, 0.02f, 0.003f, 0.012f, 0.03f, 0.030f, 0, 0, 0, 0 };
   static const px_tone drop[2] = {
      { PX_WAVE_SINE, 1500, 760, 0.05f, 0.001f, 0.004f, 0.28f, 0.13f, 0, 0, 0, 0 },
      { PX_WAVE_SINE, 3000, 1500, 0.04f, 0.001f, 0,     0.10f, 0.04f, 0, 0, 0, 0 }
   };
   const bool on = g->ambience && g->own_sound;

   if (!on || g->below)
   {
      px_synth_stop(s->synth, g->wind, 0.8f);
      g->wind = 0;
   }
   if (!on || !g->below)
   {
      px_synth_stop(s->synth, g->deep, 0.8f);
      g->deep = 0;
   }
   if (!on)
      return;

   if (g->below)
   {
      if (!px_synth_move(s->synth, g->deep, 0.0f, 1.0f, 0))
         g->deep = px_synth_play(s->synth, &deep, 0.0f, 1.0f);
      if (g->wait_drop)
         g->wait_drop--;
      else
      {
         px_kit_play(s, drop, 2, (float)(px_kit_chance(&g->chance) % 160) / 100.0f - 0.8f);
         g->wait_drop = wait_for(g, 50, 210);
      }
      return;
   }

   if (!px_synth_move(s->synth, g->wind, 0.0f, 1.0f, 0))
      g->wind = px_synth_play(s->synth, &wind, 0.0f, 1.0f);

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
      g->call_left  = g->call == 1 ? 1 : g->call == 2 ? 3 + px_kit_chance(&g->chance) % 3
            : 2 + px_kit_chance(&g->chance) % 2;
      g->call_wait  = 0;
      g->call_pan   = (float)(px_kit_chance(&g->chance) % 180) / 100.0f - 0.9f;
      g->call_pitch = 2200.0f + (float)(px_kit_chance(&g->chance) % 1400);
      g->wait_bird  = wait_for(g, 70, 280);
   }

   if (g->wait_cricket)
      g->wait_cricket--;
   else
   {
      /* Three chirps, then a while of none; one cricket to the left and one to the right. */
      const unsigned n = px_kit_chance(&g->chance);
      px_synth_play(s->synth, &chirp, n & 64 ? -0.7f : 0.6f, 1.0f);
      g->wait_cricket = (n & 3) ? 4 : wait_for(g, 40, 120);
   }
}

static void sound(void *state, px_sound *s)
{
   pf *g = (pf*)state;
   const int row = px_kit_ram(s->ram, s->ram_size, RAM_HARRY_ROW);
   unsigned heard;
   bool rolls;

   /* Where Harry is, from the picture before, and from memory. */
   if (s->objects)
   {
      bool treasure = false;
      g->at = -1;
      for (unsigned i = 0; i < s->objects->count; i++)
      {
         const px_instance *in = &s->objects->inst[i];
         if (in->role == PX_ROLE_PLAYER && !in->ghost)
         {
            g->at    = in->x + 4;
            g->below = in->y + (int)in->h > ROW_TUNNEL + 8;
         }
         else if (in->role == PX_ROLE_BONUS)
            treasure = true;
      }
      if (treasure)
         g->treasure_heard = 0;
      else if (g->treasure_heard < 1000)
         g->treasure_heard++;
   }
   if (row >= 0)
      g->below = row > 60 && row < 128;

   px_kit_tia_hear(&g->tia, s->frame);
   heard = voice0_plays(&g->tia, g->heard0);
   rolls = log_rolls(s->frame);

   if (heard != g->heard0)
   {
      if (g->heard0 == HEARD_TUNE || g->heard0 == HEARD_LOST)
         stop_tune(g, s);
      if (g->heard0 == HEARD_FALL && heard == HEARD_NONE)
      {
         if (g->own_sound) play_landing(g, s);
         else              px_sound_rumble(s, 30000, 16000, 8);
      }
      switch (heard)
      {
         case HEARD_JUMP:
            if (g->own_sound) play_jump(g, s);
            else              px_sound_rumble(s, 0, 9000, 4);
            break;
         case HEARD_FALL:
            if (g->own_sound) play_fall(g, s);
            else              px_sound_rumble(s, 0, 7000, 20);
            break;
         case HEARD_LOST:
            if (g->own_sound) play_lost(g, s);
            else              px_sound_rumble(s, 65535, 36000, 40);
            break;
         case HEARD_TUNE:
            g->treasure = g->treasure_heard < 30;
            if (g->treasure)
               px_sound_rumble(s, 16000, 26000, 10);
            else
               px_sound_rumble(s, 0, 12000, 6);
            break;
         default:
            break;
      }
      if (g->own_sound && (heard == HEARD_TUNE || heard == HEARD_LOST))
         play_note(g, s, heard, true);
   }
   else if (g->own_sound && (heard == HEARD_TUNE || heard == HEARD_LOST)
         && g->tia.pitch[0] != g->tia.was_pitch[0])
      play_note(g, s, heard, false);
   g->heard0 = heard;

   play_roll(g, s, rolls);
   g->rolled = rolls;

   if (g->own_sound)
   {
      /* What is not known is heard as the game plays it. */
      if (heard != HEARD_OTHER)
         s->voice[0] = 0.0f;
      if (rolls || !g->tia.wave[1] || !g->tia.volume[1])
         s->voice[1] = 0.0f;
   }
   play_ambience(g, s);
   g->beat++;
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   pf *g = (pf*)state;
   px_kit_tia_reset(&g->seen);
   px_kit_tia_reset(&g->tia);
   g->seen0 = g->heard0 = HEARD_NONE;
   g->harry_x = g->harry_y = -1;
   g->treasure_gone = g->treasure_heard = 1000;
   g->treasure_kind = KIND_NONE;
   g->rolled = g->treasure = g->below = false;
   g->tune_low = g->tune_high = g->roll = g->wind = g->deep = 0;
   g->at = -1;
   g->chance = 0x9E3779u;
   g->wait_bird = 90;
   g->wait_cricket = 40;
   g->wait_drop = 30;
   g->call_left = g->call_wait = 0;
}

static void *create(void)
{
   pf *g = (pf*)calloc(1, sizeof(pf));
   if (g)
   {
      unsigned scene = FIRST_SCENE;
      for (unsigned n = 0; n < 255; n++)
      {
         g->screen[scene] = (uint8_t)n;
         scene = scene_right(scene);
      }
      g->backdrop = g->sparks = g->own_sound = g->ambience = true;
      g->painted_screen = ~0u;
      g->seed = 0x71F4A1u;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   pf *g = (pf*)state;
   if (g)
   {
      px_kit_canvas_free(&g->base);
      px_kit_canvas_free(&g->still);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   pf *g = (pf*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->ambience  = px_kit_on(get, OPT_AMBIENCE);
}

const px_game px_game_pitfall = {
   "Pitfall!", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
