/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Frogger (Parker Brothers, 1982).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows   0..21   the score: both players, colour 28
 *   rows  22..23   a line of the green (background, 47 7323 in RGB)
 *   rows  24..102  the river: background of dark blue (00157D). The hedges between the
 *                  five bays at the top (rows 24..35) and the borders at the left and the
 *                  right (columns 0..7 and 152..159) are the playfield, in green.
 *   rows  25..35   a frog that has come home: player 0, 8 by 11, colour D4, in its bay
 *   rows  42..101  five lanes of the river, 13 rows apart, 7 rows high each, their things
 *                  drawn with both players, set again for every lane:
 *                    42  logs and the crocodile, 32 wide (colour 12)
 *                    55  turtles in threes (22); those that dive are blue (90, 96)
 *                    68  logs, 32 wide (12)
 *                    81  short logs in copies (12), and the lady frog (0E) on them
 *                    94  turtles in twos (22 and the blue ones that dive)
 *                  A log is solid on its top row, the crocodile has only its head and
 *                  its back there: that tells them apart, as their colour does not.
 *   rows 103..113  the median: background of yellow (E6E63E); the snake (D4, player 1)
 *                  crawls on it at the later stages
 *   rows 114..180  the road: background of black. Five lanes, 13 rows apart:
 *                    120 the truck (0E), 133 a car (58), 146 a car (D4), 159 a car (68),
 *                    172 a car (28)
 *   rows 181..190  where the frog starts: yellow again
 *   rows 191..227  green; at row 194 the frogs in reserve (white squares, player 1) and
 *                  the time left (a black bar, player 0)
 *   the frog       player 0 or 1, 8 by 7, colour D6, drawn every other frame in the
 *                  row of its lane: 182, 172, 159, 146, 133, 120, 106 (the median), then
 *                  94, 81, 68, 55, 42. Where it is lost, it lies squashed (another shape)
 *                  for about seventy frames before the next frog starts.
 *
 * The lanes move at speeds of their own, the river's in both directions: at the first
 * stage from about a quarter to a half of a pixel a frame. The module measures them from
 * the objects' tracks.
 *
 * Of its memory ($80 is 0):
 *
 *   48   the frog's column plus one
 *   80   the frogs in reserve: 4 at the start, FF when the game is over. It goes down in
 *        the frame in which the frog is lost.
 *   90   the frog's lane: 0 where it starts, 6 the median, 11 the last lane of the river.
 *        When the frog is lost or comes home it is 0 again in the same frame; home is
 *        the lane going from 11 to 0 with the frogs in reserve as they were.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_fr_colors"
#define OPT_BACKDROP "proteus_fr_backdrop"
#define OPT_SPARKS   "proteus_fr_sparks"
#define OPT_SOUND    "proteus_fr_sound"
#define OPT_AMBIENCE "proteus_fr_ambience"

#define RAM_FROG_X   48
#define RAM_LIVES    80
#define RAM_LANE     90

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define SCORE_END    22     /* the row after the score */
#define RIVER_TOP    24
#define HOMES_END    37     /* the row after the bays */
#define RIVER_END    103
#define MEDIAN_END   114
#define ROAD_END     181
#define START_END    191
#define LEFT         8      /* the first column of the river and the road */
#define RIGHT        152    /* and the column after their last */

#define LANES        5      /* of the river, and of the road */
#define LANE_PITCH   13
#define RIVER_FIRST  42     /* the top row of the river's first lane, at the top */
#define ROAD_FIRST   120    /* and of the road's, at the top */

#define LANE_TOP     11     /* the frog's lane (memory's) in the last lane of the river */
#define LANE_MEDIAN  6

#define FROG_COLOR   0xD6
#define HOME_COLOR   0xD4

#define GLINTS       90

/* A headlamp's light reaches 18 captured pixels ahead and 6 aside. */
#define BEAM_REACH   18
#define BEAM_ASIDE   13     /* half rows of the picture to a captured row, and some */
#define BEAM_ROWS    (2 * BEAM_ASIDE * PX_FX_MAX_SY + 1)
#define BEAM_COLS    (BEAM_REACH * PX_FX_MAX_SX)

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "081e2c114c9c20b61acf25fc95c71bf4",   /* Frogger (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "50",
   "reverb", "room",
   NULL
};

static const char *const colors[] = { "proteus", "Proteus's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "scenery", "Water and road", "off", "Off", NULL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Logs of wood, red turtles and the blue of those that dive, a green crocodile, a pink lady frog, a bright frog that glows, and a colour for every lane of cars, one that is not the frog's green. Or the game's own colours.",
     "proteus", colors },
   { OPT_BACKDROP, "Backdrop",
     "Water with ripples and glints that drift with each lane of the river and wash at the ends of the logs and turtles, a road of asphalt with lane markings, paving where the frog is safe, and the light of the cars' headlamps on the road.",
     "scenery", backdrop },
   { OPT_SPARKS, "Explosions",
     "A splash where the frog falls in, sparks where a car hits it, and a burst of light where a frog comes home.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the hop, the splash, the squash and the chime of a frog come home, each where it happens between left and right, and the game's tune in warmer voices. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_AMBIENCE, "Traffic and river",
     "The hum of the traffic and the rush of the river, louder as the frog is nearer. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* In the order of the options' values. */
enum { COLORS_PROTEUS = 0, COLORS_ORIGINAL };
enum { BACKDROP_SCENERY = 0, BACKDROP_OFF };

/* What an object is. */
enum
{
   KIND_NONE = 0, KIND_FROG, KIND_HOME, KIND_LADY, KIND_LOG, KIND_CROCODILE, KIND_TURTLE,
   KIND_DIVER, KIND_SNAKE, KIND_CAR, KIND_HUD
};

/* How a frog is lost, by where. */
enum { FATE_ROAD = 0, FATE_RIVER, FATE_OTHER };

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   uint16_t x, y;       /* of 10000 of the river */
   uint8_t length, phase, pace;
} glint;

typedef struct
{
   /* The options. */
   unsigned colors;
   bool backdrop, sparks, own_sound, ambience;

   uint32_t frame;               /* counts the frames that advance */
   int lives, lane;              /* as memory had them; -1 before the first frame */
   int frog_x, frog_y;           /* where the frog was last seen; -1: not yet */

   /* How fast every lane moves, in 256ths of a pixel a frame, as measured; and how far the
    * water of every lane of the river has drifted, in 256ths of a pixel. */
   int32_t river_speed[LANES], road_speed[LANES];
   int32_t drift[LANES];

   /* The backdrop. */
   px_kit_texture water_shades, ground_shades;
   px_kit_canvas water, ground;
   uint32_t water_colors[256], verge_colors[256], asphalt_colors[256];
   uint32_t water_rgb, verge_rgb;   /* the game's, that the tables are for; ~0: none yet */
   bool painted;                    /* the road and the paving are in the backdrop */
   unsigned painted_colors;
   uint8_t wet[RIVER_END - RIVER_TOP][PXC_W];
   glint glints[GLINTS];
   uint8_t beam[BEAM_ROWS][BEAM_COLS];    /* the headlamps' cone, for beam_sx by beam_sy */
   uint8_t beam_from[BEAM_ROWS], beam_to[BEAM_ROWS];
   uint32_t beam_rgb[256];
   unsigned beam_sx, beam_sy;

   /* The sounds. */
   px_kit_tia tia;
   unsigned heard0;
   int sound_lane, sound_x;      /* the lane and the column as the frame before had them */
   int sound_lives;              /* and the frogs in reserve */
   unsigned since_home;          /* frames since the frog came home; 255: long ago */
   unsigned traffic, river;      /* the ambience's voices at the synth */
} frogger;

/* As the lanes move at the first stage, until they are measured: the river's from the
 * top, the road's from the top. */
static const int32_t river_start[LANES] = { 112, -124, 54, 116, -62 };
static const int32_t road_start[LANES]  = { -32, 61, -32, 24, -60 };

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

/* The lane of the river (0 at the top) or of the road whose objects are drawn from `row`,
 * or -1. */
static int lane_at(int row, int first)
{
   const int lane = (row - first + LANE_PITCH / 2) / LANE_PITCH;
   if (row < first - LANE_PITCH / 2 || lane < 0 || lane >= LANES)
      return -1;
   return lane;
}

/* The row the frog is drawn in, in a lane of memory's. */
static int frog_row(int lane)
{
   if (lane <= 0)
      return 182;
   if (lane <= 5)
      return ROAD_FIRST + LANE_PITCH * (5 - lane);
   if (lane == LANE_MEDIAN)
      return 106;
   if (lane <= LANE_TOP)
      return RIVER_FIRST + LANE_PITCH * (LANE_TOP - lane);
   return 25;
}

static unsigned bits_in(uint32_t v)
{
   unsigned n = 0;
   for (; v; v &= v - 1)
      n++;
   return n;
}

/* What an object is, from where it is, what it is drawn with and what memory says. */
static unsigned kind_of(const px_objects *o, const px_instance *in, int ram_x, int lane)
{
   if (in->y < SCORE_END || in->y >= START_END)
      return KIND_HUD;
   if (!px_kit_is_player(in))
      return KIND_NONE;
   if (in->color == FROG_COLOR
         || (ram_x >= 1 && lane >= 0 && in->w == 8 && in->x == ram_x - 1 && in->y == frog_row(lane)))
      return KIND_FROG;
   if (in->y < HOMES_END)
      return in->color == HOME_COLOR ? KIND_HOME : KIND_NONE;
   if (in->y < RIVER_END)
   {
      const int lane_of = lane_at(in->y, RIVER_FIRST);
      switch (in->color)
      {
         case 0x0E:
            return in->w <= 8 ? KIND_LADY : KIND_NONE;
         case 0x12:
            /* A log is solid along its top; the crocodile has its head and back there. */
            if ((lane_of == 0 || lane_of == 2) && in->h >= 5
                  && bits_in(o->bits[in->rows]) * 2 < in->w)
               return KIND_CROCODILE;
            return KIND_LOG;
         case 0x22:
            return KIND_TURTLE;
         case 0x90: case 0x96:
            return KIND_DIVER;
         default:
            return KIND_NONE;
      }
   }
   if (in->y < MEDIAN_END)
      return KIND_SNAKE;
   if (in->y < ROAD_END)
      return KIND_CAR;
   return KIND_NONE;
}

static unsigned role_of(unsigned kind)
{
   switch (kind)
   {
      case KIND_FROG:      return PX_ROLE_PLAYER;
      case KIND_LADY:      return PX_ROLE_BONUS;
      case KIND_CROCODILE:
      case KIND_SNAKE:
      case KIND_CAR:       return PX_ROLE_ENEMY;
      case KIND_HUD:       return PX_ROLE_HUD;
      default:             return PX_ROLE_NONE;
   }
}

/* A row of an object as an object of its own. */
static px_instance row_of(const px_instance *in, unsigned row)
{
   px_instance one = *in;
   one.y    = (int16_t)(in->y + (int)row);
   one.h    = 1;
   one.rows = in->rows + row;
   return one;
}

/* Lighter along the top, darker along the bottom: wood, a shell, a hide. */
static void tint_rounded(px_scene *s, const px_instance *in, uint32_t top, uint32_t mid,
      uint32_t bottom)
{
   for (unsigned r = 0; r < in->h; r++)
   {
      const px_instance one = row_of(in, r);
      px_scene_tint(s, &one, r == 0 ? top : r + 1 == in->h ? bottom : mid);
   }
}

static void tint(const frogger *g, px_scene *s, const px_instance *in, unsigned kind)
{
   /* The cars from the top lane down; the game's third is the frog's green. */
   static const uint32_t cars[LANES] = { 0xF2EEE2, 0xFF4F9E, 0xF6D43A, 0x5A9CFF, 0xFF7A26 };
   int lane;

   if (g->colors == COLORS_ORIGINAL)
      return;
   switch (kind)
   {
      case KIND_FROG:
      case KIND_HOME:
         px_scene_tint(s, in, 0x62F03A);
         px_scene_energy(s, in, true);
         break;
      case KIND_LADY:
         px_scene_tint(s, in, 0xFF74C8);
         px_scene_energy(s, in, true);
         break;
      case KIND_LOG:
         tint_rounded(s, in, 0xC08A56, 0x8E5C32, 0x5C3A1E);
         break;
      case KIND_CROCODILE:
         tint_rounded(s, in, 0x6CC455, 0x3E8C35, 0x285C22);
         break;
      case KIND_TURTLE:
         tint_rounded(s, in, 0xF47050, 0xD2412A, 0x8C2A1A);
         break;
      case KIND_DIVER:
         /* Half under the water already. */
         px_scene_tint(s, in, in->color == 0x96 ? 0x4C9AAE : 0x2F7890);
         break;
      case KIND_SNAKE:
         px_scene_tint(s, in, 0xC6D83A);
         break;
      case KIND_CAR:
         lane = lane_at(in->y, ROAD_FIRST);
         if (lane >= 0)
            px_scene_tint(s, in, cars[lane]);
         break;
      default:
         break;
   }
}

/* The speeds of the lanes, from how far the objects in them moved since the frame before. */
static void measure(frogger *g, const px_objects *o)
{
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      const px_obj_track *t;
      int32_t *speed;
      int lane;

      if (in->ghost || !px_kit_is_player(in) || in->color == FROG_COLOR)
         continue;
      if (in->y >= RIVER_FIRST - 3 && in->y < RIVER_END)
      {
         lane = lane_at(in->y, RIVER_FIRST);
         speed = g->river_speed;
      }
      else if (in->y >= ROAD_FIRST - 3 && in->y < ROAD_END)
      {
         lane = lane_at(in->y, ROAD_FIRST);
         speed = g->road_speed;
      }
      else
         continue;
      t = px_objects_track(o, in->track);
      /* What comes round from the other side jumps; what flickers moved in two frames. */
      if (lane < 0 || !t || t->missed || !(t->seen & 2) || t->vx < -3 || t->vx > 3
            || in->x <= LEFT || in->x + (int)in->w >= RIGHT)
         continue;
      speed[lane] += (t->vx * 256 - speed[lane]) / 64;
   }
}

/* ---------------------------------------------------------------------------
 * The backdrop
 *
 * The river is water that the module paints: the game's blue is made dark background,
 * which is where the engine shows a backdrop, and the backdrop has a texture of water there
 * that drifts with the lane it is in, at half the lane's speed, with glints on it and a
 * wash where a log or a turtle meets the water. The road is black in the game and takes a
 * backdrop as it is: asphalt, the lanes marked with dashes between them. The yellow of the
 * median and of the start is paving. The road and the paving are painted once, into a
 * canvas from which the road is put back every frame under the light of the headlamps.
 * ------------------------------------------------------------------------- */

static bool is_water(uint32_t key)
{
   const unsigned r = (key >> 16) & 0xFF, g = (key >> 8) & 0xFF, b = key & 0xFF;
   return PX_KEY_CLS(key) == PX_CLS_BK && b >= 0x60 && b >= r + 0x40 && b >= g + 0x40;
}

static bool is_verge(uint32_t key)
{
   const unsigned r = (key >> 16) & 0xFF, g = (key >> 8) & 0xFF, b = key & 0xFF;
   return PX_KEY_CLS(key) == PX_CLS_BK && r >= 0x90 && g >= 0x90 && b + 0x40 < r;
}

static void paint_textures(frogger *g, const px_scene *s)
{
   const size_t n = (size_t)g->water_shades.w * g->water_shades.h;
   uint8_t *water = g->water_shades.shades, *ground = g->ground_shades.shades;
   px_kit_texture layer = { NULL, 0, 0 };
   uint32_t seed = 0xF4066u;

   memset(water, 56, n);
   memset(ground, 70, n);
   px_kit_texture_fit(&layer, s);
   if (layer.shades)
   {
      const uint8_t *of = layer.shades;
      /* Water: depths, ripples that lie along the lanes, and a fine grain. */
      px_kit_texture_noise(&layer, 6, 5, 21);
      for (size_t i = 0; i < n; i++)
         water[i] = (uint8_t)(water[i] + of[i] / 3);
      px_kit_texture_noise(&layer, 16, 90, 22);
      for (size_t i = 0; i < n; i++)
         if (of[i] > 140)
            water[i] = (uint8_t)(water[i] + (of[i] - 140) / 2);
      px_kit_texture_noise(&layer, 70, 150, 23);
      for (size_t i = 0; i < n; i++)
         water[i] = (uint8_t)(water[i] + of[i] / 9);
      /* Ground: patches, and grit. */
      px_kit_texture_noise(&layer, 12, 10, 24);
      for (size_t i = 0; i < n; i++)
         ground[i] = (uint8_t)(ground[i] + of[i] / 4);
      px_kit_texture_noise(&layer, 220, 200, 25);
      for (size_t i = 0; i < n; i++)
         ground[i] = (uint8_t)(ground[i] + of[i] / 5);
   }
   px_kit_texture_free(&layer);

   for (unsigned i = 0; i < GLINTS; i++)
   {
      glint *k = &g->glints[i];
      k->x      = (uint16_t)(px_kit_chance(&seed) % 10000u);
      k->y      = (uint16_t)(px_kit_chance(&seed) % 10000u);
      k->length = (uint8_t)(3 + px_kit_chance(&seed) % 8);
      k->phase  = (uint8_t)px_kit_chance(&seed);
      k->pace   = (uint8_t)(2 + px_kit_chance(&seed) % 5);
   }
}

static void water_table(frogger *g, uint32_t rgb)
{
   if (g->colors == COLORS_ORIGINAL)
      px_kit_shades(g->water_colors, px_rgb_scale(rgb, 140), rgb, px_rgb_mix(rgb, 0xFFFFFF, 170));
   else
      px_kit_shades(g->water_colors, 0x051640, 0x12408E, 0x98CCF0);
   g->water_rgb = rgb;
}

static void ground_tables(frogger *g, uint32_t verge)
{
   const uint32_t mid = g->colors == COLORS_ORIGINAL ? verge : 0xB89868;
   px_kit_shades(g->verge_colors, px_rgb_scale(mid, 150), mid, px_rgb_mix(mid, 0xFFFFFF, 90));
   px_kit_shades(g->asphalt_colors, 0x0C0C10, 0x1E1E24, 0x3A3A42);
   g->verge_rgb = verge;
}

/* Paints `rgb` over captured pixel (x, y) of the backdrop, added or in place. */
static void backdrop_pixel(px_scene *s, int x, int y, uint32_t rgb, bool add, unsigned from_v,
      unsigned to_v)
{
   for (unsigned v = from_v; v < to_v && v < s->sy; v++)
   {
      uint32_t *out = s->backdrop + ((size_t)y * s->sy + v) * s->w + (size_t)x * s->sx;
      for (unsigned u = 0; u < s->sx; u++)
         out[u] = add ? px_rgb_add(out[u], rgb) : rgb;
   }
}

/* Paints `rgb` over captured pixel (x, y) of the ground's canvas. */
static void ground_pixel(frogger *g, const px_scene *s, int x, int y, uint32_t rgb,
      unsigned from_v, unsigned to_v)
{
   for (unsigned v = from_v; v < to_v && v < s->sy; v++)
   {
      uint32_t *out = g->ground.pixels + ((size_t)y * s->sy + v) * s->w + (size_t)x * s->sx;
      for (unsigned u = 0; u < s->sx; u++)
         out[u] = rgb;
   }
}

/* The road and the paving, painted once into a canvas of their own and into the backdrop.
 * The backdrop has the road from the canvas again every frame, under the headlamps. */
static void paint_ground(frogger *g, px_scene *s)
{
   const unsigned sy = s->sy, w = s->w;
   const unsigned thin = sy >= 3 ? sy / 3 : 1;

   memset(g->ground.pixels, 0, (size_t)s->w * s->h * sizeof(uint32_t));
   memset(s->backdrop, 0, (size_t)s->w * s->h * sizeof(uint32_t));
   for (unsigned y = RIVER_END; y < START_END; y++)
   {
      const bool road = y >= MEDIAN_END && y < ROAD_END;
      for (unsigned v = 0; v < sy; v++)
      {
         const unsigned Y = y * sy + v;
         uint32_t *out = g->ground.pixels + (size_t)Y * w;
         px_kit_texture_roll(&g->ground_shades, out, Y, LEFT * s->sx, RIGHT * s->sx, 0, 0,
               road ? g->asphalt_colors : g->verge_colors);
      }
   }

   /* The paving's slabs: a joint across the middle of the strip and every 12 columns. */
   for (unsigned strip = 0; strip < 2; strip++)
   {
      const unsigned top = strip ? ROAD_END : RIVER_END, end = strip ? START_END : MEDIAN_END;
      const unsigned mid = (top + end) / 2;
      for (unsigned y = top; y < end; y++)
         for (unsigned x = LEFT; x < RIGHT; x++)
         {
            const bool across = y == mid, down = (x + (y < mid ? 0 : 6)) % 12 == 0;
            if (!across && !down)
               continue;
            const unsigned rows = across ? thin : sy, cols = across ? s->sx : (s->sx + 3) / 4;
            for (unsigned v = 0; v < rows; v++)
            {
               uint32_t *out = g->ground.pixels + ((size_t)y * sy + v) * w + (size_t)x * s->sx;
               for (unsigned u = 0; u < cols; u++)
                  out[u] = px_rgb_scale(out[u], 170);
            }
         }
   }

   /* The road's edges, solid, and the lanes between them in dashes. */
   for (unsigned x = LEFT; x < RIGHT; x++)
   {
      ground_pixel(g, s, (int)x, MEDIAN_END + 1, 0x6A6A5C, 0, thin);
      ground_pixel(g, s, (int)x, ROAD_END - 2, 0x6A6A5C, sy - thin, sy);
      for (unsigned k = 1; k < LANES; k++)
         if ((x / 6) % 2 == k % 2)
            ground_pixel(g, s, (int)x, ROAD_FIRST + LANE_PITCH * (int)k - 4, 0x8C8C7E,
                  sy / 2 - thin / 2, sy / 2 - thin / 2 + thin);
   }
   memcpy(s->backdrop + (size_t)RIVER_END * sy * w, g->ground.pixels + (size_t)RIVER_END * sy * w,
         (size_t)(START_END - RIVER_END) * sy * w * sizeof(uint32_t));
}

/* The light of a car's headlamps on the road ahead of it, as a cone that fades; and the
 * lamps themselves, and the tail lamps, as light that glows. */
/* The cone of a headlamp's light, for a size of the picture: row k is 2 Y + 1 of the
 * picture less twice the car's middle, plus BEAM_ASIDE sy; column d is how far ahead. */
static void make_beam(frogger *g, unsigned sx, unsigned sy)
{
   const int reach = BEAM_REACH * 256;
   memset(g->beam, 0, sizeof(g->beam));
   for (unsigned k = 0; k <= 2 * BEAM_ASIDE * sy && k < BEAM_ROWS; k++)
   {
      /* How far aside and how far ahead, in 256ths of a captured pixel. */
      const int aside = abs((int)k - (int)(BEAM_ASIDE * sy)) * 128 / (int)sy;
      g->beam_from[k] = 255;
      g->beam_to[k]   = 0;
      for (unsigned d = 0; d < BEAM_REACH * sx && d < BEAM_COLS; d++)
      {
         const int along = (int)d * 256 / (int)sx;
         const int half = 200 + along * 3 / 10;
         int f;
         if (aside >= half)
            continue;
         f = (reach - along) * 110 / reach;
         f = f * (reach - along) / reach;
         f = f * (half - aside) / half;
         if (along < 256)
            f = f * (128 + along / 2) / 256;
         if (f <= 0)
            continue;
         g->beam[k][d] = (uint8_t)f;
         if (d < g->beam_from[k])
            g->beam_from[k] = (uint8_t)d;
         g->beam_to[k] = (uint8_t)(d + 1);
      }
   }
   for (unsigned f = 0; f < 256; f++)
      g->beam_rgb[f] = px_rgb_scale(0xFFE2A8, f);
   g->beam_sx = sx;
   g->beam_sy = sy;
}

static void light_car(frogger *g, px_scene *s, const px_instance *in)
{
   const int lane = lane_at(in->y, ROAD_FIRST);
   const int dir = lane < 0 ? 0 : g->road_speed[lane] > 0 ? 1 : -1;
   const unsigned sx = s->sx, sy = s->sy;
   /* The front edge and the middle, in the picture's pixels. */
   const int front = dir > 0 ? (in->x + (int)in->w) * (int)sx : in->x * (int)sx - 1;
   const int mid2 = (2 * in->y + (int)in->h) * (int)sy;   /* twice the middle */
   int lamps[2];

   if (!dir || sx > PX_FX_MAX_SX || sy > PX_FX_MAX_SY)
      return;
   if (g->beam_sx != sx || g->beam_sy != sy)
      make_beam(g, sx, sy);
   for (int Y = (in->y - 7) * (int)sy; Y < (in->y + (int)in->h + 7) * (int)sy; Y++)
   {
      const int k = (2 * Y + 1) - mid2 + BEAM_ASIDE * (int)sy;
      const uint8_t *beam;
      uint32_t *out;
      if (k < 0 || k >= (int)BEAM_ROWS || Y < (MEDIAN_END + 1) * (int)sy
            || Y >= (ROAD_END - 1) * (int)sy)
         continue;
      beam = g->beam[k];
      out = s->backdrop + (size_t)Y * s->w;
      for (int d = g->beam_from[k]; d < g->beam_to[k]; d++)
      {
         const int X = front + dir * d;
         if (X >= LEFT * (int)sx && X < RIGHT * (int)sx && beam[d])
            out[X] = px_rgb_add(out[X], g->beam_rgb[beam[d]]);
      }
   }

   /* The lamps glow at the front corners, the tail lamps at the back. */
   lamps[0] = in->y + 1;
   lamps[1] = in->y + (int)in->h - 2;
   for (int k = 0; k < 2; k++)
   {
      const int x = dir > 0 ? in->x + (int)in->w : in->x - 1;
      const int back = dir > 0 ? in->x - 1 : in->x + (int)in->w;
      if (x >= LEFT && x < RIGHT)
         s->light[(size_t)lamps[k] * PXC_W + (size_t)x] = 0xFF000000u | 0xFFF0C0;
      if (back >= LEFT && back < RIGHT)
         s->light[(size_t)lamps[k] * PXC_W + (size_t)back] = 0xFF000000u | 0xC01808;
   }
}

/* Dashes of light on open water that come and go, and drift with the lanes. */
static void paint_glints(const frogger *g, px_scene *s)
{
   const unsigned sx = s->sx, sy = s->sy, w = s->w;
   const unsigned thick = (sy + 2) / 3;

   for (unsigned i = 0; i < GLINTS; i++)
   {
      const glint *k = &g->glints[i];
      const unsigned wave = px_kit_wave(k->phase + g->frame * k->pace);
      const unsigned y = RIVER_TOP + 13 + (unsigned)k->y * (RIVER_END - RIVER_TOP - 13) / 10000u;
      const int lane = lane_at((int)y - 3, RIVER_FIRST);
      const int32_t drift = lane >= 0 ? g->drift[lane] : 0;
      const unsigned length = (unsigned)k->length * sx / 4;
      const unsigned span = (RIGHT - LEFT) * sx;
      int64_t at = (int64_t)k->x * span / 10000 + (int64_t)drift * sx / 256;
      unsigned X, Y, bright;
      bool open = true;

      if (wave < 110 || !length || length >= span)
         continue;
      at %= (int64_t)span;
      X = LEFT * sx + (unsigned)(at < 0 ? at + span : at);
      Y = y * sy + (unsigned)k->y % (sy > thick ? sy - thick + 1 : 1);
      if (X + length > RIGHT * sx)
         continue;
      for (unsigned c = X / sx; c <= (X + length - 1) / sx; c++)
         open = open && g->wet[y - RIVER_TOP][c];
      if (!open)
         continue;
      bright = 30 + (wave - 110) * 5 / 4;
      for (unsigned t = 0; t < thick && Y + t < s->h; t++)
      {
         uint32_t *out = s->backdrop + (size_t)(Y + t) * w + X;
         for (unsigned u = 0; u < length; u++)
         {
            const unsigned end = u < length - 1 - u ? u : length - 1 - u;
            const unsigned part = end * 4 >= length ? 256 : 96 + end * 640 / length;
            out[u] = px_rgb_add(out[u], px_rgb_scale(0xC8E4FF, bright * part >> 8));
         }
      }
   }
}

/* Where a log or a turtle meets the water: a wash ahead of it, a wake behind. */
static void paint_wash(const frogger *g, px_scene *s, const px_instance *in)
{
   const int lane = lane_at(in->y, RIVER_FIRST);
   const int dir = lane < 0 ? 0 : g->river_speed[lane] > 0 ? 1 : -1;
   const unsigned phase = g->frame * 9 + (unsigned)in->x * 31;

   if (!dir)
      return;
   for (unsigned r = 0; r < in->h; r++)
   {
      const int y = in->y + (int)r;
      const uint32_t bits = s->objects->bits[in->rows + r];
      int lo = -1, hi = -1, front, back;
      if (!bits || y < RIVER_TOP || y >= RIVER_END)
         continue;
      /* The first and the last pixel of the row: ahead and behind as it moves. */
      for (int b = 0; b < 32; b++)
         if ((bits >> b) & 1)
         {
            if (lo < 0)
               lo = b;
            hi = b;
         }
      front = dir > 0 ? in->x + hi + 1 : in->x + lo - 1;
      back  = dir > 0 ? in->x + lo - 1 : in->x + hi + 1;
      for (int d = 0; d < 2; d++)
      {
         const int x = front + dir * d;
         if (x >= LEFT && x < RIGHT && g->wet[y - RIVER_TOP][x])
            backdrop_pixel(s, x, y, px_rgb_scale(0x587088, (unsigned)(130 - d * 70)), true, 0, s->sy);
      }
      /* The wake: a few dashes that come and go. */
      for (int d = 1; d < 6; d++)
      {
         const int x = back - dir * d;
         const unsigned flick = px_kit_wave(phase + (unsigned)(d * 53 + r * 97));
         if (flick > 150 && x >= LEFT && x < RIGHT && g->wet[y - RIVER_TOP][x])
            backdrop_pixel(s, x, y, px_rgb_scale(0x3C5870, (flick - 150) * 3 / 2 - (unsigned)d * 12), true,
                  s->sy / 3, s->sy - s->sy / 3);
      }
   }
}

static void paint_scenery(frogger *g, px_scene *s)
{
   const unsigned sx = s->sx, sy = s->sy;
   const px_objects *o = s->objects;
   uint32_t verge = ~0u, water = ~0u;
   bool fresh, stale;

   if (!s->backdrop || !sx || !sy || s->frame->height < START_END || s->w != PXC_W * sx
         || s->h < START_END * sy)
      return;
   fresh = px_kit_texture_fit(&g->water_shades, s);
   fresh = px_kit_texture_fit(&g->ground_shades, s) || fresh;
   stale = px_kit_canvas_fit(&g->water, s);
   stale = px_kit_canvas_fit(&g->ground, s) || stale;
   if (!g->water_shades.shades || !g->ground_shades.shades || !g->water.pixels
         || !g->ground.pixels)
      return;
   if (fresh)
      paint_textures(g, s);

   /* Which of the river is water, and the game's colours. */
   for (unsigned y = RIVER_TOP; y < RIVER_END; y++)
   {
      const uint32_t *top = s->top + (size_t)y * PXC_W;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const bool wet = x >= LEFT && x < RIGHT && is_water(top[x]);
         g->wet[y - RIVER_TOP][x] = wet;
         if (wet && water == ~0u)
            water = top[x] & 0xFFFFFFu;
      }
   }
   for (size_t i = (size_t)RIVER_END * PXC_W; i < (size_t)START_END * PXC_W && verge == ~0u; i++)
      if (is_verge(s->top[i]))
         verge = s->top[i] & 0xFFFFFFu;
   if (water == ~0u)
      water = g->water_rgb != ~0u ? g->water_rgb : 0x00157D;
   if (verge == ~0u)
      verge = g->verge_rgb != ~0u ? g->verge_rgb : 0xE6E63E;

   if (fresh || stale || water != g->water_rgb || g->painted_colors != g->colors)
   {
      water_table(g, water);
      px_kit_texture_show(&g->water_shades, &g->water, g->water_colors);
   }
   if (fresh || stale || s->backdrop_stale || !g->painted || verge != g->verge_rgb
         || g->painted_colors != g->colors)
   {
      ground_tables(g, verge);
      paint_ground(g, s);
      g->painted = true;
   }
   g->painted_colors = g->colors;

   /* The water, each lane's where it has drifted to. */
   for (unsigned y = RIVER_TOP; y < RIVER_END; y++)
   {
      const int lane = y < HOMES_END ? -1 : lane_at((int)y - 3, RIVER_FIRST);
      const int32_t dx = lane >= 0 ? (int32_t)((int64_t)g->drift[lane] * sx / 256) : 0;
      const uint8_t *wet = g->wet[y - RIVER_TOP];
      unsigned x = LEFT;
      while (x < RIGHT)
      {
         unsigned to = x;
         if (!wet[x])
         {
            x++;
            continue;
         }
         while (to < RIGHT && wet[to])
            to++;
         for (unsigned v = 0; v < sy; v++)
         {
            const unsigned Y = y * sy + v;
            px_kit_canvas_roll(&g->water, s->backdrop + (size_t)Y * s->w, Y, x * sx, to * sx,
                  dx, 0);
         }
         x = to;
      }
   }
   paint_glints(g, s);

   /* The road as it was painted, and the headlamps on it. */
   memcpy(s->backdrop + (size_t)MEDIAN_END * sy * s->w, g->ground.pixels + (size_t)MEDIAN_END * sy * s->w,
         (size_t)(ROAD_END - MEDIAN_END) * sy * s->w * sizeof(uint32_t));
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      if (!px_kit_is_player(in) || in->color == FROG_COLOR)
         continue;
      if (in->y >= RIVER_FIRST - 3 && in->y < RIVER_END && in->color != 0x0E)
         paint_wash(g, s, in);
      else if (in->y >= ROAD_FIRST - 3 && in->y < ROAD_END && in->role == PX_ROLE_ENEMY)
         light_car(g, s, in);
   }

   /* What is painted shows where the game has background that is dark. */
   for (unsigned y = RIVER_TOP; y < START_END; y++)
   {
      uint32_t *top = s->top + (size_t)y * PXC_W, *bk = s->bk + (size_t)y * PXC_W;
      for (unsigned x = LEFT; x < RIGHT; x++)
         if (y < RIVER_END ? g->wet[y - RIVER_TOP][x] != 0 : is_verge(top[x]))
         {
            top[x] = PX_KEY(PX_CLS_BK, 0);
            bk[x]  = 0;
         }
   }
   s->backdrop_on = true;
}

/* A frog lost where it was, or come home above it. */
static void burst(frogger *g, px_scene *s, bool home, int fate, int x, int y)
{
   if (!g->sparks || x < 0)
      return;
   if (home)
   {
      px_scene_burst(s, x + 4, RIVER_TOP + 6, 0xFFE070, 40, 300);
      px_scene_burst(s, x + 4, RIVER_TOP + 6, 0x80FF60, 16, 180);
      px_scene_flash(s, 0xFFF0A0, 60);
   }
   else if (fate == FATE_RIVER)
   {
      px_scene_burst(s, x + 4, y + 3, 0xD0F0FF, 34, 260);
      px_scene_burst(s, x + 4, y + 3, 0x4C8CE0, 18, 150);
   }
   else if (fate == FATE_ROAD)
   {
      px_scene_burst(s, x + 4, y + 3, 0xFF6A30, 36, 380);
      px_scene_burst(s, x + 4, y + 3, 0x7CF050, 14, 200);
      px_scene_flash(s, 0xFF3020, 90);
   }
   else
      px_scene_burst(s, x + 4, y + 3, 0xE8E8A0, 24, 240);
}

static int fate_of(int lane)
{
   if (lane >= 1 && lane <= 5)
      return FATE_ROAD;
   if (lane >= 7 && lane <= LANE_TOP)
      return FATE_RIVER;
   return FATE_OTHER;
}

static void frame(void *state, px_scene *s)
{
   frogger *g = (frogger*)state;
   px_objects *o = s->objects;
   const int lives = px_kit_ram(s->ram, s->ram_size, RAM_LIVES);
   const int lane  = px_kit_ram(s->ram, s->ram_size, RAM_LANE);
   const int ram_x = px_kit_ram(s->ram, s->ram_size, RAM_FROG_X);
   int frog_x = -1, frog_y = -1;

   if (s->advance)
      g->frame++;

   /* The frogs last: where a frog sits on a log, of which one of the two is drawn in the
    * frame and the other from its track, the log's colour is not to be put on the frog. */
   for (unsigned pass = 0; pass < 2; pass++)
      for (unsigned i = 0; i < o->count; i++)
      {
         px_instance *in = &o->inst[i];
         const unsigned kind = kind_of(o, in, ram_x, lane);
         const bool frog = kind == KIND_FROG || kind == KIND_LADY;
         if (frog != (pass == 1))
            continue;
         in->role = (uint8_t)role_of(kind);
         if (kind == KIND_FROG && (frog_x < 0 || !in->ghost))
         {
            frog_x = in->x;
            frog_y = in->y;
         }
         tint(g, s, in, kind);
      }

   if (s->advance)
   {
      measure(g, o);
      for (unsigned k = 0; k < LANES; k++)
      {
         /* The water drifts at half the speed of its lane. */
         g->drift[k] += g->river_speed[k] / 2;
         g->drift[k] %= 256 * 4096;
      }

      /* A frog lost, or come home. */
      if (lives >= 0 && g->lives >= 0 && ((g->lives - lives) & 0xFF) == 1)
         burst(g, s, false, fate_of(g->lane),
               frog_x >= 0 ? frog_x : g->frog_x, frog_x >= 0 ? frog_y : g->frog_y);
      else if (lane == 0 && g->lane >= LANE_TOP && lives >= 0 && lives == g->lives)
         burst(g, s, true, FATE_OTHER, g->frog_x, g->frog_y);

      g->lives = lives;
      g->lane  = lane;
      if (frog_x >= 0)
      {
         g->frog_x = frog_x;
         g->frog_y = frog_y;
      }
   }

   if (g->backdrop)
      paint_scenery(g, s);
   else
      g->painted = false;
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has these, all but the tune on voice 0:
 *
 *   the tune   at the start of a game and when it is over: voice 0 waveform 5 and voice 1
 *              waveform 12 together, a note every 15 frames from volume 14 down to 1.
 *              Voice 0 is the higher part (pitches 15 to 26); pitch 0, far too high to
 *              hear, is its rest. Voice 1 is the lower (15 to 23).
 *   the hop    waveform 4, pitches 13, 7, 16 at volumes 9, 8, 6, nine frames; a hop
 *              that follows at once begins again at 13
 *   lost       waveform 8 at pitch 16 and volume 14 for three frames, 13 at 5 for two,
 *              8 at 30 for four: the same for a car, the river, the snake and the time
 *   home       waveform 13, pitches 2, 3, 5 at volumes 14, 8, 2, six frames
 *   the time   the same four times over, 25 frames, when the time is running out: some
 *              380 frames before it is up
 *
 * Here each is a sound of several voices of its own, where the frog is between left and
 * right. Whether a car or the river took the frog is told by the lane memory had in the
 * frame before; whether the frog came home or the time runs out, by memory having had the
 * frog in the last lane of the river a frame or two before. The tune is the game's, its
 * notes played a little lower and warmer. What is not known is heard as the game plays it.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_HOP, SOUND_LOST, SOUND_CHIME, SOUND_TUNE, SOUND_KNOWN, SOUND_OTHER };

/* Frames after the frog left the last lane of the river for home in which the game's
 * chime begins. */
#define HOME_SOON 4

static unsigned voice0_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   if (!volume)
      return SOUND_NONE;
   switch (wave)
   {
      case 4:  return pitch == 13 ? SOUND_HOP : pitch == 7 || pitch == 16 ? SOUND_KNOWN : SOUND_OTHER;
      case 5:  return SOUND_TUNE;
      case 8:  return pitch == 16 ? SOUND_LOST : pitch == 30 ? SOUND_KNOWN : SOUND_OTHER;
      case 13: return pitch == 2 ? SOUND_CHIME : pitch == 3 || pitch == 5 ? SOUND_KNOWN : SOUND_OTHER;
      default: return SOUND_OTHER;
   }
}

static float pan_of(int column)
{
   return px_kit_pan(column >= 0 ? column + 4 : -1);
}

static void play_hop(px_sound *s, int column)
{
   static const px_tone p[3] = {
      /* wave             freq  to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_TRIANGLE, 300,  820,  0.06f, 0.002f, 0.01f, 0.11f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE,   150,  410,  0.06f, 0.002f, 0,     0.08f, 0.10f, 2400, 700, 0, 0 },
      { PX_WAVE_NOISE,   5000,  0,    0,     0,      0,     0.025f, 0.08f, 3500, 1200, 0, 0 }
   };
   px_kit_play(s, p, 3, pan_of(column));
   px_sound_rumble(s, 0, 7000, 2);
}

static void play_lost(px_sound *s, int fate, int column)
{
   static const px_tone road[3] = {
      { PX_WAVE_NOISE,  5000, 600, 0.20f, 0,      0.01f, 0.28f, 0.55f, 4000, 300, 0, 0 },
      { PX_WAVE_SQUARE,  220,  55, 0.18f, 0.001f, 0.02f, 0.24f, 0.24f, 1500, 200, 0, 0 },
      { PX_WAVE_SINE,    110,  38, 0.15f, 0.001f, 0.03f, 0.32f, 0.80f, 0, 0, 0, 0 }
   };
   static const px_tone river[4] = {
      { PX_WAVE_NOISE,  9000, 1500, 0.35f, 0.002f, 0.03f, 0.50f, 0.42f, 7000, 450, 0, 0 },
      { PX_WAVE_SINE,    900,  180, 0.30f, 0.002f, 0.02f, 0.38f, 0.26f, 0, 0, 18.0f, 0.08f },
      { PX_WAVE_SINE,   1400,  320, 0.22f, 0.020f, 0,     0.28f, 0.14f, 0, 0, 23.0f, 0.10f },
      { PX_WAVE_SINE,     90,   45, 0.20f, 0.001f, 0.02f, 0.30f, 0.60f, 0, 0, 0, 0 }
   };
   static const px_tone other[2] = {
      { PX_WAVE_SAW,     420,   90, 0.70f, 0.005f, 0.05f, 0.80f, 0.28f, 2200, 300, 7.0f, 0.05f },
      { PX_WAVE_SINE,    210,   60, 0.70f, 0.005f, 0.05f, 0.80f, 0.50f, 0, 0, 7.0f, 0.05f }
   };
   const float pan = pan_of(column);
   if (fate == FATE_ROAD)
   {
      px_kit_play(s, road, 3, pan);
      px_sound_rumble(s, 60000, 40000, 20);
   }
   else if (fate == FATE_RIVER)
   {
      px_kit_play(s, river, 4, pan);
      px_sound_rumble(s, 26000, 42000, 14);
   }
   else
   {
      px_kit_play(s, other, 2, pan);
      px_sound_rumble(s, 30000, 30000, 16);
   }
}

static void play_home(px_sound *s, int column)
{
   /* A chime: a major chord that rings. */
   static const px_tone p[4] = {
      { PX_WAVE_SINE,     1046.5f, 0, 0, 0.002f, 0.02f, 0.90f, 0.32f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,     1318.5f, 0, 0, 0.002f, 0.02f, 0.80f, 0.22f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,     1568.0f, 0, 0, 0.002f, 0.02f, 0.70f, 0.20f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 2093.0f, 0, 0, 0.001f, 0,     0.35f, 0.10f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 4, pan_of(column) * 0.6f);
   px_sound_rumble(s, 12000, 22000, 10);
}

/* The time runs out: a beep like a watch's, from where the time is shown. */
static void play_hurry(px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_SQUARE, 1760, 0, 0, 0.002f, 0.03f, 0.08f, 0.10f, 3500, 2000, 0, 0 },
      { PX_WAVE_SINE,    880, 0, 0, 0.002f, 0.03f, 0.10f, 0.22f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 2, px_kit_pan(140));
   px_sound_rumble(s, 0, 4000, 2);
}

/* A note of the tune: the higher part like a marimba, the lower like a soft bass, both an
 * octave down. */
static void play_note(px_sound *s, bool high, float hz)
{
   const float f = px_kit_tune(hz / 2.0f);
   if (f <= 0.0f || f > 4000.0f)
      return;
   if (high)
   {
      const px_tone p[3] = {
         { PX_WAVE_TRIANGLE, f,        0, 0, 0.004f, 0.03f, 0.30f, 0.56f, 0, 0, 5.5f, 0.003f },
         { PX_WAVE_SQUARE,   f,        0, 0, 0.004f, 0.02f, 0.22f, 0.15f, 2600, 900, 0, 0 },
         { PX_WAVE_SINE,     f * 4.0f, 0, 0, 0.001f, 0,     0.10f, 0.08f, 0, 0, 0, 0 }
      };
      px_kit_play(s, p, 3, -0.2f);
   }
   else
   {
      const px_tone p[2] = {
         { PX_WAVE_SINE, f, 0, 0, 0.004f, 0.04f, 0.32f, 0.75f, 0, 0, 0, 0 },
         { PX_WAVE_SAW,  f, 0, 0, 0.004f, 0.02f, 0.26f, 0.22f, 800, 250, 0, 0 }
      };
      px_kit_play(s, p, 2, 0.2f);
   }
}

/* A note begins: louder than a note that has died down, or at another pitch. The long
 * note at the end of the tune wavers between 12 and 14, which is not a note begun. */
static bool note_begins(const px_kit_tia *t, unsigned voice)
{
   if (!t->volume[voice])
      return false;
   if (t->wave[voice] != t->was_wave[voice] || !t->was_volume[voice])
      return true;
   if (t->pitch[voice] != t->was_pitch[voice])
      return t->volume[voice] >= 8;
   return px_kit_tia_louder(t, voice) && t->was_volume[voice] < 8;
}

/* The hum of the traffic and the rush of the river, louder the nearer the frog is. */
static void play_ambience(frogger *g, px_sound *s, int lane, int lives)
{
   static const px_tone traffic = { PX_WAVE_NOISE, 900,  0, 0, 1.2f, 0, 0, 1.0f, 260,  0, 0.23f, 0.25f };
   static const px_tone river   = { PX_WAVE_NOISE, 7000, 0, 0, 1.2f, 0, 0, 1.0f, 1300, 0, 0.37f, 0.30f };
   float near_road, near_river;

   if (!g->ambience || !g->own_sound || lane < 0 || lives < 0 || lives == 0xFF)
   {
      px_synth_stop(s->synth, g->traffic, 0.8f);
      px_synth_stop(s->synth, g->river, 0.8f);
      g->traffic = g->river = 0;
      return;
   }
   near_road  = lane <= 5 ? 1.0f : lane == LANE_MEDIAN ? 0.7f : 0.35f;
   near_river = lane >= 7 ? 1.0f : lane == LANE_MEDIAN ? 0.6f : 0.25f;
   if (!px_synth_move(s->synth, g->traffic, -0.3f, 0.20f * near_road, 0))
      g->traffic = px_synth_play(s->synth, &traffic, -0.3f, 0.20f * near_road);
   if (!px_synth_move(s->synth, g->river, 0.3f, 0.10f * near_river, 0))
      g->river = px_synth_play(s->synth, &river, 0.3f, 0.10f * near_river);
}

static void sound(void *state, px_sound *s)
{
   frogger *g = (frogger*)state;
   const int lane  = px_kit_ram(s->ram, s->ram_size, RAM_LANE);
   const int lives = px_kit_ram(s->ram, s->ram_size, RAM_LIVES);
   const int ram_x = px_kit_ram(s->ram, s->ram_size, RAM_FROG_X);
   int column = ram_x >= 1 ? ram_x - 1 : -1;
   unsigned heard;

   /* Where the frog is, from the picture before if memory does not say. */
   for (unsigned i = 0; column < 0 && s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_PLAYER && !in->ghost)
         column = in->x;
   }

   /* The frog left the last lane of the river, and was not lost: it came home. */
   if (lane == 0 && g->sound_lane >= LANE_TOP && lives >= 0 && lives == g->sound_lives)
      g->since_home = 0;
   else if (g->since_home < 255)
      g->since_home++;

   px_kit_tia_hear(&g->tia, s->frame);
   heard = voice0_plays(g->tia.wave[0], g->tia.pitch[0], g->tia.volume[0]);
   if (heard == SOUND_HOP && (g->heard0 != SOUND_HOP || px_kit_tia_louder(&g->tia, 0)))
   {
      if (g->own_sound) play_hop(s, column);
      else              px_sound_rumble(s, 0, 7000, 2);
   }
   else if (heard == SOUND_LOST && g->heard0 != SOUND_LOST)
   {
      /* Memory has the frog back where it starts by now: where it was is of the frame
       * before. */
      const int where = lane > 0 ? lane : g->sound_lane;
      const int at = lane > 0 ? column : g->sound_x;
      if (g->own_sound)
         play_lost(s, fate_of(where), at);
      else
         px_sound_rumble(s, 50000, 40000, 18);
   }
   else if (heard == SOUND_CHIME && g->heard0 != SOUND_CHIME)
   {
      const bool home = g->since_home <= HOME_SOON;
      if (g->own_sound && home)
         play_home(s, g->sound_x);
      else if (g->own_sound)
         play_hurry(s);
      else if (home)
         px_sound_rumble(s, 12000, 22000, 10);
      else
         px_sound_rumble(s, 0, 4000, 2);
   }
   if (g->own_sound)
   {
      if (heard == SOUND_TUNE && note_begins(&g->tia, 0) && g->tia.pitch[0])
         play_note(s, true, px_kit_tia_hz(5, g->tia.pitch[0]));
      if (g->tia.wave[1] == 12 && note_begins(&g->tia, 1))
         play_note(s, false, px_kit_tia_hz(12, g->tia.pitch[1]));
      if (heard != SOUND_OTHER)
         s->voice[0] = 0.0f;
      if (!g->tia.volume[1] || g->tia.wave[1] == 12)
         s->voice[1] = 0.0f;
   }
   g->heard0 = heard;
   if (lane >= 0)
      g->sound_lane = lane;
   g->sound_lives = lives;
   if (column >= 0)
      g->sound_x = column;
   play_ambience(g, s, lane, lives);
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   frogger *g = (frogger*)state;
   g->lives = g->lane = -1;
   g->frog_x = g->frog_y = -1;
   memcpy(g->river_speed, river_start, sizeof(g->river_speed));
   memcpy(g->road_speed, road_start, sizeof(g->road_speed));
   memset(g->drift, 0, sizeof(g->drift));
   px_kit_tia_reset(&g->tia);
   g->heard0 = SOUND_NONE;
   g->sound_lane = 0;
   g->sound_x = -1;
   g->sound_lives = -1;
   g->since_home = 255;
   g->traffic = g->river = 0;
}

static void *create(void)
{
   frogger *g = (frogger*)calloc(1, sizeof(frogger));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = g->ambience = true;
      g->water_rgb = g->verge_rgb = ~0u;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   frogger *g = (frogger*)state;
   if (g)
   {
      px_kit_texture_free(&g->water_shades);
      px_kit_texture_free(&g->ground_shades);
      px_kit_canvas_free(&g->water);
      px_kit_canvas_free(&g->ground);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   frogger *g = (frogger*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_pick(get, OPT_BACKDROP, backdrop) == BACKDROP_SCENERY;
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->ambience  = px_kit_on(get, OPT_AMBIENCE);
   g->painted   = false;
}

const px_game px_game_frogger = {
   "Frogger", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
