/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pitfall II: Lost Caverns (Activision, 1984).
 *
 * The cartridge carries a chip of its own, the DPC, which draws most of the picture's data
 * and plays the music. What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  18..42   the score: both players, three copies each, on the background's blue,
 *                  which is there in the caverns too
 *   rows  43..194  the world, in bands of 45 rows that scroll up a row a frame when Harry
 *                  falls to the band below. A band is a floor, playfield $18, nine rows; the
 *                  ledge's rough underside, playfield $12, five rows that end in a ragged
 *                  edge; then 31 rows of cavern, background black, with walls at its sides:
 *                  playfield $12 in stripes. A hole in a floor is a gap in it.
 *                  Above ground the band has the sky's last rows in playfield stripes $88 to
 *                  $28, the canopy as background $D4 with its edge as playfield $D4, and the
 *                  jungle as background $C6 with the trunks as playfield $12.
 *                  The underground river is background $84, the rock over it playfield $00.
 *   rows 195..     the publisher's name: both players and the ball in colours
 *   Harry          player 0, 16 to 21 rows: hair $12, face $4A, shirt $C8, trousers $D2
 *   the rest       player 1, set again for every band: a frog ($26 $28 $14 $2A), a
 *                  scorpion ($0E, 11 rows), an electric eel ($0E, 3 rows, in the river), a
 *                  bat ($04), the condor ($0A), a checkpoint's cross ($32), a gold bar
 *                  ($1E, with glitter above it), the ring of the treasure ($0E over $1E),
 *                  Quickclaw the cat ($18). Rhonda, the rat, the balloon and the diamond
 *                  were not seen in the runs this module was made from; they keep their
 *                  colours.
 *   ladders        the ball, four pixels wide, two rows of every four, colour $12
 *
 * Of its memory ($80 is 0):
 *
 *   72, 73     the score, four decimal digits (BCD). It rises for a treasure; when Harry is
 *              hurt it runs down while he is carried back to the last cross
 *   86         how far the view has scrolled down within the level: the floors are at
 *              43 + 45n - this, mod 45
 *  103, 105    Harry's column, and his row ($60 on a floor, $7F falling)
 *  104, 106    the screen: its column (0..8) and its level. Written before Harry walks off
 *              a screen, they take him to another: that is how the screens were surveyed
 *
 * The sound: see "The sounds" below. The music is the DPC's: three square waves it adds and
 * gives the TIA as the volume of voice 0, which the game writes on every line. The capture's
 * log of writes has every one of them (none dropped: about 1970 writes a frame), so the
 * three voices are there to be taken apart.
 *
 * What surprised:
 *
 *   - Stella's frames of this game depend on how long they take to emulate, even from a
 *     saved state: runs are not the same twice (test/games2600.sh says so).
 *   - Voice 1 plays the music's drums, and nothing else: the game has no sounds but its
 *     music.
 */
#include "../kit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_p2_colors"
#define OPT_BACKDROP "proteus_p2_backdrop"
#define OPT_SPARKS   "proteus_p2_sparks"
#define OPT_SOUND    "proteus_p2_sound"
#define OPT_AMBIENCE "proteus_p2_ambience"

#define RAM_SCORE    72     /* and 73 */
#define RAM_SCROLL   86
#define RAM_HARRY_X  103
#define RAM_COLUMN   104
#define RAM_HARRY_Y  105
#define RAM_LEVEL    106

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define ROW_WORLD   43     /* below the score */
#define ROW_BOTTOM  195    /* the publisher's name */
#define BAND        45     /* a floor and the cavern below it */
#define FLOOR_ROWS  9      /* the floor itself */
#define LEDGE_ROWS  14     /* and its underside */

/* The game's colours of its scenery. */
#define PF_FLOOR    0x18
#define PF_EARTH    0x12
#define PF_ROCK     0x00   /* over the river */
#define PF_LEAVES   0xD4
#define BK_CANOPY   0xD4
#define BK_JUNGLE   0xC6
#define BK_RIVER    0x84
#define BK_EARTH    0x12

/* The line rate: the TIA's 31399.5 clocks a second, two a line. */
#define LINE_HZ     15699.75f
#define MAX_RISES   64     /* of a wave in a frame */
/* Lines: the lowest note there is, 65 Hz. A rest of a frame is longer. */
#define MAX_PERIOD  240.0f

/* What an object is: a px_instance's group. */
enum
{
   KIND_NONE = 0, KIND_HARRY, KIND_FROG, KIND_SCORPION, KIND_EEL, KIND_BAT, KIND_CONDOR,
   KIND_CROSS, KIND_GOLD, KIND_RING, KIND_CAT, KIND_LADDER, KIND_LEDGE, KIND_SCORE
};

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "6d842c96d5a01967be9680080dd5be54",   /* David Crane's Pitfall II - Lost Caverns (USA) */
   NULL
};

static const char *const fx[] = {
   "width", "50",
   "reverb", "hall",
   NULL
};

static const char *const colors[] = { "caverns", "Caverns", "original", "The game's own", NULL };
static const char *const backdrop[] = { "rock", "Cavern rock", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_CAVERNS = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Stone floors, rock and earth in shades, a river that moves, crosses that glow, gold that glints, an eel that crackles, or the game's own colours.",
     "caverns", colors },
   { OPT_BACKDROP, "Backdrop",
     "The rock of the caverns behind the black, with shadow under every ledge, scrolling with the game.",
     "rock", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks and a flash where Harry takes a treasure, is hurt, touches a cross, lands or splashes into the river.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "The game's music, its three voices and its drums, played note for note with other instruments. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_AMBIENCE, "Cavern sounds",
     "Water that drips and air that moves in the caverns, the river where it is, wind above ground. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

/* One of the DPC's three voices, as the volume of voice 0 shows it. */
typedef struct
{
   uint32_t rise;       /* the line its wave last rose on */
   bool risen;          /* it has, since the reset */
   bool high;           /* the wave is high */
   float period;        /* lines from rise to rise */
   bool on;             /* a note sounds */
   float hz;            /* the note */
   unsigned id[2];      /* the synth's voices that play it */
} dpc_voice;

typedef struct
{
   /* The options. */
   unsigned colors;
   bool backdrop, sparks, own_sound, ambience;

   uint32_t frame;               /* counts the frames that advance */

   /* The picture. */
   int floor;                    /* the first floor's row, or -1 */
   int harry_x, harry_y;         /* Harry's middle and his feet; -1: not seen */
   int treasure_x, treasure_y;
   unsigned treasure_gone;       /* frames since a treasure was seen */
   int score;                    /* as memory had it; -1: not known */
   unsigned running_down;        /* frames the score has run down for */
   unsigned falling;             /* frames the floors have moved for */
   uint32_t checkpoint;          /* the cross touched last */
   bool on_cross;                /* Harry is at one */
   unsigned water_seen;          /* frames since the river was on the screen */
   bool surface;                 /* the jungle is on the screen */

   uint32_t *tile;               /* the cavern's rock: a band of the backdrop */
   unsigned tile_w, tile_h, tile_sx, tile_sy;
   int tile_column;              /* what it was painted for */
   int painted_floor;            /* where the backdrop was painted with it */
   int painted_column;
   int column;                   /* the screen's, for the textures */
   int river_from, river_to;     /* the rows the river is in; -1: none */
   bool river_painted;           /* the backdrop has it */
   unsigned earth, painted_earth;   /* pixels of earth not dug, and in the backdrop */
   int tab_column;               /* what the tables below were made for */
   uint32_t floor_tab[BAND][PXC_W], earth_tab[BAND][PXC_W];

   /* The sound. */
   px_kit_tia tia;
   uint32_t lines;               /* lines heard since the reset */
   dpc_voice music[3];
   bool music_heard;             /* the frame before was the DPC's music */
   int heard_score;
   unsigned heard_running;
   int scroll;                   /* memory's scroll, as the sound follows it */
   unsigned heard_falling;
   uint32_t chance;
   unsigned air, river, wind;    /* sounds that go on */
   unsigned wait_drop, wait_bird;
} p2;

/* ---------------------------------------------------------------------------
 * Chance and texture
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

/* Where a row is in its band: 0 at the floor's top. */
static unsigned band_row(const p2 *g, int y)
{
   const int floor = g->floor >= 0 ? g->floor : ROW_WORLD;
   return (unsigned)(((y - floor) % BAND + BAND) % BAND);
}

/* ---------------------------------------------------------------------------
 * The scenery
 * ------------------------------------------------------------------------- */

/* The first floor's row: the first of nine rows that are all but its holes floor. */
static int find_floor(const px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   unsigned before = 0;
   for (unsigned y = ROW_WORLD; y < ROW_BOTTOM; y++)
   {
      unsigned n = 0;
      for (unsigned x = 8; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         if ((f->tags[i] & PXC_PF) && f->color[PXC_L_PF][i] == PF_FLOOR)
            n++;
      }
      if (n >= 100 && before < 100 && y > ROW_WORLD)
         return (int)y;
      before = n;
   }
   return -1;
}

/* A floor: stone worn smooth on top, darker below, with grit. */
static uint32_t floor_at(const p2 *g, int x, unsigned by)
{
   const int xw = x + g->column * PXC_W;
   const unsigned grit = hash(xw / 2, (int)by, 11);
   uint32_t rgb = px_rgb_mix(0xE0C488, 0x94703E, by * 30);
   rgb = px_rgb_scale(rgb, 226 + (noise(xw * 256 / 7, (int)by * 128, 12) * 30 >> 8));
   if (grit > 236)
      rgb = px_rgb_scale(rgb, 200);
   return by == 0 ? px_rgb_add(rgb, 0x181410) : rgb;
}

/* The earth under a floor and the walls: rock in layers. */
static uint32_t earth_at(const p2 *g, int x, unsigned by)
{
   const int xw = x + g->column * PXC_W;
   const unsigned layer = noise(xw * 256 / 13, (int)by * 256 / 3, 13);
   const unsigned grain = hash(xw, (int)by, 14);
   uint32_t rgb = by < LEDGE_ROWS
         ? px_rgb_mix(0x7A5A36, 0x3E2C1A, by < FLOOR_ROWS ? 0 : (by - FLOOR_ROWS) * 50) : 0x5E4A34;
   return px_rgb_scale(rgb, 180 + (layer * 60 >> 8) + (grain * 16 >> 8));
}

/* Rock over the river: dark and wet. */
static uint32_t rock_at(const p2 *g, int x, int y)
{
   const int xw = x + g->column * PXC_W;
   return px_rgb_scale(0x3A342E, 170 + (noise(xw * 256 / 5, y * 256 / 3, 15) * 86 >> 8));
}

/* The canopy's leaves, darker at the top, in clumps. */
static uint32_t leaves_at(int x, int y, unsigned down)
{
   const unsigned clump = noise(x * 256 / 9, y * 256 / 5, 5);
   return px_rgb_scale(px_rgb_mix(0x113410, 0x2E7420, down > 256 ? 256 : down), 184 + (clump * 72 >> 8));
}

/* The river: waves that go by, light on them, brighter at the top. */
static uint32_t water_at(const p2 *g, int x, int depth)
{
   const unsigned t = g->frame, xs = (unsigned)x * 16u, ys = (unsigned)(depth < 0 ? 0 : depth) * 16u;
   const unsigned a = px_kit_wave(xs / 3 + t * 2 + px_kit_wave(ys * 5 + t * 3) / 3);
   const unsigned b = px_kit_wave(xs / 5 + ys * 7 + 256 - (t & 255));
   const unsigned light = a * b >> 8;
   uint32_t rgb = px_rgb_scale(px_rgb_mix(0x1E62A0, 0x0A2A58, depth * 8 > 256 ? 256 : (unsigned)depth * 8),
         180 + (a >> 2));
   if (light > 170)
      rgb = px_rgb_add(rgb, px_rgb_scale(0x78B4E0, (light - 170) * 2));
   if (depth < 2)
   {
      /* The surface, where the ripples catch the light. */
      const unsigned ripple = px_kit_wave(xs / 2 + t * 5 + (unsigned)depth * 90);
      rgb = px_rgb_add(rgb, px_rgb_scale(0x5080A0, ripple > 170 ? (ripple - 170) * 2 : 0));
   }
   return rgb;
}

static void set_bk(px_scene *s, size_t i, uint32_t rgb)
{
   s->bk[i] = rgb;
   if (PX_KEY_CLS(s->top[i]) == PX_CLS_BK)
      s->top[i] = PX_KEY(PX_CLS_BK, rgb);
}

/* Shades for the scenery, pixel by pixel. The rows of the canopy and the jungle, and where
 * the river is, are found from the background's colour on the row. */
static void paint_scenery(p2 *g, px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   int canopy_top = -1, canopy_end = -1, jungle_top = -1, jungle_end = -1;
   int river_top[PXC_W];

   for (unsigned y = ROW_WORLD; y < ROW_BOTTOM; y++)
   {
      const unsigned bk = f->color[PXC_L_BK][(size_t)y * PXC_W + 80];
      if (bk == BK_CANOPY)
      {
         if (canopy_top < 0) canopy_top = (int)y;
         canopy_end = (int)y + 1;
      }
      else if (bk == BK_JUNGLE)
      {
         if (jungle_top < 0) jungle_top = (int)y;
         jungle_end = (int)y + 1;
      }
   }
   for (unsigned x = 0; x < PXC_W; x++)
      river_top[x] = -1;
   g->river_from = g->river_to = -1;
   g->earth = 0;
   if (g->tab_column != g->column)
   {
      /* Floors and earth are the same in every band of a screen. */
      for (unsigned by = 0; by < BAND; by++)
         for (unsigned x = 0; x < PXC_W; x++)
         {
            g->floor_tab[by][x] = floor_at(g, (int)x, by < FLOOR_ROWS ? by : FLOOR_ROWS - 1);
            g->earth_tab[by][x] = earth_at(g, (int)x, by);
         }
      g->tab_column = g->column;
   }

   for (unsigned y = ROW_WORLD; y < ROW_BOTTOM; y++)
   {
      const unsigned by = band_row(g, (int)y);
      const bool jungle = jungle_top >= 0 && (int)y >= jungle_top && (int)y < jungle_end;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         const unsigned cls = PX_KEY_CLS(s->top[i]);
         const unsigned bk = f->color[PXC_L_BK][i];

         if (bk == BK_RIVER && river_top[x] < 0)
            river_top[x] = (int)y;
         if (bk != BK_RIVER)
            river_top[x] = -1;

         if (cls == PX_CLS_PF && (f->tags[i] & PXC_PF))
         {
            const unsigned pf = f->color[PXC_L_PF][i];
            uint32_t rgb;
            if (pf == PF_FLOOR)
               rgb = g->floor_tab[by < FLOOR_ROWS ? by : FLOOR_ROWS - 1][x];
            else if (pf == PF_EARTH && jungle)
            {
               /* A trunk is round: lit from the right. */
               const bool first = !x || PX_KEY_CLS(s->top[i - 1]) != PX_CLS_PF;
               const bool last  = x + 1 >= PXC_W || PX_KEY_CLS(s->top[i + 1]) != PX_CLS_PF;
               rgb = px_rgb_scale(first ? 0x3A2612 : last ? 0x7A5630 : 0x5A3C1E,
                     216 + (hash((int)x, (int)y / 3, 6) * 40 >> 8));
            }
            else if (pf == PF_EARTH)
               rgb = g->earth_tab[by][x];
            else if (pf == PF_LEAVES && jungle)
               rgb = leaves_at((int)x, (int)y, 256);
            else if (pf == PF_ROCK && bk == BK_RIVER)
               rgb = rock_at(g, (int)x, (int)y);
            else
               continue;
            s->top[i] = PX_KEY(PX_CLS_PF, rgb);
            continue;
         }
         /* The background is given a colour a row: the effects treat it faster so. */
         if (bk == BK_CANOPY && canopy_top >= 0)
            set_bk(s, i, px_rgb_mix(0x0E2A0C, 0x24601A,
                  (unsigned)((int)y - canopy_top) * 256u / (unsigned)(canopy_end - canopy_top)));
         else if (bk == BK_JUNGLE && jungle_top >= 0)
            set_bk(s, i, px_rgb_mix(0x3E7C34, 0x86B85C,
                  (unsigned)((int)y - jungle_top) * 256u / (unsigned)(jungle_end - jungle_top)));
         else if (bk == BK_RIVER && river_top[x] >= 0)
         {
            /* The river moves in the backdrop, where there is one. */
            const int depth = (int)y - river_top[x];
            set_bk(s, i, g->backdrop ? 0 : px_rgb_mix(0x2C7CB8, 0x0E3868, depth * 8 > 256 ? 256 : (unsigned)depth * 8));
            if (g->river_from < 0)
               g->river_from = (int)y;
            g->river_to = (int)y + 1;
            if (cls == PX_CLS_BK)
               s->light[i] = 0xFF000000u | 0x061C34;
         }
         else if (bk == BK_EARTH)
         {
            /* Earth not dug: in the backdrop, where there is one. */
            set_bk(s, i, g->backdrop ? 0 : px_rgb_mix(0x5A4630, 0x261A10, by * 5 > 256 ? 256 : by * 5));
            g->earth++;
         }
      }
   }

   /* The bars HMOVE leaves at the left were given the game's scenery next to them before
    * the module saw the frame: they are given it again as it is now. */
   if (s->cfg && s->cfg->bars)
      for (unsigned y = ROW_WORLD; y < ROW_BOTTOM; y++)
      {
         const size_t row = (size_t)y * PXC_W;
         unsigned n = 0;
         while (n < PXC_W && (f->tags[row + n] & PXC_BLANK))
            n++;
         if (!n || n > 16 || n >= PXC_W)
            continue;
         for (unsigned x = 0; x < n; x++)
         {
            const unsigned cls = PX_KEY_CLS(s->top[row + x]);
            if (cls != PX_CLS_PF && cls != PX_CLS_BK)
               continue;
            s->top[row + x] = PX_KEY_CLS(s->top[row + n]) == PX_CLS_SPRITE
                  ? PX_KEY(PX_CLS_BK, s->bk[row + n]) : s->top[row + n];
            s->bk[row + x] = s->bk[row + n];
         }
      }
}

/* ---------------------------------------------------------------------------
 * The backdrop: the rock of the caverns
 * ------------------------------------------------------------------------- */

/* A band of it, from the floor's top: the dark of the holes, then the cavern with shadow
 * under the ledge, rock hanging from it and lying on the floor below. */
static uint32_t cavern_at(int x256, unsigned y256, int column)
{
   const int x = x256 + column * PXC_W * 256;
   const unsigned by = y256 >> 8;
   const unsigned layers = noise(x / 11, (int)y256 / 6, 30);
   const unsigned strata = noise(x / 45, (int)y256 * 2, 31);
   const unsigned grain  = noise(x / 2, (int)y256 / 2, 32);
   const unsigned hang   = (noise(x / 5, 0, 33) * 9 + noise(x / 2, 0, 34) * 3) >> 8;
   const unsigned lie    = (noise(x / 7, 1, 33) * 5 + noise(x / 3, 1, 34) * 2) >> 8;
   unsigned light;
   uint32_t rgb;

   if (by < LEDGE_ROWS)
      return px_rgb_scale(0x0A0806, 180 + (grain >> 2));
   /* Of 256: from the ledge down to the next floor. */
   light = 110 + (layers * 90 >> 8) + (strata * 36 >> 8) + (grain * 20 >> 8);
   rgb = px_rgb_scale(px_rgb_mix(0x2C2622, 0x3E3226, (y256 - LEDGE_ROWS * 256) / (BAND - LEDGE_ROWS)), light);
   if (by < LEDGE_ROWS + 6)
      rgb = px_rgb_scale(rgb, 90 + (y256 - LEDGE_ROWS * 256) * 166 / (6 * 256));
   if (by < LEDGE_ROWS + hang)
      rgb = px_rgb_mix(rgb, 0x120E0C, 150);
   if (by >= BAND - lie)
      rgb = px_rgb_mix(rgb, 0x4E4034, 110);
   return rgb;
}

static void paint_tile(p2 *g, const px_scene *s, int column)
{
   const unsigned w = s->w, h = BAND * s->sy;
   if (!g->tile || g->tile_w != w || g->tile_sy != s->sy || g->tile_sx != s->sx)
   {
      free(g->tile);
      g->tile = (uint32_t*)malloc((size_t)w * h * sizeof(uint32_t));
      g->tile_w = w;
      g->tile_h = h;
      g->tile_sx = s->sx;
      g->tile_sy = s->sy;
      g->tile_column = -1000;
   }
   if (!g->tile || g->tile_column == column)
      return;
   for (unsigned Y = 0; Y < h; Y++)
      for (unsigned X = 0; X < w; X++)
         g->tile[(size_t)Y * w + X] = cavern_at((int)(X * 256u / s->sx), Y * 256u / s->sy, column);
   g->tile_column = column;
}

/* The river, in the backdrop: at the game's pixels, a block of the picture's each. */
static void paint_river(p2 *g, px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   if (g->river_from < 0)
      return;
   for (int y = g->river_from; y < g->river_to; y++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         uint32_t rgb;
         /* The bars at the left are the river's too. */
         if (f->color[PXC_L_BK][i] != BK_RIVER
               && !(x < 16 && (f->tags[i] & PXC_BLANK) && f->color[PXC_L_BK][(size_t)y * PXC_W + 16] == BK_RIVER))
            continue;
         rgb = water_at(g, (int)x, y - g->river_from);
         for (unsigned Y = (unsigned)y * s->sy; Y < ((unsigned)y + 1) * s->sy && Y < s->h; Y++)
            for (unsigned X = x * s->sx; X < (x + 1) * s->sx && X < s->w; X++)
               s->backdrop[(size_t)Y * s->w + X] = rgb;
      }
   g->river_painted = true;
}

static void paint_backdrop(p2 *g, px_scene *s, int column)
{
   paint_tile(g, s, column);
   if (!g->tile)
      return;
   s->backdrop_on = true;
   if (!s->backdrop_stale && g->painted_floor == g->floor && g->painted_column == column
         && g->painted_earth == g->earth && (g->river_from >= 0 || !g->river_painted))
   {
      paint_river(g, s);
      return;
   }
   for (unsigned Y = 0; Y < s->h; Y++)
   {
      uint32_t *out = s->backdrop + (size_t)Y * s->w;
      if (Y < ROW_WORLD * s->sy || Y >= ROW_BOTTOM * s->sy)
         memset(out, 0, (size_t)s->w * sizeof(uint32_t));
      else
      {
         const int from = (g->floor >= 0 ? g->floor : ROW_WORLD) * (int)s->sy;
         const int band = BAND * (int)s->sy;
         const unsigned row = (unsigned)((((int)Y - from) % band + band) % band);
         memcpy(out, g->tile + (size_t)row * g->tile_w, (size_t)s->w * sizeof(uint32_t));
      }
   }
   /* The earth that is not dug, where the game has its colour as the background. */
   if (g->earth)
      for (unsigned y = ROW_WORLD; y < ROW_BOTTOM; y++)
      {
         const unsigned by = band_row(g, (int)y);
         for (unsigned x = 0; x < PXC_W; x++)
         {
            const size_t i = (size_t)y * PXC_W + x;
            const unsigned n = x < 16 && (s->frame->tags[i] & PXC_BLANK) ? 16 : x;
            uint32_t rgb;
            if (s->frame->color[PXC_L_BK][(size_t)y * PXC_W + n] != BK_EARTH)
               continue;
            rgb = px_rgb_scale(g->earth_tab[by < LEDGE_ROWS ? LEDGE_ROWS : by][x],
                  150 + (hash((int)x / 3, (int)y / 2, 16) * 30 >> 8));
            for (unsigned Y = y * s->sy; Y < (y + 1) * s->sy && Y < s->h; Y++)
               for (unsigned X = x * s->sx; X < (x + 1) * s->sx && X < s->w; X++)
                  s->backdrop[(size_t)Y * s->w + X] = rgb;
         }
      }
   g->painted_floor = g->floor;
   g->painted_column = column;
   g->painted_earth = g->earth;
   g->river_painted = false;
   paint_river(g, s);
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

static bool has_row(const px_objects *o, const px_instance *in, unsigned color)
{
   for (unsigned r = 0; r < in->h; r++)
      if ((o->colors[in->rows + r] & 0xFE) == color)
         return true;
   return false;
}

/* What an object is, from what it is drawn with, where, and in which colours. */
static unsigned kind_of(const px_scene *s, const px_instance *in)
{
   const px_objects *o = s->objects;
   const unsigned color = in->color & 0xFE;

   if (in->y >= ROW_BOTTOM)
      return KIND_NONE;
   if (in->cls == PXC_L_BL)
      return in->w == 4 && in->y >= ROW_WORLD && color == PF_EARTH ? KIND_LADDER : KIND_NONE;
   if (!px_kit_is_player(in))
      return KIND_NONE;
   if (in->y + (int)in->h <= ROW_WORLD)
      return KIND_SCORE;
   if (in->cls == PXC_L_P0)
      return color == 0x12 || has_row(o, in, 0xC8) ? KIND_HARRY : KIND_NONE;
   /* A player stretched wide is a ledge by the river. */
   if (in->w > 8)
      return color == PF_FLOOR || color == PF_EARTH ? KIND_LEDGE : KIND_NONE;
   switch (color)
   {
      case 0x26: return KIND_FROG;
      case 0x04: return KIND_BAT;
      case 0x0A: return KIND_CONDOR;
      case 0x32: return KIND_CROSS;
      case 0x1E: return KIND_GOLD;
      case 0x18: return KIND_CAT;
      case 0x0E:
      {
         const int x = in->x + 4, y = in->y + (int)in->h / 2;
         if (has_row(o, in, 0x1E))
            return KIND_RING;
         if (in->h <= 4 && x >= 0 && x < PXC_W && y >= 0 && y < (int)s->frame->height
               && s->frame->color[PXC_L_BK][(size_t)y * PXC_W + (size_t)x] == BK_RIVER)
            return KIND_EEL;
         /* Something of the same colour flies, eight rows high: not known. */
         return in->h >= 10 ? KIND_SCORPION : KIND_NONE;
      }
      default:   return KIND_NONE;
   }
}

static unsigned role_of(unsigned kind)
{
   switch (kind)
   {
      case KIND_HARRY:    return PX_ROLE_PLAYER;
      case KIND_FROG:
      case KIND_SCORPION:
      case KIND_EEL:
      case KIND_BAT:
      case KIND_CONDOR:   return PX_ROLE_ENEMY;
      case KIND_CROSS:
      case KIND_GOLD:
      case KIND_RING:     return PX_ROLE_BONUS;
      case KIND_SCORE:    return PX_ROLE_HUD;
      default:            return PX_ROLE_NONE;
   }
}

/* The colour of a row of an object, which has the game's colour `color` there; `original`
 * where the module has none for it. */
static uint32_t color_of(const p2 *g, const px_instance *in, unsigned row, unsigned color, uint32_t original)
{
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
      case KIND_FROG:
         switch (color)
         {
            case 0x26: return 0x46A83A;
            case 0x28: return 0x8CD86A;   /* its belly */
            case 0x14: return 0xF4DC48;   /* its eyes */
            case 0x2A: return 0x2E7428;   /* its legs */
            default:   return original;
         }
      case KIND_SCORPION:
         /* The sting red, the body the colour of bone. */
         return px_rgb_mix(0xE8503A, 0xEAD8A6, down * 2 > 256 ? 256 : down * 2);
      case KIND_EEL:
         /* It crackles. */
         return px_rgb_mix(0x40E0FF, 0xF0FFFF, px_kit_wave(g->frame * 37 + row * 70 + (unsigned)in->x * 9));
      case KIND_BAT:
         return px_rgb_mix(0x8A74B8, 0x3A2C52, down);
      case KIND_CONDOR:
         return px_rgb_mix(0xF4F0E4, 0x7A5A3C, down);
      case KIND_CROSS:
         /* A light that beats slowly. */
         return px_rgb_add(0xE8201A, px_rgb_scale(0x5A3020, px_kit_wave(g->frame * 3)));
      case KIND_GOLD:
      case KIND_RING:
      {
         const unsigned shine = px_kit_wave(g->frame * 6 + row * 24);
         if (color == 0x0E)
            return px_rgb_add(0x9CC8E0, px_rgb_scale(0x303838, shine));
         if (color == 0x1E)
            return px_rgb_add(0xC88A10, px_rgb_scale(0x584C20, shine));
         return original;
      }
      case KIND_CAT:
         return px_rgb_mix(0xF2B24A, 0xA8641E, down);
      case KIND_LADDER:
         return 0xB88A4C;
      case KIND_LEDGE:
         /* Stone as the floors are, earth under it. */
         if (color == PF_FLOOR)
            return px_rgb_mix(0xE0C488, 0x94703E, row * 30 > 256 ? 256 : row * 30);
         return color == PF_EARTH ? px_rgb_mix(0x6A5238, 0x3A2A1A, down) : original;
      case KIND_SCORE:
         return 0xF6EED2;
      default:
         return original;
   }
}

/* Gives an object its colours, row by row, and the light it has. Ladders are scenery,
 * which does not glow. */
static void paint_object(const p2 *g, px_scene *s, const px_instance *in)
{
   const px_objects *o = s->objects;
   const bool scenery = in->group == KIND_LADDER || in->group == KIND_LEDGE;
   const bool glows = in->group == KIND_CROSS || in->group == KIND_EEL;

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
         /* Scenery has no light or shadow, where no other object is. */
         if (scenery && !(s->frame->tags[i] & (in->group == KIND_LEDGE ? PXC_P0 : PXC_P0 | PXC_P1)))
         {
            s->sprite[i] = 0;
            s->energy[i] = 0;
         }
         /* Another object may be in front of this one. */
         if (PX_KEY_CLS(s->top[i]) != PX_CLS_SPRITE || (!in->ghost && s->frame->winner[i] != color))
            continue;
         if (scenery)
         {
            s->top[i] = PX_KEY(PX_CLS_PF, rgb);
            continue;
         }
         s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
         if (s->sprite[i])
            s->sprite[i] = 0xFF000000u | rgb;
         if (glows && (color & 0xFE) != 0x00)
            s->energy[i] = 1;
      }
   }
}

/* The score as a number, or -1. */
static int score_of(const uint8_t *ram, size_t size)
{
   const int hi = px_kit_ram(ram, size, RAM_SCORE), lo = px_kit_ram(ram, size, RAM_SCORE + 1);
   if (hi < 0 || lo < 0 || (hi & 15) > 9 || hi >> 4 > 9 || (lo & 15) > 9 || lo >> 4 > 9)
      return -1;
   return ((hi >> 4) * 10 + (hi & 15)) * 100 + (lo >> 4) * 10 + (lo & 15);
}

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static void frame(void *state, px_scene *s)
{
   p2 *g = (p2*)state;
   px_objects *o = s->objects;
   const int column = px_kit_ram(s->ram, s->ram_size, RAM_COLUMN);
   const int level = px_kit_ram(s->ram, s->ram_size, RAM_LEVEL);
   const int floor_before = g->floor;
   bool treasure_seen = false, water = false;

   if (s->frame->height < ROW_BOTTOM || !s->frame->tags)
      return;
   if (s->advance)
      g->frame++;

   g->floor = find_floor(s);
   if (g->floor < 0)
   {
      /* No floor to be seen (the river's screens): from memory, or as it was. */
      const int scroll = px_kit_ram(s->ram, s->ram_size, RAM_SCROLL);
      g->floor = scroll >= 0 ? ROW_WORLD + ((BAND - scroll % BAND) % BAND) : floor_before;
   }
   g->column = column < 0 ? 0 : column;
   g->surface = false;
   for (unsigned y = ROW_WORLD; y < ROW_BOTTOM; y += 4)
   {
      const unsigned bk = s->frame->color[PXC_L_BK][(size_t)y * PXC_W + 80];
      water = water || bk == BK_RIVER;
      g->surface = g->surface || bk == BK_JUNGLE;
   }

   g->earth = 0;
   g->river_from = g->river_to = -1;
   if (g->colors != COLORS_ORIGINAL)
      paint_scenery(g, s);

   g->harry_x = g->harry_y = -1;
   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->group = (uint8_t)kind_of(s, in);
      in->role  = (uint8_t)role_of(in->group);
      if (in->group == KIND_HARRY && !in->ghost)
      {
         g->harry_x = in->x + 4;
         g->harry_y = in->y + (int)in->h;
      }
      else if ((in->group == KIND_GOLD || in->group == KIND_RING) && s->advance)
      {
         g->treasure_x = in->x + 4;
         g->treasure_y = in->y + (int)in->h - 3;
         treasure_seen = true;
      }
      if (g->colors != COLORS_ORIGINAL && in->group != KIND_NONE)
         paint_object(g, s, in);
   }

   if (g->backdrop)
   {

      paint_backdrop(g, s, g->column);
   }

   if (!s->advance)
      return;

   if (water)
      g->water_seen = 0;
   else if (g->water_seen < 1000)
      g->water_seen++;

   if (g->sparks)
   {
      const int score = score_of(s->ram, s->ram_size);
      if (score >= 0 && g->score >= 0)
      {
         if (score > g->score)
         {
            const bool there = g->treasure_gone < 40;
            const int x = there ? g->treasure_x : g->harry_x, y = there ? g->treasure_y : g->harry_y - 10;
            if (x >= 0)
            {
               px_scene_burst(s, x, y, 0xFFD040, 44, 380);
               px_scene_burst(s, x, y, 0xFFFFFF, 12, 200);
            }
            px_scene_flash(s, 0xFFD060, 60);
         }
         else if (score < g->score && !g->running_down && g->harry_x >= 0)
         {
            px_scene_burst(s, g->harry_x, g->harry_y - 10, 0xFF5030, 36, 360);
            px_scene_flash(s, 0xFF2010, 90);
         }
      }

      /* A cross Harry comes to is where he comes back to. The one he begins on is not
       * come to. */
      {
         bool on = false;
         for (unsigned i = 0; i < o->count && g->harry_x >= 0; i++)
         {
            const px_instance *in = &o->inst[i];
            uint32_t key;
            if (in->group != KIND_CROSS || in->ghost)
               continue;
            if (g->harry_x < in->x - 2 || g->harry_x > in->x + 10 || g->harry_y < in->y || g->harry_y > in->y + 26)
               continue;
            on = true;
            key = ((uint32_t)(column & 255) << 16) | ((uint32_t)(level & 255) << 8) | (uint32_t)(in->x & 255);
            if (key != g->checkpoint && !g->on_cross)
            {
               px_scene_burst(s, in->x + 4, in->y + 3, 0xFF6040, 30, 260);
               px_scene_burst(s, in->x + 4, in->y + 3, 0xFFF0E0, 10, 140);
            }
            g->checkpoint = key;
         }
         if (g->harry_x >= 0)
            g->on_cross = on;
      }

      /* Harry lands when the floors stop. */
      if (floor_before >= 0 && g->floor != floor_before)
         g->falling++;
      else
      {
         if (g->falling >= 8 && g->harry_x >= 0 && g->harry_y > 0 && g->harry_y < (int)s->frame->height)
         {
            const bool splash = s->frame->color[PXC_L_BK][(size_t)g->harry_y * PXC_W + (size_t)g->harry_x] == BK_RIVER;
            if (splash)
               px_scene_burst(s, g->harry_x, g->harry_y - 2, 0x9CD8FF, 26, 260);
            else
               px_scene_burst(s, g->harry_x, g->harry_y - 1, 0xB09878, 14, 160);
         }
         g->falling = 0;
      }

      /* The eels give off sparks now and then. */
      for (unsigned i = 0; i < o->count; i++)
      {
         const px_instance *in = &o->inst[i];
         if (in->group == KIND_EEL && !in->ghost && (g->frame + (unsigned)in->x) % 23 == 0)
            px_scene_burst(s, in->x + (int)(g->frame % 8), in->y + 1, 0x80F0FF, 4, 120);
         /* and gold glints. */
         else if ((in->group == KIND_GOLD || in->group == KIND_RING) && !in->ghost
               && in->h >= 5 && (g->frame + (unsigned)in->y) % 47 == 0)
            px_scene_burst(s, in->x + 1 + (int)(g->frame % 6), in->y + (int)in->h - 4, 0xFFF4C0, 3, 60);
      }
   }

   {
      const int score = score_of(s->ram, s->ram_size);
      if (score >= 0 && g->score >= 0 && score < g->score)
         g->running_down = 30;
      else if (g->running_down)
         g->running_down--;
      g->score = score;
   }
   if (treasure_seen)
      g->treasure_gone = 0;
   else if (g->treasure_gone < 1000)
      g->treasure_gone++;
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game plays music and nothing else.
 *
 *   voice 0   the DPC's three voices. The chip adds three square waves, of pitches it
 *             counts itself, and the game writes what it gives (0, 4, 5, 9, 6, 10, 11 or
 *             15: which of the three are high) to the volume of voice 0 on every line, at
 *             waveform 0. From the writes each wave's rises are had, and from them its
 *             pitch: 15700 lines a second over the lines from rise to rise. They fall on
 *             the notes of the scale (47 lines is E4, 95 lines E3, 21 lines F#5). A note
 *             ends with a frame in which the wave does not move.
 *   voice 1   the drums, each note set at the end of a frame:
 *               waveform 12 at pitch 16, volume 8 for a frame           a low drum
 *               waveform 4 at pitch 16, volume 8 for a frame            a high drum
 *               waveform 8 at pitch 2, volume 5 down to 1, a frame each  a cymbal
 *               waveform 1 at pitches 6, 4, 3, 2, a frame each           a roll, at the
 *                                                                          start of a tune
 *
 * Here the three voices are instruments of their own, note for note, where the game has
 * them: a round bass and two soft leads apart between left and right. The drums are drums.
 * ------------------------------------------------------------------------- */

/* The DPC's volumes: which of its waves are high, or -1. */
static int dpc_waves(unsigned volume)
{
   static const int8_t waves[16] = { 0, -1, -1, -1, 1, 2, 4, -1, -1, 3, 5, 6, -1, -1, -1, 7 };
   return waves[volume & 15];
}

static void music_stop(p2 *g, px_sound *s, unsigned v, float seconds)
{
   dpc_voice *m = &g->music[v];
   px_synth_stop(s->synth, m->id[0], seconds);
   px_synth_stop(s->synth, m->id[1], seconds);
   m->id[0] = m->id[1] = 0;
   m->on = false;
}

static void music_play(p2 *g, px_sound *s, unsigned v, float hz)
{
   /* wave             freq to glide attack  hold decay gain   cutoff to  vibrato */
   static const px_tone bass[2] = {
      { PX_WAVE_TRIANGLE, 1, 0, 0, 0.006f, 0, 0, 0.24f, 0,    0, 0, 0 },
      { PX_WAVE_SAW,      1, 0, 0, 0.006f, 0, 0, 0.08f, 900,  0, 0, 0 }
   };
   static const px_tone lead[2] = {
      { PX_WAVE_TRIANGLE, 1, 0, 0, 0.010f, 0, 0, 0.14f, 0,    0, 5.5f, 0.004f },
      { PX_WAVE_SQUARE,   1, 0, 0, 0.020f, 0, 0, 0.05f, 2400, 0, 5.5f, 0.004f }
   };
   static const float pan[3] = { -0.30f, 0.0f, 0.30f };
   dpc_voice *m = &g->music[v];
   const px_tone *p = hz < 270.0f ? bass : lead;
   px_tone t;

   music_stop(g, s, v, 0.02f);
   for (unsigned n = 0; n < 2; n++)
   {
      t = p[n];
      t.freq = hz;
      m->id[n] = px_synth_play(s->synth, &t, pan[v], 1.0f);
   }
   m->on = true;
   m->hz = hz;
}

/* The DPC's voices in the frame's writes. False if voice 0 did not play them. */
static bool hear_music(p2 *g, px_sound *s)
{
   const struct pxc_frame *f = s->frame;
   uint16_t lines[3][MAX_RISES];
   unsigned count[3] = { 0, 0, 0 }, writes = 0;
   bool dpc = true;

   for (uint32_t i = 0; f && i < f->write_count; i++)
   {
      const struct pxc_regwrite *w = &f->writes[i];
      const uint32_t t = g->lines + w->scanline;
      int waves;
      if (w->reg != 0x19)
         continue;
      writes++;
      waves = dpc_waves(w->value);
      if (waves < 0)
      {
         dpc = false;
         continue;
      }
      for (unsigned v = 0; v < 3; v++)
      {
         dpc_voice *m = &g->music[v];
         const bool high = (waves >> v) & 1;
         if (high && !m->high)
         {
            if (m->risen && count[v] < MAX_RISES)
               lines[v][count[v]++] = (uint16_t)(t - m->rise > 65535u ? 65535u : t - m->rise);
            m->rise  = t;
            m->risen = true;
         }
         m->high = high;
      }
   }
   g->lines += f && f->scanlines_total ? f->scanlines_total : 262;

   /* The DPC writes voice 0 on every line; a frame of few writes is not its music. */
   if (!dpc || writes < 100 || g->tia.wave[0] != 0)
   {
      for (unsigned v = 0; v < 3; v++)
         if (g->music[v].on)
            music_stop(g, s, v, 0.10f);
      return false;
   }

   for (unsigned v = 0; v < 3; v++)
   {
      dpc_voice *m = &g->music[v];
      const float since = (float)(g->lines - m->rise);
      const unsigned n = count[v];
      float sum = 0.0f, last = n ? (float)lines[v][n - 1] : 0.0f;
      unsigned same = 0;
      bool gap = false;

      /* The note the frame ends with: the rises at its end that are as far apart as the
       * last two. A longer wait before them is a rest, or another note. */
      if (n && last <= MAX_PERIOD)
         for (unsigned k = n; k-- > 0; )
         {
            const float d = (float)lines[v][k];
            if (fabsf(d - last) <= last * 0.15f + 1.5f)
            {
               sum += d;
               same++;
               continue;
            }
            gap = d > last * 2.2f;
            break;
         }
      if (same)
         m->period = sum / (float)same;
      else if (n)
         gap = true;   /* it rose after a rest, and once only */

      if (!m->risen || m->period <= 0.0f || (!same && gap)
            || since > (m->period * 2.2f > 60.0f ? m->period * 2.2f : 60.0f))
      {
         /* The wave stands still: silence. */
         if (m->on)
            music_stop(g, s, v, 0.06f);
         continue;
      }
      if (!same)
         continue;
      if (g->own_sound)
      {
         const float hz = px_kit_tune(LINE_HZ / m->period);
         if (!m->on || gap || fabsf(hz - m->hz) > m->hz * 0.02f)
            music_play(g, s, v, hz);
      }
   }
   return true;
}

static void play_drum(p2 *g, px_sound *s)
{
   static const px_tone low[3] = {
      { PX_WAVE_SINE,  150,  46, 0.12f, 0.001f, 0.01f, 0.26f, 0.50f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 300, 90, 0.05f, 0.001f, 0,  0.08f, 0.20f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 3000,   0, 0,     0,      0,     0.02f, 0.10f, 1800, 400, 0, 0 }
   };
   static const px_tone high[3] = {
      { PX_WAVE_NOISE, 7000, 0,   0,     0,      0.01f, 0.16f, 0.30f, 6000, 1600, 0, 0 },
      { PX_WAVE_TRIANGLE, 240, 170, 0.06f, 0.001f, 0.01f, 0.10f, 0.30f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,  480, 330, 0.04f, 0.001f, 0,     0.05f, 0.08f, 0, 0, 0, 0 }
   };
   static const px_tone cymbal = { PX_WAVE_NOISE, 12000, 0, 0, 0, 0.005f, 0.10f, 0.11f, 11000, 7000, 0, 0 };
   const unsigned wave = g->tia.wave[1], pitch = g->tia.pitch[1];

   if (wave == 12 && pitch == 16)
      px_kit_play(s, low, 3, 0.0f);
   else if (wave == 4 && pitch == 16)
      px_kit_play(s, high, 3, -0.12f);
   else if (wave == 8 && pitch == 2)
      px_synth_play(s->synth, &cymbal, 0.35f, (float)g->tia.volume[1] / 5.0f);
   else if (wave == 1)
   {
      /* The roll: a drum tuned to the game's pitch, two octaves down. */
      px_tone tom = { PX_WAVE_SINE, 1, 1, 0.10f, 0.001f, 0.01f, 0.18f, 0.40f, 0, 0, 0, 0 };
      const float hz = px_kit_tia_hz(wave, pitch) / 4.0f;
      tom.freq = hz;
      tom.freq_end = hz * 0.7f;
      px_synth_play(s->synth, &tom, -0.25f, 1.0f);
   }
}

static bool is_drum(const px_kit_tia *t)
{
   return (t->pitch[1] == 16 && (t->wave[1] == 12 || t->wave[1] == 4))
         || (t->wave[1] == 8 && t->pitch[1] == 2) || t->wave[1] == 1;
}

/* A number of frames between two, by chance. */
static unsigned wait_for(p2 *g, unsigned least, unsigned most)
{
   return least + px_kit_chance(&g->chance) % (most - least + 1);
}

/* The caverns' own: drops of water and air that moves, the river where it is, wind above
 * ground and a bird now and then. */
static void play_ambience(p2 *g, px_sound *s, bool surface, bool river)
{
   static const px_tone air   = { PX_WAVE_NOISE, 600, 0, 0, 1.5f, 0, 0, 0.040f, 260, 0, 0.09f, 0.40f };
   static const px_tone rush  = { PX_WAVE_NOISE, 3000, 0, 0, 1.0f, 0, 0, 0.045f, 1400, 0, 0.3f, 0.20f };
   static const px_tone wind  = { PX_WAVE_NOISE, 2600, 0, 0, 1.5f, 0, 0, 0.040f, 520, 0, 0.11f, 0.30f };
   static const px_tone drop[2] = {
      { PX_WAVE_SINE, 1500, 760, 0.05f, 0.001f, 0.004f, 0.30f, 0.10f, 0, 0, 0, 0 },
      { PX_WAVE_SINE, 3000, 1500, 0.04f, 0.001f, 0,     0.10f, 0.03f, 0, 0, 0, 0 }
   };
   const bool on = g->ambience && g->own_sound;

   if (!on || surface)
   {
      px_synth_stop(s->synth, g->air, 0.8f);
      g->air = 0;
   }
   if (!on || !surface)
   {
      px_synth_stop(s->synth, g->wind, 0.8f);
      g->wind = 0;
   }
   if (!on || !river)
   {
      px_synth_stop(s->synth, g->river, 0.8f);
      g->river = 0;
   }
   if (!on)
      return;

   if (river && !px_synth_move(s->synth, g->river, 0.0f, 1.0f, 0))
      g->river = px_synth_play(s->synth, &rush, 0.0f, 1.0f);
   if (surface)
   {
      static const px_tone chirp = { PX_WAVE_SINE, 2600, 3400, 0.06f, 0.004f, 0.02f, 0.10f, 0.045f, 0, 0, 0, 0 };
      if (!px_synth_move(s->synth, g->wind, 0.0f, 1.0f, 0))
         g->wind = px_synth_play(s->synth, &wind, 0.0f, 1.0f);
      if (g->wait_bird)
         g->wait_bird--;
      else
      {
         px_tone p = chirp;
         const float pan = (float)(px_kit_chance(&g->chance) % 180) / 100.0f - 0.9f;
         p.freq += (float)(px_kit_chance(&g->chance) % 1200);
         p.freq_end = p.freq * 1.3f;
         px_synth_play(s->synth, &p, pan, 1.0f);
         g->wait_bird = wait_for(g, 90, 320);
      }
      return;
   }
   if (!px_synth_move(s->synth, g->air, 0.0f, 1.0f, 0))
      g->air = px_synth_play(s->synth, &air, 0.0f, 1.0f);
   if (g->wait_drop)
      g->wait_drop--;
   else
   {
      px_kit_play(s, drop, 2, (float)(px_kit_chance(&g->chance) % 160) / 100.0f - 0.8f);
      g->wait_drop = wait_for(g, 40, 200);
   }
}

static void sound(void *state, px_sound *s)
{
   p2 *g = (p2*)state;
   const int score = score_of(s->ram, s->ram_size);
   const int scroll = px_kit_ram(s->ram, s->ram_size, RAM_SCROLL);
   const int level = px_kit_ram(s->ram, s->ram_size, RAM_LEVEL);
   bool music;

   px_kit_tia_hear(&g->tia, s->frame);
   music = s->frame && hear_music(g, s);
   if (!g->own_sound)
      for (unsigned v = 0; v < 3; v++)
         if (g->music[v].on)
            music_stop(g, s, v, 0.05f);

   if (g->own_sound && px_kit_tia_louder(&g->tia, 1) && is_drum(&g->tia))
      play_drum(g, s);

   /* What happens to Harry is felt: memory tells it. */
   if (score >= 0 && g->heard_score >= 0)
   {
      if (score > g->heard_score)
         px_sound_rumble(s, 16000, 26000, 12);
      else if (score < g->heard_score && !g->heard_running)
         px_sound_rumble(s, 50000, 30000, 30);
      if (score < g->heard_score)
         g->heard_running = 30;
      else if (g->heard_running)
         g->heard_running--;
   }
   g->heard_score = score;
   if (scroll >= 0 && g->scroll >= 0 && scroll != g->scroll)
      g->heard_falling++;
   else
   {
      if (g->heard_falling >= 8)
         px_sound_rumble(s, 26000, 14000, 8);
      g->heard_falling = 0;
   }
   g->scroll = scroll;

   if (g->own_sound)
   {
      if (music)
         s->voice[0] = 0.0f;
      if (!g->tia.volume[1] || is_drum(&g->tia))
         s->voice[1] = 0.0f;
   }
   g->music_heard = music;
   play_ambience(g, s, g->surface && level == 0, g->water_seen < 30);
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   p2 *g = (p2*)state;
   g->floor = -1;
   g->harry_x = g->harry_y = -1;
   g->treasure_gone = 1000;
   g->score = g->heard_score = -1;
   g->running_down = g->heard_running = 0;
   g->falling = g->heard_falling = 0;
   g->scroll = -1;
   g->checkpoint = 0xFFFFFFFFu;
   g->on_cross = true;
   g->water_seen = 1000;
   g->painted_floor = g->painted_column = -1000;
   px_kit_tia_reset(&g->tia);
   g->lines = 0;
   memset(g->music, 0, sizeof(g->music));
   g->music_heard = false;
   g->air = g->river = g->wind = 0;
   g->chance = 0x2F6A51u;
   g->wait_drop = 60;
   g->wait_bird = 90;
}

static void *create(void)
{
   p2 *g = (p2*)calloc(1, sizeof(p2));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = g->ambience = true;
      g->tile_column = g->tab_column = -1000;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   p2 *g = (p2*)state;
   if (g)
      free(g->tile);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   p2 *g = (p2*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->ambience  = px_kit_on(get, OPT_AMBIENCE);
   g->painted_floor = -1000;
}

const px_game px_game_pitfall_2 = {
   "Pitfall II: Lost Caverns", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
