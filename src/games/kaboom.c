/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Kaboom! (Activision, 1981).
 *
 * The Mad Bomber walks along the top of a wall and drops bombs; the player, with a paddle,
 * moves three buckets of water, one above the other, to catch them. A bomb that reaches the
 * ground goes off, and every other bomb on the screen goes off after it, one by one; then
 * the screen flashes and the lowest bucket is gone. With none left, the game is over.
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  18..57   the wall: background in grey ($06), columns 8..151. Black around it.
 *   rows  19..26   the score, on the wall: player 1, three copies at column 91, yellow ($1A)
 *   rows  30..59   the Mad Bomber: player 1, 8 by 30, a colour a row (hat $10..$14, mask
 *                  and stripes $02, face $38 and $3A, shirt $0C). Two shapes: he smiles
 *                  while bombs go off.
 *   rows  58..199  the field: background in green ($D4)
 *   bombs          player 0, 8 wide, 12 to 15 rows, a colour a row: the spark of the fuse
 *                  ($4E) at the top, the fuse ($08), the bomb in $00..$06. The spark comes
 *                  and goes, which makes the bomb 12 rows high with $08 on top. A bomb that
 *                  goes off is a splash of 7, then 13 or 15 rows in a colour that changes
 *                  every frame
 *   rows 159..198  the buckets: player 1, double width (16 by 8), at rows 159, 175 and 191
 *                  while there are three. The rim $16, water ($86) under it, then $14 and
 *                  $12. A bucket lost is the lowest.
 *   rows 201..207  Activision's name: players 0 and 1, $88
 *
 * Of its memory ($80 is 0): byte 5 is the wall's colour and byte 6 the field's; 9..16 the
 * bomb's shades, 17..19 the fuse's and 20..23 the spark's. Byte 26 is the Bomber's column
 * (less 17), byte 29 the buckets' (less 18), a little behind the paddle. Bytes 35..37 are
 * the score in six digits of BCD. Byte 33 is how many buckets are left. When a bucket is
 * lost, byte 46 counts from $1F down to 0 (it has other uses besides), and while it does,
 * the colours of the wall, the field, the score and the bombs are their own with the
 * count's bits flipped (an exclusive or): the screen flashes. The Bomber and the buckets keep
 * their colours. At 0 the bucket is taken away. The module tells the flash by byte 5, the
 * wall's colour, against its own.
 *
 * The game needs paddles; the test program has --paddles.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define LEFT        8       /* the columns of the wall and the field */
#define RIGHT       152
#define WALL_TOP    18      /* the rows of the wall */
#define FIELD_TOP   58      /* the rows of the field */
#define FIELD_END   200
#define SCORE_END   28      /* the score is above this */
#define BOMBER_END  62      /* the Bomber is above this */
#define BUCKET_TOP  150     /* the buckets are below this */
#define BUCKET_GAP  16      /* from one bucket to the next */
#define FIRST_BUCKET 159
#define BUCKETS     3

#define WALL_COLOR  0x06    /* the colours of the wall and the field when memory is not had */
#define FIELD_COLOR 0xD4

#define BOMBS       16      /* at most on the screen, and more */
#define STARS       48

enum { TAG_FALLS = 1, TAG_WENT_OFF };

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "5428cdfada281c569c74c7308c7f2c26",   /* Kaboom! (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "70",
   "reverb", "room",
   NULL
};

#define OPT_COLORS   "proteus_kb_colors"
#define OPT_BACKDROP "proteus_kb_backdrop"
#define OPT_SPARKS   "proteus_kb_sparks"
#define OPT_SOUND    "proteus_kb_sound"

static const char *const colors[] = { "proteus", "Proteus's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "city", "Rooftops at night", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_PROTEUS = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Steel bombs with burning fuses, brass buckets of bright water and a crisper Mad Bomber, or the game's own colours.",
     "proteus", colors },
   { OPT_BACKDROP, "Backdrop",
     "A brick wall under the Mad Bomber and a city's rooftops at night where the bombs fall, in place of the game's grey and green.",
     "city", backdrop },
   { OPT_SPARKS, "Explosions",
     "Fire where a bomb goes off, a splash where a bucket catches one, and a blast where a bucket is lost.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the fuses, a bomb caught, a bomb going off and a bucket lost, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   int16_t x, y, h;
   uint32_t track;
} bomb;

typedef struct
{
   uint16_t x, y;          /* in the picture */
   uint8_t phase, pace, size;
   uint32_t rgb;
} star;

typedef struct
{
   unsigned colors;
   bool backdrop, sparks, own_sound;

   uint32_t frame;
   px_kit_tags known;      /* the bombs: TAG_FALLS, or TAG_WENT_OFF once they burst */

   /* What memory said in the frame before; -1: not known. */
   long score;
   int buckets;
   int flash;

   bomb falling[BOMBS];    /* the bombs that fell in the frame before */
   unsigned falling_count;
   int bucket_x;           /* the buckets' column in the frame before, -1: not seen */
   int splash;             /* frames left of a bucket's water shining; which bucket */
   unsigned splash_bucket;

   /* The colours of the game's indices, for the Bomber, a bomb and a bucket. */
   uint32_t bomber_map[256], bomb_map[256], bucket_map[256];
   uint32_t strobed[256];

   px_kit_canvas city;     /* the backdrop without its stars */
   star stars[STARS];
   uint32_t seed;

   /* The sound. */
   px_kit_tia tia;
   unsigned heard;         /* what voice 0 played in the frame before: SOUND_* */
   unsigned fuse_quiet;    /* frames since a fuse was heard */
   unsigned sizzle, crackle_seed;   /* the fuse's voice at the synth */
   int fuse_x, went_off_x, catch_x; /* where things are, from the picture before */
} kb;

/* ---------------------------------------------------------------------------
 * The colours
 * ------------------------------------------------------------------------- */

static void make_maps(kb *g)
{
   for (unsigned i = 0; i < 256; i++)
   {
      const unsigned hue = i >> 4, lum = i & 0x0E;
      uint32_t bomber = PX_KIT_KEEP, bombc = PX_KIT_KEEP, bucket = PX_KIT_KEEP;

      /* The Bomber: a black mask and black and white stripes, an olive hat, a warm face. */
      if (hue == 0)
         bomber = lum <= 2 ? 0x17161E : lum >= 0x0A ? 0xF6F2E6 : 0x807C88;
      else if (hue == 1)
         bomber = lum <= 0 ? 0x2A3418 : lum <= 2 ? 0x46562A : 0x68803C;
      else if (hue == 3)
         bomber = lum <= 8 ? 0xDC845A : 0xF4AE80;

      /* A bomb of blued steel, lit from the upper left, and a fuse of tarred rope. */
      if (hue == 0)
         switch (lum)
         {
            case 0x00: bombc = 0x0A0C12; break;
            case 0x02: bombc = 0x283044; break;
            case 0x04: bombc = 0x4A5670; break;
            case 0x06: bombc = 0x7888AC; break;
            default:   bombc = 0xC8A474; break;
         }
      else if (hue == 4)
         bombc = 0xFFD050;   /* the spark: made to flicker in frame() */

      /* Brass buckets full of bright water. */
      if (hue == 1)
         bucket = lum <= 2 ? 0x96601A : lum <= 4 ? 0xC98E26 : lum <= 6 ? 0xF4C448 : 0xFFE27A;
      else if (hue >= 7 && hue <= 10)
         bucket = 0x38C4FF;

      g->bomber_map[i] = bomber;
      g->bomb_map[i]   = bombc;
      g->bucket_map[i] = bucket;
   }
}

/* While the game flashes, every colour is its own with some bits flipped: the map for what
 * the colour was. */
static const uint32_t *unflip(kb *g, const uint32_t *map, unsigned flip)
{
   if (!flip)
      return map;
   for (unsigned i = 0; i < 256; i++)
      g->strobed[i] = map[(i ^ flip) & 0xFF];
   return g->strobed;
}

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

static uint32_t hash(uint32_t a)
{
   a ^= a >> 16;
   a *= 0x7FEB352Du;
   a ^= a >> 15;
   a *= 0x846CA68Bu;
   a ^= a >> 16;
   return a;
}

/* The wall the Mad Bomber walks on: bricks in courses, a coping of slate on top, lit from
 * below by the city. */
static uint32_t wall_pixel(unsigned X, unsigned Y, unsigned sx, unsigned sy)
{
   const unsigned top = WALL_TOP * sy, coping = (WALL_TOP + 3) * sy;
   const unsigned mortar_h = sy >= 3 ? 2 : 1, mortar_w = sx >= 4 ? 2 : 1;
   unsigned course, in_y, brick, in_x, shade, lit;
   uint32_t rgb, cell;

   if (Y < coping)
   {
      /* A lighter edge along the top of the coping. */
      rgb = Y < top + (sy > 1 ? sy / 2 : 1) ? 0x6A6878 : 0x3A3A48;
      return rgb;
   }
   course = (Y - coping) / (5 * sy);
   in_y = (Y - coping) % (5 * sy);
   X += (course & 1) * 6 * sx;
   brick = X / (12 * sx);
   in_x = X % (12 * sx);
   if (in_y < mortar_h || in_x < mortar_w)
      return 0x1E1618;
   cell = hash(course * 131u + brick * 7919u);
   shade = 200 + (cell >> 24) % 56;
   rgb = (cell >> 8) % 5 == 0 ? 0x6A3A28 : (cell >> 8) % 3 == 0 ? 0x7E3624 : 0x72301F;
   /* The upper edge of a brick catches a little light. */
   if (in_y < mortar_h + (sy > 1 ? sy / 2 : 1))
      shade += 24;
   /* Lit from below, darker at the top. */
   lit = 150 + (Y - coping) * 106 / ((FIELD_TOP - WALL_TOP - 3) * sy);
   return px_rgb_scale(px_rgb_scale(rgb, shade > 256 ? 256 : shade), lit > 256 ? 256 : lit);
}

/* The skyline: a building a stretch of columns, its roof at a row of its own. */
static unsigned roof_of(unsigned column, unsigned *building)
{
   unsigned x = 0, k = 0;
   for (;;)
   {
      const uint32_t c = hash(k * 2654435761u + 17u);
      const unsigned width = 9 + c % 14;
      if (column < x + width || x + width >= PXC_W)
      {
         *building = k;
         return 164 + (c >> 8) % 22 - ((c >> 16) % 5 == 0 ? 10u : 0u);
      }
      x += width;
      k++;
   }
}

/* The field: a night sky over a city, with a moon, darkest at the top and warm at the
 * rooftops, where the lights of the city are. */
static uint32_t field_pixel(unsigned X, unsigned Y, unsigned sx, unsigned sy, unsigned w, unsigned h)
{
   const unsigned y = Y / sy, x = X / sx;
   const unsigned down = (Y - FIELD_TOP * sy) * 256 / ((FIELD_END - FIELD_TOP) * sy);
   unsigned building, roof;
   uint32_t rgb;
   /* In a picture of 640 by 456, where a circle is round. */
   const int u = (int)(X * 640 / w), v = (int)(Y * 456 / h);
   const int mu = u - 470, mv = v - 170;
   const int d2 = mu * mu + mv * mv;

   rgb = px_rgb_mix(0x070A22, 0x1C1646, down < 160 ? down * 256 / 160 : 256);
   if (down > 150)
      rgb = px_rgb_mix(rgb, 0x5C2A44, (down - 150) * 256 / 106 > 256 ? 256 : (down - 150) * 256 / 106);

   /* The moon, and the light around it. */
   if (d2 < 30 * 30)
   {
      /* Seas on it, darker. */
      const uint32_t spot = hash((uint32_t)((u / 6) * 97 + (v / 6) * 13));
      rgb = (spot & 7) == 0 ? 0xC8C4A8 : 0xECE6C8;
      if (d2 > 27 * 27)
         rgb = px_rgb_mix(rgb, 0xB0AC98, 128);
   }
   else if (d2 < 110 * 110)
   {
      const unsigned near = (unsigned)(110 * 110 - d2) * 256 / (110 * 110 - 30 * 30);
      rgb = px_rgb_add(rgb, px_rgb_scale(0x2A2A3A, near * near >> 8));
   }

   /* The rooftops. */
   roof = roof_of(x, &building);
   if (y >= roof)
   {
      const uint32_t c = hash(building * 977u + 5u);
      const unsigned sub_x = X % (4 * sx), sub_y = Y % (6 * sy);
      rgb = (c & 1) ? 0x14122A : 0x1A1630;
      /* Windows, two pixels by three, some of them lit. */
      if (y >= roof + 3 && sub_x >= sx && sub_x < 3 * sx && sub_y >= 2 * sy && sub_y < 5 * sy)
      {
         const uint32_t lit = hash(building * 31u + (X / (4 * sx)) * 7u + (Y / (6 * sy)) * 131u);
         if (lit % 100 < 34)
            rgb = px_rgb_scale((lit >> 8) % 4 == 0 ? 0xA8D0FF : 0xFFC860, 110 + (lit >> 12) % 80);
         else
            rgb = 0x221E38;
      }
      else if (y == roof)
         rgb = 0x2C2648;   /* the edge of a roof, in the light of the sky */
   }
   return rgb;
}

static void paint_city(kb *g, const px_scene *s)
{
   const unsigned w = g->city.w, h = g->city.h, sx = s->sx, sy = s->sy;
   for (unsigned Y = 0; Y < h; Y++)
   {
      uint32_t *out = g->city.pixels + (size_t)Y * w;
      const unsigned y = Y / sy;
      for (unsigned X = 0; X < w; X++)
      {
         const unsigned x = X / sx;
         if (x < LEFT || x >= RIGHT || y < WALL_TOP || y >= FIELD_END)
            out[X] = 0;
         else if (y < FIELD_TOP)
            out[X] = wall_pixel(X - LEFT * sx, Y, sx, sy);
         else
            out[X] = field_pixel(X, Y, sx, sy, w, h);
      }
   }

   /* The stars that twinkle: in the sky, above the rooftops and away from the moon. */
   g->seed = 0x6B0Bu;
   for (unsigned i = 0; i < STARS; i++)
   {
      star *st = &g->stars[i];
      const unsigned span_y = 90 * sy;
      st->x     = (uint16_t)(LEFT * sx + px_kit_chance(&g->seed) % ((RIGHT - LEFT) * sx));
      st->y     = (uint16_t)(FIELD_TOP * sy + sy + px_kit_chance(&g->seed) % span_y);
      st->phase = (uint8_t)px_kit_chance(&g->seed);
      st->pace  = (uint8_t)(1 + px_kit_chance(&g->seed) % 4);
      st->size  = (uint8_t)(w >= 1200 ? 2 : 1);
      st->rgb   = px_kit_chance(&g->seed) % 5 == 0 ? 0xFFE0B8 : 0xDCE4FF;
   }
}

static void put_star(const kb *g, uint32_t *out, const star *st, uint32_t rgb)
{
   for (unsigned t = 0; t < st->size; t++)
      for (unsigned u = 0; u < st->size; u++)
      {
         const unsigned X = st->x + u, Y = st->y + t;
         if (X < g->city.w && Y < g->city.h)
         {
            const size_t i = (size_t)Y * g->city.w + X;
            out[i] = rgb ? px_rgb_add(g->city.pixels[i], rgb) : g->city.pixels[i];
         }
      }
}

static void paint_backdrop(kb *g, px_scene *s)
{
   if (!s->backdrop || !s->sx || !s->sy)
      return;
   if (px_kit_canvas_fit(&g->city, s))
   {
      paint_city(g, s);
      memcpy(s->backdrop, g->city.pixels, (size_t)s->w * s->h * sizeof(uint32_t));
   }
   else if (!g->city.pixels)
      return;
   else if (s->backdrop_stale)
      memcpy(s->backdrop, g->city.pixels, (size_t)s->w * s->h * sizeof(uint32_t));

   if (s->advance)
      for (unsigned i = 0; i < STARS; i++)
      {
         star *st = &g->stars[i];
         const unsigned wave = px_kit_wave(st->phase + g->frame * st->pace);
         const size_t at = (size_t)st->y * g->city.w + st->x;
         /* A star behind a lit window or the moon is not drawn. */
         if (st->x >= g->city.w || st->y >= g->city.h || (g->city.pixels[at] & 0xC0C0C0u))
            continue;
         put_star(g, s->backdrop, st, px_rgb_scale(st->rgb, 40 + (wave * 150 >> 8)));
      }
   s->backdrop_on = true;
}

/* Where the game has its wall and its field, the background is made dark: the backdrop
 * shows there. */
static void show_backdrop(px_scene *s, uint32_t wall, uint32_t field)
{
   const unsigned h = s->frame->height < FIELD_END ? s->frame->height : FIELD_END;
   for (unsigned y = WALL_TOP; y < h; y++)
   {
      uint32_t *top = s->top + (size_t)y * PXC_W;
      uint32_t *bk  = s->bk + (size_t)y * PXC_W;
      const uint32_t own = y < FIELD_TOP ? wall : field;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const unsigned cls = PX_KEY_CLS(top[x]);
         if ((cls == PX_CLS_BK || cls == PX_CLS_PF) && (top[x] & 0xFFFFFFu) == own)
         {
            top[x] = PX_KEY(PX_CLS_BK, 0);
            bk[x]  = 0;
         }
      }
   }
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

static unsigned role_of(const px_instance *in)
{
   if (in->y >= FIELD_END)
      return PX_ROLE_NONE;              /* Activision's name */
   if (in->cls == PXC_L_P1)
   {
      if (in->y < SCORE_END && in->h <= 10)
         return PX_ROLE_HUD;
      if (in->y < BOMBER_END)
         return PX_ROLE_ENEMY;
      if (in->y >= BUCKET_TOP)
         return PX_ROLE_PLAYER;
   }
   else if (in->cls == PXC_L_P0 && in->y >= SCORE_END)
      return PX_ROLE_BOMB;
   return PX_ROLE_NONE;
}

/* A bomb that goes off: it is a splash (7 rows), or its colour is neither the spark's nor
 * the fuse's. */
static bool goes_off(const px_instance *in, unsigned flip)
{
   const unsigned c = (in->color ^ flip) & 0xFE;
   return in->h <= 8 || (c != 0x4E && c != 0x08);
}

static unsigned bucket_at(int y)
{
   const int k = (y - FIRST_BUCKET + BUCKET_GAP / 2) / BUCKET_GAP;
   return k < 0 ? 0u : k >= BUCKETS ? BUCKETS - 1u : (unsigned)k;
}

static long bcd(int a, int b, int c)
{
   if (a < 0 || b < 0 || c < 0)
      return -1;
   return ((a >> 4) * 10 + (a & 15)) * 10000L + ((b >> 4) * 10 + (b & 15)) * 100L
         + (c >> 4) * 10 + (c & 15);
}

static void frame(void *state, px_scene *s)
{
   kb *g = (kb*)state;
   px_objects *o = s->objects;
   const uint32_t *palette = s->frame->palette;
   const int wall_idx = px_kit_ram(s->ram, s->ram_size, 5);
   const int field_idx = px_kit_ram(s->ram, s->ram_size, 6);
   /* What the flash flips of the colours in memory: 0 while the game does not flash. */
   const unsigned flip = wall_idx >= 0 ? ((unsigned)wall_idx ^ WALL_COLOR) & 0x1F : 0;
   const int flash = (int)flip;
   const long score = bcd(px_kit_ram(s->ram, s->ram_size, 35), px_kit_ram(s->ram, s->ram_size, 36),
         px_kit_ram(s->ram, s->ram_size, 37));
   const int buckets = px_kit_ram(s->ram, s->ram_size, 33);
   bomb falling[BOMBS];
   unsigned falling_count = 0;
   int bucket_x = -1, lowest_bucket = -1;
   const unsigned spark_wave = px_kit_wave(g->frame * 29);
   const uint32_t spark = px_rgb_mix(0xFFF2A0, 0xFF6A18, spark_wave);

   if (s->advance)
   {
      g->frame++;
      px_kit_tags_begin(&g->known);
   }

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role  = (uint8_t)role_of(in);
      in->group = 0;
      if (in->role == PX_ROLE_PLAYER && !in->ghost)
      {
         bucket_x = in->x + in->w / 2;
         if (in->y > lowest_bucket)
            lowest_bucket = in->y;
      }
      else if (in->role == PX_ROLE_BOMB)
      {
         const bool off = goes_off(in, flip);
         in->group = off ? 1 : 0;
         if (s->advance && !in->ghost)
         {
            px_kit_tagged *t = px_kit_tags_keep(&g->known, in, TAG_FALLS);
            if (off && t && t->tag != TAG_WENT_OFF)
            {
               /* It goes off: fire where it was. */
               t->tag = TAG_WENT_OFF;
               if (g->sparks)
               {
                  px_scene_burst(s, in->x + 4, in->y + in->h / 2, 0xFFB030, 34, 300);
                  px_scene_burst(s, in->x + 4, in->y + in->h / 2, 0xFF4A10, 18, 190);
                  px_scene_burst(s, in->x + 4, in->y + in->h / 2, 0xFFF4D0, 8, 420);
                  px_scene_flash(s, 0xFF7A20, 34);
               }
            }
            if (!off && falling_count < BOMBS)
            {
               falling[falling_count].x = in->x;
               falling[falling_count].y = in->y;
               falling[falling_count].h = (int16_t)in->h;
               falling[falling_count].track = in->track;
               falling_count++;
            }
         }
      }
   }

   if (s->advance && g->sparks && score >= 0 && g->score >= 0 && score > g->score
         && score - g->score < 100 && !flash)
   {
      /* A bomb caught: the one that fell furthest of those that are gone. */
      int best = -1;
      for (unsigned k = 0; k < g->falling_count; k++)
      {
         bool gone = true;
         for (unsigned j = 0; j < falling_count; j++)
            gone = gone && falling[j].track != g->falling[k].track;
         if (gone && g->falling[k].y + g->falling[k].h >= BUCKET_TOP - 8
               && (best < 0 || g->falling[k].y > g->falling[best].y))
            best = (int)k;
      }
      if (best >= 0 || g->bucket_x >= 0)
      {
         const int x = best >= 0 ? g->falling[best].x + 4 : g->bucket_x;
         const int y = best >= 0 ? g->falling[best].y + g->falling[best].h : FIRST_BUCKET;
         g->splash_bucket = bucket_at(y);
         g->splash = 12;
         px_scene_burst(s, x, y, 0x9AE4FF, 22, 170);
         px_scene_burst(s, x, y, 0xFFFFFF, 8, 240);
         px_scene_burst(s, x, y - 3, 0xFFA040, 5, 120);   /* the fuse, put out */
      }
   }

   if (s->advance && g->sparks && flash > 0 && g->flash == 0)
   {
      /* The last bomb went off: the lowest bucket goes up with it. */
      const int y = lowest_bucket >= 0 ? lowest_bucket + 4 : FIRST_BUCKET + BUCKET_GAP * 2 + 4;
      const int x = bucket_x >= 0 ? bucket_x : 80;
      px_scene_burst(s, x, y, 0xFFC040, 60, 460);
      px_scene_burst(s, x, y, 0xFF3A10, 40, 320);
      px_scene_burst(s, x, y, 0x70D0FF, 24, 260);
      px_scene_flash(s, 0xFFE8C0, 190);
   }
   if (s->advance && g->sparks && buckets >= 0 && g->buckets > buckets && buckets < BUCKETS)
   {
      /* The bucket is taken away: what is left of it. */
      const int y = FIRST_BUCKET + BUCKET_GAP * buckets + 4;
      const int x = g->bucket_x >= 0 ? g->bucket_x : 80;
      px_scene_burst(s, x, y, 0xF4C448, 26, 280);
      px_scene_burst(s, x, y, 0x38C4FF, 16, 200);
   }

   /* The colours. */
   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      if (g->colors == COLORS_ORIGINAL)
         break;
      switch (in->role)
      {
         case PX_ROLE_ENEMY:
            px_kit_repaint(s, in, g->bomber_map);
            break;
         case PX_ROLE_HUD:
            px_scene_tint(s, in, 0xFFD23A);
            break;
         case PX_ROLE_PLAYER:
         {
            uint32_t map[256];
            const uint32_t *base = g->bucket_map;
            const bool shines = g->splash > 0 && bucket_at(in->y) == g->splash_bucket;
            memcpy(map, base, sizeof(map));
            if (shines)
               for (unsigned k = 0; k < 256; k++)
                  if (map[k] == 0x38C4FF)
                     map[k] = px_rgb_mix(0x38C4FF, 0xE8FAFF, (unsigned)g->splash * 12);
            px_kit_repaint(s, in, map);
            if (shines)
               px_scene_energy(s, in, true);
            break;
         }
         case PX_ROLE_BOMB:
            if (in->group)
            {
               /* Going off: white hot to red, and brighter. */
               px_scene_tint(s, in, px_rgb_mix(0xFFD868, 0xFF3A10,
                     px_kit_wave(g->frame * 41 + in->track * 64)));
               px_scene_energy(s, in, true);
            }
            else
            {
               uint32_t map[256];
               memcpy(map, unflip(g, g->bomb_map, flip), sizeof(map));
               for (unsigned k = 0; k < 256; k++)
                  if (((k ^ flip) >> 4) == 4)
                     map[k] = spark;
               px_kit_repaint(s, in, map);
            }
            break;
         default:
            break;
      }
   }

   if (g->backdrop)
   {
      const uint32_t wall  = palette[wall_idx >= 0 ? (unsigned)wall_idx : WALL_COLOR ^ flip] & 0xFFFFFFu;
      const uint32_t field = palette[field_idx >= 0 ? (unsigned)field_idx : FIELD_COLOR ^ flip] & 0xFFFFFFu;
      paint_backdrop(g, s);
      if (s->backdrop_on)
      {
         show_backdrop(s, wall, field);
         /* The game flashes its colours when a bucket is lost: the city flickers with the fire. */
         if (flash > 0 && s->advance)
            px_scene_flash(s, 0xFFA868, 10 + (unsigned)flash * 2 + ((flash & 2) ? 24u : 0u));
      }
   }

   if (s->advance)
   {
      px_kit_tags_end(&g->known);
      memcpy(g->falling, falling, sizeof(bomb) * falling_count);
      g->falling_count = falling_count;
      g->bucket_x = bucket_x;
      g->score    = score;
      g->buckets  = buckets;
      g->flash    = s->ram ? flash : -1;
      if (g->splash > 0)
         g->splash--;
   }
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has one voice, voice 0; voice 1 is silent.
 *
 *   a fuse      noise (waveform 8) at pitch 0 to 3 and volume 1 to 3, both by chance a
 *               frame, for as long as bombs fall
 *   a catch     waveform 12, pitch 16 then 15, volume 12, two frames; then noise from pitch
 *               14 down to 1 at volume 8 and less, which is the fuses again
 *   going off   noise at pitch 8, volume 15 (or 14) and down by one or two a frame, for
 *               each bomb; the next begins at 15 again before the last has ended
 *   the blast   noise at pitch 31, volume 15 down by one a frame: the bucket is lost
 *   between     waveform 0 at volume 1, which is silence
 *
 * Each is a sound of several voices of Proteus's own, where it happens between left and
 * right, and the game's voice is silent while it plays what is known. The catch is played
 * at the pitch the game gives it.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_FUSE, SOUND_CATCH, SOUND_OFF, SOUND_BLAST, SOUND_OTHER };

static unsigned voice0_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   if (!volume || wave == 0)
      return SOUND_NONE;
   if (wave == 12)
      return SOUND_CATCH;
   if (wave == 8 && pitch == 31)
      return SOUND_BLAST;
   if (wave == 8 && pitch == 8 && volume >= 4)
      return SOUND_OFF;
   if (wave == 8 && volume <= 8)
      return SOUND_FUSE;
   return SOUND_OTHER;
}

static void play_catch(kb *g, px_sound *s, unsigned pitch)
{
   const float hz = px_kit_tune(px_kit_tia_hz(12, pitch));
   const px_tone p[4] = {
      /* wave          freq      to        glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SINE,  hz * 2.0f, hz,      0.06f, 0.002f, 0.02f, 0.30f, 0.70f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, hz * 3.0f, hz * 2.0f, 0.05f, 0.002f, 0.01f, 0.20f, 0.26f, 0, 0, 5.0f, 0.01f },
      { PX_WAVE_NOISE, 3500,     900,      0.16f, 0.004f, 0.02f, 0.22f, 0.30f, 5000, 700, 0, 0 },
      { PX_WAVE_SINE,   170,      70,      0.10f, 0.001f, 0.01f, 0.16f, 0.55f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->catch_x));
}

static void play_off(kb *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 5200,  600, 0.30f, 0,      0.02f, 0.45f, 0.40f, 4800, 500, 0, 0 },
      { PX_WAVE_SINE,   110,   36, 0.20f, 0.001f, 0.03f, 0.40f, 0.70f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE,  900,  150, 0.60f, 0.01f,  0.05f, 0.80f, 0.24f, 1000, 140, 0, 0 },
      { PX_WAVE_SAW,    240,   60, 0.25f, 0.001f, 0.01f, 0.30f, 0.12f, 1800, 200, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->went_off_x >= 0 ? g->went_off_x : g->catch_x));
}

static void play_blast(kb *g, px_sound *s)
{
   static const px_tone p[5] = {
      { PX_WAVE_NOISE, 6000,  200, 1.40f, 0,      0.20f, 1.90f, 0.44f, 5000, 120, 0, 0 },
      { PX_WAVE_SINE,    62,   22, 1.20f, 0.002f, 0.20f, 1.60f, 0.85f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    300,   45, 1.10f, 0.002f, 0.05f, 1.30f, 0.22f, 2600, 160, 9.0f, 0.05f },
      { PX_WAVE_NOISE,  700,  120, 2.00f, 0.25f,  0.20f, 2.40f, 0.26f, 800, 90, 0, 0 },
      { PX_WAVE_NOISE, 2400, 1200, 0.30f, 0.004f, 0.05f, 0.60f, 0.18f, 3000, 800, 0, 0 }
   };
   px_kit_play(s, p, 5, px_kit_pan(g->catch_x));
}

/* The fuses: a hiss where the lowest bomb falls, as loud as the game makes its own, and
 * crackles now and then. */
static void play_fuse(kb *g, px_sound *s, bool heard)
{
   static const px_tone hiss = { PX_WAVE_NOISE, 7000, 0, 0, 0.05f, 0, 0, 1.0f, 6500, 0, 0, 0 };
   static const px_tone pop  = { PX_WAVE_NOISE, 4000, 1500, 0.03f, 0, 0.004f, 0.04f, 0.5f, 6000, 2000, 0, 0 };
   const float pan = px_kit_pan(g->fuse_x);

   g->fuse_quiet = heard ? 0 : g->fuse_quiet + 1;
   if (!g->own_sound || g->fuse_quiet > 3)
   {
      px_synth_stop(s->synth, g->sizzle, 0.15f);
      g->sizzle = 0;
      return;
   }
   {
      const float gain = 0.055f + 0.018f * (float)(heard ? g->tia.volume[0] : 1);
      if (!px_synth_move(s->synth, g->sizzle, pan, gain, 0))
         g->sizzle = px_synth_play(s->synth, &hiss, pan, gain);
   }
   /* The game's pitch that is lowest is a crackle. */
   if (heard && (px_kit_chance(&g->crackle_seed) & 7) == 0)
      px_synth_play(s->synth, &pop, pan, 0.045f);
}

static void sound(void *state, px_sound *s)
{
   kb *g = (kb*)state;
   unsigned heard;
   int lowest = -1;

   /* Where things are, from the picture before. */
   g->fuse_x = g->went_off_x = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->ghost)
         continue;
      if (in->role == PX_ROLE_PLAYER)
         g->catch_x = in->x + in->w / 2;
      else if (in->role == PX_ROLE_BOMB && in->group)
         g->went_off_x = in->x + 4;
      else if (in->role == PX_ROLE_BOMB && in->y > lowest)
      {
         lowest = in->y;
         g->fuse_x = in->x + 4;
      }
   }

   px_kit_tia_hear(&g->tia, s->frame);
   heard = voice0_plays(g->tia.wave[0], g->tia.pitch[0], g->tia.volume[0]);

   if (heard == SOUND_CATCH && g->heard != SOUND_CATCH && g->tia.volume[0] >= 8)
   {
      if (g->own_sound) play_catch(g, s, g->tia.pitch[0]);
      px_sound_rumble(s, 0, 14000, 3);
   }
   else if (heard == SOUND_OFF && (g->heard != SOUND_OFF || px_kit_tia_louder(&g->tia, 0))
         && g->tia.volume[0] >= 12)
   {
      if (g->own_sound) play_off(g, s);
      px_sound_rumble(s, 30000, 26000, 10);
   }
   else if (heard == SOUND_BLAST && g->heard != SOUND_BLAST)
   {
      if (g->own_sound) play_blast(g, s);
      px_sound_rumble(s, 65535, 48000, 50);
   }
   g->heard = heard;

   play_fuse(g, s, heard == SOUND_FUSE);

   if (g->own_sound && heard != SOUND_NONE && heard != SOUND_OTHER)
      s->voice[0] = 0.0f;
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   kb *g = (kb*)state;
   g->score = -1;
   g->buckets = -1;
   g->flash = -1;
   g->falling_count = 0;
   g->bucket_x = -1;
   g->splash = 0;
   px_kit_tags_reset(&g->known);
   px_kit_tia_reset(&g->tia);
   g->heard = SOUND_NONE;
   g->fuse_quiet = 99;
   g->sizzle = 0;
   g->fuse_x = g->went_off_x = g->catch_x = -1;
   g->crackle_seed = 0xF05Eu;
}

static void *create(void)
{
   kb *g = (kb*)calloc(1, sizeof(kb));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = true;
      make_maps(g);
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   kb *g = (kb*)state;
   if (g)
      px_kit_canvas_free(&g->city);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   kb *g = (kb*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
}

const px_game px_game_kaboom = {
   "Kaboom!", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
