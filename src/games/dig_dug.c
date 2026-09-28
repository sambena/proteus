/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Dig Dug (Atari, 1983).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  14..29   the sky: playfield, in blue (84), across columns 12..147. Dig Dug walks
 *                  in from the right along row 18 at the start of a round and digs down.
 *   rows  30..189  the earth: playfield across columns 12..147, in four bands of colour that
 *                  get darker and redder with depth (rows 30..61, 62..109, 110..157, 158..189
 *                  in the first round). Every fourth row (33, 37, ... 185) the game leaves
 *                  the earth out between the borders, which gives it stripes. Tunnels are
 *                  background (black). The grid is 8 columns and 16 rows.
 *   Dig Dug        a player of colour 7E, 10 or 11 rows high, in the frames of the one that
 *                  draws him; in the frames between, a small part of him of colour 42, 3 or
 *                  4 rows high, some five rows below his top. Caught, he is 8 rows high,
 *                  then 10 and flat.
 *   the pump       his colour (7E): a line 4 rows high and 8 or 16 wide, a wavy hose that
 *                  goes out in the way he faces. It shares its object with the enemy that
 *                  is pumped, and the two are drawn in turns.
 *   Pookas         players of colour 3A, 9 or 10 rows high; 3E and 6 or 7 rows high while
 *                  they are ghosts (goggles that go through the earth). Pumped, a Pooka is
 *                  drawn twice as wide (16 columns), and bursts.
 *   Fygars         players of colour D8, 11 or 12 rows high; DE while they are ghosts.
 *   Fygar's fire   a player of a colour of the earth's (34 where it was seen), 8 by 9 as
 *                  it starts and 32 by 8 at full length, next to the Fygar on its row.
 *   rocks          the ball, 4 by 7, in the colour of the earth's band, in a hole of the
 *                  earth. When Dig Dug has dug under one, the ball is no longer drawn there
 *                  and a player of a darker colour (24) is: 8 by 7 and 8 by 8 in turns
 *                  while it shakes, then 8 by 7 as it falls, four rows every four frames,
 *                  and 8 by 4 as it crumbles.
 *   rows 192..198  the lives: squares of playfield at the left; the score: players at the
 *                  right, in colour 36.
 *
 * Several things are drawn with one player at different heights of the picture, and the
 * pumped enemy and the pump take turns, but no two of the same kind are near enough to be
 * mistaken for each other: they are told apart by where they are, as for any game.
 *
 * The console's memory keeps where things are in the form the TIA's motion registers take
 * (the fine offset in the high four bits: byte 22 is Dig Dug's column), not as columns and
 * rows; this module goes by the picture and does not read it.
 *
 * The earth, the sky and the tunnels are painted at the size of the picture from which
 * captured pixels are earth and which are tunnel, and the game's own are made dark
 * background, which is where a backdrop shows: see paint_earth().
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define SKY_TOP     14
#define EARTH_TOP   30
#define EARTH_END   190    /* the row after the earth's last */
#define HUD_TOP     190
#define FIELD_LEFT  12
#define FIELD_RIGHT 148    /* the column after the field's last */
#define ROCKS       6

#define COLOR_DIG     0x7E
#define COLOR_DIG2    0x42
#define COLOR_POOKA   0x3A
#define COLOR_GHOST   0x3E
#define COLOR_FYGAR   0xD8
#define COLOR_FGHOST  0xDE

/* What a captured pixel of the field is. */
enum { CELL_OUT = 0, CELL_SKY, CELL_DIRT, CELL_TUNNEL, CELL_UNSEEN = 0xFF };

/* What an object is. */
enum { KIND_NONE = 0, KIND_DIG, KIND_DIG_PART, KIND_HOSE, KIND_POOKA, KIND_GHOST,
   KIND_FYGAR, KIND_FGHOST, KIND_PUMPED, KIND_FIRE, KIND_ROCK, KIND_LOOSE, KIND_HUD };

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "6dda84fb8e442ecf34241ac0d1d91d69",   /* Dig Dug (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "medium",
   "width", "50",
   "reverb", "room",
   NULL
};

#define OPT_COLORS   "proteus_dd_colors"
#define OPT_BACKDROP "proteus_dd_backdrop"
#define OPT_SPARKS   "proteus_dd_sparks"
#define OPT_SOUND    "proteus_dd_sound"
#define OPT_DIGGING  "proteus_dd_digging"

static const char *const colors[] = { "arcade", "The arcade's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "earth", "Earth, tunnels and sky", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_ARCADE = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Dig Dug in white and blue, red Pookas with goggles, green Fygars, fire that glows, rocks of stone, a pump that shines and enemies that glow as they swell; or the game's own colours.",
     "arcade", colors },
   { OPT_BACKDROP, "Backdrop",
     "The earth in its bands of soil with grain and pebbles, darker and redder with depth; tunnels dug dark with crumbling edges; a sky made from the game's blue. Or the game's own stripes.",
     "earth", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks where a pumped enemy bursts, dust where a rock shakes loose and where it crashes, crumbs where Dig Dug digs, and a flash when he is caught.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the pump's hose, every stroke of the pump, an enemy bursting, Fygar's fire, a rock shaking loose and crashing, each where it happens between left and right; the game's tunes, its own notes in softer voices. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_DIGGING, "Digging",
     "The crunch of the earth where Dig Dug digs, which the game has not. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   /* The options. */
   unsigned colors;
   bool backdrop, sparks, own_sound, digging;

   uint32_t frame;                  /* counts the pictures that advance */

   /* The earth. */
   uint8_t cell[PXC_MAX_H][PXC_W];  /* CELL_* of every captured pixel of the field */
   uint8_t was[PXC_MAX_H][PXC_W];   /* as it was painted; CELL_UNSEEN: not painted */
   uint8_t last[PXC_MAX_H][PXC_W];  /* as the frame before had it; CELL_UNSEEN: not known */
   uint8_t row_color[PXC_MAX_H];    /* the palette index of the earth's band on a row */
   uint8_t was_color[PXC_MAX_H];
   uint8_t dirty[PXC_MAX_H][PXC_W / 8 + 1];
   uint16_t band_top[PXC_MAX_H], band_rows[PXC_MAX_H];
   px_kit_texture soil, edge;       /* shades of the grain, and of how far edges crumble */
   uint32_t (*tables)[256];         /* the soil's colours for every palette index */
   uint8_t made[256];
   uint32_t sky[256];
   uint8_t sky_index;
   bool painted;                    /* the backdrop has the earth as `was` has it */
   struct { int16_t x0, y0, x1, y1; } rocks[ROCKS], was_rocks[ROCKS];   /* cells they cover */
   unsigned rock_count, was_rock_count;
   bool earth;                      /* the frame had the earth */

   /* What happened. */
   unsigned dug;                    /* captured pixels of earth dug in the last frame */
   int dig_x, dig_y;                /* where */
   unsigned since_crumbs;
   int pump_x, pump_y;              /* where an enemy was pumped last; -1: none */
   uint32_t pump_rgb;
   unsigned pump_away;              /* frames since it was seen */
   bool pump_burst;                 /* its burst was shown */
   unsigned pop_heard;              /* frames since the sound heard a burst; 0: none */
   int rock_x, rock_y;              /* the loose rock; -1: none */
   int rock_fell;                   /* rows it fell */
   unsigned rock_still;             /* frames it stood after falling */
   bool rock_crashed;
   unsigned rock_away;
   int dig_px, dig_py;              /* where Dig Dug was seen last */
   bool caught_heard, crash_heard;  /* what the sound heard, for the picture to show */
   int dig_at, fire_at;            /* columns, for where a sound is; -1: not known */

   /* The sounds. */
   px_kit_tia tia;
   unsigned heard[2];               /* SOUND_* of each voice in the frame before */
   unsigned since_stroke;           /* frames since the pump's stroke was heard */
   unsigned since_pop;              /* frames since a burst began */
   unsigned since_dig_sound;
   unsigned since_rock;             /* frames since a rock shook loose */
   unsigned since_end;              /* frames since the tune of Dig Dug's end began */
} dd;

/* ---------------------------------------------------------------------------
 * The picture: the earth
 * ------------------------------------------------------------------------- */

static bool is_pf(const px_scene *s, unsigned r, unsigned c)
{
   return (s->frame->tags[(size_t)r * PXC_W + c] & PXC_PF) != 0;
}

/* The earth leaves out every fourth row between its borders. */
static bool is_gap_row(unsigned r)
{
   return r > EARTH_TOP && r + 1 < EARTH_END && (r & 3) == 1;
}

/* Which captured pixels are sky, earth or tunnel. False if the picture has not the earth
 * (another screen, or a picture too short). */
static bool find_earth(dd *g, const px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   const uint8_t *pf = f->color[PXC_L_PF];

   if (f->height < EARTH_END || !f->tags || !pf)
      return false;
   /* The borders of the earth are there on every row of it. */
   for (unsigned r = EARTH_TOP; r < EARTH_END; r += 16)
      if (!is_pf(s, r, FIELD_LEFT) || !is_pf(s, r, FIELD_RIGHT - 1))
         return false;

   memset(g->cell, CELL_OUT, sizeof(g->cell));
   for (unsigned r = SKY_TOP; r < EARTH_END; r++)
   {
      const bool sky = r < EARTH_TOP;
      g->row_color[r] = pf[(size_t)r * PXC_W + FIELD_LEFT];
      for (unsigned c = FIELD_LEFT; c < FIELD_RIGHT; c++)
      {
         bool earth = is_pf(s, r, c);
         if (!earth && is_gap_row(r))
            earth = is_pf(s, r - 1, c) || is_pf(s, r + 1, c);
         if (sky)
            g->cell[r][c] = earth ? CELL_SKY : CELL_OUT;
         else
            g->cell[r][c] = earth ? CELL_DIRT : CELL_TUNNEL;
      }
   }
   g->sky_index = g->row_color[(SKY_TOP + EARTH_TOP) / 2];

   /* The bands: rows of one colour. */
   for (unsigned r = EARTH_TOP; r < EARTH_END;)
   {
      unsigned end = r + 1;
      while (end < EARTH_END && g->row_color[end] == g->row_color[r])
         end++;
      for (unsigned k = r; k < end; k++)
      {
         g->band_top[k]  = (uint16_t)r;
         g->band_rows[k] = (uint16_t)(end - r);
      }
      r = end;
   }
   return true;
}

/* The soil's colours for a band of the game's colour: its grain from dark and redder to
 * light and warmer. */
static const uint32_t *soil_table(dd *g, const uint32_t *palette, uint8_t index)
{
   uint32_t *t = g->tables[index];
   if (!g->made[index])
   {
      const uint32_t mid = palette[index] & 0xFFFFFFu;
      px_kit_shades(t, px_rgb_scale(px_rgb_mix(mid, 0x5A1004, 70), 120), mid,
            px_rgb_mix(mid, 0xFFE4A8, 80));
      g->made[index] = 1;
   }
   return t;
}

/* The textures, painted once for a size of the picture: in cells of chance to the picture
 * and not to the pixel, so that they look alike at every size. */
static void paint_textures(dd *g, const px_scene *s)
{
   const size_t n = (size_t)s->w * s->h;
   uint8_t *soil = g->soil.shades, *edge = g->edge.shades;
   px_kit_texture layer = { NULL, 0, 0 };

   memset(soil, 64, n);
   memset(edge, 0, n);
   px_kit_texture_fit(&layer, s);
   if (!layer.shades)
      return;
   /* Strata that lie along the bands, lumps, pebbles, and the grain. */
   px_kit_texture_noise(&layer, 5, 70, 31);
   for (size_t i = 0; i < n; i++)
      soil[i] = (uint8_t)(soil[i] + layer.shades[i] / 4);
   px_kit_texture_noise(&layer, 24, 40, 32);
   for (size_t i = 0; i < n; i++)
      soil[i] = (uint8_t)(soil[i] + layer.shades[i] / 5);
   px_kit_texture_noise(&layer, 150, 110, 33);
   for (size_t i = 0; i < n; i++)
   {
      const unsigned v = layer.shades[i];
      /* Pebbles: the peaks of the noise, light, and its troughs, dark. */
      if (v > 205)
         soil[i] = (uint8_t)(soil[i] + 60 > 255 ? 255 : soil[i] + 60);
      else if (v < 40)
         soil[i] = (uint8_t)(soil[i] > 50 ? soil[i] - 50 : 0);
   }
   px_kit_texture_noise(&layer, 400, 300, 34);
   for (size_t i = 0; i < n; i++)
      soil[i] = (uint8_t)(soil[i] + layer.shades[i] / 6);
   /* How far the earth crumbles into a tunnel: coarse bites with a fine edge. */
   px_kit_texture_noise(&layer, 70, 60, 35);
   for (size_t i = 0; i < n; i++)
      edge[i] = (uint8_t)(layer.shades[i] * 3 / 4);
   px_kit_texture_noise(&layer, 260, 200, 36);
   for (size_t i = 0; i < n; i++)
      edge[i] = (uint8_t)(edge[i] + layer.shades[i] / 4);
   px_kit_texture_free(&layer);
}

static uint8_t cell_at(const dd *g, int r, int c)
{
   if (r < SKY_TOP || r >= EARTH_END || c < FIELD_LEFT || c >= FIELD_RIGHT)
      return CELL_OUT;
   return g->cell[r][c];
}

/* How near a pixel of a cell is to a neighbour of a kind, in halves of an output pixel;
 * 0xFFFF if it has none. */
static unsigned near_kind(const dd *g, unsigned r, unsigned c, unsigned u, unsigned v,
      unsigned sx, unsigned sy, uint8_t kind)
{
   const unsigned dl = u * 2 + 1, dr = (sx - 1 - u) * 2 + 1, du = v * 2 + 1, dd_ = (sy - 1 - v) * 2 + 1;
   const bool L = cell_at(g, (int)r, (int)c - 1) == kind, R = cell_at(g, (int)r, (int)c + 1) == kind;
   const bool U = cell_at(g, (int)r - 1, (int)c) == kind, D = cell_at(g, (int)r + 1, (int)c) == kind;
   unsigned d = 0xFFFF;
   if (L && dl < d) d = dl;
   if (R && dr < d) d = dr;
   if (U && du < d) d = du;
   if (D && dd_ < d) d = dd_;
   /* Corners, where the pixels either side are not of the kind. */
   if (!L && !U && cell_at(g, (int)r - 1, (int)c - 1) == kind)
      d = d < (dl > du ? dl : du) ? d : (dl > du ? dl : du);
   if (!R && !U && cell_at(g, (int)r - 1, (int)c + 1) == kind)
      d = d < (dr > du ? dr : du) ? d : (dr > du ? dr : du);
   if (!L && !D && cell_at(g, (int)r + 1, (int)c - 1) == kind)
      d = d < (dl > dd_ ? dl : dd_) ? d : (dl > dd_ ? dl : dd_);
   if (!R && !D && cell_at(g, (int)r + 1, (int)c + 1) == kind)
      d = d < (dr > dd_ ? dr : dd_) ? d : (dr > dd_ ? dr : dd_);
   return d;
}

/* A tunnel: dark, of the band's colour, with loose crumbs by its walls. */
static uint32_t tunnel_pixel(const uint32_t *table, unsigned grain, unsigned bite, unsigned wall,
      unsigned sy)
{
   uint32_t rgb = px_rgb_scale(px_rgb_mix(table[40], 0x060302, 170), 150 + grain * 106 / 255);
   if (wall < sy * 2 && bite > 150 && grain > 150)
      rgb = px_rgb_scale(table[70], 150);
   return rgb;
}

static void paint_cell(dd *g, px_scene *s, unsigned r, unsigned c)
{
   const unsigned sx = s->sx, sy = s->sy, w = s->w;
   const uint8_t kind = g->cell[r][c];
   const uint32_t *palette = s->frame->palette;

   for (unsigned v = 0; v < sy; v++)
   {
      const unsigned Y = r * sy + v;
      uint32_t *out = s->backdrop + (size_t)Y * w + (size_t)c * sx;
      const uint8_t *grain = g->soil.shades + (size_t)Y * w + (size_t)c * sx;
      const uint8_t *bite = g->edge.shades + (size_t)Y * w + (size_t)c * sx;

      if (kind == CELL_OUT)
      {
         for (unsigned u = 0; u < sx; u++)
            out[u] = 0;
         continue;
      }
      if (kind == CELL_SKY)
      {
         const unsigned span = (EARTH_TOP - SKY_TOP) * sy;
         const uint32_t rgb = g->sky[(Y - SKY_TOP * sy) * 255 / (span > 1 ? span - 1 : 1)];
         for (unsigned u = 0; u < sx; u++)
            out[u] = px_rgb_add(rgb, px_rgb_scale(0xFFFFFF, grain[u] > 150 ? (grain[u] - 150) / 5 : 0));
         continue;
      }
      {
         const uint32_t *table = soil_table(g, palette, g->row_color[r]);
         /* Darker with depth: through the earth, and within every band. */
         const unsigned earth_h = (EARTH_END - EARTH_TOP) * sy, band_h = g->band_rows[r] * sy;
         const unsigned deep = (Y - EARTH_TOP * sy) * 56 / earth_h
               + (Y - g->band_top[r] * sy) * 34 / (band_h ? band_h : 1);
         const unsigned light = 256 - deep;
         const bool surface = kind == CELL_DIRT && cell_at(g, (int)r - 1, (int)c) == CELL_SKY && v == 0;

         for (unsigned u = 0; u < sx; u++)
         {
            if (kind == CELL_TUNNEL)
            {
               const unsigned wall = near_kind(g, r, c, u, v, sx, sy, CELL_DIRT);
               out[u] = tunnel_pixel(table, grain[u], bite[u], wall, sy);
               continue;
            }
            {
               /* The earth crumbles into the tunnel next to it: up to three quarters of a
                * row in, as far as the chance of the edge says; darker at the lip. */
               const unsigned near = near_kind(g, r, c, u, v, sx, sy, CELL_TUNNEL);
               const unsigned crumble = bite[u] * sy * 3 / 2 / 256;
               uint32_t rgb;
               if (near < crumble)
               {
                  out[u] = tunnel_pixel(table, grain[u], bite[u], 0, sy);
                  continue;
               }
               rgb = px_rgb_scale(table[grain[u]], light);
               if (near < crumble + sy)
                  rgb = px_rgb_scale(rgb, 130 + (near - crumble) * 110 / sy);
               if (surface)
                  rgb = px_rgb_mix(rgb, 0xFFF0C0, 90);
               out[u] = rgb;
            }
         }
      }
   }
}

/* A rock: a boulder of stone lit from the upper left, in place of the game's block. */
static void paint_boulder(const dd *g, px_scene *s, const px_instance *in)
{
   const float sx = (float)s->sx, sy = (float)s->sy;
   const float cx = ((float)in->x + (float)in->w * 0.5f) * sx, cy = ((float)in->y + (float)in->h * 0.5f) * sy;
   const float rx = 3.4f * sx, ry = ((float)in->h * 0.5f + 1.4f) * sy;
   const int X0 = (int)(cx - rx), X1 = (int)(cx + rx) + 1, Y0 = (int)(cy - ry), Y1 = (int)(cy + ry) + 1;

   for (int Y = Y0 < 0 ? 0 : Y0; Y < Y1 && Y < (int)s->h; Y++)
      for (int X = X0 < 0 ? 0 : X0; X < X1 && X < (int)s->w; X++)
      {
         const float nx = ((float)X + 0.5f - cx) / rx, ny = ((float)Y + 0.5f - cy) / ry;
         const float d = nx * nx + ny * ny;
         const size_t i = (size_t)Y * s->w + (size_t)X;
         float nz, lit;
         unsigned grain, f;
         if (d >= 1.0f)
            continue;
         nz  = sqrtf(1.0f - d);
         lit = -0.45f * nx - 0.55f * ny + 0.70f * nz;
         if (lit < 0.0f)
            lit = 0.0f;
         grain = g->soil.shades[i];
         f = (unsigned)(40.0f + 230.0f * lit) * (150 + grain / 2) / 256;
         /* A dark rim where it meets the earth. */
         if (d > 0.84f)
            f = f * 140 / 256;
         s->backdrop[i] = px_rgb_scale(0x9A8C7C, f > 300 ? 300 : f);
      }
}

/* Where the rocks are: the ball, and the player that shakes, falls and crumbles. Their
 * cells are painted anew from the earth when they move on. */
static void find_rocks(dd *g, const px_scene *s, const uint8_t *kinds)
{
   const px_objects *o = s->objects;
   g->rock_count = 0;
   for (unsigned i = 0; i < o->count && g->rock_count < ROCKS; i++)
   {
      const px_instance *in = &o->inst[i];
      int x0, x1, y0, y1;
      if (kinds[i] != KIND_ROCK && kinds[i] != KIND_LOOSE)
         continue;
      x0 = in->x + (int)in->w / 2 - 5;
      x1 = in->x + (int)in->w / 2 + 5;
      y0 = in->y - 3;
      y1 = in->y + (int)in->h + 3;
      g->rocks[g->rock_count].x0 = (int16_t)(x0 < FIELD_LEFT ? FIELD_LEFT : x0);
      g->rocks[g->rock_count].x1 = (int16_t)(x1 > FIELD_RIGHT ? FIELD_RIGHT : x1);
      g->rocks[g->rock_count].y0 = (int16_t)(y0 < SKY_TOP ? SKY_TOP : y0);
      g->rocks[g->rock_count].y1 = (int16_t)(y1 > EARTH_END ? EARTH_END : y1);
      g->rock_count++;
   }
}

/* The earth, the tunnels and the sky, painted where they changed. */
static void paint_earth(dd *g, px_scene *s, const uint8_t *kinds)
{
   const unsigned sx = s->sx, sy = s->sy;
   bool fresh, all;

   if (!s->backdrop || !sx || !sy || s->w != PXC_W * sx || s->h != s->frame->height * sy)
      return;
   fresh = px_kit_texture_fit(&g->soil, s);
   fresh = px_kit_texture_fit(&g->edge, s) || fresh;
   if (!g->soil.shades || !g->edge.shades || !g->tables)
      return;
   if (fresh)
      paint_textures(g, s);
   all = fresh || s->backdrop_stale || !g->painted;
   if (all)
   {
      memset(s->backdrop, 0, (size_t)s->w * s->h * sizeof(uint32_t));
      memset(g->was, CELL_UNSEEN, sizeof(g->was));
      memset(g->was_color, 0, sizeof(g->was_color));
      px_kit_sky_shades(g->sky, s->frame->palette[g->sky_index] & 0xFFFFFFu);
   }
   else if (g->was_color[(SKY_TOP + EARTH_TOP) / 2] != g->sky_index)
   {
      /* Another sky: all of it again. */
      px_kit_sky_shades(g->sky, s->frame->palette[g->sky_index] & 0xFFFFFFu);
      for (unsigned r = SKY_TOP; r < EARTH_TOP; r++)
         g->was_color[r] = (uint8_t)~g->row_color[r];
   }

   /* What changed, and the pixels next to it, which crumble towards it. */
   memset(g->dirty, 0, sizeof(g->dirty));
   for (unsigned r = SKY_TOP; r < EARTH_END; r++)
   {
      const bool row = g->row_color[r] != g->was_color[r];
      for (unsigned c = FIELD_LEFT - 1; c <= FIELD_RIGHT; c++)
         if (row || g->cell[r][c] != g->was[r][c])
            for (unsigned y = r > SKY_TOP ? r - 1 : r; y <= r + 1 && y < EARTH_END; y++)
               for (unsigned x = c - 1; x <= c + 1; x++)
                  g->dirty[y][x >> 3] |= (uint8_t)(1u << (x & 7));
   }
   /* Where rocks were, and are. */
   find_rocks(g, s, kinds);
   for (unsigned k = 0; k < g->was_rock_count + g->rock_count; k++)
   {
      const bool was = k < g->was_rock_count;
      const unsigned j = was ? k : k - g->was_rock_count;
      const int x0 = was ? g->was_rocks[j].x0 : g->rocks[j].x0, x1 = was ? g->was_rocks[j].x1 : g->rocks[j].x1;
      const int y0 = was ? g->was_rocks[j].y0 : g->rocks[j].y0, y1 = was ? g->was_rocks[j].y1 : g->rocks[j].y1;
      for (int y = y0; y < y1; y++)
         for (int x = x0; x < x1; x++)
            g->dirty[y][x >> 3] |= (uint8_t)(1u << (x & 7));
   }
   for (unsigned r = SKY_TOP; r < EARTH_END; r++)
   {
      for (unsigned c = FIELD_LEFT - 1; c <= FIELD_RIGHT; c++)
         if (g->dirty[r][c >> 3] & (1u << (c & 7)))
         {
            paint_cell(g, s, r, c);
            g->was[r][c] = g->cell[r][c];
         }
      g->was_color[r] = g->row_color[r];
   }
   g->painted = true;

   /* The rocks, painted over the earth; the game's blocks are not drawn. */
   for (unsigned i = 0; i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (kinds[i] != KIND_ROCK && kinds[i] != KIND_LOOSE)
         continue;
      paint_boulder(g, s, in);
      for (unsigned r = 0; r < in->h; r++)
         for (unsigned b = 0; b < in->w && b < 32; b++)
         {
            const int x = in->x + (int)b, y = in->y + (int)r;
            size_t i2;
            if (!((s->objects->bits[in->rows + r] >> b) & 1) || x < 0 || x >= PXC_W || y < 0
                  || y >= (int)s->frame->height)
               continue;
            i2 = (size_t)y * PXC_W + (size_t)x;
            if (PX_KEY_CLS(s->top[i2]) == PX_CLS_SPRITE)
               s->top[i2] = PX_KEY(PX_CLS_BK, 0);
            s->sprite[i2] = 0;
            s->bk[i2] = 0;
         }
   }
   memcpy(g->was_rocks, g->rocks, sizeof(g->rocks));
   g->was_rock_count = g->rock_count;

   /* What is painted shows where the game has background that is dark. */
   for (unsigned r = SKY_TOP; r < EARTH_END; r++)
      for (unsigned c = FIELD_LEFT; c < FIELD_RIGHT; c++)
      {
         const size_t i = (size_t)r * PXC_W + c;
         const unsigned cls = PX_KEY_CLS(s->top[i]);
         if (cls == PX_CLS_PF || cls == PX_CLS_BK)
         {
            s->top[i] = PX_KEY(PX_CLS_BK, 0);
            s->bk[i]  = 0;
         }
      }
   s->backdrop_on = true;
}

/* How much was dug since the frame before, and where. */
static void count_dug(dd *g)
{
   unsigned n = 0;
   long sx = 0, sy = 0;
   for (unsigned r = EARTH_TOP; r < EARTH_END; r++)
      for (unsigned c = FIELD_LEFT; c < FIELD_RIGHT; c++)
         if (g->cell[r][c] == CELL_TUNNEL && g->last[r][c] == CELL_DIRT)
         {
            n++;
            sx += c;
            sy += r;
         }
   /* A whole new field is a new round, not digging. */
   g->dug = n < 60 ? n : 0;
   if (g->dug)
   {
      g->dig_x = (int)(sx / (long)n);
      g->dig_y = (int)(sy / (long)n);
   }
}

/* ---------------------------------------------------------------------------
 * The picture: the objects
 * ------------------------------------------------------------------------- */

static bool in_field(const px_instance *in)
{
   return in->y + (int)in->h > SKY_TOP && in->y < HUD_TOP;
}

/* Of a colour of the earth's: oranges and reds, not light. */
static bool earth_colored(unsigned color)
{
   const unsigned hue = color >> 4;
   return hue >= 2 && hue <= 4;
}

static unsigned kind_of(const px_instance *in, const px_objects *o)
{
   if (in->y >= HUD_TOP)
      return KIND_HUD;
   if (!in_field(in))
      return KIND_NONE;
   if (in->cls == PXC_L_BL)
      return KIND_ROCK;
   if (!px_kit_is_player(in))
      return KIND_NONE;
   switch (in->color)
   {
      case COLOR_DIG:
         if (in->h <= 4)
            return KIND_HOSE;
         return KIND_DIG;
      case COLOR_DIG2:   return in->h <= 5 ? KIND_DIG_PART : KIND_NONE;
      case COLOR_POOKA:  return in->w >= 16 ? KIND_PUMPED : KIND_POOKA;
      case COLOR_GHOST:  return KIND_GHOST;
      case COLOR_FYGAR:  return in->w >= 16 ? KIND_PUMPED : KIND_FYGAR;
      case COLOR_FGHOST: return KIND_FGHOST;
      default: break;
   }
   if (!earth_colored(in->color))
      return KIND_NONE;
   /* Fire comes out of a Fygar on its row; what else is of the earth's colours is a rock
    * that shakes, falls or crumbles. */
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *f = &o->inst[i];
      if (f->color == COLOR_FYGAR && px_kit_is_player(f) && f->y - in->y <= 4 && in->y - f->y <= 4
            && in->x + (int)in->w >= f->x - 4 && in->x <= f->x + (int)f->w + 4)
         return KIND_FIRE;
   }
   if (in->w >= 32)
      return KIND_FIRE;
   return in->w == 8 && in->h <= 8 ? KIND_LOOSE : KIND_NONE;
}

static unsigned role_of(unsigned kind)
{
   switch (kind)
   {
      case KIND_DIG: case KIND_DIG_PART:                  return PX_ROLE_PLAYER;
      case KIND_HOSE:                                     return PX_ROLE_SHOT;
      case KIND_POOKA: case KIND_GHOST: case KIND_FYGAR:
      case KIND_FGHOST: case KIND_PUMPED:                 return PX_ROLE_ENEMY;
      case KIND_FIRE:                                     return PX_ROLE_BOMB;
      case KIND_HUD:                                      return PX_ROLE_HUD;
      default:                                            return PX_ROLE_NONE;
   }
}

#define RGB_DIG     0xF4F6FF
#define RGB_DIG2    0x3C6CF8
#define RGB_HOSE    0xD8F0FF
#define RGB_POOKA   0xF03424
#define RGB_GOGGLES 0xFFE860
#define RGB_FYGAR   0x3CCC48
#define RGB_EYE     0xFFFFFF
#define RGB_ROCK    0x857868
#define RGB_SCORE   0xFFE0A0

/* Gives rows `from` up to `to` of an object a colour. */
static void tint_rows(px_scene *s, const px_instance *in, unsigned from, unsigned to, uint32_t rgb)
{
   const px_objects *o = s->objects;
   for (unsigned r = from; r < to && r < in->h; r++)
   {
      const int y = in->y + (int)r;
      const uint32_t bits = o->bits[in->rows + r];
      if (y < 0 || y >= (int)s->frame->height)
         continue;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         const int x = in->x + (int)b;
         size_t i;
         if (!((bits >> b) & 1) || x < 0 || x >= PXC_W)
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         if (s->sprite[i])
            s->sprite[i] = 0xFF000000u | rgb;
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE)
            s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
      }
   }
}

/* The light of Fygar's fire on the tunnel around it. */
static void fire_light(px_scene *s, const px_instance *in, uint32_t rgb)
{
   const int x0 = in->x - 8, x1 = in->x + (int)in->w + 8, y0 = in->y - 5, y1 = in->y + (int)in->h + 5;
   const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
   for (int y = y0 < 0 ? 0 : y0; y < y1 && y < (int)s->frame->height; y++)
      for (int x = x0 < 0 ? 0 : x0; x < x1 && x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + (size_t)x;
         const int dx = (x - cx) * 256 / (rx ? rx : 1), dy = (y - cy) * 256 / (ry ? ry : 1);
         const int d2 = (dx * dx + dy * dy) >> 8;
         if (d2 >= 256 || PX_KEY_CLS(s->top[i]) != PX_CLS_BK)
            continue;
         s->light[i] = 0xFF000000u | px_rgb_scale(rgb, (unsigned)(256 - d2) * 3 / 4);
      }
}

static void color_things(dd *g, px_scene *s, const uint8_t *kinds)
{
   px_objects *o = s->objects;

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      const unsigned wave = px_kit_wave(g->frame * 9 + (unsigned)in->x * 5);
      switch (kinds[i])
      {
         case KIND_DIG:
            px_scene_tint(s, in, RGB_DIG);
            /* The helmet's visor and the boots in blue. */
            tint_rows(s, in, 0, 2, 0xB8D0FF);
            if (in->h > 6)
               tint_rows(s, in, in->h - 2, in->h, RGB_DIG2);
            break;
         case KIND_DIG_PART:
            px_scene_tint(s, in, RGB_DIG2);
            break;
         case KIND_HOSE:
            px_scene_tint(s, in, RGB_HOSE);
            px_scene_energy(s, in, true);
            break;
         case KIND_POOKA:
            px_scene_tint(s, in, RGB_POOKA);
            px_kit_fill_holes(s, in, RGB_GOGGLES);
            break;
         case KIND_GHOST:
         case KIND_FGHOST:
            /* Only goggles go through the earth, and blink. */
            px_scene_tint(s, in, px_rgb_scale(kinds[i] == KIND_GHOST ? RGB_GOGGLES : 0xC8F090,
                  150 + wave / 3));
            break;
         case KIND_FYGAR:
            px_scene_tint(s, in, RGB_FYGAR);
            px_kit_fill_holes(s, in, RGB_EYE);
            break;
         case KIND_PUMPED:
         {
            /* Swelling: paler and glowing, and throbbing. */
            const uint32_t own = in->color == COLOR_FYGAR ? RGB_FYGAR : RGB_POOKA;
            px_scene_tint(s, in, px_rgb_mix(own, 0xFFE0D0, 60 + wave / 3));
            px_kit_fill_holes(s, in, RGB_GOGGLES);
            px_scene_energy(s, in, true);
            break;
         }
         case KIND_FIRE:
            px_scene_tint(s, in, px_rgb_mix(0xFFD040, 0xFF5018, px_kit_wave(g->frame * 23)));
            px_scene_energy(s, in, true);
            break;
         case KIND_ROCK:
         case KIND_LOOSE:
            px_scene_tint(s, in, RGB_ROCK);
            break;
         case KIND_HUD:
            if (in->cls != PXC_L_BL)
               px_scene_tint(s, in, RGB_SCORE);
            break;
         default:
            break;
      }
   }
   /* The lives: squares of playfield. The game hides the rest of its playfield on these
    * rows in black, which stays black. */
   for (size_t i = (size_t)HUD_TOP * PXC_W; i < (size_t)s->frame->height * PXC_W; i++)
      if (PX_KEY_CLS(s->top[i]) == PX_CLS_PF && (s->top[i] & 0xE0E0E0u))
         s->top[i] = PX_KEY(PX_CLS_PF, RGB_DIG);
}

/* What happened since the frame before. */
static void find_events(dd *g, px_scene *s, const uint8_t *kinds)
{
   px_objects *o = s->objects;
   bool pumped = false;
   int loose_x = -1, loose_y = -1;

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      if (in->ghost)
         continue;
      switch (kinds[i])
      {
         case KIND_PUMPED:
            pumped = true;
            g->pump_x = in->x + (int)in->w / 2;
            g->pump_y = in->y + (int)in->h / 2;
            g->pump_rgb = in->color == COLOR_FYGAR ? RGB_FYGAR : RGB_POOKA;
            break;
         case KIND_DIG:
            g->dig_px = in->x + 4;
            g->dig_py = in->y + (int)in->h / 2;
            break;
         case KIND_LOOSE:
            if (loose_y < 0 || in->y > loose_y)
            {
               loose_x = in->x + 4;
               loose_y = in->y + (int)in->h;
            }
            break;
         default:
            break;
      }
   }
   /* Dig Dug caught: he takes shapes that differ with how he was caught, so the sound, which
    * is always the same, tells it. */
   if (g->caught_heard && g->sparks && g->dig_px >= 0)
   {
      px_scene_flash(s, 0xFF3020, 90);
      px_scene_burst(s, g->dig_px, g->dig_py, RGB_DIG, 30, 300);
      px_scene_burst(s, g->dig_px, g->dig_py, RGB_DIG2, 16, 220);
   }
   g->caught_heard = false;

   /* An enemy pumped until it bursts: the sound hears it, or it is gone and does not come
    * back where it was. */
   if (pumped)
   {
      if (g->pump_away > 30)
         g->pump_burst = false;
      g->pump_away = 0;
   }
   else if (g->pump_x >= 0)
      g->pump_away++;
   if (g->pump_x >= 0 && !g->pump_burst && g->pump_away <= 12
         && (g->pop_heard || g->pump_away == 12))
   {
      bool back = false;
      for (unsigned i = 0; i < o->count && !g->pop_heard; i++)
      {
         const px_instance *in = &o->inst[i];
         const int dx = in->x + (int)in->w / 2 - g->pump_x, dy = in->y + (int)in->h / 2 - g->pump_y;
         if ((kinds[i] == KIND_POOKA || kinds[i] == KIND_FYGAR) && dx * dx + dy * dy < 100)
            back = true;
      }
      if (!back && g->sparks)
      {
         px_scene_burst(s, g->pump_x, g->pump_y, g->pump_rgb, 30, 380);
         px_scene_burst(s, g->pump_x, g->pump_y, 0xFFF4D0, 16, 260);
         px_scene_flash(s, 0xFFF0E0, 36);
      }
      g->pump_burst = true;
   }
   if (g->pump_away > 60)
      g->pump_x = -1;
   g->pop_heard = 0;

   /* A rock that shakes loose, falls, and crashes. */
   if (loose_x >= 0)
   {
      if (g->rock_x < 0 || loose_x != g->rock_x)
      {
         if (g->sparks)
            px_scene_burst(s, loose_x, loose_y, 0x9C7A58, 8, 90);
         g->rock_fell = 0;
         g->rock_still = 0;
         g->rock_crashed = false;
      }
      else if (loose_y > g->rock_y)
      {
         g->rock_fell += loose_y - g->rock_y;
         g->rock_still = 0;
      }
      else if (g->rock_fell >= 8 && !g->rock_crashed && (g->crash_heard || ++g->rock_still == 10))
      {
         if (g->sparks)
         {
            px_scene_burst(s, loose_x, loose_y, 0xB09070, 28, 220);
            px_scene_burst(s, loose_x, loose_y, RGB_ROCK, 14, 160);
         }
         g->rock_crashed = true;
      }
      g->rock_x = loose_x;
      g->rock_y = loose_y;
      g->rock_away = 0;
   }
   else if (g->rock_x >= 0 && ++g->rock_away > 30)
      g->rock_x = -1;
   g->crash_heard = false;

   /* Crumbs where Dig Dug digs. */
   g->since_crumbs++;
   if (g->dug && g->sparks && g->since_crumbs >= 8)
   {
      const uint32_t *palette = s->frame->palette;
      px_scene_burst(s, g->dig_x, g->dig_y, px_rgb_scale(palette[g->row_color[g->dig_y]] & 0xFFFFFFu, 200),
            3, 70);
      g->since_crumbs = 0;
   }
}

static void frame(void *state, px_scene *s)
{
   dd *g = (dd*)state;
   px_objects *o = s->objects;
   uint8_t kinds[PX_MAX_INSTANCES];

   if (s->advance)
      g->frame++;

   g->dig_at = g->fire_at = -1;
   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      kinds[i]  = (uint8_t)kind_of(in, o);
      in->role  = (uint8_t)role_of(kinds[i]);
      in->group = kinds[i];
   }
   if (g->colors != COLORS_ORIGINAL)
      color_things(g, s, kinds);
   for (unsigned i = 0; i < o->count; i++)
      if (kinds[i] == KIND_FIRE && g->colors != COLORS_ORIGINAL)
         fire_light(s, &o->inst[i], 0xFF8030);

   g->earth = find_earth(g, s);
   if (s->advance)
   {
      g->dug = 0;
      if (g->earth)
      {
         count_dug(g);
         memcpy(g->last, g->cell, sizeof(g->last));
      }
      else
         memset(g->last, CELL_UNSEEN, sizeof(g->last));
   }
   if (g->backdrop && g->earth)
      paint_earth(g, s, kinds);
   else
      g->painted = false;

   if (s->advance)
      find_events(g, s, kinds);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game's, as its registers have them at the end of a frame:
 *
 *   both      the tune at the start of a round: voice 0 waveform 4 and voice 1 waveform
 *             13, volume 4, notes of 5 to 30 frames
 *   both      the tune while Dig Dug moves: voice 0 waveform 4 (notes of 3 and 9 frames)
 *             and voice 1 waveform 13 (9 frames), volume 2
 *   voice 0   the hose thrown: waveform 4, volume 3, pitch 11, 8, 6 over and over, 3
 *             frames each, 27 frames
 *   voice 0   a stroke of the pump: waveform 4, volume 10, the pitch from 26 down to 19, a
 *             frame in two; then volume 4 and the pitch from 18 up through 23 to 28
 *   voice 0   an enemy bursts: after a stroke, waveform 4 at volume 4, the pitch 31 and 30
 *             in turns for 8 frames, then about 15 for 12
 *   voice 0   Fygar's fire: waveform 7, volume 3, the pitch from 16 down to 13; with it
 *   voice 1   waveform 4 at pitch 10, volume 3, two frames in four
 *   voice 0   a rock shakes loose: waveform 13 at volume 7, the pitch 30 and 28 in turns,
 *             12 frames; then waveform 4 at volume 7, pitch 20, 21, 19, 20, 12 frames
 *   voice 0   a rock crashes: waveform 13, volume 3, pitch 20, 18, 20, 19, 15, 14, 15, 14
 *   both      Dig Dug caught: voice 0 waveform 4 at volume 4, pitch 15, 18, 21, 25; voice 1
 *             waveform 13 at volume 7, pitch 14, 17, 20, 24; 20 frames
 *   both      his end: waveform 13 on both, the pitch climbing from 13 to 19 while the
 *             volume falls from 6 to 2 (voice 1 from 5 to 1), 64 frames
 *   voice 0   while Dig Dug stands still: waveform 4 at volume 3, pitch 15 and 14 in turns
 *             for 24 frames, then 31 and 30; left as the game plays it
 *
 * The tunes are the game's: their notes are played again in softer voices, an octave down.
 * The game has no sound for digging; the crunch of it is Proteus's, with an option.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_TUNE, SOUND_HOSE, SOUND_STROKE, SOUND_POP, SOUND_FIRE, SOUND_LOOSE,
   SOUND_CRASH, SOUND_CAUGHT, SOUND_END, SOUND_OTHER };

static unsigned voice0_plays(const dd *g)
{
   const px_kit_tia *t = &g->tia;
   const unsigned w = t->wave[0], p = t->pitch[0], v = t->volume[0];
   if (!v)
      return SOUND_NONE;
   if (w == 13 && g->since_end < 70)
      return SOUND_END;
   if (w == 4 && v == 10)
      return SOUND_STROKE;
   if (w == 4 && v == 4 && g->since_pop < 22)
      return SOUND_POP;
   if (w == 4 && v == 4 && g->since_stroke <= 12)
      return (p == 30 || p == 31) ? SOUND_POP : SOUND_STROKE;
   if (w == 4 && (v == 2 || v == 4))
      return SOUND_TUNE;
   if (w == 4 && v == 3 && (p == 11 || p == 8 || p == 6))
      return SOUND_HOSE;
   if (w == 7)
      return SOUND_FIRE;
   if ((w == 13 || w == 4) && v == 7)
      return SOUND_LOOSE;
   if (w == 13 && v == 3)
      return SOUND_CRASH;
   return SOUND_OTHER;
}

static unsigned voice1_plays(const dd *g)
{
   const px_kit_tia *t = &g->tia;
   const unsigned w = t->wave[1], p = t->pitch[1], v = t->volume[1];
   if (!v)
      return SOUND_NONE;
   if (w == 13 && g->since_end < 70)
      return SOUND_END;
   if (w == 13 && v == 7)
      return SOUND_CAUGHT;
   if (w == 13 && (v == 2 || v == 4))
      return SOUND_TUNE;
   if (w == 4 && p == 10 && v == 3)
      return SOUND_FIRE;
   return SOUND_OTHER;
}

/* A note of the game's tunes, an octave down: a soft lead for voice 0, a bass for voice 1. */
static void play_note(px_sound *s, unsigned voice, unsigned wave, unsigned pitch, float gain)
{
   px_tone lead[2] = {
      /* wave            freq  to  glide  attack  hold   decay  gain   cutoff to    vibrato */
      { PX_WAVE_SQUARE,   0,   0,  0,     0.004f, 0.05f, 0.22f, 0.20f, 2400, 900, 5.5f, 0.003f },
      { PX_WAVE_TRIANGLE, 0,   0,  0,     0.004f, 0.06f, 0.26f, 0.45f, 0,    0,   0,    0 }
   };
   px_tone bass[2] = {
      { PX_WAVE_TRIANGLE, 0,   0,  0,     0.006f, 0.08f, 0.26f, 0.60f, 0,    0,   0,    0 },
      { PX_WAVE_SAW,      0,   0,  0,     0.006f, 0.05f, 0.18f, 0.14f, 800,  300, 0,    0 }
   };
   const float f = px_kit_tune(px_kit_tia_hz(wave, pitch) / 2.0f);
   if (f <= 20.0f)
      return;
   if (voice == 0)
   {
      lead[0].freq = f;
      lead[1].freq = f;
      lead[0].gain *= gain;
      lead[1].gain *= gain;
      px_kit_play(s, lead, 2, 0.15f);
   }
   else
   {
      bass[0].freq = f;
      bass[1].freq = f;
      bass[0].gain *= gain;
      bass[1].gain *= gain;
      px_kit_play(s, bass, 2, -0.15f);
   }
}

static void play_hose(const dd *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave          freq   to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_NOISE,  5000,    0, 0,     0.01f,  0.04f, 0.16f, 0.22f, 5000, 1200, 0, 0 },
      { PX_WAVE_TRIANGLE, 900, 380, 0.16f, 0.002f, 0.04f, 0.18f, 0.30f, 0, 0, 16.0f, 0.02f },
      { PX_WAVE_SQUARE,   450, 190, 0.16f, 0.002f, 0.02f, 0.14f, 0.08f, 1800, 500, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->dig_at));
   px_sound_rumble(s, 0, 9000, 3);
}

static void play_stroke(const dd *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* air pushed in, and the enemy's skin stretching */
      { PX_WAVE_NOISE,  1200, 3800, 0.14f, 0.01f,  0.05f, 0.14f, 0.34f, 1500, 4200, 0, 0 },
      { PX_WAVE_SAW,     110,  230, 0.20f, 0.004f, 0.08f, 0.16f, 0.26f, 1400, 2200, 7.0f, 0.02f },
      { PX_WAVE_SINE,     70,   55, 0.10f, 0.002f, 0.03f, 0.10f, 0.50f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->pump_x >= 0 ? g->pump_x : g->dig_at));
   px_sound_rumble(s, 12000, 8000, 4);
}

static void play_pop(const dd *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE,  9000, 700, 0.22f, 0,      0.02f, 0.28f, 0.50f, 9000, 600, 0, 0 },
      { PX_WAVE_SINE,    190,  45, 0.16f, 0.001f, 0.02f, 0.24f, 0.70f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE, 1300, 180, 0.14f, 0.001f, 0.01f, 0.18f, 0.20f, 5000, 700, 0, 0 },
      { PX_WAVE_NOISE,  3000,   0, 0,     0.05f,  0.05f, 0.40f, 0.14f, 1800, 400, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->pump_x >= 0 ? g->pump_x : g->dig_at));
   px_sound_rumble(s, 30000, 40000, 10);
}

static void play_fire(const dd *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* the roar, the breath behind it, and crackles */
      { PX_WAVE_NOISE,   900, 500, 0.40f, 0.03f, 0.20f, 0.40f, 0.42f, 1300, 500, 0, 0 },
      { PX_WAVE_SAW,      80,  55, 0.40f, 0.03f, 0.18f, 0.30f, 0.26f, 500, 250, 9.0f, 0.06f },
      { PX_WAVE_NOISE,  7000,   0, 0,     0.01f, 0.25f, 0.20f, 0.14f, 7000, 2500, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->fire_at));
   px_sound_rumble(s, 0, 14000, 10);
}

static void play_loose(const dd *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* stone grinding against the earth */
      { PX_WAVE_NOISE,   500, 300, 0.40f, 0.02f, 0.25f, 0.30f, 0.50f, 600, 300, 0, 0 },
      { PX_WAVE_SAW,      52,  46, 0.40f, 0.02f, 0.25f, 0.25f, 0.30f, 300, 200, 13.0f, 0.08f },
      { PX_WAVE_NOISE,  2500,   0, 0,     0.01f, 0.30f, 0.15f, 0.08f, 2500, 900, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->rock_x));
   px_sound_rumble(s, 9000, 12000, 20);
}

static void play_crash(const dd *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE,  3500, 200, 0.50f, 0,      0.04f, 0.60f, 0.50f, 3500, 150, 0, 0 },
      { PX_WAVE_SINE,     75,  30, 0.40f, 0.001f, 0.05f, 0.50f, 0.70f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE,  160,  50, 0.30f, 0.001f, 0.02f, 0.30f, 0.20f, 900, 150, 0, 0 },
      { PX_WAVE_NOISE,  1500, 500, 0.60f, 0.10f,  0.10f, 0.80f, 0.16f, 900, 200, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->rock_x));
   px_sound_rumble(s, 50000, 40000, 16);
}

static void play_caught(const dd *g, px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_SQUARE, 1500, 160, 0.30f, 0.001f, 0.02f, 0.30f, 0.14f, 4000, 500, 0, 0 },
      { PX_WAVE_NOISE,  6000, 800, 0.20f, 0,      0.02f, 0.25f, 0.20f, 6000, 600, 0, 0 }
   };
   px_kit_play(s, p, 2, px_kit_pan(g->dig_at));
   px_sound_rumble(s, 65535, 40000, 30);
}

static void play_dig(const dd *g, px_sound *s)
{
   px_tone p[2] = {
      /* earth breaking, low; and grit */
      { PX_WAVE_NOISE,  900, 300, 0.07f, 0.004f, 0.02f, 0.08f, 0.30f, 1100, 350, 0, 0 },
      { PX_WAVE_NOISE, 4000,   0, 0,     0.002f, 0.01f, 0.05f, 0.08f, 4000, 1500, 0, 0 }
   };
   /* Not twice alike. */
   p[0].freq += (float)((g->frame * 37u) % 300u);
   px_kit_play(s, p, 2, px_kit_pan(g->dig_x >= 0 ? g->dig_x : g->dig_at));
}

static void sound(void *state, px_sound *s)
{
   dd *g = (dd*)state;
   unsigned heard0, heard1;

   /* Where things are, from the picture before. */
   g->dig_at = g->fire_at = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->ghost)
         continue;
      if (in->role == PX_ROLE_PLAYER && in->h > 6)
         g->dig_at = in->x + 4;
      else if (in->role == PX_ROLE_BOMB)
         g->fire_at = in->x + (int)in->w / 2;
   }

   px_kit_tia_hear(&g->tia, s->frame);
   g->since_stroke++;
   g->since_pop++;
   g->since_dig_sound++;
   g->since_rock++;
   g->since_end++;
   /* Dig Dug's end begins on both voices at once, waveform 13, at volumes 6 and 5. */
   if (g->tia.wave[0] == 13 && g->tia.wave[1] == 13 && g->tia.volume[0] == 6 && g->tia.volume[1] == 5
         && g->since_end >= 70)
      g->since_end = 0;
   heard0 = voice0_plays(g);
   heard1 = voice1_plays(g);

   switch (heard0)
   {
      case SOUND_STROKE:
         /* A stroke begins loud at the top of its pitches. */
         if (g->tia.volume[0] == 10 && g->since_stroke > 3 && g->tia.pitch[0] >= 24)
         {
            if (g->own_sound) play_stroke(g, s);
            else              px_sound_rumble(s, 12000, 8000, 4);
         }
         if (g->tia.volume[0] == 10)
            g->since_stroke = 0;
         break;
      case SOUND_POP:
         if (g->heard[0] != SOUND_POP)
         {
            g->since_pop = 0;
            g->pop_heard = 1;
            if (g->own_sound) play_pop(g, s);
            else              px_sound_rumble(s, 30000, 40000, 10);
         }
         break;
      case SOUND_HOSE:
         if (g->heard[0] != SOUND_HOSE)
         {
            if (g->own_sound) play_hose(g, s);
            else              px_sound_rumble(s, 0, 9000, 3);
         }
         break;
      case SOUND_FIRE:
         if (g->heard[0] != SOUND_FIRE)
         {
            if (g->own_sound) play_fire(g, s);
            else              px_sound_rumble(s, 0, 14000, 10);
         }
         break;
      case SOUND_LOOSE:
         if (g->heard[0] != SOUND_LOOSE && g->since_rock > 30)
         {
            if (g->own_sound) play_loose(g, s);
            else              px_sound_rumble(s, 9000, 12000, 20);
            g->since_rock = 0;
         }
         break;
      case SOUND_CRASH:
         if (g->heard[0] != SOUND_CRASH)
         {
            g->crash_heard = true;
            if (g->own_sound) play_crash(g, s);
            else              px_sound_rumble(s, 50000, 40000, 16);
         }
         break;
      case SOUND_TUNE:
      case SOUND_END:
         if (g->own_sound && (g->heard[0] != heard0 || px_kit_tia_louder(&g->tia, 0)
               || g->tia.pitch[0] != g->tia.was_pitch[0] || g->tia.wave[0] != g->tia.was_wave[0]))
            play_note(s, 0, g->tia.wave[0], g->tia.pitch[0],
                  heard0 == SOUND_END ? 0.25f + 0.08f * g->tia.volume[0]
                  : g->tia.volume[0] <= 2 ? 0.55f : 0.9f);
         break;
      default:
         break;
   }

   if (heard1 == SOUND_CAUGHT && g->heard[1] != SOUND_CAUGHT)
   {
      g->caught_heard = true;
      if (g->own_sound) play_caught(g, s);
      else              px_sound_rumble(s, 65535, 40000, 30);
   }
   switch (heard1)
   {
      /* The notes of the game's tunes, and of the jingle when he is caught. */
      case SOUND_CAUGHT:
      case SOUND_TUNE:
      case SOUND_END:
         if (g->own_sound && (g->heard[1] != heard1 || px_kit_tia_louder(&g->tia, 1)
               || g->tia.pitch[1] != g->tia.was_pitch[1] || g->tia.wave[1] != g->tia.was_wave[1]))
            play_note(s, 1, g->tia.wave[1], g->tia.pitch[1],
                  heard1 == SOUND_END ? 0.25f + 0.08f * g->tia.volume[1]
                  : g->tia.volume[1] <= 2 ? 0.55f : 0.9f);
         if (heard1 == SOUND_END && g->heard[1] != SOUND_END)
            px_sound_rumble(s, 20000, 20000, 20);
         break;
      default:
         break;
   }
   g->heard[0] = heard0;
   g->heard[1] = heard1;

   /* The crunch of digging, where the picture saw earth dug. */
   if (g->own_sound && g->digging && g->dug && g->since_dig_sound >= 7)
   {
      play_dig(g, s);
      g->since_dig_sound = 0;
   }

   if (g->own_sound)
   {
      if (heard0 != SOUND_OTHER)
         s->voice[0] = 0.0f;
      if (heard1 != SOUND_OTHER)
         s->voice[1] = 0.0f;
   }
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   dd *g = (dd*)state;
   px_kit_tia_reset(&g->tia);
   g->heard[0] = g->heard[1] = SOUND_NONE;
   g->since_stroke = g->since_pop = g->since_dig_sound = g->since_rock = g->since_end = 1000;
   g->painted = false;
   g->earth   = false;
   memset(g->was, CELL_UNSEEN, sizeof(g->was));
   memset(g->last, CELL_UNSEEN, sizeof(g->last));
   g->dug = 0;
   g->dig_x = g->dig_y = -1;
   g->since_crumbs = 0;
   g->pump_x = g->pump_y = -1;
   g->pump_rgb = RGB_POOKA;
   g->pump_away = 1000;
   g->pump_burst = true;
   g->pop_heard = 0;
   g->rock_x = g->rock_y = -1;
   g->rock_fell = 0;
   g->rock_still = 0;
   g->rock_crashed = true;
   g->rock_away = 0;
   g->dig_px = g->dig_py = -1;
   g->caught_heard = g->crash_heard = false;
   g->rock_count = g->was_rock_count = 0;
   g->dig_at = g->fire_at = -1;
}

static void *create(void)
{
   dd *g = (dd*)calloc(1, sizeof(dd));
   if (g)
   {
      g->tables = (uint32_t (*)[256])calloc(256, sizeof(*g->tables));
      g->backdrop = g->sparks = g->own_sound = g->digging = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   dd *g = (dd*)state;
   if (g)
   {
      px_kit_texture_free(&g->soil);
      px_kit_texture_free(&g->edge);
      free(g->tables);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   dd *g = (dd*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->digging   = px_kit_on(get, OPT_DIGGING);
   g->painted   = false;
}

const px_game px_game_dig_dug = {
   "Dig Dug", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
