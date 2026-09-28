/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Solaris (Atari, 1986, by Doug Neubauer).
 *
 * A starfighter in a galaxy of quadrants. The game begins on a planet; the button held down
 * takes off into space, where Zylon ships attack in waves. The stick pulled back with the
 * button opens the galactic map (SCANNER): an X is moved over a sector and the button warps
 * there. The warp is a blue hyperspace that brightens, a red gate rushing past, and a
 * flicker of arrival. Some sectors hold planets, which the ship lands on after the warp;
 * a planet under attack flashes its ground, and red space is a danger zone. Hits and lost
 * ships flash the whole view.
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows   0..13   blank
 *   rows  14..180  the view. Its background is one colour a row:
 *                  space        all of it one colour: black ($00), red in a danger zone
 *                               ($40, $42), blue in the warp ($80 brightening to $8E), white
 *                               or light blue ($0E, $8C) for a frame when something hits,
 *                               black, blue ($70) and purple ($52) in turns on arrival
 *                  the gate     at the end of the warp: rows of pink and red ($44..$4E,
 *                               $58..$5E) for its ceiling and floor, cut into a trapezoid by
 *                               black playfield, the ball and missile 0 at its edges
 *                  a planet     a sky ($70) down to row 93, mountains of black playfield in
 *                               rows 88..93, two black rows, then ground ($62 on the first
 *                               planet) with black rows across it that move down: the
 *                               ground's lines coming nearer. A planet in danger turns its
 *                               ground red ($40, $42), blue ($80) and light blue ($8C) in
 *                               turns, a frame each.
 *                  the map      background $80 (blue) or of another quadrant's colour; a
 *                               grid of playfield ($AC) on rows 53..157, its columns the two
 *                               missiles; the sectors' signs, SCANNER and JUMP:nn players
 *   rows  16..22   the score: player 1, three copies
 *   stars          the ball, a pixel, colours $F2..$FE, set again on many rows
 *   enemies        player 0, several a frame in turns; the planets that fly by in space
 *                  (red $46, blue $82, $84) are player 0 too
 *   the ship       player 1 at rows 150..172; its torpedo missile 1 ($78)
 *   a planet's     ringed planet in the sky: player 1 (the disk), player 0 and missile 0
 *                  (the rings); craters on the ground: player 0, black
 *   rows 181..207  the cockpit: background $F2 with the scanner's panel in its middle, the
 *                  ships left and FUEL. On the title the copyright, on black.
 *
 * Every kind of screen is told from the picture, afresh in every frame: the map by its
 * grid, a planet by a sky over ground, space by one colour over the view. A frame that is
 * none of these (it has not been seen to happen) is taken for the kind of the frames
 * before it for a while, so that the backdrop does not come and go.
 *
 * The cockpit and the score are the game's display and stay its own to the pixel: they are
 * made blank for the effects, which draw blank as it is, with no objects fused from the
 * frames before, no glow, shade, light or sparks.
 *
 * Solaris does not play the same way twice: what it does depends on how long frames take
 * to emulate, as Pitfall II's does. A run from a saved state stays the same for a while.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_sl_colors"
#define OPT_BACKDROP "proteus_sl_backdrop"
#define OPT_SPARKS   "proteus_sl_sparks"
#define OPT_SOUND    "proteus_sl_sound"

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define TOP         14      /* the view's first row */
#define PANEL       181     /* the cockpit's first row */
#define SCORE_END   24      /* the score is above this */
#define SHIP_TOP    140     /* the ship is below this */
#define MOUNTAINS   8       /* rows of mountains above the horizon, at most */
#define HOLD        16      /* frames an unknown picture is taken for the kind before it */

#define STARS       150     /* of the rushing starfield */
#define BLOOMS      8       /* glows of explosions on the backdrop at once */
#define HALOS       8       /* planets that glow, in a frame */

enum { SCREEN_NONE = 0, SCREEN_SPACE, SCREEN_PLANET, SCREEN_MAP };

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "e72eb8d4410152bdcb69e7fba327b420",   /* Solaris (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "60",
   "reverb", "room",
   NULL
};

static const char *const colors[] = { "vivid", "Vivid", "original", "The game's own", NULL };
static const char *const backdrop[] = { "space", "Nebulae and planets", "stars", "Stars alone",
   "off", "Off", NULL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Enemies and the planets that fly by in deeper, brighter colours that glow, and a torpedo of white light; or the game's own colours.",
     "vivid", colors },
   { OPT_BACKDROP, "Backdrop",
     "Deep space of nebulae with stars that rush past, a hyperspace of streaks in the warp, a sky and a glowing ground on planets and a galaxy behind the map; the rushing stars alone; or the game's flat colours.",
     "space", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks and a burst of light where an enemy explodes and where the ship is hit.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the torpedo, the explosions, the ship hit and the engine's roar, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { NULL, NULL, NULL, NULL, NULL }
};

/* In the order of the options' values. */
enum { COLORS_VIVID = 0, COLORS_ORIGINAL };
enum { BACKDROP_SPACE = 0, BACKDROP_STARS, BACKDROP_OFF };

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

/* What a frame shows. */
typedef struct
{
   unsigned kind;            /* SCREEN_* */
   uint8_t fill;             /* SPACE, MAP: the colour of the view's background */
   uint8_t sky, ground;      /* PLANET */
   unsigned horizon;         /* PLANET: the first row below the sky */
   unsigned bottom;          /* the row after the view's last */
} view;

typedef struct
{
   float x, y, z;            /* in space: x and y across, z far (1) to near (0) */
   int px, py;               /* where it was drawn, in the picture */
   bool drawn;
   uint8_t hue;
} star;

typedef struct
{
   int16_t x, y;             /* captured pixels */
   uint16_t radius;          /* captured pixels */
   uint16_t strength;        /* of 256 */
   uint32_t rgb;
} bloom;

typedef struct
{
   /* The options. */
   unsigned colors, backdrop;
   bool sparks, own_sound;

   /* The picture. */
   uint32_t frame;                 /* counts the frames that advance */
   view was;                       /* the last frame of a kind known */
   unsigned held;                  /* frames it stood in for one of no kind */
   uint8_t row[PXC_MAX_H];         /* the background's colour of every row */
   uint8_t gate[PXC_MAX_H];        /* 1: the row is the gate's */
   uint32_t mood;                  /* the colour space is lit by, following the game's */
   unsigned mood_kind;
   uint32_t vivid[256];            /* the game's colours, deeper and brighter */
   const uint32_t *vivid_of;       /* the palette they were made from */
   uint32_t sky[256];              /* a planet's sky, top to horizon */
   uint32_t sky_of;                /* the game's colour it was made from; ~0: none */

   px_kit_canvas deep;             /* nebulae and dust, which stand still */
   unsigned deep_kind;
   star stars[STARS];
   bloom blooms[BLOOMS];
   bloom halos[HALOS];
   unsigned halo_count;
   uint32_t seed;
   bool painted;                   /* the backdrop holds a frame's */

   int shot_x, shot_y;             /* the torpedo where it was seen last; -1: never */
   uint32_t shot_at;
   int ship_x, ship_y;             /* the ship's middle; -1: not seen */
   px_kit_tia seen;                /* the game's voices as the picture follows them */

   /* The sound. */
   px_kit_tia tia;
   unsigned heard0, heard1;        /* what the voices played in the frame before */
   bool shot_on, boom_on;          /* voice 0 plays the torpedo, an explosion */
   unsigned roar[3];               /* the engine's voices at the synth */
} sl;

/* ---------------------------------------------------------------------------
 * The colours
 * ------------------------------------------------------------------------- */

static unsigned hue_of(unsigned index) { return index >> 4; }
static unsigned lum_of(unsigned index) { return index & 0x0E; }

/* Deeper and brighter: away from grey, and up. */
static uint32_t vivid(uint32_t rgb)
{
   int c[3] = { (int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF) };
   int top = c[0] > c[1] ? c[0] : c[1];
   top = top > c[2] ? top : c[2];
   for (unsigned k = 0; k < 3; k++)
   {
      int v = top - (top - c[k]) * 3 / 2;
      v = v * 9 / 8 + 8;
      c[k] = v < 0 ? 0 : v > 255 ? 255 : v;
   }
   return ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
}

static void make_vivid(sl *g, const uint32_t *palette)
{
   for (unsigned i = 0; i < 256; i++)
      g->vivid[i] = lum_of(i) || hue_of(i) ? vivid(palette[i] & 0xFFFFFFu) : PX_KIT_KEEP;
   g->vivid_of = palette;
}

/* ---------------------------------------------------------------------------
 * What the frame shows
 * ------------------------------------------------------------------------- */

/* The background's colour on a row: where two of three places across agree. */
static uint8_t row_colour(const struct pxc_frame *f, unsigned y)
{
   const uint8_t *bk = f->color[PXC_L_BK] + (size_t)y * PXC_W;
   const uint8_t a = bk[24], b = bk[80], c = bk[136];
   return a == c && a != b ? a : b;
}

/* The gate's ceiling and floor: pink and light red. */
static bool gate_colour(unsigned index)
{
   return (hue_of(index) == 4 || hue_of(index) == 5) && lum_of(index) >= 8;
}

/* Pixels of the playfield on a row: of a colour the background has not there (the map's
 * grid), or black (mountains). The game draws playfield in the background's colour too,
 * where it is not to be seen: the warp's tunnel. */
static unsigned playfield(const struct pxc_frame *f, unsigned y, bool black)
{
   const uint8_t *tags = f->tags + (size_t)y * PXC_W;
   const uint8_t *pf   = f->color[PXC_L_PF] + (size_t)y * PXC_W;
   const uint8_t *bk   = f->color[PXC_L_BK] + (size_t)y * PXC_W;
   unsigned n = 0;
   for (unsigned x = 0; x < PXC_W; x++)
      if ((tags[x] & PXC_PF) && (black ? pf[x] == 0 : lum_of(pf[x]) >= 4 && pf[x] != bk[x]))
         n++;
   return n;
}

static bool mountains(const struct pxc_frame *f, unsigned horizon)
{
   unsigned rows = 0;
   for (unsigned y = horizon > MOUNTAINS + TOP ? horizon - MOUNTAINS : TOP; y < horizon; y++)
      if (playfield(f, y, true) >= 8)
         rows++;
   return rows >= 2;
}

static void look(sl *g, const px_scene *s, view *v)
{
   const struct pxc_frame *f = s->frame;
   const unsigned h = f->height < PANEL ? f->height : PANEL;
   unsigned count[256], rows = 0, grid = 0, best = 0;

   memset(v, 0, sizeof(*v));
   memset(g->gate, 0, sizeof(g->gate));
   v->bottom = h;
   if (h < TOP + 100)
      return;

   memset(count, 0, sizeof(count));
   for (unsigned y = TOP; y < h; y++)
   {
      g->row[y] = row_colour(f, y);
      if (gate_colour(g->row[y]))
      {
         g->gate[y] = 1;
         continue;
      }
      count[g->row[y]]++;
      rows++;
   }
   for (unsigned i = 1; i < 256; i++)
      if (count[i] > count[best])
         best = i;

   /* The map: lines of coloured playfield across the view, a row each, over one colour.
    * The warp's tunnel is playfield on rows one after another. */
   {
      unsigned across[PXC_MAX_H];
      for (unsigned y = 39; y <= h && y < PXC_MAX_H; y++)
         across[y] = y < h ? playfield(f, y, false) : 0;
      for (unsigned y = 40; y < h; y++)
         if (across[y] >= 90 && across[y - 1] < 40 && across[y + 1] < 40)
            grid++;
   }
   if (grid >= 5 && count[best] * 10 >= rows * 9)
   {
      v->kind = SCREEN_MAP;
      v->fill = (uint8_t)best;
      return;
   }

   /* A planet: a sky of one colour down to the horizon, and ground below. */
   {
      const uint8_t sky = g->row[TOP + 2];
      unsigned y = TOP;
      while (y < h && g->row[y] == sky)
         y++;
      if (sky && y >= 50 && y + 30 <= h)
      {
         unsigned below[256], ground = 0, most = 0;
         memset(below, 0, sizeof(below));
         for (unsigned k = y; k < h; k++)
            below[g->row[k]]++;
         /* The ground's colour: the most rows below the horizon that are not black. */
         for (unsigned i = 1; i < 256; i++)
            if (i != sky && below[i] > most)
            {
               most   = below[i];
               ground = i;
            }
         if (ground && ground != sky && below[ground] >= 20
               && (below[ground] + below[0]) * 10 >= (h - y) * 8)
         {
            v->kind    = SCREEN_PLANET;
            v->sky     = sky;
            v->ground  = (uint8_t)ground;
            v->horizon = y;
            return;
         }
      }
      /* The planet of the frames before, whose ground took another colour for a frame:
       * its sky and its mountains are where they were. */
      if (g->was.kind == SCREEN_PLANET && sky == g->was.sky && g->was.horizon < h
            && mountains(f, g->was.horizon))
      {
         unsigned below[256], ground = 0;
         memset(below, 0, sizeof(below));
         for (unsigned k = g->was.horizon; k < h; k++)
            below[g->row[k]]++;
         for (unsigned i = 1; i < 256; i++)
            if (below[i] > below[ground])
               ground = i;
         *v = g->was;
         v->ground = (uint8_t)ground;
         v->bottom = h;
         return;
      }
   }

   /* Space: one colour over most of the view, besides the gate's rows. */
   if (rows >= 40 && count[best] * 2 >= rows)
   {
      v->kind = SCREEN_SPACE;
      v->fill = (uint8_t)best;
   }
}

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

/* Nebulae in two colours over black, and dust: what stands still behind space. */
static void paint_deep(sl *g, const px_scene *s)
{
   const unsigned w = g->deep.w, h = g->deep.h;
   const size_t n = (size_t)w * h;
   uint32_t *out = g->deep.pixels;
   px_kit_texture a = { NULL, 0, 0 }, b = { NULL, 0, 0 }, c = { NULL, 0, 0 };
   uint32_t seed = 0x501A415u;

   for (size_t i = 0; i < n; i++)
      out[i] = 0x020208;
   if (g->backdrop == BACKDROP_SPACE && px_kit_texture_fit(&a, s) && px_kit_texture_fit(&b, s)
         && px_kit_texture_fit(&c, s) && a.shades && b.shades && c.shades
         && a.w == w && a.h == h)
   {
      px_kit_texture_noise(&a, 5, 4, 71);
      px_kit_texture_noise(&b, 18, 14, 72);
      px_kit_texture_noise(&c, 6, 5, 73);
      for (unsigned y = 0; y < h; y++)
      {
         /* Darker towards the corners. */
         const int dy = ((int)(2 * y + 1) - (int)h) * 256 / (int)h;
         for (unsigned x = 0; x < w; x++)
         {
            const size_t i = (size_t)y * w + x;
            const int dx = ((int)(2 * x + 1) - (int)w) * 256 / (int)w;
            const unsigned dim = 256 - ((((unsigned)(dx * dx) >> 8) + ((unsigned)(dy * dy) >> 8)) * 70u >> 8);
            const int v1 = (a.shades[i] * 3 + b.shades[i]) / 4 - 128;
            const int v2 = (c.shades[i] * 3 + (255 - b.shades[i])) / 4 - 124;
            unsigned f1 = v1 > 0 ? (unsigned)v1 * 2 : 0, f2 = v2 > 0 ? (unsigned)v2 * 2 : 0;
            uint32_t rgb = 0x020208;
            f1 = f1 > 255 ? 255 : f1;
            f2 = f2 > 255 ? 255 : f2;
            f1 = f1 * f1 >> 8;
            f2 = f2 * f2 >> 8;
            rgb = px_rgb_add(rgb, px_rgb_scale(0x4E1470, f1));
            rgb = px_rgb_add(rgb, px_rgb_scale(0x0C4064, f2));
            if (f1 > 150)
               rgb = px_rgb_add(rgb, px_rgb_scale(0x80305A, (f1 - 150) * 2));
            if (f2 > 160)
               rgb = px_rgb_add(rgb, px_rgb_scale(0x30A0B0, (f2 - 160)));
            out[i] = px_rgb_scale(rgb, dim);
         }
      }
   }
   px_kit_texture_free(&a);
   px_kit_texture_free(&b);
   px_kit_texture_free(&c);

   /* Dust: stars too far off to move. */
   for (size_t k = 0; k < n / 700; k++)
   {
      const size_t i = px_kit_chance(&seed) % n;
      const unsigned bright = 30 + px_kit_chance(&seed) % 80;
      out[i] = px_rgb_add(out[i], px_rgb_scale(px_kit_chance(&seed) % 3 ? 0xD8E0FF : 0xFFE0C0, bright));
   }
   g->deep_kind = g->backdrop;
}

static void star_new(sl *g, star *st, bool far)
{
   st->x  = ((float)(px_kit_chance(&g->seed) % 2001) - 1000.0f) / 1000.0f;
   st->y  = ((float)(px_kit_chance(&g->seed) % 2001) - 1000.0f) / 1000.0f;
   st->z  = far ? 1.0f : 0.08f + (float)(px_kit_chance(&g->seed) % 920) / 1000.0f;
   st->hue = (uint8_t)(px_kit_chance(&g->seed) % 5);
   st->drawn = false;
}

/* Adds a colour to a pixel of the picture, within the view's rows. */
static void add_px(const px_scene *s, uint32_t *out, int X, int Y, int y0, int y1, uint32_t rgb)
{
   if (X < 0 || X >= (int)s->w || Y < y0 || Y >= y1)
      return;
   out[(size_t)Y * s->w + (size_t)X] = px_rgb_add(out[(size_t)Y * s->w + (size_t)X], rgb);
}

/* The stars that rush past: from far to near, streaks of how far they came. */
static void paint_stars(sl *g, px_scene *s, const view *v, float speed, uint32_t tint)
{
   static const uint32_t colours[5] = { 0xF0F4FF, 0xC8D8FF, 0xFFE8C8, 0xE0E8FF, 0xFFD0E0 };
   const int y0 = (int)(TOP * s->sy), y1 = (int)(v->bottom * s->sy);
   const float cx = (float)s->w * 0.5f, cy = (float)(y0 + y1) * 0.5f;
   const float reach = (float)(s->w < (unsigned)(y1 - y0) * 2 ? s->w : (unsigned)(y1 - y0) * 2) * 0.05f;

   for (unsigned i = 0; i < STARS; i++)
   {
      star *st = &g->stars[i];
      int X, Y, steps, lx, ly;
      unsigned bright;
      uint32_t rgb;

      if (s->advance)
         st->z -= speed * (0.35f + 0.65f * (1.0f - st->z));
      if (st->z < 0.04f)
      {
         star_new(g, st, true);
         continue;
      }
      X = (int)(cx + st->x / st->z * reach);
      Y = (int)(cy + st->y / st->z * reach);
      if (X < -40 || X > (int)s->w + 40 || Y < y0 - 40 || Y > y1 + 40)
      {
         star_new(g, st, true);
         continue;
      }
      bright = (unsigned)((1.0f - st->z) * (1.0f - st->z) * 255.0f);
      rgb = px_rgb_scale(tint ? px_rgb_mix(colours[st->hue], tint, 110) : colours[st->hue], 40 + bright * 215 / 255);
      lx = st->drawn ? st->px : X;
      ly = st->drawn ? st->py : Y;
      steps = abs(X - lx) > abs(Y - ly) ? abs(X - lx) : abs(Y - ly);
      if (steps > 90)
         steps = 90;
      for (int k = 0; k <= steps; k++)
      {
         /* The streak is brightest at its head. */
         const int x = steps ? lx + (X - lx) * k / steps : X, y = steps ? ly + (Y - ly) * k / steps : Y;
         const uint32_t c = steps ? px_rgb_scale(rgb, 60 + 196 * (unsigned)k / (unsigned)steps) : rgb;
         add_px(s, s->backdrop, x, y, y0, y1, c);
         if (st->z < 0.35f)
            add_px(s, s->backdrop, x + 1, y, y0, y1, c);
         if (st->z < 0.2f)
         {
            add_px(s, s->backdrop, x, y + 1, y0, y1, c);
            add_px(s, s->backdrop, x + 1, y + 1, y0, y1, c);
         }
      }
      if (s->advance || !st->drawn)
      {
         st->px = X;
         st->py = Y;
         st->drawn = true;
      }
   }
}

/* A soft round light on the backdrop: an explosion's, or a planet's glow. */
static void paint_glow(px_scene *s, const view *v, const bloom *b)
{
   const int sx = (int)s->sx, sy = (int)s->sy;
   const int cx = b->x * sx + sx / 2, cy = b->y * sy + sy / 2;
   const int r = b->radius * sx, r2 = r * r;
   const int y0 = (int)(TOP * s->sy), y1 = (int)(v->bottom * s->sy);

   if (!b->strength || r <= 0)
      return;
   for (int Y = cy - r < y0 ? y0 : cy - r; Y < cy + r && Y < y1; Y++)
   {
      uint32_t *out = s->backdrop + (size_t)Y * s->w;
      const int dy = Y - cy;
      for (int X = cx - r < 0 ? 0 : cx - r; X < cx + r && X < (int)s->w; X++)
      {
         const int dx = X - cx, d2 = dx * dx + dy * dy;
         unsigned f;
         if (d2 >= r2)
            continue;
         f = (unsigned)((r2 - d2) * 256 / r2);
         f = f * f >> 8;
         out[X] = px_rgb_add(out[X], px_rgb_scale(b->rgb, f * b->strength >> 8));
      }
   }
}

static void paint_space(sl *g, px_scene *s, const view *v)
{
   const unsigned w = s->w, sy = s->sy;
   const uint32_t tint = px_rgb_scale(g->mood, 150);
   const bool warp = v->kind == SCREEN_SPACE && hue_of(v->fill) == 8;
   float speed = 0.0035f;

   for (unsigned Y = TOP * sy; Y < v->bottom * sy && Y < s->h; Y++)
   {
      const uint32_t *in = g->deep.pixels + (size_t)Y * w;
      uint32_t *out = s->backdrop + (size_t)Y * w;
      if (v->kind == SCREEN_MAP)
         for (unsigned X = 0; X < w; X++)
            out[X] = px_rgb_add(px_rgb_scale(in[X], 110), tint);
      else if (tint)
         for (unsigned X = 0; X < w; X++)
            out[X] = px_rgb_add(in[X], tint);
      else
         memcpy(out, in, w * sizeof(uint32_t));
   }
   if (v->kind == SCREEN_MAP)
      return;
   if (warp)
      speed = 0.010f + 0.0025f * (float)lum_of(v->fill);
   else if (v->fill)
      speed = 0.008f;
   paint_stars(g, s, v, speed, warp ? 0xA0C0FF : 0);
}

/* A planet: a sky from the game's colour of it, deeper above and hazy at the horizon, a
 * line of light at the horizon, and ground in the game's colour, dark far off and lit near,
 * its lines lit. */
static void paint_planet(sl *g, px_scene *s, const view *v)
{
   const unsigned w = s->w, sy = s->sy;
   const uint32_t *palette = s->frame->palette;
   const uint32_t sky = palette[v->sky] & 0xFFFFFFu, ground = palette[v->ground] & 0xFFFFFFu;
   const unsigned top = TOP * sy, horizon = v->horizon * sy, bottom = v->bottom * sy;
   const bool nebula = g->backdrop == BACKDROP_SPACE;
   unsigned band = v->horizon;
   uint32_t haze, far, near;

   if (g->sky_of != sky)
   {
      px_kit_sky_shades(g->sky, px_rgb_mix(sky, 0x1A0A50, 90));
      g->sky_of = sky;
   }
   haze = g->sky[255];
   while (band < v->bottom && g->row[band] == 0)
      band++;

   for (unsigned Y = top; Y < horizon && Y < s->h; Y++)
   {
      const uint32_t *in = g->deep.pixels + (size_t)Y * w;
      uint32_t *out = s->backdrop + (size_t)Y * w;
      const uint32_t c = g->sky[(Y - top) * 255 / (horizon - top)];
      for (unsigned X = 0; X < w; X++)
         out[X] = nebula ? px_rgb_add(c, px_rgb_scale(in[X], 110)) : c;
   }

   far  = px_rgb_mix(px_rgb_scale(ground, 70), haze, 70);
   near = px_rgb_add(px_rgb_scale(ground, 200), 0x0A0806);
   for (unsigned y = v->horizon; y < v->bottom; y++)
   {
      const unsigned d = (y - v->horizon) * 256 / (v->bottom - v->horizon);
      uint32_t c;
      bool lit = false;
      if (y < band)
      {
         /* The horizon glows. */
         c = px_rgb_add(px_rgb_mix(haze, ground, 60), 0x281C30);
         lit = true;
      }
      else if (g->row[y] == 0)
      {
         /* The ground's lines, lit: brighter as they come nearer. */
         c = px_rgb_add(px_rgb_add(ground, px_rgb_scale(ground, 80 + d / 2)), 0x201828);
         lit = true;
      }
      else
         c = px_rgb_mix(far, near, d > 256 ? 256 : d);
      for (unsigned v2 = 0; v2 < sy; v2++)
      {
         const unsigned Y = y * sy + v2;
         const uint32_t *in = g->deep.pixels + (size_t)Y * w;
         uint32_t *out = s->backdrop + (size_t)Y * w;
         if (Y >= s->h || Y >= bottom)
            break;
         for (unsigned X = 0; X < w; X++)
            out[X] = lit ? c : px_rgb_add(c, px_rgb_scale(in[X], 40));
      }
      if (lit)
         for (unsigned x = 0; x < PXC_W; x++)
         {
            const size_t i = (size_t)y * PXC_W + x;
            if (PX_KEY_CLS(s->top[i]) == PX_CLS_BK && !(s->top[i] & 0xFFFFFFu))
               s->light[i] = 0xFF000000u | px_rgb_scale(c, 150);
         }
   }
}

static void paint_backdrop(sl *g, px_scene *s, const view *v)
{
   const unsigned w = s->w, sy = s->sy;

   if (!s->backdrop || !s->sx || !sy || w != PXC_W * s->sx)
      return;
   if (px_kit_canvas_fit(&g->deep, s) || (g->deep.pixels && g->deep_kind != g->backdrop))
      paint_deep(g, s);
   if (!g->deep.pixels)
      return;

   /* The picture stands still: so does the backdrop. */
   if (!s->advance && !s->backdrop_stale && g->painted)
   {
      s->backdrop_on = true;
      return;
   }

   /* Black above and below the view: the border stays black. */
   memset(s->backdrop, 0, (size_t)w * TOP * sy * sizeof(uint32_t));
   if (v->bottom * sy < s->h)
      memset(s->backdrop + (size_t)w * v->bottom * sy, 0, (size_t)w * (s->h - v->bottom * sy) * sizeof(uint32_t));

   if (v->kind == SCREEN_PLANET)
      paint_planet(g, s, v);
   else
      paint_space(g, s, v);

   for (unsigned i = 0; i < g->halo_count; i++)
      paint_glow(s, v, &g->halos[i]);
   for (unsigned i = 0; i < BLOOMS; i++)
      paint_glow(s, v, &g->blooms[i]);
   s->backdrop_on = true;
   g->painted = true;
}

/* Where the game has the background of the view in the colours the backdrop stands for,
 * it is made dark: the backdrop shows there. Playfield of those colours goes with it: the
 * game hides playfield so, and it would show as a shape on the backdrop. */
static void show_backdrop(const sl *g, px_scene *s, const view *v)
{
   const uint32_t *palette = s->frame->palette;
   for (unsigned y = TOP; y < v->bottom; y++)
   {
      const size_t at = (size_t)y * PXC_W;
      uint32_t *top = s->top + at, *bk = s->bk + at, *sprite = s->sprite + at;
      uint8_t *energy = s->energy + at;
      unsigned own = g->row[y];
      uint32_t rgb;

      /* The colour the backdrop stands for on the row. On a planet, the row's own, where it
       * is the sky's or the ground's. In space and on the map, the view's: rows of another
       * colour (the gate's) have the tunnel around them in it. */
      switch (v->kind)
      {
         case SCREEN_PLANET:
            if (y < v->horizon ? own != v->sky : own != v->ground && own != 0)
               continue;
            break;
         case SCREEN_SPACE:
         case SCREEN_MAP:
            own = v->fill;
            break;
         default:
            continue;
      }
      /* What is of that colour is not to be seen in the game: not the background, nor the
       * playfield and the objects of its colour. */
      rgb = palette[own] & 0xFFFFFFu;
      for (unsigned x = 0; x < PXC_W; x++)
         if (PX_KEY_CLS(top[x]) != PX_CLS_BLANK && (top[x] & 0xFFFFFFu) == rgb)
         {
            top[x]    = PX_KEY(PX_CLS_BK, 0);
            bk[x]     = 0;
            sprite[x] = 0;
            energy[x] = 0;
         }
   }
}

/* The cockpit and the score are the game's display: they are drawn as the game drew them,
 * to the pixel. Blank, as the effects take it, is drawn as it is: no objects fused from
 * the frames before, no glow, shade, light or sparks. */
static void keep_display(px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   const uint32_t *palette = f->palette;

   for (unsigned y = TOP; y < f->height; y++)
   {
      const bool cockpit = y >= PANEL;
      if (!cockpit && y >= SCORE_END)
      {
         y = PANEL - 1;
         continue;
      }
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         const uint8_t tags = f->tags[i];
         const uint32_t rgb = palette[f->winner[i]] & 0xFFFFFFu;
         if (cockpit || (tags & PXC_SPRITES))
         {
            /* The score's digits, and all of the cockpit. */
            s->top[i] = PX_KEY(PX_CLS_BLANK, rgb);
            s->bk[i]  = 0;
         }
         else if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE)
         {
            /* An object drawn from its track where the game has none: the background. */
            s->top[i] = PX_KEY(PX_CLS_BK, palette[f->color[PXC_L_BK][i]]);
            s->bk[i]  = palette[f->color[PXC_L_BK][i]] & 0xFFFFFFu;
         }
         else
            continue;
         s->sprite[i] = 0;
         s->energy[i] = 0;
         s->light[i]  = 0;
      }
   }
}

/* Light where the game's scenery is to glow: the gate's ceiling and floor, the map's grid. */
static void light_scenery(const sl *g, px_scene *s, const view *v)
{
   const struct pxc_frame *f = s->frame;
   for (unsigned y = TOP; y < v->bottom; y++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         if (v->kind == SCREEN_SPACE && g->gate[y] && PX_KEY_CLS(s->top[i]) == PX_CLS_BK)
            s->light[i] = 0xFF000000u | px_rgb_scale(s->top[i] & 0xFFFFFFu, 140);
         else if (v->kind == SCREEN_MAP && PX_KEY_CLS(s->top[i]) == PX_CLS_PF
               && lum_of(f->color[PXC_L_PF][i]) >= 4)
            s->light[i] = 0xFF000000u | px_rgb_scale(s->top[i] & 0xFFFFFFu, 220);
      }
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

/* A planet that flies by, or the one in a planet's sky: round, red or blue. */
static bool is_planet(const px_instance *in)
{
   const unsigned hue = hue_of(in->color);
   return in->cls == PXC_L_P0 && in->h >= 3 && (hue == 4 || hue == 8) && lum_of(in->color) >= 2;
}

static void roles(sl *g, px_scene *s, const view *v)
{
   px_objects *o = s->objects;
   const bool vivid_on = g->colors == COLORS_VIVID;

   g->halo_count = 0;
   if (vivid_on && g->vivid_of != s->frame->palette)
      make_vivid(g, s->frame->palette);

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role = PX_ROLE_NONE;
      if (in->y < SCORE_END || in->y >= (int)v->bottom)
      {
         in->role = PX_ROLE_HUD;
         continue;
      }
      if (v->kind == SCREEN_MAP || v->kind == SCREEN_NONE)
         continue;

      if (v->kind == SCREEN_PLANET && in->y < (int)v->horizon)
      {
         /* The ringed planet in the sky glows. */
         px_scene_energy(s, in, true);
         if (in->cls == PXC_L_P1 && in->h >= 6 && g->halo_count < HALOS)
         {
            bloom *b = &g->halos[g->halo_count++];
            b->x = (int16_t)(in->x + 4);
            b->y = (int16_t)(in->y + in->h / 2);
            b->radius = (uint16_t)(in->h * 2);
            b->strength = 70;
            b->rgb = px_rgb_mix(s->frame->palette[in->color] & 0xFFFFFFu, 0x8090FF, 90);
         }
         continue;
      }

      switch (in->cls)
      {
         case PXC_L_P1:
            if (in->y >= SHIP_TOP)
            {
               in->role = PX_ROLE_PLAYER;
               if (!in->ghost && in->h >= 4)
               {
                  g->ship_x = in->x + 4;
                  g->ship_y = in->y + in->h / 2;
               }
            }
            break;
         case PXC_L_M1:
            in->role = PX_ROLE_SHOT;
            if (!in->ghost)
            {
               g->shot_x  = in->x;
               g->shot_y  = in->y;
               g->shot_at = g->frame;
            }
            break;
         case PXC_L_P0:
            in->role = PX_ROLE_ENEMY;
            break;
         default:
            break;
      }

      if (in->role == PX_ROLE_ENEMY && (lum_of(in->color) || hue_of(in->color)))
      {
         /* Not energy: what flies by fast would leave trails across the view. */
         if (vivid_on)
            px_kit_repaint(s, in, g->vivid);
         /* Planets that fly by glow. */
         if (v->kind == SCREEN_SPACE && is_planet(in) && g->halo_count < HALOS)
         {
            bloom *b = &g->halos[g->halo_count++];
            b->x = (int16_t)(in->x + 4);
            b->y = (int16_t)(in->y + in->h / 2);
            b->radius = (uint16_t)(in->h * 2 + 6);
            b->strength = 90;
            b->rgb = vivid_on ? g->vivid[in->color] : s->frame->palette[in->color] & 0xFFFFFFu;
         }
      }
      else if (in->role == PX_ROLE_SHOT)
      {
         if (vivid_on)
            px_scene_tint(s, in, 0xE8FFFF);
         px_scene_energy(s, in, true);
      }
      else if (in->role == PX_ROLE_PLAYER)
         px_scene_energy(s, in, true);
   }
}

static void add_bloom(sl *g, int x, int y, uint32_t rgb, unsigned radius, unsigned strength)
{
   bloom *b = &g->blooms[0];
   for (unsigned i = 1; i < BLOOMS; i++)
      if (g->blooms[i].strength < b->strength)
         b = &g->blooms[i];
   b->x = (int16_t)x;
   b->y = (int16_t)y;
   b->rgb = rgb;
   b->radius = (uint16_t)radius;
   b->strength = (uint16_t)strength;
}

/* What the game's voices say happened, as the picture hears them: sparks and light. */
static void events(sl *g, px_scene *s, const view *v)
{
   const px_kit_tia *t = &g->seen;
   const bool boom = t->volume[0] >= 12 && t->wave[0] == 15 && t->pitch[0] == 4
         && !(t->was_wave[0] == 15 && t->was_pitch[0] == 4 && t->was_volume[0] >= 12);
   const bool hit = t->volume[1] >= 6 && t->wave[1] == 15
         && !(t->was_wave[1] == 15 && t->was_volume[1] >= 6);

   for (unsigned i = 0; i < BLOOMS; i++)
      g->blooms[i].strength = (uint16_t)(g->blooms[i].strength * 205 / 256);

   if (!g->sparks || v->kind == SCREEN_MAP || v->kind == SCREEN_NONE)
      return;
   if (boom)
   {
      /* The torpedo is drawn at the ship while it flies into the view: what exploded is the
       * enemy nearest to it. */
      const px_objects *o = s->objects;
      const int from_x = g->shot_x >= 0 && g->frame - g->shot_at <= 30 ? g->shot_x
            : g->ship_x >= 0 ? g->ship_x : PXC_W / 2;
      const int from_y = g->ship_y >= 0 ? g->ship_y : SHIP_TOP;
      int x = from_x, y = (TOP + SHIP_TOP) / 2, best = 1 << 30;
      for (unsigned i = 0; i < o->count; i++)
      {
         const px_instance *in = &o->inst[i];
         int dx, dy;
         if (in->role != PX_ROLE_ENEMY || is_planet(in) || !(lum_of(in->color) || hue_of(in->color)))
            continue;
         dx = in->x + (int)in->w / 2 - from_x;
         dy = (in->y + (int)in->h / 2 - from_y) / 2;
         if (dx * dx + dy * dy < best)
         {
            best = dx * dx + dy * dy;
            x = in->x + (int)in->w / 2;
            y = in->y + (int)in->h / 2;
         }
      }
      px_scene_burst(s, x, y, 0xFFFFFF, 14, 300);
      px_scene_burst(s, x, y, 0xFFA040, 30, 380);
      px_scene_burst(s, x, y, 0xFF4060, 16, 260);
      add_bloom(g, x, y, 0xFF8840, 22, 230);
   }
   if (hit && g->ship_x >= 0)
   {
      px_scene_burst(s, g->ship_x, g->ship_y, 0xFFF0C0, 40, 460);
      px_scene_burst(s, g->ship_x, g->ship_y, 0xFF5020, 36, 340);
      add_bloom(g, g->ship_x, g->ship_y, 0xFF6030, 34, 256);
   }
}

static void frame(void *state, px_scene *s)
{
   sl *g = (sl*)state;
   view v;

   if (s->advance)
   {
      g->frame++;
      px_kit_tia_hear(&g->seen, s->frame);
   }

   keep_display(s);
   look(g, s, &v);
   if (v.kind == SCREEN_NONE && g->was.kind != SCREEN_NONE && g->held < HOLD && v.bottom == g->was.bottom)
   {
      /* Not a frame of a kind seen: taken for the frames before it. */
      v = g->was;
      if (s->advance)
         g->held++;
   }
   else if (v.kind != SCREEN_NONE && s->advance)
   {
      g->was  = v;
      g->held = 0;
   }

   /* Space is lit by the game's colour for it, which follows the game's a little behind: a
    * flash stays a moment, a flicker is softened. */
   if (v.kind == SCREEN_SPACE || v.kind == SCREEN_MAP)
   {
      const uint32_t target = v.fill ? s->frame->palette[v.fill] & 0xFFFFFFu : 0;
      if (g->mood_kind != v.kind || v.kind == SCREEN_MAP)
         g->mood = target;
      else if (s->advance)
         g->mood = px_rgb_mix(g->mood, target, 100);
      g->mood_kind = v.kind;
   }
   else
      g->mood_kind = v.kind;

   /* The backdrop takes the place of the game's colours first: what is recoloured after is
    * what is to be seen. */
   if (g->backdrop != BACKDROP_OFF && v.kind != SCREEN_NONE)
      show_backdrop(g, s, &v);
   roles(g, s, &v);
   if (s->advance)
      events(g, s, &v);
   if (v.kind != SCREEN_NONE)
      light_scenery(g, s, &v);
   if (g->backdrop != BACKDROP_OFF && v.kind != SCREEN_NONE)
      paint_backdrop(g, s, &v);
   else
      g->painted = false;
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 *   voice 0   the torpedo: noise (waveform 8) at pitch 2, volume 10, for two frames, then
 *             waveform 15 through pitches 2, 6, 3, 8, 4, 10, 5, 9 ... at volumes 10, 14, 8,
 *             12 ... down to 1, for 20 to 56 frames while it flies. Fired again before it
 *             ends: noise at the pitch it had, then pitch 2 again.
 *   voice 0   an enemy explodes: waveform 15 at pitches 4, 8, 12 and volumes 14, 12, 10,
 *             a frame each, then noise from pitch 12 down to 27 and volume 15 down to 1
 *             in 52 frames. It may follow the torpedo's without a silence between.
 *   voice 0   noise from pitch 23 down, louder: the warp and the take-off; and waveform 4
 *             at pitches 20, 19, 18, 17 in turns at volume 1, all but silent. Both are
 *             heard as the game plays them.
 *   voice 1   the engine's roar: noise, its pitch and volume going up and down, for as long
 *             as the ship flies
 *   voice 1   the ship hit: waveform 15 at pitches 15, 8, 6, 9, 7 and volumes 7 up to 14
 *             for 10 frames, then noise that falls away in 82 frames. Lost, the same on
 *             voice 0 too.
 *
 * No tune was heard: not at power on, on the title for 20000 frames, when a game begins,
 * nor when one ends. A tune the game plays elsewhere is heard as the game plays it.
 * ------------------------------------------------------------------------- */

enum { V0_NONE = 0, V0_SHOT, V0_BOOM, V0_OTHER };
enum { V1_NONE = 0, V1_ROAR, V1_HIT, V1_OTHER };

static unsigned voice0_plays(sl *g)
{
   const px_kit_tia *t = &g->tia;
   const unsigned wave = t->wave[0], pitch = t->pitch[0], volume = t->volume[0];

   if (!volume)
   {
      g->shot_on = g->boom_on = false;
      return V0_NONE;
   }
   if (wave == 15 && pitch == 4 && volume >= 12)
   {
      g->boom_on = true;
      g->shot_on = false;
      return V0_BOOM;
   }
   if (g->boom_on && (wave == 15 || (wave == 8 && pitch >= 12)))
      return V0_BOOM;
   g->boom_on = false;
   if (wave == 8 && pitch == 2 && volume >= 8)
   {
      g->shot_on = true;
      return V0_SHOT;
   }
   if (g->shot_on && (wave == 15 || (wave == 8 && pitch <= 11)))
      return V0_SHOT;
   g->shot_on = false;
   return V0_OTHER;
}

static unsigned voice1_plays(const sl *g)
{
   const px_kit_tia *t = &g->tia;
   if (!t->volume[1])
      return V1_NONE;
   if (t->wave[1] == 15 && t->volume[1] >= 6)
      return V1_HIT;
   return t->wave[1] == 8 ? V1_ROAR : V1_OTHER;
}

/* The torpedo: a bright zap that falls, a hum under it, and a click. */
static void play_shot(px_sound *s, float pan)
{
   static const px_tone p[4] = {
      /* wave           freq  to   glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SQUARE, 2200, 320, 0.22f, 0.001f, 0.01f, 0.26f, 0.16f, 6000, 900, 0, 0 },
      { PX_WAVE_SAW,    1100, 160, 0.24f, 0.001f, 0.01f, 0.28f, 0.16f, 3200, 500, 0, 0 },
      { PX_WAVE_SINE,    220,  90, 0.20f, 0.001f, 0.02f, 0.24f, 0.30f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE,  9000,   0, 0.04f, 0,      0,     0.04f, 0.12f, 7000, 2000, 0, 0 }
   };
   px_kit_play(s, p, 4, pan);
   px_sound_rumble(s, 0, 10000, 3);
}

/* An enemy explodes: a crack, a roar that falls away, and a boom under it. */
static void play_boom(px_sound *s, float pan)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 9000,  700, 0.35f, 0,      0.02f, 0.70f, 0.40f, 7000, 500, 0, 0 },
      { PX_WAVE_NOISE, 2400,  200, 0.60f, 0.01f,  0.05f, 0.90f, 0.30f, 1800, 150, 0, 0 },
      { PX_WAVE_SINE,   130,   38, 0.30f, 0.001f, 0.03f, 0.60f, 0.62f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    420,   60, 0.35f, 0.001f, 0.02f, 0.45f, 0.14f, 2200, 300, 13.0f, 0.04f }
   };
   if (pan > -2.0f)
      px_kit_play(s, p, 4, pan);
   px_sound_rumble(s, 32000, 36000, 14);
}

/* The ship hit: metal torn, a deep blow that shakes, and a long rumble after. */
static void play_hit(px_sound *s, float pan)
{
   static const px_tone p[5] = {
      { PX_WAVE_NOISE, 6000,  220, 1.30f, 0,      0.10f, 1.60f, 0.46f, 5000, 140, 0, 0 },
      { PX_WAVE_SINE,    80,   24, 1.00f, 0.002f, 0.12f, 1.40f, 0.80f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    700,   50, 0.90f, 0.001f, 0.05f, 1.00f, 0.18f, 2600, 200, 9.0f, 0.06f },
      { PX_WAVE_SQUARE, 1500, 400, 0.30f, 0.001f, 0.02f, 0.35f, 0.08f, 5000, 1000, 21.0f, 0.10f },
      { PX_WAVE_NOISE,   700,  90, 2.00f, 0.15f,  0.30f, 2.40f, 0.26f, 600, 90, 0, 0 }
   };
   if (pan > -2.0f)
      px_kit_play(s, p, 5, pan);
   px_sound_rumble(s, 65535, 50000, 45);
}

/* The engine's roar, as loud as the game's and brighter with its pitch: a rush of air, a
 * growl an octave down, and a hum under both. */
static void play_roar(sl *g, px_sound *s, float pan)
{
   static const px_tone tones[3] = {
      /* wave          freq  to  glide  attack  hold  decay  gain   cutoff */
      { PX_WAVE_NOISE, 2000, 0, 0,     0.05f,  0,    0,     1.0f,  1500, 0, 0, 0 },
      { PX_WAVE_NOISE,  500, 0, 0,     0.05f,  0,    0,     1.0f,   500, 0, 0, 0 },
      { PX_WAVE_SINE,    46, 0, 0,     0.10f,  0,    0,     1.0f,     0, 0, 5.0f, 0.03f }
   };
   const float loud = (float)g->tia.volume[1] / 15.0f;
   const float rush = 30000.0f / (float)(g->tia.pitch[1] + 1);
   const float freq[3] = { rush, rush * 0.25f, 46.0f };
   const float gain[3] = { 0.30f * loud, 0.34f * loud, 0.22f * loud };

   for (unsigned i = 0; i < 3; i++)
      if (!px_synth_move(s->synth, g->roar[i], pan, gain[i], freq[i]))
      {
         px_tone t = tones[i];
         t.freq = freq[i];
         g->roar[i] = px_synth_play(s->synth, &t, pan, gain[i]);
      }
}

static void stop_roar(sl *g, px_sound *s, float seconds)
{
   for (unsigned i = 0; i < 3; i++)
   {
      px_synth_stop(s->synth, g->roar[i], seconds);
      g->roar[i] = 0;
   }
}

static void sound(void *state, px_sound *s)
{
   sl *g = (sl*)state;
   int ship = -1, shot = -1;
   unsigned heard0, heard1;

   /* Where the ship and its torpedo are, from the picture before. */
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_PLAYER)
         ship = in->x + 4;
      else if (in->role == PX_ROLE_SHOT && !in->ghost)
         shot = in->x;
   }

   px_kit_tia_hear(&g->tia, s->frame);
   heard0 = voice0_plays(g);
   heard1 = voice1_plays(g);

   if (heard0 == V0_SHOT && g->tia.wave[0] == 8 && g->tia.pitch[0] == 2
         && !(g->tia.was_wave[0] == 8 && g->tia.was_pitch[0] == 2 && g->tia.was_volume[0]))
   {
      if (g->own_sound) play_shot(s, px_kit_pan(ship));
      else              px_sound_rumble(s, 0, 10000, 3);
   }
   if (heard0 == V0_BOOM && g->heard0 != V0_BOOM)
      play_boom(s, g->own_sound ? px_kit_pan(shot >= 0 ? shot : ship) : -3.0f);
   if (heard1 == V1_HIT && g->heard1 != V1_HIT)
      play_hit(s, g->own_sound ? px_kit_pan(ship) : -3.0f);

   if (g->own_sound && heard1 == V1_ROAR)
      play_roar(g, s, px_kit_pan(ship) * 0.5f);
   else if (g->roar[0])
      stop_roar(g, s, heard1 == V1_HIT ? 0.05f : 0.25f);

   if (g->own_sound)
   {
      if (heard0 == V0_SHOT || heard0 == V0_BOOM)
         s->voice[0] = 0.0f;
      if (heard1 == V1_ROAR || heard1 == V1_HIT)
         s->voice[1] = 0.0f;
   }
   g->heard0 = heard0;
   g->heard1 = heard1;
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   sl *g = (sl*)state;
   memset(&g->was, 0, sizeof(g->was));
   g->held = 0;
   g->mood = 0;
   g->mood_kind = SCREEN_NONE;
   g->painted = false;
   g->shot_x = g->shot_y = g->ship_x = g->ship_y = -1;
   g->shot_at = 0;
   memset(g->blooms, 0, sizeof(g->blooms));
   g->halo_count = 0;
   g->seed = 0x5014A515u;
   for (unsigned i = 0; i < STARS; i++)
      star_new(g, &g->stars[i], false);
   px_kit_tia_reset(&g->seen);
   px_kit_tia_reset(&g->tia);
   g->heard0 = V0_NONE;
   g->heard1 = V1_NONE;
   g->shot_on = g->boom_on = false;
   memset(g->roar, 0, sizeof(g->roar));
}

static void *create(void)
{
   sl *g = (sl*)calloc(1, sizeof(sl));
   if (g)
   {
      g->sparks = g->own_sound = true;
      g->sky_of = ~0u;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   sl *g = (sl*)state;
   if (g)
      px_kit_canvas_free(&g->deep);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   sl *g = (sl*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_pick(get, OPT_BACKDROP, backdrop);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->vivid_of  = NULL;
   g->painted   = false;
}

const px_game px_game_solaris = {
   "Solaris", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
