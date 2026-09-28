/* SPDX-License-Identifier: GPL-3.0-or-later */
/* River Raid (Activision, 1982).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  13..173  the river: the banks and the islands are the playfield, the water is
 *                  the background (colour 84). The banks are of one green (D6) from a
 *                  bridge to the next and of another (D2) from there on, in turns. A
 *                  bridge's road is playfield too, in greys (04, 08) with a yellow line
 *                  (1C). The first eight columns are blanked on every line (HMOVE).
 *   the river's    player 1, set again for every block of 32 rows, of which the picture
 *   objects        has five and a part of a sixth. A block has one object: a helicopter
 *                  (8 wide), a ship (16), a jet (8), a fuel depot (8 by 24, its letters
 *                  are holes), a house with a tree (16, on the bank, in two parts with a
 *                  row of nothing between) or the bridge (32). All have a colour a row.
 *                  What is hit becomes three shapes of wreckage, some 15 frames each.
 *   rows 156..168  the jet: player 0, 13 rows of one colour (1C), 14 when it banks
 *   the shot       missile 0
 *   row  174       a black line
 *   rows 175..211  the panel, on grey background: the score, the fuel gauge, the jets in
 *                  reserve and the maker's name are both players in three copies each;
 *                  the gauge's needle is the ball
 *
 * Of its memory ($80 is 0):
 *
 *   11        the line of a block the river has come to, 0..31. It goes with the picture
 *             of the same frame: the objects are a row further down when it is one more.
 *             The block whose object is i of the six ends above row 172 + this - 32 i.
 *   12        and the part of a line, of 256, to which twice the speed is added a frame
 *   32..37    what the six blocks have, the lowest first: 0 nothing, 1 to 3 wreckage, 4 a
 *             jet, 5 and 6 a helicopter (its rotor one way and the other), 7 a ship, 8 the
 *             bridge, 9 a house, 10 a fuel depot. When the line goes round they move on
 *             by one. A hit shows here a frame before the picture has the wreckage.
 *   50, 117   the shot's row, and its column plus one
 *   51        the jet's column
 *   53        the speed: 64 slow, 128 as it is left alone, 254 fast; a line a frame at 128
 *   55        the fuel, 255 full; the warning sounds below 64
 *   57        the blocks to the next bridge
 *   58        the jet's shape: BB level, CD banking, DF wrecked, 0 none
 *   64        the jets in reserve times eight; 88 and 89 for none
 *
 * Its sounds are listed where they are told apart, below.
 *
 * This is the first game whose scenery moves. The water and the banks are textures of the
 * kit's (px_kit_texture) rolled into the backdrop by how far memory says the river has
 * moved (px_kit_scroll); where they are shown the game's own is made dark background, which
 * is where a backdrop shows. See paint_river().
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>
#ifdef RR_TIMING
#include <stdio.h>
#include <time.h>
#endif

#define OPT_COLORS  "proteus_rr_colors"
#define OPT_BACKDROP "proteus_rr_backdrop"
#define OPT_SPARKS   "proteus_rr_sparks"
#define OPT_SOUND    "proteus_rr_sound"
#define OPT_ROTORS   "proteus_rr_rotors"

#define RAM_LINE     11
#define RAM_BLOCKS   32
#define RAM_SHOT_Y   50
#define RAM_JET_X    51
#define RAM_SPEED    53
#define RAM_FUEL     55
#define RAM_JET      58
#define RAM_SHOT_X   117

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define TOP        13     /* the river's first row */
#define BOTTOM     174    /* and the row after its last */
#define ROWS       (BOTTOM - TOP)
#define PANEL      175
#define BLOCK      32     /* rows to a block of the river */
#define BLOCKS     6
#define FIRST_END  172    /* the row after the lowest block's last, with the line at 0 */

#define JET_WRECKED 0xDF
#define FUEL_LOW    64

enum
{
   KIND_NONE = 0, KIND_WRECK, KIND_WRECK_2, KIND_WRECK_3, KIND_JET, KIND_HELICOPTER,
   KIND_HELICOPTER_2, KIND_SHIP, KIND_BRIDGE, KIND_HOUSE, KIND_FUEL, KINDS
};

/* px_instance.group: which of the enemies one is. */
enum { GROUP_HELICOPTER = 0, GROUP_SHIP, GROUP_JET, GROUP_BRIDGE };

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "393948436d1f4cc3192410bb918f9724",   /* River Raid (USA) */
   NULL
};

static const char *const fx[] = {
   "width", "50",
   NULL
};

static const char *const colors[] = { "proteus", "Proteus's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "river", "Water and banks", "water", "Water",
   "off", "Off", NULL };
/* In the order of the options' values. */
enum { COLORS_PROTEUS = 0, COLORS_ORIGINAL };
enum { BACKDROP_RIVER = 0, BACKDROP_WATER, BACKDROP_OFF };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Helicopters, ships and jets in colours that stand out from the water, the fuel depots' letters lit, and a river of deeper blue between banks of richer green; or the game's own colours.",
     "proteus", colors },
   { OPT_BACKDROP, "Backdrop",
     "Water with ripples and glints that moves with the river, shallow at the banks, and banks of grass with a shore of sand; the water alone; or the game's flat colours.",
     "river", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks and a flash where something is hit, more of both for a bridge, and the jet's own wreck.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the engine, the shot, the explosions, the fuel and its warning, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_ROTORS, "Helicopter rotors",
     "The beat of a helicopter's rotor, louder as it comes nearer. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

/* The river's six blocks as memory has them, followed from frame to frame. The picture and
 * the sound each follow them, since either may be without the other. */
typedef struct
{
   int line;              /* the line the river had come to; -1: not known */
   int had[BLOCKS];       /* what the blocks had, moved on as they were */
   int kind[BLOCKS];      /* what each shows in the frame at hand: KIND_*, -1: not known */
   int hit[BLOCKS];       /* what was hit in it in this frame: KIND_*, or 0 */
} blocks;

#define LANDS  4
#define GLINTS 160

/* A glint on the water: a dash of light that comes and goes. */
typedef struct
{
   uint16_t x, y;       /* in the hundredths of a picture that goes round */
   uint8_t length;      /* in eighths of one of the game's pixels */
   uint8_t phase, pace;
} glint;

typedef struct
{
   /* The options. */
   unsigned colors, backdrop;
   bool sparks, own_sound, rotors;

   uint32_t frame;               /* counts the frames that advance */
   blocks seen;                  /* the blocks, for the picture */
   int jet;                      /* the jet's shape as memory had it; -1: not known */

   /* The river. */
   px_kit_scroll scroll;
   px_kit_texture water_shades, land_shades;
   px_kit_canvas water, land;    /* the textures in the colours at hand */
   uint32_t water_colors[256], sand_colors[256];
   uint32_t water_rgb;           /* the colour of the game's the water is for; ~0: none */
   uint32_t land_colors[LANDS][256];
   uint32_t land_rgb[LANDS];     /* the greens of the game's the tables are for */
   unsigned lands, land_next;
   uint32_t canvas_rgb;          /* the green of the game's the land's canvas is for */
   unsigned painted_colors;      /* the option the tables were made with */
   glint glints[GLINTS];
   uint8_t side[ROWS + 2][PXC_W];    /* 255 where the land is, 0 where the water */
   uint8_t shore[ROWS + 2][PXC_W];   /* the same, going over from one to the other */
   uint8_t land_of[ROWS];            /* the table of its green, for every row */
   uint16_t across[ROWS + 2][PXC_W]; /* to work the shore out with */

   /* What objects get for colours: a table a kind. */
   uint32_t paint[KINDS][256];

   /* The sounds. */
   px_kit_tia tia;               /* the game's two voices */
   blocks heard;                 /* the blocks, for the sound */
   unsigned heard0, heard1;      /* what the voices played in the frame before */
   unsigned engine[3];           /* the engine's voices at the synth */
   int engine_pitch;             /* the game's pitch for it, as heard last */
   unsigned beat;                /* counts frames for the rotors */
   int jet_at, shot_at;          /* columns, for where a sound is; -1: not known */
} rr;

/* ---------------------------------------------------------------------------
 * The blocks
 * ------------------------------------------------------------------------- */

static void blocks_reset(blocks *k)
{
   k->line = -1;
   for (unsigned b = 0; b < BLOCKS; b++)
   {
      k->had[b]  = -1;
      k->kind[b] = -1;
      k->hit[b]  = 0;
   }
}

/* Reads the blocks after a frame. What is hit is wreckage in memory a frame before it is in
 * the picture: for that frame a block is what it was, and `hit` says so. */
static void blocks_follow(blocks *k, const uint8_t *ram, size_t size)
{
   const int line = px_kit_ram(ram, size, RAM_LINE);

   if (line < 0 || size < RAM_BLOCKS + BLOCKS)
   {
      blocks_reset(k);
      return;
   }
   /* The line went round: the blocks moved on by one. */
   if (k->line >= 0 && line < k->line)
   {
      for (unsigned b = 0; b + 1 < BLOCKS; b++)
         k->had[b] = k->had[b + 1];
      k->had[BLOCKS - 1] = -1;
   }
   for (unsigned b = 0; b < BLOCKS; b++)
   {
      const int now = ram[RAM_BLOCKS + b];
      k->kind[b] = now;
      k->hit[b]  = 0;
      if (now == KIND_WRECK && k->had[b] >= KIND_JET && k->had[b] <= KIND_FUEL)
         k->kind[b] = k->hit[b] = k->had[b];
      k->had[b] = now;
   }
   k->line = line;
}

/* Which block a row is in; -1 if in none or not known. */
static int block_of(const blocks *k, int row)
{
   int b;
   if (k->line < 0)
      return -1;
   b = FIRST_END + k->line - 1 - row;
   b = b < 0 ? 0 : b / BLOCK;
   return b < BLOCKS ? b : -1;
}

/* ---------------------------------------------------------------------------
 * The river
 *
 * The scenery is painted at the picture's size and not the game's. Every pixel of the
 * game's river is land or water (what is on top of either, an object or a road, is of the
 * side that is above it). That, blurred a little, says how near the other side is, and
 * between the pixels' middles it goes over smoothly: water, shallows, foam, the wet and
 * the dry sand, grass. What the game has as land is never painted as water nor the other
 * way round: the shore is where the game has it, to the pixel.
 *
 * Where the river is painted the game's pixel is made background of black, which is where
 * the engine shows a backdrop.
 * ------------------------------------------------------------------------- */

static uint32_t brighter(uint32_t rgb, unsigned f256)
{
   unsigned r = ((rgb >> 16) & 0xFF) * f256 >> 8, g = ((rgb >> 8) & 0xFF) * f256 >> 8;
   unsigned b = (rgb & 0xFF) * f256 >> 8;
   return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

static bool is_water(uint32_t key)
{
   const unsigned r = (key >> 16) & 0xFF, g = (key >> 8) & 0xFF, b = key & 0xFF;
   return PX_KEY_CLS(key) == PX_CLS_BK && b >= 0x60 && b >= r + 0x40 && b >= g + 0x40;
}

static bool is_land(uint32_t key)
{
   const unsigned r = (key >> 16) & 0xFF, g = (key >> 8) & 0xFF, b = key & 0xFF;
   return PX_KEY_CLS(key) == PX_CLS_PF && g >= r + 0x18 && g >= b + 0x18;
}

static void water_table(rr *g, uint32_t rgb)
{
   if (g->colors == COLORS_ORIGINAL)
      px_kit_shades(g->water_colors, px_rgb_scale(rgb, 150), rgb, px_rgb_mix(rgb, 0xFFFFFF, 190));
   else
      px_kit_shades(g->water_colors, 0x0A1C5A, 0x1C4CB0, 0xC4ECFF);
   px_kit_shades(g->sand_colors, 0x9C8250, 0xD8C080, 0xF4E4B0);
   g->water_rgb = rgb;
}

/* The table for a green of the game's, made when the green is first seen. */
static unsigned land_table(rr *g, uint32_t rgb)
{
   uint32_t mid = rgb;
   unsigned t;
   for (t = 0; t < g->lands; t++)
      if (g->land_rgb[t] == rgb)
         return t;
   t = g->land_next;
   g->land_next = (g->land_next + 1) % LANDS;
   if (g->lands < LANDS)
      g->lands++;
   if (g->colors != COLORS_ORIGINAL)
   {
      /* Less yellow, more of the green. */
      const unsigned r = ((rgb >> 16) & 0xFF) * 200 >> 8, gr = ((rgb >> 8) & 0xFF) * 270 >> 8;
      const unsigned b = (rgb & 0xFF) * 215 >> 8;
      mid = (r << 16) | ((gr > 255 ? 255 : gr) << 8) | b;
   }
   px_kit_shades(g->land_colors[t], px_rgb_scale(mid, 160), mid,
         px_rgb_add(brighter(mid, 330), 0x181C00));
   g->land_rgb[t] = rgb;
   return t;
}

/* The textures, painted once for a size of the picture. They are in cells of chance to the
 * picture and not to the pixel, so they look alike at every size. */
static void paint_textures(rr *g, const px_scene *s)
{
   const size_t n = (size_t)g->water_shades.w * g->water_shades.h;
   uint8_t *water = g->water_shades.shades, *land = g->land_shades.shades;
   px_kit_texture layer = { NULL, 0, 0 };
   uint32_t seed = 0x2A1Du;

   memset(water, 60, n);
   memset(land, 52, n);
   px_kit_texture_fit(&layer, s);
   if (layer.shades)
   {
      const uint8_t *of = layer.shades;
      /* Water: depths, ripples that lie across the river, and a fine grain. */
      px_kit_texture_noise(&layer, 7, 5, 11);
      for (size_t i = 0; i < n; i++)
         water[i] = (uint8_t)(water[i] + of[i] / 3);
      px_kit_texture_noise(&layer, 18, 70, 12);
      for (size_t i = 0; i < n; i++)
         if (of[i] > 150)
            water[i] = (uint8_t)(water[i] + (of[i] - 150) / 2);
      px_kit_texture_noise(&layer, 64, 120, 13);
      for (size_t i = 0; i < n; i++)
         water[i] = (uint8_t)(water[i] + of[i] / 8);
      /* Land: meadows lighter and darker, and the grass's grain. */
      px_kit_texture_noise(&layer, 9, 7, 16);
      for (size_t i = 0; i < n; i++)
         land[i] = (uint8_t)(land[i] + of[i] / 3);
      px_kit_texture_noise(&layer, 200, 150, 17);
      for (size_t i = 0; i < n; i++)
         land[i] = (uint8_t)(land[i] + of[i] / 5);
      px_kit_texture_noise(&layer, 90, 40, 18);
      for (size_t i = 0; i < n; i++)
         land[i] = (uint8_t)(land[i] + of[i] / 8);
   }
   px_kit_texture_free(&layer);

   for (unsigned i = 0; i < GLINTS; i++)
   {
      glint *k = &g->glints[i];
      k->x      = (uint16_t)(px_kit_chance(&seed) % 10000u);
      k->y      = (uint16_t)(px_kit_chance(&seed) % 10000u);
      k->length = (uint8_t)(3 + px_kit_chance(&seed) % 10);
      k->phase  = (uint8_t)px_kit_chance(&seed);
      k->pace   = (uint8_t)(2 + px_kit_chance(&seed) % 5);
   }
}

/* Which side every pixel of the river is on, and how near the other. Returns whether there
 * is a river at all. */
static bool find_shore(rr *g, const px_scene *s)
{
   uint16_t (*across)[PXC_W] = g->across;
   unsigned found = 0;
   int land = -1;

   for (unsigned x = 0; x < PXC_W; x++)
   {
      int last = -1;
      for (unsigned r = 0; r < ROWS; r++)
      {
         const uint32_t key = s->top[(size_t)(TOP + r) * PXC_W + x];
         if (is_water(key))
            last = 0;
         else if (is_land(key))
            last = 255;
         /* What is on top of the river is of the side above it; 1 until that is known. */
         g->side[r + 1][x] = (uint8_t)(last < 0 ? 1 : last);
         found += last >= 0;
      }
      /* And what is at the top, of the side below. */
      last = 255;
      for (unsigned r = ROWS; r-- > 0;)
         if (g->side[r + 1][x] == 1)
            g->side[r + 1][x] = (uint8_t)last;
         else
            last = g->side[r + 1][x];
      g->side[0][x]        = g->side[1][x];
      g->side[ROWS + 1][x] = g->side[ROWS][x];
   }
   if (!found)
      return false;

   /* The green of every row: where a row has none (a road), that of the row above. */
   for (unsigned r = 0; r < ROWS; r++)
   {
      const uint32_t *row = s->top + (size_t)(TOP + r) * PXC_W;
      for (unsigned x = 0; x < PXC_W; x += 4)
         if (is_land(row[x]))
         {
            land = (int)land_table(g, row[x] & 0xFFFFFFu);
            break;
         }
      g->land_of[r] = (uint8_t)(land < 0 ? 0 : land);
   }
   if (land < 0)
      land_table(g, 0x639336);

   /* A blur of three across and five down, which is about as far either way. */
   for (unsigned r = 0; r < ROWS + 2; r++)
      for (unsigned x = 0; x < PXC_W; x++)
         across[r][x] = (uint16_t)(g->side[r][x ? x - 1 : 0] + g->side[r][x]
               + g->side[r][x + 1 < PXC_W ? x + 1 : x]);
   for (unsigned r = 0; r < ROWS + 2; r++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         unsigned sum = 0;
         for (int d = -2; d <= 2; d++)
         {
            const int at = (int)r + d;
            sum += across[at < 0 ? 0 : at > ROWS + 1 ? ROWS + 1 : at][x];
         }
         g->shore[r][x] = (uint8_t)(sum / 15);
      }
   return true;
}

/* A pixel of the picture where the river goes over from water to land: `near` is 0 in open
 * water, 127 at the water's edge, 128 at the land's and 255 far from the water. */
static uint32_t shore_pixel(const rr *g, unsigned x, unsigned y, unsigned near, int32_t dy,
      unsigned land)
{
   if (near < 128)
   {
      unsigned shade = px_kit_texture_at(&g->water_shades, x, y, 0, dy);
      /* Shallows, and foam at the very edge. */
      shade += near / 3;
      if (near > 100)
         shade += (near - 100) * 3;
      return g->water_colors[shade > 255 ? 255 : shade];
   }
   else
   {
      const unsigned shade = px_kit_texture_at(&g->land_shades, x, y, 0, dy);
      const unsigned far = near - 128;
      uint32_t sand = g->sand_colors[shade];
      /* Wet sand at the edge, dry sand, and grass from a third of the way on. */
      if (far < 12)
         sand = px_rgb_scale(sand, 176 + far * 6);
      if (far < 40)
         return sand;
      if (far >= 100)
         return g->land_colors[land][shade];
      return px_rgb_mix(sand, g->land_colors[land][shade], (far - 40) * 256 / 60);
   }
}

/* The glints: dashes of light on open water that come and go, and drift down the river a
 * little faster than the banks go by. */
static void paint_glints(const rr *g, px_scene *s, int32_t dy)
{
   const unsigned sx = s->sx, sy = s->sy, w = s->w, h = s->h;
   const unsigned thick = sy >= 6 ? 2 : 1;

   for (unsigned i = 0; i < GLINTS; i++)
   {
      const glint *k = &g->glints[i];
      const unsigned wave = px_kit_wave(k->phase + g->frame * k->pace);
      const unsigned length = (unsigned)k->length * sx / 8;
      const unsigned X = (k->x * w / 10000u + g->frame * sx / 40u) % w;
      const int64_t down = (int64_t)k->y * h / 10000 + dy + (int64_t)(g->frame * sy / 6u);
      const unsigned Y = (unsigned)(down % (int64_t)h + (int64_t)h) % h;
      unsigned r, bright;
      bool open = true;

      if (wave < 96 || !length || Y < TOP * sy || Y + thick > BOTTOM * sy || X + length > w)
         continue;
      r = Y / sy - TOP + 1;
      for (unsigned c = X / sx; c <= (X + length - 1) / sx; c++)
         open = open && !g->side[r][c] && !g->shore[r][c];
      if (!open)
         continue;

      bright = (wave - 96) * 3 / 2;
      for (unsigned t = 0; t < thick; t++)
      {
         uint32_t *out = s->backdrop + (size_t)(Y + t) * w + X;
         for (unsigned u = 0; u < length; u++)
         {
            /* Fainter towards its ends. */
            const unsigned end = u < length - 1 - u ? u : length - 1 - u;
            const unsigned part = end * 4 >= length ? 256 : 96 + end * 640 / length;
            out[u] = px_rgb_add(out[u], px_rgb_scale(0xB8DCFF, bright * part >> 8));
         }
      }
   }
}

static void paint_river(rr *g, px_scene *s)
{
   const unsigned sx = s->sx, sy = s->sy, w = s->w;
   const bool banks = g->backdrop == BACKDROP_RIVER;
   uint8_t near[PXC_W];
   unsigned rows_of[LANDS] = { 0 }, most = 0;
   int32_t dy;
   bool fresh, stale;

   if (!s->backdrop || !sx || !sy || s->frame->height < BOTTOM || w != PXC_W * sx)
      return;

   fresh = px_kit_texture_fit(&g->water_shades, s);
   fresh = px_kit_texture_fit(&g->land_shades, s) || fresh;
   stale = px_kit_canvas_fit(&g->water, s);
   stale = px_kit_canvas_fit(&g->land, s) || stale;
   if (!g->water_shades.shades || !g->land_shades.shades || !g->water.pixels || !g->land.pixels)
      return;
   if (fresh)
      paint_textures(g, s);
   if (fresh || stale || g->painted_colors != g->colors)
   {
      /* Outside the river the backdrop is black: the line under it is the game's black. */
      memset(s->backdrop, 0, (size_t)s->w * s->h * sizeof(uint32_t));
      g->lands = g->land_next = 0;
      g->water_rgb = g->canvas_rgb = ~0u;
      g->painted_colors = g->colors;
   }

   /* The canvases are in the colours the game has at hand: its water's, and the green of
    * most of its rows. */
#ifdef RR_TIMING
   {
      static double sum; static unsigned n;
      struct timespec a, b;
      bool found;
      clock_gettime(CLOCK_MONOTONIC, &a);
      found = find_shore(g, s);
      clock_gettime(CLOCK_MONOTONIC, &b);
      sum += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
      if (++n % 400 == 0) fprintf(stderr, "shore %.3f ms on average\n", sum / n);
      if (!found)
         return;
   }
#else
   if (!find_shore(g, s))
      return;
#endif
   for (size_t i = (size_t)TOP * PXC_W; i < (size_t)BOTTOM * PXC_W; i++)
      if (is_water(s->top[i]))
      {
         if ((s->top[i] & 0xFFFFFFu) != g->water_rgb)
         {
            water_table(g, s->top[i] & 0xFFFFFFu);
            px_kit_texture_show(&g->water_shades, &g->water, g->water_colors);
         }
         break;
      }
   if (g->water_rgb == ~0u)
   {
      water_table(g, 0x2428B0);
      px_kit_texture_show(&g->water_shades, &g->water, g->water_colors);
   }
   for (unsigned r = 0; r < ROWS; r++)
      if (++rows_of[g->land_of[r]] > rows_of[most])
         most = g->land_of[r];
   if (banks && g->canvas_rgb != g->land_rgb[most])
   {
      px_kit_texture_show(&g->land_shades, &g->land, g->land_colors[most]);
      g->canvas_rgb = g->land_rgb[most];
   }

   /* The river moves down the picture as its line counts up. */
   dy = g->scroll.total * (int32_t)sy;

   for (unsigned Y = TOP * sy; Y < BOTTOM * sy; Y++)
   {
      const unsigned r = Y / sy - TOP + 1;
      /* Where in the game's pixel, of 256: between its middle and that of the next. */
      const unsigned v = (2 * (Y % sy) + 1) * 256 / (2 * sy);
      const uint8_t *a = g->shore[v < 128 ? r - 1 : r], *b = g->shore[v < 128 ? r : r + 1];
      const unsigned fv = v < 128 ? v + 128 : v - 128;
      const uint8_t *side = g->side[r];
      const unsigned land = g->land_of[r - 1];
      uint32_t *out = s->backdrop + (size_t)Y * w;
      unsigned x = 0;

      for (unsigned i = 0; i < PXC_W; i++)
         near[i] = (uint8_t)((a[i] * (256 - fv) + b[i] * fv) >> 8);

      while (x < PXC_W)
      {
         const unsigned l = near[x ? x - 1 : 0], m = near[x], n = near[x + 1 < PXC_W ? x + 1 : x];
         const bool open = !l && !m && !n && !side[x];
         const bool inland = l == 255 && m == 255 && n == 255 && side[x];
         unsigned to = x + 1;

         if (side[x] && !banks)
         {
            x++;
            continue;
         }
         if (open || inland)
         {
            /* As far as it goes on like this. */
            while (to < PXC_W && near[to] == m && near[to + 1 < PXC_W ? to + 1 : to] == m
                  && (side[to] != 0) == inland)
               to++;
            if (open)
               px_kit_canvas_roll(&g->water, out, Y, x * sx, to * sx, 0, dy);
            else if (g->land_rgb[land] == g->canvas_rgb)
               px_kit_canvas_roll(&g->land, out, Y, x * sx, to * sx, 0, dy);
            else
               px_kit_texture_roll(&g->land_shades, out, Y, x * sx, to * sx, 0, dy,
                     g->land_colors[land]);
            x = to;
            continue;
         }
         for (unsigned u = 0; u < sx; u++)
         {
            const unsigned hx = (2 * u + 1) * 256 / (2 * sx);
            const unsigned fh = hx < 128 ? hx + 128 : hx - 128;
            unsigned t = hx < 128 ? (l * (256 - fh) + m * fh) >> 8 : (m * (256 - fh) + n * fh) >> 8;
            /* The shore is where the game has it. Without the banks the land's side of it
             * is the game's, and the water's is all there is. */
            if (side[x])
               t = t < 128 ? 128 : t;
            else
               t = t > 127 ? 127 : t;
            out[x * sx + u] = shore_pixel(g, x * sx + u, Y, t, dy, land);
         }
         x++;
      }
   }
   paint_glints(g, s, dy);

   /* What is painted shows where the game has background that is dark. */
   for (unsigned r = 0; r < ROWS; r++)
   {
      uint32_t *top = s->top + (size_t)(TOP + r) * PXC_W;
      uint32_t *bk  = s->bk + (size_t)(TOP + r) * PXC_W;
      for (unsigned x = 0; x < PXC_W; x++)
         if (is_water(top[x]) || (banks && is_land(top[x])))
         {
            top[x] = PX_KEY(PX_CLS_BK, 0);
            bk[x]  = 0;
         }
   }
   s->backdrop_on = true;
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

/* The colours of Proteus's for the rows of each kind, by the rows' own. */
static void make_paint(rr *g)
{
   for (unsigned k = 0; k < KINDS; k++)
      for (unsigned i = 0; i < 256; i++)
         g->paint[k][i] = PX_KIT_KEEP;

   /* The game's helicopters are dark green and dark blue, and its jets light blue, on
    * blue water. */
   g->paint[KIND_HELICOPTER][0x2A] = 0xFFD070;   /* the rotor */
   g->paint[KIND_HELICOPTER][0xB0] = 0x8BC34A;
   g->paint[KIND_HELICOPTER][0x80] = 0x3E6B1E;
   memcpy(g->paint[KIND_HELICOPTER_2], g->paint[KIND_HELICOPTER], sizeof(g->paint[0]));

   g->paint[KIND_SHIP][0x00] = 0x2C303A;         /* the bridge and the funnel */
   g->paint[KIND_SHIP][0x32] = 0xE04A2C;         /* the hull */
   g->paint[KIND_SHIP][0xA8] = 0xC8ECFF;         /* the water at it */

   g->paint[KIND_JET][0x8C] = 0xF4F6FA;
   g->paint[KIND_JET][0x9C] = 0xCBD4E2;
   g->paint[KIND_JET][0xAC] = 0x9AA8BE;

   g->paint[KIND_FUEL][0x48] = 0xF0443C;
   g->paint[KIND_FUEL][0x0C] = 0xFAFAF2;
}

/* What an object of the river is where memory does not say: by its first row's colour. */
static int looks_like(const px_instance *in)
{
   switch (in->color)
   {
      case 0x2A: return KIND_HELICOPTER;
      case 0x8C: return KIND_JET;
      case 0x48: return in->w <= 8 ? KIND_FUEL : -1;
      case 0x00: return in->w == 16 && in->h == 8 ? KIND_SHIP : -1;
      default:   return -1;
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

/* The light of the jet's engine on the water behind it, longer the faster it flies. */
static void light_exhaust(const rr *g, px_scene *s, const px_instance *in, int speed)
{
   const unsigned length = 3 + (unsigned)(speed < 0 ? 128 : speed) / 48 + (g->frame & 1);
   for (unsigned r = 0; r < length; r++)
      for (int c = 3; c <= 5; c++)
      {
         const int x = in->x + c, y = in->y + (int)in->h + (int)r;
         size_t i;
         if (x < 0 || x >= PXC_W || y < TOP || y >= BOTTOM || (c != 4 && r > length / 2))
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_BK || PX_KEY_CLS(s->top[i]) == PX_CLS_PF)
            s->light[i] = 0xFF000000u | px_rgb_scale(0xFFA040, 256 - r * 200 / length);
      }
}

static void burst_block(rr *g, px_scene *s, unsigned block, int kind)
{
   const px_objects *o = s->objects;
   int left = PXC_W, right = -1, top = BOTTOM, bottom = -1, x, y;

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      if (in->cls != PXC_L_P1 || in->ghost || in->y >= BOTTOM
            || block_of(&g->seen, in->y + (int)in->h / 2) != (int)block)
         continue;
      if (in->x < left)                 left = in->x;
      if (in->x + (int)in->w > right)   right = in->x + (int)in->w;
      if (in->y < top)                  top = in->y;
      if (in->y + (int)in->h > bottom)  bottom = in->y + (int)in->h;
   }
   if (right < 0)
      return;
   x = (left + right) / 2;
   y = (top + bottom) / 2;

   switch (kind)
   {
      case KIND_BRIDGE:
         for (int k = 0; k < 4; k++)
            px_scene_burst(s, left + 4 + k * (right - left - 8) / 3, y, k & 1 ? 0xFFE9A0 : 0xFF8A30,
                  26, 460);
         px_scene_flash(s, 0xFFE0A0, 170);
         break;
      case KIND_FUEL:
         px_scene_burst(s, x, y, 0xFF5A30, 22, 380);
         px_scene_burst(s, x, y, 0xFFF0B0, 16, 260);
         px_scene_flash(s, 0xFF9040, 70);
         break;
      case KIND_SHIP:
         px_scene_burst(s, x, y, 0xFF7A3A, 18, 320);
         px_scene_burst(s, x, y, 0xD0F0FF, 12, 240);
         px_scene_flash(s, 0xFFF0D0, 40);
         break;
      case KIND_JET:
         px_scene_burst(s, x, y, 0xF0F4FF, 24, 380);
         px_scene_flash(s, 0xFFF0D0, 40);
         break;
      default:
         px_scene_burst(s, x, y, 0xFFD070, 16, 320);
         px_scene_burst(s, x, y, 0x9CDC5A, 10, 240);
         px_scene_flash(s, 0xFFF0D0, 40);
         break;
   }
}

static void frame(void *state, px_scene *s)
{
   rr *g = (rr*)state;
   px_objects *o = s->objects;
   const bool proteus = g->colors != COLORS_ORIGINAL;
   const int jet   = px_kit_ram(s->ram, s->ram_size, RAM_JET);
   const int fuel  = px_kit_ram(s->ram, s->ram_size, RAM_FUEL);
   const int speed = px_kit_ram(s->ram, s->ram_size, RAM_SPEED);
   int jet_x = -1, jet_y = 0;

   /* A frame that is drawn again is to look as it did: nothing moves on, nothing is
    * counted, nothing bursts. */
   if (s->advance)
   {
      g->frame++;
      blocks_follow(&g->seen, s->ram, s->ram_size);
      px_kit_scroll_follow(&g->scroll, g->seen.line, BLOCK);
   }

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      int block, kind;

      in->role = PX_ROLE_NONE;
      if (in->y >= PANEL)
      {
         in->role = PX_ROLE_HUD;
         /* The needle of the gauge blinks when the fuel is low. */
         if (in->cls == PXC_L_BL && proteus && fuel >= 0 && fuel < FUEL_LOW && jet > 0
               && jet != JET_WRECKED && (g->frame & 16))
            px_scene_tint(s, in, 0xFF3A20);
         continue;
      }
      if (in->y >= BOTTOM || in->y + (int)in->h <= TOP)
         continue;

      switch (in->cls)
      {
         case PXC_L_P0:
            in->role = PX_ROLE_PLAYER;
            if (in->ghost)
               break;
            jet_x = in->x + 4;
            jet_y = in->y + (int)in->h / 2;
            if (jet == JET_WRECKED)
            {
               px_scene_energy(s, in, true);
               break;
            }
            if (proteus)
               for (unsigned r = 0; r < in->h; r++)
               {
                  const px_instance row = row_of(in, r);
                  px_scene_tint(s, &row, px_rgb_mix(0xFFF8C8, 0xFFB020, r * 256 / in->h));
               }
            light_exhaust(g, s, in, speed);
            break;

         case PXC_L_M0:
            in->role = PX_ROLE_SHOT;
            if (proteus)
               px_scene_tint(s, in, 0xFFF6B0);
            break;

         case PXC_L_P1:
            block = block_of(&g->seen, in->y + (int)in->h / 2);
            kind  = block >= 0 ? g->seen.kind[block] : looks_like(in);
            switch (kind)
            {
               case KIND_HELICOPTER:
               case KIND_HELICOPTER_2:
                  in->role  = PX_ROLE_ENEMY;
                  in->group = GROUP_HELICOPTER;
                  break;
               case KIND_SHIP:
                  in->role  = PX_ROLE_ENEMY;
                  in->group = GROUP_SHIP;
                  break;
               case KIND_JET:
                  in->role  = PX_ROLE_ENEMY;
                  in->group = GROUP_JET;
                  break;
               case KIND_BRIDGE:
                  in->role  = PX_ROLE_ENEMY;
                  in->group = GROUP_BRIDGE;
                  break;
               case KIND_FUEL:
                  in->role = PX_ROLE_BONUS;
                  /* Its letters are lit, and beat slowly. */
                  if (proteus)
                     px_kit_fill_holes(s, in, px_rgb_add(0xE89010,
                           px_rgb_scale(0x176820, px_kit_wave(g->frame * 3))));
                  break;
               case KIND_WRECK:
               case KIND_WRECK_2:
               case KIND_WRECK_3:
                  px_scene_energy(s, in, true);
                  break;
               default:
                  break;
            }
            if (proteus && kind > 0 && kind < KINDS)
               px_kit_repaint(s, in, g->paint[kind]);
            break;

         default:
            break;
      }
   }

   if (s->advance && g->sparks)
   {
      for (unsigned b = 0; b < BLOCKS; b++)
         if (g->seen.hit[b])
            burst_block(g, s, b, g->seen.hit[b]);
      if (jet == JET_WRECKED && g->jet > 0 && g->jet != JET_WRECKED && jet_x >= 0)
      {
         px_scene_burst(s, jet_x, jet_y, 0xFFE0A0, 44, 440);
         px_scene_burst(s, jet_x, jet_y, 0xFF5020, 20, 260);
         px_scene_flash(s, 0xFF3020, 120);
      }
   }
   if (s->advance)
      g->jet = jet;

   if (g->backdrop != BACKDROP_OFF)
   {
#ifdef RR_TIMING
      static double sum, least = 1e9; static unsigned n;
      struct timespec a, b; double ms;
      clock_gettime(CLOCK_MONOTONIC, &a);
      paint_river(g, s);
      clock_gettime(CLOCK_MONOTONIC, &b);
      ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
      if (ms > 20.0) fprintf(stderr, "river %.3f ms in frame %u\n", ms, g->frame);
      else if (ms > 0.2) { sum += ms; n++; if (ms < least) least = ms; }
      if (n % 200 == 199) fprintf(stderr, "river %.3f ms on average, %.3f at least\n", sum / n, least);
#else
      paint_river(g, s);
#endif
   }
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has six, and the TIA's two voices for them:
 *
 *   voice 0   the engine: noise (waveform 8) at pitch 31 less a sixteenth of the speed,
 *             which is 27 slow, 23 as the jet is left alone and 16 fast; volume 5, 7 while
 *             it gathers speed and 4 while it loses it
 *   voice 0   fuel is taken on: waveform 4 at pitch 31, the volume from 8 down to 1 in
 *             eight frames, again and again; at pitch 15 when the tank is full
 *   voice 0   the fuel is low: waveform 12, the pitch from 31 down to 14 in 36 frames, at
 *             volume 15, every 63 frames
 *   voice 0   the jet is wrecked: noise at pitch 28, the volume from 15 down to 1 in 30
 *             frames
 *   voice 1   the shot: waveform 12, the pitch from 13 up to 27 in 15 frames, volume 8
 *   voice 1   something is hit: noise at pitches from 24 to 31 by chance, the volume
 *             from 15 down to 4 in 23 frames. A bridge sounds like a helicopter.
 *
 * Voice 0 has one thing to say at a time: the engine is not heard while fuel is taken on
 * or the warning sounds. Here the engine runs on under them, its pitch from the speed in
 * memory. What is hit is known from memory too, so a bridge has a sound of its own.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_ENGINE, SOUND_FUEL, SOUND_FULL, SOUND_LOW, SOUND_LOST, SOUND_SHOT,
   SOUND_HIT, SOUND_OTHER };

static unsigned voice0_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   if (!volume)
      return SOUND_NONE;
   switch (wave)
   {
      case 8:  return pitch == 28 ? SOUND_LOST : pitch >= 16 && pitch <= 27 && volume <= 7
                     ? SOUND_ENGINE : SOUND_OTHER;
      case 4:  return pitch == 31 ? SOUND_FUEL : pitch == 15 ? SOUND_FULL : SOUND_OTHER;
      case 12: return pitch >= 14 && volume == 15 ? SOUND_LOW : SOUND_OTHER;
      default: return SOUND_OTHER;
   }
}

static unsigned voice1_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   if (!volume)
      return SOUND_NONE;
   switch (wave)
   {
      case 12: return pitch >= 13 && pitch <= 27 && volume == 8 ? SOUND_SHOT : SOUND_OTHER;
      case 8:  return pitch >= 24 ? SOUND_HIT : SOUND_OTHER;
      default: return SOUND_OTHER;
   }
}

static void play_shot(rr *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave           freq   to   glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_NOISE,  9000, 2200, 0.10f, 0,      0.01f, 0.12f, 0.20f, 6500, 1400, 0, 0 },
      { PX_WAVE_SAW,    1500,  280, 0.13f, 0.001f, 0.02f, 0.15f, 0.22f, 5000, 900, 0, 0 },
      { PX_WAVE_SINE,    220,   90, 0.08f, 0.001f, 0.01f, 0.10f, 0.30f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->jet_at));
   px_sound_rumble(s, 0, 12000, 3);
}

static void play_hit(rr *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE,  6500, 700, 0.30f, 0,      0.03f, 0.36f, 0.48f, 5000, 400, 0, 0 },
      { PX_WAVE_SQUARE,  620,  85, 0.24f, 0.001f, 0.01f, 0.26f, 0.20f, 3500, 500, 0, 0 },
      { PX_WAVE_SINE,    130,  40, 0.14f, 0.001f, 0.02f, 0.30f, 0.70f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->shot_at >= 0 ? g->shot_at : g->jet_at));
   px_sound_rumble(s, 20000, 28000, 8);
}

/* A bridge comes down: longer and lower, from both sides. */
static void play_bridge(px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 5000, 260, 0.9f,  0,      0.12f, 1.2f, 0.60f, 4000, 150, 0, 0 },
      { PX_WAVE_SINE,    72,  26, 0.8f,  0.002f, 0.10f, 1.1f, 0.85f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    300,  36, 0.7f,  0.002f, 0.05f, 0.8f, 0.26f, 2200, 200, 9.0f, 0.04f },
      { PX_WAVE_NOISE, 9000, 3000, 0.2f, 0,      0.01f, 0.2f, 0.30f, 8000, 2500, 0, 0 }
   };
   px_kit_play(s, p, 2, -0.45f);
   px_kit_play(s, p + 2, 2, 0.45f);
   px_sound_rumble(s, 52000, 40000, 30);
}

static void play_lost(rr *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE, 5000, 300, 1.0f, 0,      0.15f, 1.3f, 0.62f, 3500, 150, 0, 0 },
      { PX_WAVE_SINE,    80,  28, 0.9f, 0.002f, 0.10f, 1.2f, 0.85f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    440,  40, 0.8f, 0.002f, 0.05f, 0.9f, 0.30f, 2500, 200, 11.0f, 0.04f }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->jet_at));
   px_sound_rumble(s, 65535, 40000, 45);
}

/* Fuel runs in, a gulp at a time; a bell when the tank is full. */
static void play_fuel(rr *g, px_sound *s, bool full)
{
   static const px_tone gulp[2] = {
      { PX_WAVE_SINE,     300,  620, 0.09f, 0.004f, 0.02f, 0.10f, 0.26f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 600, 1240, 0.09f, 0.004f, 0.01f, 0.08f, 0.08f, 0, 0, 0, 0 }
   };
   static const px_tone bell[2] = {
      { PX_WAVE_SINE,     1175, 0, 0, 0.002f, 0.01f, 0.22f, 0.20f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 1760, 0, 0, 0.002f, 0.01f, 0.16f, 0.07f, 0, 0, 0, 0 }
   };
   px_kit_play(s, full ? bell : gulp, 2, px_kit_pan(g->jet_at));
   px_sound_rumble(s, 0, 5000, 2);
}

/* The warning: a horn that rises, as the game's does. */
static void play_low(px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_SQUARE, 520, 1040, 0.50f, 0.006f, 0.48f, 0.10f, 0.15f, 2600, 3400, 7.0f, 0.012f },
      { PX_WAVE_SAW,    260,  520, 0.50f, 0.006f, 0.48f, 0.10f, 0.10f, 1400, 2000, 0, 0 }
   };
   px_kit_play(s, p, 2, 0.0f);
   px_sound_rumble(s, 0, 9000, 6);
}

static void stop_engine(rr *g, px_sound *s, float seconds)
{
   for (unsigned i = 0; i < 3; i++)
   {
      px_synth_stop(s->synth, g->engine[i], seconds);
      g->engine[i] = 0;
   }
}

/* The engine: a rush of air, a turbine under it and its whine above, all of which rise
 * with the game's pitch, where the jet is between left and right. `volume` is the game's:
 * 7 while the jet gathers speed, 4 while it loses it. */
static void play_engine(rr *g, px_sound *s, unsigned volume)
{
   static const px_tone tones[3] = {
      { PX_WAVE_NOISE, 3000, 0, 0, 0.25f, 0, 0, 1.0f, 1500, 0, 0, 0 },
      { PX_WAVE_SAW,     80, 0, 0, 0.25f, 0, 0, 1.0f,  520, 0, 5.0f, 0.008f },
      { PX_WAVE_SINE,  1300, 0, 0, 0.25f, 0, 0, 1.0f,    0, 0, 0, 0 }
   };
   static const float gains[3] = { 0.13f, 0.11f, 0.014f };
   /* From 0 slow to 1 fast. */
   const float fast = (27.0f - (float)g->engine_pitch) / 11.0f;
   const float push = volume >= 7 ? 1.35f : volume <= 4 ? 0.75f : 1.0f;
   const float pan = px_kit_pan(g->jet_at) * 0.7f;
   const float freq[3] = { 3000.0f * (1.0f + 1.6f * fast), 80.0f * (1.0f + 0.9f * fast),
      1300.0f * (1.0f + 0.7f * fast) };

   for (unsigned i = 0; i < 3; i++)
      if (!px_synth_move(s->synth, g->engine[i], pan, gains[i] * push, freq[i]))
      {
         px_tone t = tones[i];
         t.freq = freq[i];
         g->engine[i] = px_synth_play(s->synth, &t, pan, gains[i] * push);
      }
}

/* A beat of the rotor of the helicopter that is nearest to the jet. */
static void play_rotor(px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_NOISE, 1100, 500, 0.05f, 0.004f, 0.008f, 0.07f, 1.0f, 480, 220, 0, 0 },
      { PX_WAVE_SINE,    74,  52, 0.05f, 0.004f, 0.008f, 0.06f, 1.0f, 0, 0, 0, 0 }
   };
   int nearest = -1, at = -1;

   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_ENEMY && in->group == GROUP_HELICOPTER && !in->ghost
            && in->y < BOTTOM && in->y > nearest)
      {
         nearest = in->y;
         at      = in->x + 4;
      }
   }
   if (nearest >= 0)
   {
      /* From the top of the picture to where the jet is. */
      const float gain = 0.05f + 0.13f * (float)(nearest < 156 ? nearest : 156) / 156.0f;
      px_synth_play(s->synth, &p[0], px_kit_pan(at), gain);
      px_synth_play(s->synth, &p[1], px_kit_pan(at), gain * 1.2f);
   }
}

static void sound(void *state, px_sound *s)
{
   rr *g = (rr*)state;
   unsigned heard0, heard1;
   bool running, bridge = false;
   const int speed = px_kit_ram(s->ram, s->ram_size, RAM_SPEED);
   const int jet_x = px_kit_ram(s->ram, s->ram_size, RAM_JET_X);
   const int shot_x = px_kit_ram(s->ram, s->ram_size, RAM_SHOT_X);

   /* Where things are: from memory, or else from the picture before. */
   g->jet_at  = jet_x >= 0 ? jet_x + 4 : -1;
   g->shot_at = shot_x > 0 ? shot_x - 1 : -1;
   for (unsigned i = 0; jet_x < 0 && s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_PLAYER && !in->ghost)
         g->jet_at = in->x + 4;
      else if (in->role == PX_ROLE_SHOT)
         g->shot_at = in->x;
   }

   blocks_follow(&g->heard, s->ram, s->ram_size);
   for (unsigned b = 0; b < BLOCKS; b++)
      bridge = bridge || g->heard.hit[b] == KIND_BRIDGE;

   px_kit_tia_hear(&g->tia, s->frame);
   heard0 = voice0_plays(g->tia.wave[0], g->tia.pitch[0], g->tia.volume[0]);
   heard1 = voice1_plays(g->tia.wave[1], g->tia.pitch[1], g->tia.volume[1]);

   /* Voice 0. The fuel's sounds begin again whenever they are louder than they were, the
    * warning when its pitch is back where it begins. */
   if (heard0 == SOUND_LOST && g->heard0 != SOUND_LOST)
   {
      if (g->own_sound) play_lost(g, s);
      else              px_sound_rumble(s, 65535, 40000, 45);
   }
   else if ((heard0 == SOUND_FUEL || heard0 == SOUND_FULL)
         && (heard0 != g->heard0 || px_kit_tia_louder(&g->tia, 0)))
   {
      if (g->own_sound) play_fuel(g, s, heard0 == SOUND_FULL);
      else              px_sound_rumble(s, 0, 5000, 2);
   }
   else if (heard0 == SOUND_LOW && (g->heard0 != SOUND_LOW || g->tia.pitch[0] > g->tia.was_pitch[0]))
   {
      if (g->own_sound) play_low(s);
      else              px_sound_rumble(s, 0, 9000, 6);
   }

   /* Voice 1. A shot begins where its pitch does, a hit at full volume. */
   if (heard1 == SOUND_SHOT && (g->heard1 != SOUND_SHOT || g->tia.pitch[1] < g->tia.was_pitch[1]))
   {
      if (g->own_sound) play_shot(g, s);
      else              px_sound_rumble(s, 0, 12000, 3);
   }
   else if (heard1 == SOUND_HIT && g->tia.volume[1] == 15
         && (g->heard1 != SOUND_HIT || px_kit_tia_louder(&g->tia, 1)))
   {
      /* What memory says was hit; without it, something was. */
      if (bridge)
      {
         if (g->own_sound) play_bridge(s);
         else              px_sound_rumble(s, 52000, 40000, 30);
      }
      else
      {
         if (g->own_sound) play_hit(g, s);
         else              px_sound_rumble(s, 20000, 28000, 8);
      }
   }

   /* The engine runs while the game's does, and under what the game plays in its place. */
   running = heard0 == SOUND_ENGINE || heard0 == SOUND_FUEL || heard0 == SOUND_FULL
         || heard0 == SOUND_LOW;
   if (heard0 == SOUND_ENGINE)
      g->engine_pitch = g->tia.pitch[0];
   else if (running && speed >= 0)
      g->engine_pitch = 31 - speed / 16;
   if (g->engine_pitch < 16) g->engine_pitch = 16;
   if (g->engine_pitch > 27) g->engine_pitch = 27;

   if (g->own_sound && running)
      play_engine(g, s, heard0 == SOUND_ENGINE ? g->tia.volume[0] : 5);
   else
      stop_engine(g, s, heard0 == SOUND_LOST ? 0.08f : 0.4f);

   if (g->own_sound && g->rotors && running && ++g->beat % 5 == 0)
      play_rotor(s);

   if (g->own_sound)
   {
      if (heard0 != SOUND_OTHER)
         s->voice[0] = 0.0f;
      if (heard1 != SOUND_OTHER)
         s->voice[1] = 0.0f;
   }
   g->heard0 = heard0;
   g->heard1 = heard1;
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

/* The game was reset, or a state was loaded: what was known is not so any more. */
static void reset(void *state)
{
   rr *g = (rr*)state;
   g->jet = -1;
   blocks_reset(&g->seen);
   blocks_reset(&g->heard);
   px_kit_scroll_reset(&g->scroll);
   px_kit_tia_reset(&g->tia);
   g->heard0 = g->heard1 = SOUND_NONE;
   g->engine[0] = g->engine[1] = g->engine[2] = 0;
   g->engine_pitch = 23;
   g->beat   = 0;
   g->jet_at = g->shot_at = -1;
}

static void *create(void)
{
   rr *g = (rr*)calloc(1, sizeof(rr));
   if (g)
   {
      g->sparks = g->own_sound = g->rotors = true;
      g->painted_colors = ~0u;
      make_paint(g);
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   rr *g = (rr*)state;
   if (g)
   {
      px_kit_texture_free(&g->water_shades);
      px_kit_texture_free(&g->land_shades);
      px_kit_canvas_free(&g->water);
      px_kit_canvas_free(&g->land);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   rr *g = (rr*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_pick(get, OPT_BACKDROP, backdrop);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->rotors    = px_kit_on(get, OPT_ROTORS);
}

const px_game px_game_river_raid = {
   "River Raid", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
