/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pole Position (Atari, 1983).
 *
 * A qualifying lap of the Fuji circuit against the clock (T), then, for those who make it,
 * the race. The car drives by itself once the lights have gone; the stick steers, pushed up
 * it is in low gear (LO) and pulled down in high (HI), and the button brakes. A car run into
 * burns for two seconds; the clock does not wait. At 0 the game is over and the title and
 * the road come in turns until it is reset.
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  14..96   the sky: background ($86). On it the score in a box of playfield (black)
 *                  with digits of both players, G, T and L and their values (players and
 *                  missiles, white), and the speed on rows 55..59: a bar of playfield (red)
 *                  over background of darker blues ($80, $A0) as far as it is not full.
 *   rows  67..76   two clouds: players 0 and 1, double width, white ($0E)
 *   rows  85..96   the hills: playfield and players in brown ($24), their crests players
 *                  and missiles in green ($D2)
 *   rows  97..104  their foot: background, brown ($24)
 *   rows 105..205  the road: background, grey ($04), from edge to edge of the picture. Its
 *                  edges are the ball and missile 0, 4 wide, set again on every row: one
 *                  draws the left and the other the right, and which is which changes from
 *                  one stretch of frames to another. Their colours are the kerbs' stripes,
 *                  red ($42, $44) and white ($0C, $0E), and move down the picture with the
 *                  speed. Missile 1 draws the dashes down the middle ($2C).
 *   other cars     player 1, four times as wide, in the rows of the road; they are drawn
 *                  larger as they come nearer
 *   rows 186..205  the car: player 0, four times as wide, black ($00) and orange ($28), over
 *                  a body of playfield, red ($44). It stays in the middle; the road moves.
 *                  Run into a car, it is a flat wreck that flickers red and yellow.
 *   title          the whole picture dark blue, a chequered flag of playfield, the name
 *
 * Of its memory ($80 is 0): 5 has bit 6 set while the car burns after a crash. 8 is the
 * speed: $60 at most in low gear and $A3 in high. 62 is how far the hills and the clouds
 * have turned, 0..159: they are drawn at its column of the frame before, plus one.
 *
 * The picture is a road into the distance. The kit finds its horizon and the road's edges
 * afresh in every frame (px_kit_horizon_find, px_kit_road_find): the backdrop is painted
 * from what the game draws in that frame, and a screen without a horizon (the title) keeps
 * the game's own colours. The sky, Mount Fuji and the far mountains are a canvas that goes
 * round with byte 62; the game's hills are painted where the game has them; the grass and
 * the asphalt take their stripes from the kerbs'.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_pp_colors"
#define OPT_BACKDROP "proteus_pp_backdrop"
#define OPT_SPARKS   "proteus_pp_sparks"
#define OPT_SOUND    "proteus_pp_sound"
#define OPT_PASSING  "proteus_pp_passing"

#define RAM_STATE    5
#define RAM_SPEED    8
#define RAM_TURN     62

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define TOP          14      /* the first row of the picture that is not black */
#define END          206     /* the row after its last */
#define HUD_END      62      /* the score, the clock and the speed are above this */
#define CLOUDS_END   80      /* the clouds are above this */
#define HILLS        14      /* rows of hills above the horizon, at most */
#define CAR_TOP      186     /* the car's first row */
#define CAR_X        80      /* the middle of the car */
#define CAR_Y        196
#define PERIOD       160     /* byte 62 goes round at this */
#define EDGES        (PXC_BL | PXC_M0)

#define CRASHED      0x40    /* of byte 5 */

/* Where Mount Fuji stands on the canvas that goes round, in columns of the game. */
#define FUJI_X       80
#define FUJI_RISE    30      /* its peak, in rows above the horizon */
#define TURN_OFFSET  (-8)    /* at byte 62 = 56, where the game begins, Fuji is at column 128 */

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "a4ff39d513b993159911efe01ac12eba",   /* Pole Position (USA) */
   NULL
};

static const char *const fx[] = {
   "width", "60",
   NULL
};

static const char *const colors[] = { "arcade", "The arcade's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "circuit", "Fuji and the circuit", "sky", "Sky and Mount Fuji",
   "off", "Off", NULL };
/* In the order of the options' values. */
enum { COLORS_ARCADE = 0, COLORS_ORIGINAL };
enum { BACKDROP_CIRCUIT = 0, BACKDROP_SKY, BACKDROP_OFF };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "The arcade's colours: a red car with a yellow stripe, bright kerbs, cars that stand out, clouds with shade, green hills and grass; or the game's own colours.",
     "arcade", colors },
   { OPT_BACKDROP, "Backdrop",
     "A sky that deepens above and is hazy at the horizon, Mount Fuji and far mountains that turn with the road, hills of grass and trees, and grass beside the asphalt in stripes that move with the kerbs; the sky and the mountains alone; or the game's flat colours.",
     "circuit", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks, fire and a flash of light on the circuit where the car crashes, and embers while it burns.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the engine, which follows the game's, the tyres, the bumps off the road and the crash, and the game's tunes with other voices. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_PASSING, "Cars going by",
     "The rush of a car as it is overtaken, which the arcade has and the game has not, where it goes by. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   /* The options. */
   unsigned colors, backdrop;
   bool sparks, own_sound, passing;

   uint32_t frame;               /* counts the frames that advance */
   int turned;                   /* byte 62 as memory has it now; -1: not known */
   int shown;                    /* where the hills are drawn in the frame at hand: byte 62 of the one before */
   int crashed;                  /* byte 5's bit, as it was; -1: not known */
   unsigned burning;             /* frames since the crash, while the car burns */

   /* The backdrop. */
   px_kit_horizon at;            /* the horizon of the frame at hand */
   px_kit_road road;             /* the road's edges */
   px_kit_canvas far;            /* the sky, Fuji and the far mountains, which go round */
   px_kit_texture grain;         /* for the hills */
   uint32_t far_sky;             /* the game's sky and the rows the canvas was painted for */
   unsigned far_top, far_horizon, far_colors;
   uint32_t sky[256];            /* the sky, top to horizon */
   uint32_t hill_body[256], hill_crest[256];
   uint32_t hill_rgb, crest_rgb; /* the game's colours the hills' tables are for; ~0: none */
   unsigned glow;                /* a flash on the circuit, of 256, and its colour */
   uint32_t glow_rgb;

   /* The colours of the game's indices for the car, the other cars and the kerbs. */
   uint32_t car_map[256], rival_map[256], kerb_map[256];

   /* The sounds. */
   px_kit_tia tia;
   unsigned heard0, heard1;      /* what the voices played in the frame before */
   unsigned engine[4];           /* the engine's voices at the synth */
   int engine_pitch;             /* the game's pitch for it, as heard last */
   unsigned squeal[2];           /* the tyres' */
   unsigned fire;                /* the fire's crackle */
   unsigned tune[2];             /* the voices that play the game's tunes */
   uint8_t tune_pitch[2];
   uint32_t seed;
   int rival_y, rival_x;         /* the nearest of the other cars in the picture before */
} pp;

/* ---------------------------------------------------------------------------
 * The colours
 * ------------------------------------------------------------------------- */

static void make_maps(pp *g)
{
   for (unsigned i = 0; i < 256; i++)
   {
      const unsigned hue = i >> 4, lum = i & 0x0E;
      uint32_t car = PX_KIT_KEEP, rival = PX_KIT_KEEP, kerb = PX_KIT_KEEP;

      /* The car: a red body, black wings and wheels with a sheen, a yellow stripe. */
      if (hue == 0 && lum <= 2)
         car = 0x24242C;
      else if (hue == 2)
         car = 0xFFD23A;
      else if (hue == 4)
         car = 0xF0141E;

      /* The other cars, in the arcade's brighter colours. */
      switch (hue)
      {
         case 0x1: case 0x2: rival = 0xFFC81E; break;
         case 0x3: case 0x4: rival = 0xFF3A30; break;
         case 0x7: case 0x8: case 0x9: rival = 0x3A86FF; break;
         case 0xA: case 0xB: rival = 0x30C8FF; break;
         case 0xC: case 0xD: rival = 0x3CD864; break;
         case 0x0: rival = lum >= 0x0A ? 0xF4F4F4 : PX_KIT_KEEP; break;
         default: break;
      }

      /* Kerbs of red and white; far away the game fades them to grey, which is kept. */
      if (hue == 4)
         kerb = 0xF0202A;
      else if (hue == 0 && lum >= 0x0A)
         kerb = 0xEAEAEA;
      else if (hue == 2)
         kerb = 0xFFE070;   /* the dashes down the middle */

      g->car_map[i]   = car;
      g->rival_map[i] = rival;
      g->kerb_map[i]  = kerb;
   }
}

/* How light a palette index is, 0..15. */
static unsigned lum_of(unsigned index)
{
   return index & 0x0E;
}

/* ---------------------------------------------------------------------------
 * The far canvas: the sky, Mount Fuji and the mountains behind the game's hills
 *
 * It is in columns of the game and goes round at 160 of them, as the hills do. It is
 * painted at the picture's size, again whenever the sky's colour or the horizon change.
 * ------------------------------------------------------------------------- */

/* A ridge that fits itself round the canvas: rows above the horizon at a column. */
static float ridge(float u)
{
   const float t = u * 6.2831853f / (float)PERIOD;
   return 5.0f + 2.2f * sinf(t * 3.0f + 0.7f) + 1.4f * sinf(t * 7.0f + 2.1f)
         + 0.8f * sinf(t * 13.0f + 0.3f) + 0.5f * sinf(t * 29.0f + 1.3f);
}

/* Columns from Fuji's middle, the shorter way round. */
static float from_fuji(float u)
{
   float d = fmodf(u - (float)FUJI_X, (float)PERIOD);
   if (d < -(float)PERIOD / 2) d += (float)PERIOD;
   if (d >= (float)PERIOD / 2) d -= (float)PERIOD;
   return d;
}

static uint32_t far_pixel(const pp *g, float u, float v, float px)
{
   const px_kit_horizon *at = &g->at;
   const float horizon = (float)at->horizon, top = (float)at->top;
   const uint32_t haze = g->sky[255];
   float t = (v - top) / (horizon - top);
   uint32_t rgb;
   const float peak = horizon - FUJI_RISE, d = from_fuji(u);
   float cover;

   if (t < 0.0f) t = 0.0f;
   if (t > 1.0f) t = 1.0f;
   rgb = g->sky[(unsigned)(t * 255.0f)];

   /* Mount Fuji: slopes that curve in towards a flat top, snow to below half way down in
    * streaks, lit from the left. */
   if (v >= peak)
   {
      const float down = (v - peak) / FUJI_RISE;
      const float half = 3.0f + 46.0f * powf(down, 1.55f);
      cover = half - fabsf(d);
      if (cover > 0.0f)
      {
         const float side = d / half;   /* -1 at the left edge, 1 at the right */
         /* The snow reaches down the gullies further, smoothly from one to the next. */
         const float pos = (d + 60.0f) / 2.6f, frac = pos - floorf(pos);
         const unsigned cell = (unsigned)pos;
         const float n0 = ((cell * 2654435761u) >> 24) / 255.0f;
         const float n1 = (((cell + 1) * 2654435761u) >> 24) / 255.0f;
         const float gully = n0 + (n1 - n0) * frac * frac * (3.0f - 2.0f * frac);
         const float snowline = 0.30f + 0.04f * sinf(d * 0.45f + 1.0f) + 0.16f * gully * gully;
         const unsigned lit = side < -0.15f ? 256 : side > 0.35f ? 170 : (unsigned)(256 - (side + 0.15f) * 172);
         uint32_t own;
         if (down < snowline)
            own = px_rgb_mix(0xB0BEE0, 0xF6F9FF, lit - 170 > 86 ? 256 : (lit - 170) * 3);
         else
            own = px_rgb_scale(g->far_colors == COLORS_ARCADE ? 0x4A5896 : px_rgb_mix(at->band, 0x404060, 128), lit);
         own = px_rgb_mix(own, haze, 60 + (unsigned)(down * 60.0f));
         rgb = px_rgb_mix(rgb, own, cover >= px ? 256 : (unsigned)(cover / px * 256.0f));
      }
   }

   /* The far mountains, in front of Fuji's foot and hazier. */
   cover = v - (horizon - ridge(u));
   if (cover > 0.0f)
   {
      const float depth = cover / 10.0f;
      uint32_t own = g->far_colors == COLORS_ARCADE ? 0x5A70A0 : px_rgb_mix(at->band, 0x607090, 160);
      own = px_rgb_scale(own, 256 - (unsigned)((depth > 1.0f ? 1.0f : depth) * 50.0f));
      own = px_rgb_mix(own, haze, 120);
      rgb = px_rgb_mix(rgb, own, cover >= 1.0f ? 256 : (unsigned)(cover * 256.0f));
   }
   return rgb;
}

static void paint_far(pp *g, const px_scene *s)
{
   const unsigned w = g->far.w, h = g->far.h, sx = s->sx, sy = s->sy;
   uint32_t sky = g->at.sky;

   if (g->colors == COLORS_ARCADE)
      sky = px_rgb_mix(sky, 0x3C8CF0, 140);
   px_kit_sky_shades(g->sky, sky);
   memset(g->far.pixels, 0, (size_t)w * h * sizeof(uint32_t));
   g->far_colors = g->colors;
   for (unsigned Y = g->at.top * sy; Y < g->at.horizon * sy && Y < h; Y++)
   {
      uint32_t *out = g->far.pixels + (size_t)Y * w;
      const float v = ((float)Y + 0.5f) / (float)sy;
      for (unsigned X = 0; X < w; X++)
         out[X] = far_pixel(g, ((float)X + 0.5f) / (float)sx, v, 1.0f / (float)sx);
   }
   g->far_sky     = g->at.sky;
   g->far_top     = g->at.top;
   g->far_horizon = g->at.horizon;
}

/* ---------------------------------------------------------------------------
 * The hills and the road, painted where the game has them
 * ------------------------------------------------------------------------- */

/* The hills' colours: from the game's brown (the band's) and the green of their crests, in
 * the haze of the horizon. */
static void body_table(pp *g, uint32_t rgb)
{
   uint32_t body = g->colors == COLORS_ARCADE ? 0x5E8048 : rgb;
   body = px_rgb_mix(body, g->sky[255], 50);
   px_kit_shades(g->hill_body, px_rgb_scale(body, 150), body, px_rgb_add(px_rgb_scale(body, 280), 0x0C0A00));
   g->hill_rgb = rgb;
}

static void crest_table(pp *g, uint32_t rgb)
{
   uint32_t crest = g->colors == COLORS_ARCADE ? 0x2C5A2C : rgb;
   crest = px_rgb_mix(crest, g->sky[255], 30);
   px_kit_shades(g->hill_crest, px_rgb_scale(crest, 130), crest, px_rgb_scale(crest, 330));
   g->crest_rgb = rgb;
}

/* The grain of the hills: coarse and fine. */
static void paint_grain(pp *g, const px_scene *s)
{
   px_kit_texture layer = { NULL, 0, 0 };
   const size_t n = (size_t)g->grain.w * g->grain.h;
   memset(g->grain.shades, 70, n);
   px_kit_texture_fit(&layer, s);
   if (layer.shades)
   {
      px_kit_texture_noise(&layer, 20, 40, 21);
      for (size_t i = 0; i < n; i++)
         g->grain.shades[i] = (uint8_t)(g->grain.shades[i] + layer.shades[i] / 3);
      px_kit_texture_noise(&layer, 120, 160, 22);
      for (size_t i = 0; i < n; i++)
         g->grain.shades[i] = (uint8_t)(g->grain.shades[i] + layer.shades[i] / 4);
   }
   px_kit_texture_free(&layer);
}

static void paint_hills(pp *g, px_scene *s, int32_t dx)
{
   const unsigned sx = s->sx, sy = s->sy, w = s->w;
   const unsigned from = g->at.horizon > HILLS + g->at.top ? g->at.horizon - HILLS : g->at.top;

   for (unsigned y = from; y < g->at.ground; y++)
   {
      const uint32_t *top = s->top + (size_t)y * PXC_W;
      /* Where the hill is, from its top down: lighter at its crest. */
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         const uint32_t own = top[x] & 0xFFFFFFu;
         const bool band = y >= g->at.horizon;
         const bool body = band || own == g->at.band;
         /* Above the horizon, what is not sky is hills: the game's playfield and objects. */
         if (PX_KEY_CLS(top[x]) == PX_CLS_BLANK)
            continue;
         if (!band && (PX_KEY_CLS(top[x]) == PX_CLS_BK || own == g->at.sky))
            continue;
         if (!body && own != g->crest_rgb)
            crest_table(g, own);
         for (unsigned v = 0; v < sy; v++)
         {
            const unsigned Y = y * sy + v;
            /* The foot of the hills is darker, towards the road. */
            const int lift = band ? -(int)((y - g->at.horizon) * 10) : 20;
            uint32_t *out = s->backdrop + (size_t)Y * w + x * sx;
            px_kit_texture_roll(&g->grain, out - x * sx, Y, x * sx, x * sx + sx, dx, 0,
                  body ? g->hill_body : g->hill_crest);
            if (lift)
               for (unsigned u = 0; u < sx; u++)
                  out[u] = lift > 0 ? px_rgb_add(out[u], px_rgb_scale(out[u], (unsigned)lift))
                        : px_rgb_scale(out[u], (unsigned)(256 + lift));
         }
         s->top[i]    = PX_KEY(PX_CLS_BK, 0);
         s->bk[i]     = 0;
         s->sprite[i] = 0;
         s->energy[i] = 0;
      }
   }
}

/* The grass beside the road and the asphalt on it, a row at a time: lighter where the kerb
 * is white, darker where it is red, which moves with the speed as the kerbs do, and hazier
 * the further off. */
static void paint_road(pp *g, px_scene *s)
{
   const unsigned sx = s->sx, sy = s->sy, w = s->w;
   const unsigned ground = g->at.ground, bottom = g->at.bottom;
   const uint32_t haze = g->sky[255];
   const bool arcade = g->colors == COLORS_ARCADE;
   const uint32_t road = arcade ? 0x5C5E66 : g->at.ground_rgb;

   for (unsigned y = ground; y < bottom; y++)
   {
      const unsigned d = (y - ground) * 256 / (bottom - ground);   /* 0 far, 256 near */
      const unsigned far = (256 - d) * (256 - d) >> 8;
      const bool light = g->road.edge[y] && lum_of(g->road.edge[y]) >= 0x0A && (g->road.edge[y] >> 4) == 0;
      uint32_t grass, asphalt;
      int left = g->road.left[y], right = g->road.right[y];

      if (arcade)
         grass = light ? 0x6CC24A : 0x4AA034;
      else
         grass = light ? px_rgb_add(g->at.ground_rgb, 0x101010) : g->at.ground_rgb;
      asphalt = light ? px_rgb_add(road, 0x0A0A0A) : road;
      grass   = px_rgb_mix(grass, px_rgb_mix(haze, 0x9CC890, 140), far * 110 >> 8);
      asphalt = px_rgb_mix(asphalt, haze, far * 90 >> 8);

      left  = left < -1 ? -1 : left > PXC_W - 1 ? PXC_W - 1 : left;
      right = right > PXC_W ? PXC_W : right < 0 ? 0 : right;
      for (unsigned v = 0; v < sy; v++)
      {
         uint32_t *out = s->backdrop + (size_t)(y * sy + v) * w;
         unsigned X = 0;
         const unsigned a = (unsigned)(left + 1) * sx, b = (unsigned)right * sx;
         for (; X < a && X < w; X++)
            out[X] = grass;
         for (; X < b && X < w; X++)
            out[X] = asphalt;
         for (; X < w; X++)
            out[X] = grass;
      }
   }
}

/* Light from the burning car on the circuit about it, and a flash over all of it. */
static void light_circuit(pp *g, px_scene *s, unsigned from, unsigned to)
{
   const unsigned sx = s->sx, sy = s->sy, w = s->w;
   const uint32_t flash = g->glow ? px_rgb_scale(g->glow_rgb, g->glow) : 0;
   const unsigned fire = g->burning ? 100 + px_kit_wave(g->frame * 37) / 3 : 0;

   if (!flash && !fire)
      return;
   for (unsigned Y = from * sy; Y < to * sy; Y++)
   {
      uint32_t *out = s->backdrop + (size_t)Y * w;
      const int dy = (int)(Y / sy) - CAR_Y;
      for (unsigned X = 0; X < w; X++)
      {
         uint32_t add = flash;
         if (fire && Y / sy >= g->at.ground)
         {
            const int dx = ((int)X - CAR_X * (int)sx) / (int)sx;
            const int r2 = dx * dx + dy * dy * 4;
            if (r2 < 60 * 60)
               add = px_rgb_add(add, px_rgb_scale(0xFF7A20, fire * (unsigned)(3600 - r2) / 3600));
         }
         if (add)
            out[X] = px_rgb_add(out[X], add);
      }
   }
}

static void paint_backdrop(pp *g, px_scene *s)
{
   const unsigned sx = s->sx, sy = s->sy, w = s->w;
   const bool circuit = g->backdrop == BACKDROP_CIRCUIT;
   int32_t dx;
   bool fresh;

   if (!s->backdrop || !sx || !sy || w != PXC_W * sx)
      return;
   fresh = px_kit_canvas_fit(&g->far, s);
   if (px_kit_texture_fit(&g->grain, s) && g->grain.shades)
      paint_grain(g, s);
   if (!g->far.pixels || !g->grain.shades)
      return;
   if (fresh || g->far_sky != g->at.sky || g->far_top != g->at.top || g->far_horizon != g->at.horizon
         || g->far_colors != g->colors)
   {
      paint_far(g, s);
      g->hill_rgb = g->crest_rgb = ~0u;
   }
   if (g->hill_rgb != g->at.band)
      body_table(g, g->at.band);

   /* Black where the picture is: the backdrop shows nowhere else, but is kept black. */
   memset(s->backdrop, 0, (size_t)w * g->at.top * sy * sizeof(uint32_t));
   memset(s->backdrop + (size_t)w * g->at.bottom * sy, 0,
         (size_t)w * (s->h - g->at.bottom * sy) * sizeof(uint32_t));

   /* The sky and the mountains, turned as the hills are. */
   dx = ((g->shown < 0 ? 0 : g->shown) + 1 + TURN_OFFSET) * (int32_t)sx;
   for (unsigned Y = g->at.top * sy; Y < g->at.horizon * sy; Y++)
      px_kit_canvas_roll(&g->far, s->backdrop + (size_t)Y * w, Y, 0, w, dx, 0);
   for (unsigned y = g->at.top; y < g->at.horizon; y++)
   {
      uint32_t *top = s->top + (size_t)y * PXC_W, *bk = s->bk + (size_t)y * PXC_W;
      for (unsigned x = 0; x < PXC_W; x++)
         if (PX_KEY_CLS(top[x]) == PX_CLS_BK && (bk[x] & 0xFFFFFFu) == g->at.sky)
         {
            top[x] = PX_KEY(PX_CLS_BK, 0);
            bk[x]  = 0;
         }
   }

   /* The hills, the band at their foot and the road. */
   paint_hills(g, s, dx);
   if (circuit)
   {
      paint_road(g, s);
      for (unsigned y = g->at.ground; y < g->at.bottom; y++)
      {
         uint32_t *top = s->top + (size_t)y * PXC_W, *bk = s->bk + (size_t)y * PXC_W;
         for (unsigned x = 0; x < PXC_W; x++)
            if (PX_KEY_CLS(top[x]) == PX_CLS_BK && (bk[x] & 0xFFFFFFu) == g->at.ground_rgb)
            {
               top[x] = PX_KEY(PX_CLS_BK, 0);
               bk[x]  = 0;
            }
      }
   }
   else
      memset(s->backdrop + (size_t)w * g->at.ground * sy, 0,
            (size_t)w * (g->at.bottom - g->at.ground) * sy * sizeof(uint32_t));
   light_circuit(g, s, g->at.top, circuit ? g->at.bottom : g->at.ground);
   s->backdrop_on = true;
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

/* A row of an object as an object of its own. */
static px_instance row_of(const px_instance *in, unsigned row)
{
   px_instance one = *in;
   one.y    = (int16_t)(in->y + (int)row);
   one.h    = 1;
   one.rows = in->rows + row;
   return one;
}

static unsigned role_of(const pp *g, const px_instance *in)
{
   if (in->y < HUD_END)
      return PX_ROLE_HUD;
   if (in->cls == PXC_L_P0 && in->y >= CAR_TOP)
      return PX_ROLE_PLAYER;
   if (in->cls == PXC_L_P1 && in->y >= (int)g->at.ground && in->y < CAR_TOP)
      return PX_ROLE_ENEMY;
   return PX_ROLE_NONE;
}

/* The car's body is playfield: red, in its rows. */
static void paint_body(pp *g, px_scene *s)
{
   for (unsigned y = CAR_TOP; y < END && y < s->frame->height; y++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         const unsigned idx = s->frame->winner[i];
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_PF && g->car_map[idx] != PX_KIT_KEEP)
            s->top[i] = PX_KEY(PX_CLS_PF, g->car_map[idx]);
      }
}

/* The kerbs and the dashes: pixels of the ball and the missiles on the road. */
static void paint_kerbs(pp *g, px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   for (unsigned y = g->at.ground; y < g->at.bottom; y++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         uint32_t rgb;
         unsigned layer;
         if (!(f->tags[i] & (PXC_BL | PXC_M0 | PXC_M1)) || PX_KEY_CLS(s->top[i]) != PX_CLS_SPRITE)
            continue;
         layer = (f->tags[i] & PXC_BL) ? PXC_L_BL : (f->tags[i] & PXC_M0) ? PXC_L_M0 : PXC_L_M1;
         rgb = g->kerb_map[f->color[layer][i]];
         if (rgb == PX_KIT_KEEP || (f->tags[i] & (PXC_P0 | PXC_P1)))
            continue;
         s->top[i]    = PX_KEY(PX_CLS_SPRITE, rgb);
         s->sprite[i] = 0xFF000000u | rgb;
         s->energy[i] = 0;
      }
}

static void frame(void *state, px_scene *s)
{
   pp *g = (pp*)state;
   px_objects *o = s->objects;
   const int turned = px_kit_ram(s->ram, s->ram_size, RAM_TURN);
   const int flags = px_kit_ram(s->ram, s->ram_size, RAM_STATE);
   const int crashed = flags < 0 ? -1 : (flags & CRASHED) != 0;
   const bool arcade = g->colors == COLORS_ARCADE;
   bool road;

   if (s->advance)
   {
      g->frame++;
      /* The hills are drawn where byte 62 had them the frame before. */
      g->shown  = g->turned;
      g->turned = turned;
   }

   /* Whether this is the road: a sky over a ground, and where its edges are. */
   road = px_kit_horizon_find(s, TOP, END, &g->at) && g->at.horizon > HUD_END;
   if (road && !px_kit_road_find(&g->road, s, g->at.ground, g->at.bottom, EDGES))
   {
      /* No edge at all: the road fills the picture. */
      for (unsigned y = g->at.ground; y < g->at.bottom; y++)
      {
         g->road.left[y]  = -1000;
         g->road.right[y] = 1160;
      }
   }

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role = road ? (uint8_t)role_of(g, in) : PX_ROLE_NONE;
      if (!road || !arcade)
         continue;
      switch (in->role)
      {
         case PX_ROLE_PLAYER:
            if (crashed > 0)
               break;
            px_kit_repaint(s, in, g->car_map);
            break;
         case PX_ROLE_ENEMY:
            px_kit_repaint(s, in, g->rival_map);
            break;
         case PX_ROLE_NONE:
            /* The clouds: white on top and shaded underneath. */
            if (px_kit_is_player(in) && in->y < CLOUDS_END && lum_of(in->color) >= 0x0C)
               for (unsigned r = 0; r < in->h; r++)
               {
                  const px_instance row = row_of(in, r);
                  px_scene_tint(s, &row, px_rgb_mix(0xFFFFFF, 0xB8C8E4, r * 256 / in->h));
               }
            break;
         default:
            break;
      }
   }
   if (road && arcade && crashed <= 0)
      paint_body(g, s);
   if (road && arcade)
      paint_kerbs(g, s);

   /* The crash: fire and sparks where the car is, and where it ran into the other. */
   if (s->advance)
   {
      if (crashed > 0 && g->crashed == 0 && road && g->sparks)
      {
         int rx = -1, ry = -1;
         for (unsigned i = 0; i < o->count; i++)
            if (o->inst[i].role == PX_ROLE_ENEMY && o->inst[i].y > ry)
            {
               ry = o->inst[i].y + o->inst[i].h / 2;
               rx = o->inst[i].x + o->inst[i].w / 2;
            }
         px_scene_burst(s, CAR_X, CAR_Y, 0xFFE8A0, 60, 480);
         px_scene_burst(s, CAR_X, CAR_Y, 0xFF5A18, 40, 320);
         px_scene_burst(s, CAR_X, CAR_Y - 4, 0xFFFFFF, 16, 560);
         if (rx >= 0)
            px_scene_burst(s, rx, ry, 0xFFC860, 24, 360);
         if (g->glow < 150)
         {
            g->glow     = 150;
            g->glow_rgb = 0xFFB070;
         }
      }
      g->burning = crashed > 0 ? g->burning + 1 : 0;
      if (g->burning && g->sparks && road && g->burning % 5 == 0)
         px_scene_burst(s, CAR_X - 10 + (int)(px_kit_chance(&g->seed) % 21), CAR_Y,
               g->burning % 10 ? 0xFF8A20 : 0xFFD060, 5, 120);
      g->crashed = crashed;
   }
   if (g->burning && road)
      for (unsigned i = 0; i < o->count; i++)
         if (o->inst[i].role == PX_ROLE_PLAYER && g->sparks)
            px_scene_energy(s, &o->inst[i], true);

   if (road && g->backdrop != BACKDROP_OFF)
      paint_backdrop(g, s);
   else if (!road)
      g->glow = 0;
   if (s->advance && g->glow)
   {
      g->glow = g->glow * 210 / 256;
      if (g->glow < 6)
         g->glow = 0;
   }
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 *   voice 1   the engine: waveform 15 at volume 7, the pitch from 25 down to 9 as the car
 *             gathers speed, in either gear. Off the road the volume goes 7, 12, 7, 12, a
 *             frame or two each: the bumps.
 *   voice 1   the tyres, in a curve taken fast: waveform 3 at pitch 6, the volume 7, 11,
 *             15, 11, 7 over 20 to 44 frames, in the engine's place
 *   both      the crash: noise (waveform 8) at pitch 24 on voice 0, the volume from 15 down
 *             to 2 in 112 frames, and waveform 2 at pitch 7, then 10, at volume 15 and less
 *             on voice 1
 *   both      the tunes, before the start and when the time is up: waveform 4 on voice 0
 *             and waveform 13 on voice 1, volume 8, notes of 4 to 28 frames
 *   voice 0   now and then a blip of waveform 4, two to four frames at pitch 25 to 30;
 *             heard as the game plays it
 *
 * The engine runs on under the tyres and follows the game's pitch; the crash is a crash
 * of several voices and then a fire; the tunes are the game's notes, in brass and bass.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_ENGINE, SOUND_TYRES, SOUND_CRASH, SOUND_TUNE, SOUND_OTHER };

static unsigned voice1_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   (void)pitch;
   if (!volume)
      return SOUND_NONE;
   switch (wave)
   {
      case 15: return SOUND_ENGINE;
      case 3:  return SOUND_TYRES;
      case 2:  return SOUND_CRASH;
      case 13: return SOUND_TUNE;
      default: return SOUND_OTHER;
   }
}

static unsigned voice0_plays(unsigned wave, unsigned pitch, unsigned volume, unsigned heard1)
{
   if (!volume)
      return SOUND_NONE;
   if (wave == 8 && pitch == 24)
      return SOUND_CRASH;
   if (wave == 4 && heard1 == SOUND_TUNE)
      return SOUND_TUNE;
   return SOUND_OTHER;
}

static void stop_all(unsigned *ids, unsigned n, px_sound *s, float seconds)
{
   for (unsigned i = 0; i < n; i++)
   {
      px_synth_stop(s->synth, ids[i], seconds);
      ids[i] = 0;
   }
}

/* The engine: a howl on the game's pitch, a growl an octave under it, the whine of the
 * gears and the rush of air, all of which rise with it. */
static void play_engine(pp *g, px_sound *s)
{
   static const px_tone tones[4] = {
      /* wave          freq  to  glide  attack  hold  decay  gain  cutoff to  vibrato */
      { PX_WAVE_SAW,    120, 0, 0,     0.15f,  0,    0,     1.0f, 1400, 0, 6.0f, 0.004f },
      { PX_WAVE_SQUARE,  60, 0, 0,     0.15f,  0,    0,     1.0f,  420, 0, 0, 0 },
      { PX_WAVE_SINE,   480, 0, 0,     0.15f,  0,    0,     1.0f,    0, 0, 0, 0 },
      { PX_WAVE_NOISE, 2400, 0, 0,     0.30f,  0,    0,     1.0f, 1800, 0, 0, 0 }
   };
   /* From 0 at a standstill to 1 at the top of a gear. */
   const float fast = (25.0f - (float)g->engine_pitch) / 16.0f;
   const float hz = px_kit_tia_hz(15, (unsigned)g->engine_pitch) * 6.0f;
   const float freq[4] = { hz, hz * 0.5f, hz * 4.0f, 1800.0f + 2400.0f * fast };
   const float gain[4] = { 0.32f, 0.28f, 0.025f + 0.04f * fast, 0.06f + 0.12f * fast };

   for (unsigned i = 0; i < 4; i++)
      if (!px_synth_move(s->synth, g->engine[i], 0.0f, gain[i], freq[i]))
      {
         px_tone t = tones[i];
         t.freq = freq[i];
         g->engine[i] = px_synth_play(s->synth, &t, 0.0f, gain[i]);
      }
}

/* The tyres: two screeches a little apart, and their hiss, as loud as the game's. */
static void play_tyres(pp *g, px_sound *s, unsigned volume)
{
   static const px_tone tones[2] = {
      { PX_WAVE_TRIANGLE, 1850, 0, 0, 0.03f, 0, 0, 1.0f, 0, 0, 9.0f, 0.020f },
      { PX_WAVE_NOISE,    5200, 0, 0, 0.03f, 0, 0, 1.0f, 4200, 0, 0, 0 }
   };
   const float gain = 0.04f + 0.02f * (float)volume;
   for (unsigned i = 0; i < 2; i++)
      if (!px_synth_move(s->synth, g->squeal[i], 0.0f, gain * (i ? 0.6f : 1.0f), 0))
         g->squeal[i] = px_synth_play(s->synth, &tones[i], 0.0f, gain * (i ? 0.6f : 1.0f));
   px_sound_rumble(s, 0, 6000 + 900 * volume, 2);
}

/* A bump off the road. */
static void play_bump(px_sound *s)
{
   static const px_tone p[2] = {
      { PX_WAVE_SINE,   75, 38, 0.08f, 0.002f, 0.01f, 0.12f, 0.45f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 900, 300, 0.06f, 0,     0.01f, 0.08f, 0.22f, 700, 200, 0, 0 }
   };
   px_kit_play(s, p, 2, 0.0f);
   px_sound_rumble(s, 22000, 16000, 3);
}

/* The crash: a blow, metal torn and scraped, glass and parts, and a boom under it. */
static void play_crash(px_sound *s)
{
   static const px_tone p[6] = {
      { PX_WAVE_NOISE, 7000,  250, 1.60f, 0,      0.10f, 2.00f, 0.46f, 6000, 150, 0, 0 },
      { PX_WAVE_SINE,    68,   24, 1.10f, 0.002f, 0.15f, 1.50f, 0.90f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    900,  110, 0.90f, 0.001f, 0.05f, 1.00f, 0.20f, 3000, 300, 11.0f, 0.06f },
      { PX_WAVE_SQUARE, 1400, 700, 0.35f, 0.001f, 0.02f, 0.45f, 0.08f, 5000, 1500, 23.0f, 0.10f },
      { PX_WAVE_NOISE, 10000, 6000, 0.25f, 0,     0.02f, 0.40f, 0.20f, 9000, 5000, 0, 0 },
      { PX_WAVE_NOISE,   800,  120, 2.20f, 0.20f, 0.30f, 2.60f, 0.28f, 700, 100, 0, 0 }
   };
   px_kit_play(s, p, 6, 0.0f);
   px_sound_rumble(s, 65535, 50000, 45);
}

/* The fire while the car burns, as loud as the game's noise, and pops in it. */
static void play_fire(pp *g, px_sound *s, unsigned volume)
{
   static const px_tone roar = { PX_WAVE_NOISE, 1100, 0, 0, 0.10f, 0, 0, 1.0f, 900, 0, 0, 0 };
   static const px_tone pop  = { PX_WAVE_NOISE, 3500, 900, 0.04f, 0, 0.005f, 0.06f, 1.0f, 4000, 900, 0, 0 };
   const float gain = 0.045f * (float)volume;
   if (!px_synth_move(s->synth, g->fire, 0.1f, gain, 0))
      g->fire = px_synth_play(s->synth, &roar, 0.1f, gain);
   if ((px_kit_chance(&g->seed) & 7) == 0)
      px_synth_play(s->synth, &pop, ((float)(px_kit_chance(&g->seed) % 100) - 50.0f) / 120.0f,
            0.05f + 0.01f * (float)volume);
}

/* A note of the game's tunes: brass on voice 0's, a bass on voice 1's. */
static void play_note(pp *g, px_sound *s, unsigned voice)
{
   static const px_tone brass = { PX_WAVE_SAW,      1, 0, 0, 0.012f, 0, 0, 0.34f, 2600, 0, 5.0f, 0.006f };
   static const px_tone bass  = { PX_WAVE_TRIANGLE, 1, 0, 0, 0.008f, 0, 0, 0.60f, 0, 0, 0, 0 };
   const float hz = px_kit_tune(px_kit_tia_hz(g->tia.wave[voice], g->tia.pitch[voice]));
   const float f = voice ? hz * 0.5f : hz;
   px_tone t = voice ? bass : brass;

   if (hz <= 0.0f)
      return;
   if (g->tune[voice] && g->tune_pitch[voice] == g->tia.pitch[voice]
         && px_synth_move(s->synth, g->tune[voice], voice ? 0.25f : -0.25f, 1.0f, 0))
      return;
   px_synth_stop(s->synth, g->tune[voice], 0.02f);
   t.freq = f;
   g->tune[voice] = px_synth_play(s->synth, &t, voice ? 0.25f : -0.25f, 1.0f);
   g->tune_pitch[voice] = g->tia.pitch[voice];
   if (!voice)
   {
      /* An octave over it, softer. */
      static const px_tone over = { PX_WAVE_SQUARE, 1, 0, 0, 0.012f, 0.05f, 0.25f, 0.10f, 3500, 0, 0, 0 };
      px_tone o = over;
      o.freq = f * 2.0f;
      px_synth_play(s->synth, &o, -0.25f, 1.0f);
   }
}

/* A car overtaken: the rush of it going by, lower as it goes. */
static void play_passing(px_sound *s, int x)
{
   static const px_tone p[2] = {
      { PX_WAVE_NOISE, 2600, 500, 0.45f, 0.05f, 0.05f, 0.40f, 0.26f, 2400, 400, 0, 0 },
      { PX_WAVE_SAW,    330, 190, 0.40f, 0.04f, 0.05f, 0.35f, 0.07f, 1200, 400, 0, 0 }
   };
   px_kit_play(s, p, 2, px_kit_pan(x));
}

static void sound(void *state, px_sound *s)
{
   pp *g = (pp*)state;
   unsigned heard0, heard1;
   int rival_y = -1, rival_x = -1;

   /* The other car nearest, from the picture before. */
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_ENEMY && !in->ghost && in->y > rival_y)
      {
         rival_y = in->y;
         rival_x = in->x + in->w / 2;
      }
   }

   px_kit_tia_hear(&g->tia, s->frame);
   heard1 = voice1_plays(g->tia.wave[1], g->tia.pitch[1], g->tia.volume[1]);
   heard0 = voice0_plays(g->tia.wave[0], g->tia.pitch[0], g->tia.volume[0], heard1);

   /* The crash. */
   if (heard0 == SOUND_CRASH && g->heard0 != SOUND_CRASH)
   {
      if (g->own_sound) play_crash(s);
      else              px_sound_rumble(s, 65535, 50000, 45);
   }
   if (g->own_sound && heard0 == SOUND_CRASH)
      play_fire(g, s, g->tia.volume[0]);
   else if (g->fire)
   {
      px_synth_stop(s->synth, g->fire, 0.4f);
      g->fire = 0;
   }

   /* The engine, under the tyres; not while the car burns or a tune plays. */
   if (heard1 == SOUND_ENGINE)
      g->engine_pitch = g->tia.pitch[1] < 9 ? 9 : g->tia.pitch[1] > 25 ? 25 : g->tia.pitch[1];
   if (g->own_sound && (heard1 == SOUND_ENGINE || heard1 == SOUND_TYRES))
      play_engine(g, s);
   else
      stop_all(g->engine, 4, s, heard0 == SOUND_CRASH ? 0.05f : 0.3f);

   if (heard1 == SOUND_ENGINE && g->tia.volume[1] >= 10 && px_kit_tia_louder(&g->tia, 1))
   {
      if (g->own_sound) play_bump(s);
      else              px_sound_rumble(s, 22000, 16000, 3);
   }
   if (heard1 == SOUND_TYRES && g->own_sound)
      play_tyres(g, s, g->tia.volume[1]);
   else
   {
      if (heard1 == SOUND_TYRES)
         px_sound_rumble(s, 0, 6000 + 900 * g->tia.volume[1], 2);
      stop_all(g->squeal, 2, s, 0.12f);
   }

   /* The tunes, note for note. */
   for (unsigned v = 0; v < 2; v++)
   {
      const unsigned heard = v ? heard1 : heard0;
      if (g->own_sound && heard == SOUND_TUNE)
         play_note(g, s, v);
      else if (g->tune[v])
      {
         px_synth_stop(s->synth, g->tune[v], 0.06f);
         g->tune[v] = 0;
      }
   }

   /* A car that was about to be overtaken and is gone has gone by. */
   if (g->own_sound && g->passing && g->rival_y >= 166 && rival_y < 150 && heard0 != SOUND_CRASH)
      play_passing(s, g->rival_x);
   g->rival_y = rival_y;
   g->rival_x = rival_x;

   if (g->own_sound)
   {
      if (heard0 == SOUND_CRASH || heard0 == SOUND_TUNE)
         s->voice[0] = 0.0f;
      if (heard1 != SOUND_OTHER && heard1 != SOUND_NONE)
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
   pp *g = (pp*)state;
   g->turned  = -1;
   g->shown   = -1;
   g->crashed = -1;
   g->burning = 0;
   g->glow    = 0;
   px_kit_tia_reset(&g->tia);
   g->heard0 = g->heard1 = SOUND_NONE;
   memset(g->engine, 0, sizeof(g->engine));
   memset(g->squeal, 0, sizeof(g->squeal));
   memset(g->tune, 0, sizeof(g->tune));
   g->fire = 0;
   g->engine_pitch = 25;
   g->rival_y = g->rival_x = -1;
   g->seed = 0x9E11u;
}

static void *create(void)
{
   pp *g = (pp*)calloc(1, sizeof(pp));
   if (g)
   {
      g->sparks = g->own_sound = g->passing = true;
      g->hill_rgb = g->crest_rgb = ~0u;
      make_maps(g);
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   pp *g = (pp*)state;
   if (g)
   {
      px_kit_canvas_free(&g->far);
      px_kit_texture_free(&g->grain);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   pp *g = (pp*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_pick(get, OPT_BACKDROP, backdrop);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->passing   = px_kit_on(get, OPT_PASSING);
   g->hill_rgb  = g->crest_rgb = ~0u;
}

const px_game px_game_pole_position = {
   "Pole Position", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
