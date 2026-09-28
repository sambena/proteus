/* SPDX-License-Identifier: GPL-3.0-or-later */
/* H.E.R.O. (Activision, 1984, by John Van Ryzin).
 *
 * Roderick Hero flies down a mine, one screen at a time, with a propeller on his back, to a
 * miner trapped at the bottom. Walls in his way are blown up with dynamite, creatures shot with
 * a short laser from his helmet. Walls of lava kill on touch. A lantern shot or touched puts
 * the lights out: the walls go black and only the creatures are seen. Power drains while he
 * flies; what is left is counted off as a bonus when the miner is reached, and then the sticks
 * of dynamite left.
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  21..26   black, above the mine
 *   rows  27..152  the mine. Its walls are playfield, one colour a row, in three bands with a
 *                  darker row between them: rows 31..69, 72..108 and 111..148 (level 1: $22,
 *                  $24, $26, the dark rows $20). The rows 27..30 and 149..152 are the rock's
 *                  wavy edges, lighter ($28..$2C). The background is black.
 *                  Walls that dynamite breaks are the ball, eight wide, set to the playfield's
 *                  colour on every row, so that they are of the wall's colour.
 *                  Lava is a band of the walls in red, $42 and $44 in turns every three frames.
 *                  In the dark the playfield of the bands is black ($00); the wavy edges stay,
 *                  grey. Lava stays red.
 *                  At the bottom of a level, the miner's screen has a stream under the floor:
 *                  background of a colour ($84 blue on level 1, $54, $24 ...) on rows 148..152.
 *   the hero       player 1, up to 26 rows with the propeller
 *   the laser      missile 1, one row, $46
 *   the rest       player 0: spiders, bats, moths, snakes, tentacles, the lantern (grey with
 *                  yellow bands), the dynamite (a red stick, its fuse yellow), the miner, the
 *                  blast. In the dark all of them are grey ($04).
 *   rows 153..160  the power bar: background $42, the power left in yellow playfield, POWER
 *   rows 161..199  the lives, the dynamite left, the score or the level ("LEVEL: n" at the
 *                  start of one), on grey ($04)
 *   rows 200..211  the copyright and Activision's name in turns
 *
 * The eight pixels at the left are blanked on every line of the mine (HMOVE).
 *
 * A blast of dynamite flashes the mine's background: $0A for two frames, black for six, $0A for
 * two. A hero that dies flashes it so every eight frames for as long as his sound lasts.
 *
 * Of its memory ($80 is 0):
 *
 *    28        the screen of the level, from 0
 *    43        the power left
 *    50        the sticks of dynamite left
 *    55..57    the score, in decimal digits (BCD): 50 for a creature, 75 for a wall
 *    69..71    the walls' colours of the level: the lowest band, the middle, the top
 *   114        not 0 from the moment the miner is reached until the next level
 *   117        the level, from 0
 *
 * The game has five games to choose from with SELECT, which begin at levels 1, 5, 9, 13 and
 * 17. When the game is not played it shows levels 1 to 4 played by itself, without a sound.
 *
 * The power bar and all below it are the game's display and stay its own to the pixel: they
 * are made blank for the effects, which draw blank as it is. The black above the mine is
 * blank too, so that no light of Proteus's falls on it.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_hr_colors"
#define OPT_BACKDROP "proteus_hr_backdrop"
#define OPT_SPARKS   "proteus_hr_sparks"
#define OPT_SOUND    "proteus_hr_sound"

#define RAM_SCREEN   28
#define RAM_POWER    43
#define RAM_SCORE    55
#define RAM_WALLS    69
#define RAM_RESCUED  114
#define RAM_LEVEL    117

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define ROW_TOP      21      /* the first row of the picture that is drawn */
#define ROW_CEILING  27      /* the first row of the mine's rock */
#define ROW_BODY     31      /* the bands of the walls, below the wavy edge */
#define ROW_BOTTOM   149     /* the wavy edge at the bottom */
#define ROW_PANEL    153     /* the power bar */
#define ROW_END      212     /* the row after the game's display */
#define ROWS         212     /* what the module needs of a frame to know the game in it */
#define LIGHT_ROWS   (ROW_PANEL - ROW_TOP)

#define LAMPS        12      /* lights at once */
#define WALLS        6       /* walls of the ball in a frame */
#define CREATURES    16
#define FADE         24      /* of 256, a frame: how fast the lights go out and come back */

/* What a player 0 is: a tag. */
enum { KIND_NONE = 0, KIND_CREATURE, KIND_LANTERN, KIND_DYNAMITE, KIND_MINER };

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "fca4a5be1251927027f2c24774a02160",   /* H.E.R.O. (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "50",
   "reverb", "room",
   NULL
};

static const char *const colors[] = { "rock", "Rock and lava", "original", "The game's own", NULL };
static const char *const backdrop[] = { "mine", "The mine, lit", "off", "Off", NULL };
/* In the order of the options' values. */
enum { COLORS_ROCK = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Walls of rock with strata and shading in the colours the game gives them, lava that glows and flows, creatures in stronger colours, a laser that glows; or the game's own colours.",
     "rock", colors },
   { OPT_BACKDROP, "Backdrop",
     "The rock of the mine behind the shafts, lit by the lanterns, the lava and the hero's helmet lamp; a darkness where only the lamp shows the walls when the lights go out; a stream that ripples. Or the game's black.",
     "mine", backdrop },
   { OPT_SPARKS, "Explosions",
     "A blast of light, debris and dust where dynamite goes off, sparks from the fuse and where a creature is shot, a burst where the hero dies and a celebration where the miner is rescued.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the propeller, the laser, the fuse and the blast, a creature shot, the hero's death, the power counted off and the dynamite counted, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

/* A light: round, or a cone to one side (the helmet lamp). */
typedef struct
{
   int16_t x, y;          /* captured pixels */
   int16_t radius;        /* in rows */
   int8_t cone;           /* 0: round; 1, -1: a cone to the right, to the left */
   uint16_t strength;     /* of 256 */
   uint32_t rgb;
} lamp;

/* A light that fades by itself: a blast's, the dust after it. */
typedef struct
{
   int16_t x, y, radius;
   uint16_t strength;     /* of 256; 0: none */
   uint16_t fade;         /* of 256, a frame */
   uint32_t rgb;
} flare;

typedef struct
{
   int16_t x, y;
   uint32_t rgb;
} creature;

typedef struct
{
   /* The options. */
   unsigned colors;
   bool backdrop, sparks, own_sound;

   uint32_t frame;                 /* counts the frames that advance */
   px_kit_tags known;              /* what player 0's are */

   /* The mine as the frame shows it. */
   bool dark;                      /* the lights are out */
   unsigned dim;                   /* of 256: how lit the mine is, following `dark` */
   uint32_t row_rgb[PXC_MAX_H];    /* the walls' colour of every row, as it was when lit */
   uint8_t lava[PXC_MAX_H];        /* the row is of lava */
   unsigned water_top;             /* the stream's first row; 0: none */
   uint32_t water_rgb;
   bool flashing;                  /* the game flashes the mine's background */
   unsigned flash_gap;             /* frames since it last began to */
   int screen;                     /* memory's screen; -1: not known */

   /* What is in the mine. */
   int hero_x, hero_y, hero_h;     /* the hero's left, top and height; x -1: not seen */
   int facing;                     /* 1: right, -1: left */
   int laser_x, laser_y, laser_w;  /* laser_x -1: none */
   int dynamite_x, dynamite_y;     /* the stick, where it was seen last */
   uint32_t dynamite_at;
   int miner_x, miner_y;
   uint32_t miner_at;
   creature creatures[CREATURES], were[CREATURES];
   unsigned creature_count, were_count;
   int16_t wall_x[WALLS], wall_top[WALLS], wall_bottom[WALLS];
   unsigned wall_count;
   int16_t was_wall_x[WALLS], was_wall_top[WALLS], was_wall_bottom[WALLS];
   unsigned was_wall_count;

   /* Light. */
   lamp lamps[LAMPS];
   unsigned lamp_count;
   flare flares[4];
   uint16_t light[3][LIGHT_ROWS][PXC_W];   /* what the lights add, a captured pixel each */
   uint32_t vivid[256];            /* the game's colours, stronger */
   const uint32_t *vivid_of;

   /* The backdrop. */
   px_kit_texture rock;
   px_kit_canvas stone;            /* the rock in the level's colour */
   uint32_t stone_of;              /* the colour it was made for */
   uint32_t shades[256];
   bool painted;

   /* What happens. */
   px_kit_tia seen;                /* the game's voices as the picture follows them */
   unsigned seen0;
   int score;                      /* -1: not known */
   int rescued;                    /* memory's; -1: not known */
   unsigned party;                 /* frames of the celebration left */
   uint32_t seed;

   /* The sound. */
   px_kit_tia tia;
   unsigned heard0, heard1;
   unsigned laser, whirr[2], fuse, fill[2];   /* voices at the synth */
   int at;                         /* the hero's column, for where a sound is */
   int sound_score;
} hr;

/* ---------------------------------------------------------------------------
 * Colours
 * ------------------------------------------------------------------------- */

static unsigned hue_of(unsigned index) { return index >> 4; }
static unsigned lum_of(unsigned index) { return index & 0x0E; }

/* Stronger: away from grey, and up. */
static uint32_t vivid(uint32_t rgb)
{
   int c[3] = { (int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF) };
   int top = c[0] > c[1] ? c[0] : c[1];
   top = top > c[2] ? top : c[2];
   for (unsigned k = 0; k < 3; k++)
   {
      int v = top - (top - c[k]) * 3 / 2;
      v = v * 9 / 8 + 10;
      c[k] = v < 0 ? 0 : v > 255 ? 255 : v;
   }
   return ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
}

static void make_vivid(hr *g, const uint32_t *palette)
{
   for (unsigned i = 0; i < 256; i++)
      g->vivid[i] = lum_of(i) || hue_of(i) ? vivid(palette[i] & 0xFFFFFFu) : PX_KIT_KEEP;
   g->vivid_of = palette;
}

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

/* A colour lit: `f` of 256 is as it is, more is brighter. */
static uint32_t lit(uint32_t rgb, unsigned f)
{
   unsigned r = ((rgb >> 16) & 0xFF) * f >> 8, gr = ((rgb >> 8) & 0xFF) * f >> 8, b = (rgb & 0xFF) * f >> 8;
   return ((r > 255 ? 255 : r) << 16) | ((gr > 255 ? 255 : gr) << 8) | (b > 255 ? 255 : b);
}

/* A colour lit by a light of its own colour. */
static uint32_t lit_by(uint32_t rgb, unsigned ambient, unsigned lr, unsigned lg, unsigned lb)
{
   unsigned r = ((rgb >> 16) & 0xFF) * (ambient + lr) >> 8;
   unsigned gr = ((rgb >> 8) & 0xFF) * (ambient + lg) >> 8;
   unsigned b = (rgb & 0xFF) * (ambient + lb) >> 8;
   return ((r > 255 ? 255 : r) << 16) | ((gr > 255 ? 255 : gr) << 8) | (b > 255 ? 255 : b);
}

/* ---------------------------------------------------------------------------
 * What the frame shows
 * ------------------------------------------------------------------------- */

/* The playfield's colour on a row: the one most of its pixels have; -1 where there is none. */
static int pf_colour(const struct pxc_frame *f, unsigned y)
{
   const uint8_t *tags = f->tags + (size_t)y * PXC_W, *pf = f->color[PXC_L_PF] + (size_t)y * PXC_W;
   unsigned first = 256, n1 = 0, second = 256, n2 = 0;
   for (unsigned x = 8; x < PXC_W; x++)
   {
      if (!(tags[x] & PXC_PF))
         continue;
      if (first == 256 || pf[x] == first) { first = pf[x]; n1++; }
      else if (second == 256 || pf[x] == second) { second = pf[x]; n2++; }
   }
   if (!n1)
      return -1;
   return (int)(n2 > n1 ? second : first);
}

/* The background's colour on a row: where two of three places across agree. */
static unsigned bk_colour(const struct pxc_frame *f, unsigned y)
{
   const uint8_t *bk = f->color[PXC_L_BK] + (size_t)y * PXC_W;
   const uint8_t a = bk[24], b = bk[80], c = bk[136];
   return a == c && a != b ? a : b;
}

static bool lava_colour(unsigned index)
{
   return hue_of(index) == 4 && lum_of(index) >= 2;
}

/* The walls' colour of a row, in the level's colours as memory has them, where the picture has
 * not shown them lit. */
static uint32_t band_colour(const px_scene *s, unsigned y)
{
   const unsigned band = y < 71 ? 2 : y < 110 ? 1 : 0;
   const int c = px_kit_ram(s->ram, s->ram_size, RAM_WALLS + band);
   return c > 0 ? s->frame->palette[c] & 0xFFFFFFu : 0x6A4A2Au;
}

static void look(hr *g, px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   const uint32_t *palette = f->palette;
   unsigned black = 0, coloured = 0, flash_rows = 0, water = 0;
   uint32_t water_rgb = 0;

   for (unsigned y = ROW_CEILING; y < ROW_PANEL; y++)
   {
      const int c = pf_colour(f, y);
      const unsigned bk = bk_colour(f, y);
      g->lava[y] = c >= 0 && lava_colour((unsigned)c) && y >= ROW_BODY && y < ROW_BOTTOM;
      if (c >= 0 && y >= ROW_BODY && y < ROW_BOTTOM && !g->lava[y])
      {
         const uint8_t *tags = f->tags + (size_t)y * PXC_W;
         unsigned n = 0;
         for (unsigned x = 8; x < PXC_W; x++)
            n += (tags[x] & PXC_PF) != 0;
         if (c == 0) black += n;
         else        coloured += n;
      }
      if (bk && y < ROW_BOTTOM - 4)
         flash_rows++;
      if (bk && y >= ROW_BOTTOM - 4 && !water)
      {
         water = y;
         water_rgb = palette[bk] & 0xFFFFFFu;
      }
   }

   /* The game flashes its background: a blast, or the hero dies. */
   g->flashing = flash_rows >= 40;
   if (!g->flashing)
   {
      g->water_top = water;
      g->water_rgb = water_rgb;
   }
   /* No walls at all is no frame of the mine: it stays as it was. */
   if (black + coloured >= 200)
      g->dark = black > coloured * 2;

   for (unsigned y = ROW_CEILING; y < ROW_PANEL; y++)
   {
      const int c = pf_colour(f, y);
      if (c > 0 && !g->lava[y] && (!g->dark || y < ROW_BODY || y >= ROW_BOTTOM))
         g->row_rgb[y] = palette[c] & 0xFFFFFFu;
      else if (!g->row_rgb[y])
         g->row_rgb[y] = band_colour(s, y);
   }
}

/* ---------------------------------------------------------------------------
 * The game's display: kept to the pixel
 * ------------------------------------------------------------------------- */

/* The power bar and all below it are drawn as the game drew them, to the pixel. So is the
 * black above the mine and where the game blanks its lines, which no light is to fall on. */
static void keep_display(px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   const uint32_t *palette = f->palette;

   for (unsigned y = 0; y < f->height; y++)
   {
      const bool panel = y >= ROW_PANEL && y < ROW_END;
      const bool border = y < ROW_CEILING || y >= ROW_END;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         const uint8_t tags = f->tags[i];
         if (!panel && !(tags & PXC_BLANK) && !(border && !(tags & PXC_SPRITES)))
            continue;
         s->top[i]    = PX_KEY(PX_CLS_BLANK, palette[f->winner[i]]);
         s->bk[i]     = 0;
         s->sprite[i] = 0;
         s->energy[i] = 0;
         s->light[i]  = 0;
      }
   }
}

/* ---------------------------------------------------------------------------
 * The objects
 * ------------------------------------------------------------------------- */

static unsigned rows_of_hue(const px_objects *o, const px_instance *in, unsigned hue)
{
   unsigned n = 0;
   for (unsigned r = 0; r < in->h; r++)
   {
      const unsigned c = o->colors[in->rows + r];
      n += hue_of(c) == hue && lum_of(c) >= 2;
   }
   return n;
}

/* What a player 0 is, from its colours: the dynamite is a red stick with a yellow fuse, the
 * miner has green in him, the lantern is grey with yellow bands and hangs still. */
static unsigned kind_of(const px_objects *o, const px_instance *in, bool still)
{
   const unsigned yellow = rows_of_hue(o, in, 1);
   if (in->h >= 9 && in->h <= 12 && hue_of(in->color) == 1 && rows_of_hue(o, in, 4) >= 4)
      return KIND_DYNAMITE;
   if (in->h >= 10 && (rows_of_hue(o, in, 0xC) + rows_of_hue(o, in, 0xD)) >= 2 && yellow)
      return KIND_MINER;
   if (still && in->h >= 6 && in->h <= 9 && yellow >= 2 && hue_of(in->color) == 0)
      return KIND_LANTERN;
   return KIND_CREATURE;
}

static void add_lamp(hr *g, int x, int y, int radius, int cone, unsigned strength, uint32_t rgb)
{
   lamp *l;
   if (g->lamp_count >= LAMPS || !strength)
      return;
   l = &g->lamps[g->lamp_count++];
   l->x = (int16_t)x;
   l->y = (int16_t)y;
   l->radius = (int16_t)radius;
   l->cone = (int8_t)cone;
   l->strength = (uint16_t)(strength > 1024 ? 1024 : strength);
   l->rgb = rgb;
}

static void add_flare(hr *g, int x, int y, int radius, unsigned strength, unsigned fade, uint32_t rgb)
{
   flare *f = &g->flares[0];
   for (unsigned i = 1; i < 4; i++)
      if (g->flares[i].strength < f->strength)
         f = &g->flares[i];
   f->x = (int16_t)x;
   f->y = (int16_t)y;
   f->radius = (int16_t)radius;
   f->strength = (uint16_t)strength;
   f->fade = (uint16_t)fade;
   f->rgb = rgb;
}

/* Colours every pixel of an object where it is on top. */
static void tint_where_on_top(px_scene *s, const px_instance *in, uint32_t rgb)
{
   const px_objects *o = s->objects;
   for (unsigned r = 0; r < in->h; r++)
   {
      const int y = in->y + (int)r;
      const uint32_t bits = o->bits[in->rows + r];
      if (y < ROW_CEILING || y >= ROW_PANEL)
         continue;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         const int x = in->x + (int)b;
         size_t i;
         if (!((bits >> b) & 1) || x < 0 || x >= PXC_W)
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         if (PX_KEY_CLS(s->top[i]) != PX_CLS_SPRITE)
            continue;
         s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
         if (s->sprite[i])
            s->sprite[i] = 0xFF000000u | rgb;
      }
   }
}

static void objects(hr *g, px_scene *s)
{
   px_objects *o = s->objects;
   const bool rock = g->colors == COLORS_ROCK;
   const uint32_t *palette = s->frame->palette;
   int hero_x = -1;

   if (rock && g->vivid_of != palette)
      make_vivid(g, palette);
   g->laser_x = -1;
   g->wall_count = 0;
   g->creature_count = 0;
   px_kit_tags_begin(&g->known);

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role = PX_ROLE_NONE;
      if (in->y >= ROW_PANEL || in->y + (int)in->h <= ROW_CEILING)
      {
         in->role = PX_ROLE_HUD;
         continue;
      }
      switch (in->cls)
      {
         case PXC_L_BL:
            /* A wall that dynamite breaks: tall, of the walls' colour. */
            if (in->h >= 16 && in->w >= 4)
            {
               if (!in->ghost && g->wall_count < WALLS)
               {
                  g->wall_x[g->wall_count]      = in->x;
                  g->wall_top[g->wall_count]    = in->y;
                  g->wall_bottom[g->wall_count] = (int16_t)(in->y + (int)in->h);
                  g->wall_count++;
               }
               in->group = 1;
            }
            break;
         case PXC_L_P1:
            in->role = PX_ROLE_PLAYER;
            if (!in->ghost && in->h >= 8)
            {
               hero_x    = in->x;
               g->hero_y = in->y;
               g->hero_h = (int)in->h;
            }
            break;
         case PXC_L_M1:
            in->role = PX_ROLE_SHOT;
            if (!in->ghost)
            {
               g->laser_x = in->x;
               g->laser_y = in->y;
               g->laser_w = (int)in->w;
            }
            if (rock)
               px_scene_tint(s, in, 0xFF7058);
            px_scene_energy(s, in, true);
            break;
         case PXC_L_P0:
         {
            const px_obj_track *t = px_objects_track(o, in->track);
            const bool still = t && t->vx == 0 && t->vy == 0 && (t->seen & 0xFF) == 0xFF;
            px_kit_tagged *k = in->ghost ? px_kit_tags_find(&g->known, in) : NULL;
            unsigned kind;
            if (!in->ghost)
            {
               kind = kind_of(o, in, still);
               k = px_kit_tags_keep(&g->known, in, kind);
               /* A lantern stays one when the lights go out and it is grey. */
               if (k && k->tag != KIND_LANTERN)
                  k->tag = (uint8_t)kind;
            }
            kind = k ? k->tag : KIND_CREATURE;
            in->group = (uint8_t)kind;
            switch (kind)
            {
               case KIND_DYNAMITE:
                  if (!in->ghost)
                  {
                     g->dynamite_x  = in->x + 4;
                     g->dynamite_y  = in->y + 2;
                     g->dynamite_at = g->frame;
                  }
                  if (rock)
                     px_kit_repaint(s, in, g->vivid);
                  break;
               case KIND_MINER:
                  in->role = PX_ROLE_BONUS;
                  if (!in->ghost)
                  {
                     g->miner_x  = in->x + 4;
                     g->miner_y  = in->y + (int)in->h / 2;
                     g->miner_at = g->frame;
                  }
                  if (rock)
                     px_kit_repaint(s, in, g->vivid);
                  break;
               case KIND_LANTERN:
                  if (!g->dark)
                  {
                     if (rock)
                        px_kit_repaint(s, in, g->vivid);
                     px_scene_energy(s, in, true);
                     add_lamp(g, in->x + 4, in->y + (int)in->h / 2, 46, 0, 150 * g->dim / 256, 0xFFC878);
                  }
                  break;
               default:
                  in->role = PX_ROLE_ENEMY;
                  if (!in->ghost && g->creature_count < CREATURES)
                  {
                     creature *c = &g->creatures[g->creature_count++];
                     c->x = (int16_t)(in->x + 4);
                     c->y = (int16_t)(in->y + (int)in->h / 2);
                     c->rgb = rock ? g->vivid[in->color] & 0xFFFFFFu : palette[in->color] & 0xFFFFFFu;
                     if (c->rgb == (PX_KIT_KEEP & 0xFFFFFFu))
                        c->rgb = 0xC0C0C0;
                  }
                  if (!rock)
                     break;
                  if (g->dark)
                  {
                     /* In the dark, the creatures glow. */
                     tint_where_on_top(s, in, 0x9CFFB0);
                     px_scene_energy(s, in, true);
                  }
                  else
                     px_kit_repaint(s, in, g->vivid);
                  break;
            }
            break;
         }
         default:
            break;
      }

      /* A wall the tracker draws from the frames before, where the game has none: it was
       * blown up. It is gone. */
      if (in->ghost && in->cls == PXC_L_BL)
         for (unsigned r = 0; r < in->h; r++)
         {
            const int y = in->y + (int)r;
            if (y < ROW_CEILING || y >= ROW_PANEL)
               continue;
            for (unsigned b = 0; b < in->w; b++)
            {
               const int x = in->x + (int)b;
               size_t i;
               if (x < 0 || x >= PXC_W)
                  continue;
               i = (size_t)y * PXC_W + (size_t)x;
               if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE && !(s->frame->tags[i] & PXC_SPRITES))
               {
                  s->top[i]    = PX_KEY(PX_CLS_BK, 0);
                  s->bk[i]     = 0;
                  s->sprite[i] = 0;
                  s->energy[i] = 0;
               }
            }
         }
   }
   px_kit_tags_end(&g->known);

   /* Which way the hero looks: where his laser is, or where he went. */
   if (hero_x >= 0)
   {
      if (g->laser_x >= 0)
         g->facing = g->laser_x + g->laser_w / 2 < hero_x + 4 ? -1 : 1;
      else if (g->hero_x >= 0 && hero_x != g->hero_x && s->advance)
         g->facing = hero_x < g->hero_x ? -1 : 1;
   }
   g->hero_x = hero_x;
}

/* ---------------------------------------------------------------------------
 * Light
 * ------------------------------------------------------------------------- */

/* Rows are about half as wide as columns: distances are in rows. */
#define ASPECT_X 2

static void light_lamp(hr *g, const lamp *l)
{
   const int r = l->radius, r2 = r * r;
   const unsigned lr = (l->rgb >> 16) & 0xFF, lg = (l->rgb >> 8) & 0xFF, lb = l->rgb & 0xFF;
   int y0 = l->y - r, y1 = l->y + r, x0 = l->x - r / ASPECT_X, x1 = l->x + r / ASPECT_X;

   if (l->cone > 0) x0 = l->x - 4;
   if (l->cone < 0) x1 = l->x + 4;
   if (y0 < ROW_TOP) y0 = ROW_TOP;
   if (y1 >= ROW_PANEL) y1 = ROW_PANEL - 1;
   if (x0 < 0) x0 = 0;
   if (x1 >= PXC_W) x1 = PXC_W - 1;
   for (int y = y0; y <= y1; y++)
   {
      const int dy = y - l->y;
      for (int x = x0; x <= x1; x++)
      {
         const int dx = (x - l->x) * ASPECT_X, d2 = dx * dx + dy * dy;
         unsigned f;
         if (d2 >= r2)
            continue;
         f = (unsigned)((r2 - d2) * 256 / r2);
         f = f * f >> 8;
         if (l->cone)
         {
            /* A cone of light forward, softer at its sides, and a little all round. */
            const int ahead = dx * l->cone, side = dy < 0 ? -dy : dy;
            unsigned c = 40;
            if (ahead > 0)
            {
               const int width = ahead * 3 / 4 + 3;
               if (side < width)
                  c = 40 + (unsigned)(216 * (width - side) / width);
            }
            f = f * c >> 8;
         }
         f = f * l->strength >> 8;
         if (!f)
            continue;
         {
            uint16_t *R = &g->light[0][y - ROW_TOP][x], *G = &g->light[1][y - ROW_TOP][x], *B = &g->light[2][y - ROW_TOP][x];
            const unsigned nr = *R + (lr * f >> 8), ng = *G + (lg * f >> 8), nb = *B + (lb * f >> 8);
            *R = (uint16_t)(nr > 1024 ? 1024 : nr);
            *G = (uint16_t)(ng > 1024 ? 1024 : ng);
            *B = (uint16_t)(nb > 1024 ? 1024 : nb);
         }
      }
   }
}

/* The glow of lava on what is near it: its rows, spread. */
static void light_lava(hr *g, const px_scene *s)
{
   static uint16_t near[LIGHT_ROWS][PXC_W];
   bool any = false;
   memset(near, 0, sizeof(near));
   for (unsigned y = ROW_BODY; y < ROW_BOTTOM; y++)
   {
      const uint8_t *tags = s->frame->tags + (size_t)y * PXC_W;
      if (!g->lava[y])
         continue;
      for (unsigned x = 8; x < PXC_W; x++)
         if (tags[x] & PXC_PF)
         {
            near[y - ROW_TOP][x] = 256;
            any = true;
         }
   }
   if (!any)
      return;
   /* Spread across, then down: boxes of 13 columns and 17 rows. */
   for (unsigned y = 0; y < LIGHT_ROWS; y++)
   {
      unsigned sum = 0, out[PXC_W];
      for (int x = -6; x < (int)PXC_W + 6; x++)
      {
         if (x + 6 < (int)PXC_W) sum += near[y][x + 6];
         if (x - 7 >= 0)         sum -= near[y][x - 7];
         if (x >= 0 && x < (int)PXC_W)
            out[x] = sum / 13;
      }
      for (unsigned x = 0; x < PXC_W; x++)
         near[y][x] = (uint16_t)out[x];
   }
   for (unsigned x = 0; x < PXC_W; x++)
   {
      unsigned sum = 0, out[LIGHT_ROWS];
      for (int y = -8; y < LIGHT_ROWS + 8; y++)
      {
         if (y + 8 < LIGHT_ROWS) sum += near[y + 8][x];
         if (y - 9 >= 0)         sum -= near[y - 9][x];
         if (y >= 0 && y < LIGHT_ROWS)
            out[y] = sum / 17;
      }
      for (unsigned y = 0; y < LIGHT_ROWS; y++)
      {
         const unsigned f = out[y] * (180 + (px_kit_wave(g->frame * 3 + x * 5 + y * 3) >> 3)) >> 8;
         g->light[0][y][x] = (uint16_t)(g->light[0][y][x] + (f * 255 >> 8));
         g->light[1][y][x] = (uint16_t)(g->light[1][y][x] + (f * 70 >> 8));
         g->light[2][y][x] = (uint16_t)(g->light[2][y][x] + (f * 16 >> 8));
      }
   }
}

static void make_light(hr *g, const px_scene *s)
{
   memset(g->light, 0, sizeof(g->light));

   /* The hero's helmet lamp: a little in the light, a cone in the dark. */
   if (g->hero_x >= 0)
   {
      const int eye_x = g->hero_x + 4 + g->facing * 2, eye_y = g->hero_y + (g->hero_h >= 20 ? 7 : 3);
      const unsigned dark = 256 - g->dim;
      add_lamp(g, eye_x, eye_y, 64 + (int)(dark * 26 / 256), g->facing, 60 + dark * 330 / 256, 0xFFEEC8);
      add_lamp(g, eye_x, eye_y + 4, 16, 0, 20 + dark * 90 / 256, 0xFFE0B0);
   }
   /* The laser lights what it passes. */
   if (g->laser_x >= 0)
      for (int x = g->laser_x; x < g->laser_x + g->laser_w; x += 4)
         add_lamp(g, x, g->laser_y, 12, 0, 70 + (256 - g->dim) / 3, 0xFF5038);
   /* The fuse. */
   if (g->frame - g->dynamite_at < 3)
      add_lamp(g, g->dynamite_x, g->dynamite_y - 2, 16, 0,
            60 + (px_kit_wave(g->frame * 37) >> 2) + (256 - g->dim) / 3, 0xFFB050);
   /* The miner is to be found. */
   if (g->frame - g->miner_at < 3)
      add_lamp(g, g->miner_x, g->miner_y, 24, 0, (g->party ? 200 : 40) + (256 - g->dim) / 4, 0xFFE8A0);

   for (unsigned i = 0; i < 4; i++)
   {
      const flare *f = &g->flares[i];
      if (f->strength)
         add_lamp(g, f->x, f->y, f->radius, 0, f->strength, f->rgb);
   }
   for (unsigned i = 0; i < g->lamp_count; i++)
      light_lamp(g, &g->lamps[i]);
   light_lava(g, s);
}

/* ---------------------------------------------------------------------------
 * The walls
 * ------------------------------------------------------------------------- */

static bool is_wall(const hr *g, const px_scene *s, int x, int y)
{
   uint8_t tags;
   if (x < 8 || x >= PXC_W || y < ROW_CEILING || y >= ROW_PANEL)
      return x < 8;
   tags = s->frame->tags[(size_t)y * PXC_W + (size_t)x];
   if (tags & PXC_PF)
      return true;
   if (tags & PXC_BL)
      for (unsigned w = 0; w < g->wall_count; w++)
         if (x >= g->wall_x[w] && x < g->wall_x[w] + 8 && y >= g->wall_top[w] && y < g->wall_bottom[w])
            return true;
   return false;
}

/* Rock: strata that wander, grain, lit from above; lava that flows and glows. */
static void paint_walls(hr *g, px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   const unsigned t = g->frame;

   for (int y = ROW_CEILING; y < ROW_PANEL; y++)
   {
      const bool lava = g->lava[y] != 0, edge = y < ROW_BODY || y >= ROW_BOTTOM;
      const int c = pf_colour(f, (unsigned)y);
      /* In the dark the game's own colour is black but for the edges, which it shows. */
      const uint32_t base = lava ? 0 : edge && g->dark && c > 0 ? f->palette[c] & 0xFFFFFFu : g->row_rgb[y];
      const unsigned ambient = edge && g->dark ? 256 : g->dim;
      for (int x = 8; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + (size_t)x;
         const uint8_t tags = f->tags[i];
         const unsigned L0 = g->light[0][y - ROW_TOP][x], L1 = g->light[1][y - ROW_TOP][x],
               L2 = g->light[2][y - ROW_TOP][x];
         uint32_t rgb;
         int shade;
         if ((tags & (PXC_P0 | PXC_P1 | PXC_M0 | PXC_M1)) || !is_wall(g, s, x, y))
            continue;

         if (lava)
         {
            /* Molten rock that creeps, bright where it is thin. */
            const unsigned a = noise(x * 256 / 9, y * 256 / 5 - (int)(t * 14), 21);
            const unsigned b = noise(x * 256 / 4 + (int)(t * 5), y * 256 / 3 + (int)(t * 9), 22);
            const unsigned v = (a * 3 + b) / 4;
            rgb = v < 128 ? px_rgb_mix(0x5A0A02, 0xC82A08, v * 2)
                  : v < 200 ? px_rgb_mix(0xC82A08, 0xFF8418, (v - 128) * 256 / 72)
                  : px_rgb_mix(0xFF8418, 0xFFE890, (v - 200) * 256 / 56);
            /* A crust at its edges. */
            if (!is_wall(g, s, x, y - 1) || !is_wall(g, s, x, y + 1) || !is_wall(g, s, x - 1, y)
                  || !is_wall(g, s, x + 1, y))
               rgb = px_rgb_mix(rgb, 0x3A0A04, 110);
            s->top[i]    = PX_KEY(PX_CLS_PF, rgb);
            s->light[i]  = 0xFF000000u | px_rgb_scale(rgb, 200);
            s->sprite[i] = 0;
            s->energy[i] = 0;
            continue;
         }

         /* Strata that wander, and grain. */
         {
            const int wander = (int)noise(x * 256 / 23, y / 16 * 256, 3) / 8;
            const unsigned strata = noise(x * 256 / 17, (y * 256 + wander * 16) / 3, 4);
            const unsigned grain = hash(x, y, 5);
            const unsigned vein = noise(x * 256 / 7 + wander * 40, y * 256 / 9, 6);
            shade = 160 + (int)(strata * 110 >> 8) + (int)(grain * 30 >> 8);
            /* Cracks. */
            if (vein > 124 && vein < 132)
               shade -= 60;
         }
         /* Lit from above: the tops of rock light, its undersides dark, its sides shaded. */
         if (!is_wall(g, s, x, y - 1))       shade += 100;
         else if (!is_wall(g, s, x, y - 2))  shade += 45;
         if (!is_wall(g, s, x, y + 1))       shade -= 70;
         else if (!is_wall(g, s, x, y + 2))  shade -= 30;
         if (!is_wall(g, s, x - 1, y))       shade -= 30;
         if (!is_wall(g, s, x + 1, y))       shade -= 45;
         if (shade < 40) shade = 40;
         rgb = lit(base, (unsigned)shade);
         rgb = lit_by(rgb, ambient, L0, L1, L2);
         s->top[i]    = PX_KEY(PX_CLS_PF, rgb);
         s->sprite[i] = 0;
         s->energy[i] = 0;
      }
   }
}

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

/* The rock behind the shafts, in the level's colour, dark. */
static void paint_stone(hr *g, const px_scene *s, uint32_t tint)
{
   if (px_kit_texture_fit(&g->rock, s))
   {
      px_kit_texture t2 = { NULL, 0, 0 };
      px_kit_texture_noise(&g->rock, 9, 12, 81);
      if (px_kit_texture_fit(&t2, s) && t2.shades && t2.w == g->rock.w && t2.h == g->rock.h)
      {
         px_kit_texture_noise(&t2, 40, 60, 82);
         for (size_t i = 0; i < (size_t)t2.w * t2.h; i++)
            g->rock.shades[i] = (uint8_t)((g->rock.shades[i] * 3 + t2.shades[i]) / 4);
      }
      px_kit_texture_free(&t2);
   }
   if (!g->rock.shades)
      return;
   px_kit_shades(g->shades, px_rgb_scale(tint, 6), px_rgb_add(px_rgb_scale(tint, 28), 0x030303),
         px_rgb_add(px_rgb_scale(tint, 66), 0x080605));
   px_kit_texture_show(&g->rock, &g->stone, g->shades);
   g->stone_of = tint;
}

/* The stream at the bottom: ripples in the game's colour for it, lit. */
static uint32_t water_at(const hr *g, unsigned X, unsigned Y, unsigned sx, unsigned sy)
{
   const unsigned t = g->frame;
   const unsigned x = X * 16u / sx, y = Y * 16u / sy - g->water_top * 16u;
   const unsigned a = px_kit_wave(x / 3 + t * 2 + px_kit_wave(y * 7 + t * 3) / 3);
   const unsigned b = px_kit_wave(x / 5 + y * 9 + 256 - (t & 255));
   const unsigned light = a * b >> 8;
   uint32_t rgb = px_rgb_scale(px_rgb_mix(g->water_rgb, px_rgb_scale(g->water_rgb, 110), y > 60 ? 256 : y * 4), 150 + (a >> 2));
   if (light > 140)
      rgb = px_rgb_add(rgb, px_rgb_scale(px_rgb_add(g->water_rgb, 0x606060), (light - 140) * 2));
   if (y < 12)
      rgb = px_rgb_add(rgb, px_rgb_scale(0x808080, 100 - y * 8));
   return rgb;
}

static void paint_backdrop(hr *g, px_scene *s)
{
   const unsigned w = s->w, h = s->h, sx = s->sx, sy = s->sy;
   const unsigned ambient = g->dim;
   const unsigned haze = 24;
   uint32_t tint;

   if (!s->backdrop || !sx || !sy || w != PXC_W * sx)
      return;
   tint = g->row_rgb[90] ? g->row_rgb[90] : 0x6A4A2A;
   if (px_kit_canvas_fit(&g->stone, s) || g->stone_of != tint || !g->rock.shades)
      paint_stone(g, s, tint);
   if (!g->stone.pixels || !g->rock.shades)
      return;
   if (!s->advance && !s->backdrop_stale && g->painted)
   {
      s->backdrop_on = true;
      return;
   }

   for (unsigned Y = 0; Y < h; Y++)
   {
      uint32_t *out = s->backdrop + (size_t)Y * w;
      const uint32_t *in = g->stone.pixels + (size_t)Y * w;
      const int fy = (int)((2 * Y + 1) * 128 / sy) - 128 - ROW_TOP * 256;
      unsigned y0, y1, wy;
      uint16_t row[3][PXC_W];
      bool water;

      if (Y < ROW_CEILING * sy || Y >= ROW_PANEL * sy)
      {
         memset(out, 0, w * sizeof(uint32_t));
         continue;
      }
      y0 = fy < 0 ? 0 : (unsigned)fy >> 8;
      if (y0 >= LIGHT_ROWS) y0 = LIGHT_ROWS - 1;
      y1 = y0 + 1 < LIGHT_ROWS ? y0 + 1 : y0;
      wy = fy < 0 ? 0 : (unsigned)fy & 255;
      for (unsigned c = 0; c < 3; c++)
         for (unsigned x = 0; x < PXC_W; x++)
            row[c][x] = (uint16_t)((g->light[c][y0][x] * (256 - wy) + g->light[c][y1][x] * wy) >> 8);
      water = g->water_top && Y >= g->water_top * sy;

      /* Between the middles of two captured pixels the light goes from one to the other. */
      for (int x = -1; x < (int)PXC_W; x++)
      {
         const unsigned a = x < 0 ? 0 : (unsigned)x, e = x + 1 < (int)PXC_W ? (unsigned)x + 1 : PXC_W - 1;
         const unsigned from = x < 0 ? 0 : (unsigned)x * sx + sx / 2;
         const unsigned to = x + 1 < (int)PXC_W ? (unsigned)(x + 1) * sx + sx / 2 : w;
         const int r0 = row[0][a] << 8, g0 = row[1][a] << 8, b0 = row[2][a] << 8;
         const int dr = (row[0][e] - row[0][a]) * 256 / (int)sx, dg = (row[1][e] - row[1][a]) * 256 / (int)sx,
               db = (row[2][e] - row[2][a]) * 256 / (int)sx;
         int lr = r0, lg = g0, lb = b0;
         if (!water && !r0 && !g0 && !b0 && !row[0][e] && !row[1][e] && !row[2][e])
         {
            /* No light here: the rock as the mine is lit. */
            if (ambient >= 256)
               memcpy(out + from, in + from, (to - from) * sizeof(uint32_t));
            else if (!ambient)
               memset(out + from, 0, (to - from) * sizeof(uint32_t));
            else
               for (unsigned X = from; X < to; X++)
                  out[X] = px_rgb_scale(in[X], ambient);
            continue;
         }
         for (unsigned X = from; X < to; X++)
         {
            const uint32_t base = water ? water_at(g, X, Y, sx, sy) : in[X];
            const unsigned Lr = (unsigned)(lr > 0 ? lr : 0) >> 8, Lg = (unsigned)(lg > 0 ? lg : 0) >> 8,
                  Lb = (unsigned)(lb > 0 ? lb : 0) >> 8;
            unsigned r = (((base >> 16) & 0xFF) * (ambient + Lr) + Lr * haze) >> 8;
            unsigned gr = (((base >> 8) & 0xFF) * (ambient + Lg) + Lg * haze) >> 8;
            unsigned b = ((base & 0xFF) * (ambient + Lb) + Lb * haze) >> 8;
            out[X] = ((r > 255 ? 255 : r) << 16) | ((gr > 255 ? 255 : gr) << 8) | (b > 255 ? 255 : b);
            if (x >= 0)
            {
               lr += dr;
               lg += dg;
               lb += db;
            }
         }
      }
   }
   s->backdrop_on = true;
   g->painted = true;
}

/* Where the mine's background is, it is made black: the backdrop shows there. So it is where
 * the game flashes it and where it has its stream, which the backdrop draws. */
static void show_backdrop(hr *g, px_scene *s)
{
   for (unsigned y = ROW_CEILING; y < ROW_PANEL; y++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const size_t i = (size_t)y * PXC_W + x;
         /* The game draws some rows of its things black (the hero's first), which it takes
          * for its black background: so does the backdrop. */
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE && !(s->top[i] & 0xFFFFFFu)
               && !(s->frame->tags[i] & PXC_PF))
         {
            s->sprite[i] = 0;
            s->energy[i] = 0;
         }
         else if (PX_KEY_CLS(s->top[i]) != PX_CLS_BK)
            continue;
         s->top[i] = PX_KEY(PX_CLS_BK, 0);
         s->bk[i]  = 0;
         if (g->water_top && y >= g->water_top && g->dim)
            s->light[i] = 0xFF000000u | px_rgb_scale(g->water_rgb, 70);
      }
}

/* ---------------------------------------------------------------------------
 * What happens
 * ------------------------------------------------------------------------- */

/* What voice 0 plays: see the sounds, below. */
enum { V0_NONE = 0, V0_LASER, V0_DEATH, V0_BEEP, V0_OTHER };
enum { V1_NONE = 0, V1_WHIRR, V1_FUSE, V1_BLAST, V1_COUNT, V1_FILL, V1_OTHER };

static unsigned voice0_plays(const px_kit_tia *t, unsigned before)
{
   const unsigned wave = t->wave[0], pitch = t->pitch[0], volume = t->volume[0];
   if (!wave || !volume)
      return V0_NONE;
   if (wave != 4 && wave != 12)
      return V0_OTHER;
   /* The death begins louder than the laser, and goes on softer. */
   if (pitch == 15 && (volume >= 6 || before == V0_DEATH) && (wave == 4 || before == V0_DEATH || volume >= 6))
      return V0_DEATH;
   if (wave == 12 && pitch == 15 && !t->was_wave[0])
      return V0_BEEP;
   if (before == V0_BEEP && wave == 12 && pitch == 15)
      return V0_BEEP;
   return V0_LASER;
}

static unsigned score_of(const px_scene *s)
{
   const int a = px_kit_ram(s->ram, s->ram_size, RAM_SCORE), b = px_kit_ram(s->ram, s->ram_size, RAM_SCORE + 1),
         c = px_kit_ram(s->ram, s->ram_size, RAM_SCORE + 2);
   if (a < 0 || b < 0 || c < 0)
      return ~0u;
   return (unsigned)((a >> 4) * 100000 + (a & 15) * 10000 + (b >> 4) * 1000 + (b & 15) * 100
         + (c >> 4) * 10 + (c & 15));
}

static void events(hr *g, px_scene *s, bool flash_began)
{
   const unsigned v0 = voice0_plays(&g->seen, g->seen0);
   const bool dying = v0 == V0_DEATH;
   const unsigned score = score_of(s);
   const int rescued = px_kit_ram(s->ram, s->ram_size, RAM_RESCUED);
   const int screen = px_kit_ram(s->ram, s->ram_size, RAM_SCREEN);
   const int hx = g->hero_x >= 0 ? g->hero_x + 4 : PXC_W / 2, hy = g->hero_x >= 0 ? g->hero_y + g->hero_h / 2 : 90;

   for (unsigned i = 0; i < 4; i++)
      if (g->flares[i].strength)
         g->flares[i].strength = (uint16_t)(g->flares[i].strength * g->flares[i].fade >> 8);

   /* The hero dies. */
   if (dying && g->seen0 != V0_DEATH)
   {
      add_flare(g, hx, hy, 80, 420, 230, 0xFF5028);
      if (g->sparks)
      {
         px_scene_burst(s, hx, hy, 0xFFF4D0, 24, 360);
         px_scene_burst(s, hx, hy, 0xFF6030, 40, 300);
         px_scene_burst(s, hx, hy, 0x5078FF, 16, 220);
      }
   }

   /* The game flashes: a blast, or one of the flashes of a death. */
   if (flash_began)
   {
      if (dying || g->seen0 == V0_DEATH)
         add_flare(g, hx, hy, 150, 180, 200, 0xFF3A20);
      else if (g->flash_gap > 16)
      {
         /* A blast: where the dynamite was. */
         const bool seen = g->frame - g->dynamite_at < 90;
         const int x = seen ? g->dynamite_x : hx, y = seen ? g->dynamite_y + 4 : hy;
         add_flare(g, x, y, 220, 700, 205, 0xFFE0A8);
         add_flare(g, x, y - 6, 60, 360, 244, 0xB08868);   /* the dust */
         if (g->sparks)
         {
            const uint32_t rock = g->row_rgb[y > ROW_BODY && y < ROW_PANEL ? y : 90];
            px_scene_burst(s, x, y, 0xFFFFFF, 20, 420);
            px_scene_burst(s, x, y, 0xFFB040, 44, 380);
            px_scene_burst(s, x, y, px_rgb_add(rock, 0x202020), 40, 300);
            px_scene_burst(s, x, y, 0x806850, 24, 140);
         }
      }
      else
         add_flare(g, g->frame - g->dynamite_at < 90 ? g->dynamite_x : hx, hy, 180, 300, 205, 0xFFD8A0);
   }

   /* A wall gone: debris all along it. */
   if (g->sparks && g->was_wall_count > g->wall_count && screen == g->screen)
      for (unsigned w = 0; w < g->was_wall_count; w++)
      {
         bool still = false;
         for (unsigned k = 0; k < g->wall_count; k++)
            still = still || g->wall_x[k] == g->was_wall_x[w];
         if (still)
            continue;
         for (int y = g->was_wall_top[w] + 4; y < g->was_wall_bottom[w]; y += 10)
            px_scene_burst(s, g->was_wall_x[w] + 4, y, px_rgb_add(g->row_rgb[y], 0x181818), 8, 260);
      }

   /* A creature shot: the score goes up by 50. */
   if (score != ~0u && g->score >= 0 && score == (unsigned)g->score + 50 && g->were_count > 0)
   {
      const int from_x = g->laser_x >= 0 ? g->laser_x + (g->facing < 0 ? 0 : g->laser_w) : hx;
      unsigned best = 0;
      int d_best = 1 << 30;
      for (unsigned i = 0; i < g->were_count; i++)
      {
         const int dx = g->were[i].x - from_x, dy = g->were[i].y - hy, d = dx * dx * 4 + dy * dy;
         if (d < d_best)
         {
            d_best = d;
            best = i;
         }
      }
      add_flare(g, g->were[best].x, g->were[best].y, 40, 260, 215, 0xFFA060);
      if (g->sparks)
      {
         px_scene_burst(s, g->were[best].x, g->were[best].y, g->were[best].rgb, 26, 280);
         px_scene_burst(s, g->were[best].x, g->were[best].y, 0xFFE8B0, 12, 200);
      }
   }

   /* The miner is reached. */
   if (rescued > 0 && g->rescued == 0)
      g->party = 150;
   if (g->party)
   {
      const int mx = g->frame - g->miner_at < 30 ? g->miner_x : hx, my = g->frame - g->miner_at < 30 ? g->miner_y : hy;
      if (g->sparks && g->party % 10 == 0)
      {
         static const uint32_t festive[6] = { 0xFFD840, 0x60E0FF, 0xFF60C0, 0x80FF70, 0xFFFFFF, 0xFF8040 };
         const int x = mx + (int)(px_kit_chance(&g->seed) % 41) - 20;
         const int y = my - 12 - (int)(px_kit_chance(&g->seed) % 30);
         px_scene_burst(s, x, y > ROW_CEILING + 4 ? y : ROW_CEILING + 4, festive[(g->party / 10) % 6], 26, 260);
         add_flare(g, x, y, 50, 160, 225, festive[(g->party / 10) % 6]);
      }
      g->party--;
   }

   /* The lights go out: the lantern's glass flies. */
   if (g->dark && g->dim == 256 && screen == g->screen && g->sparks)
   {
      for (unsigned i = 0; i < PX_KIT_TAGS; i++)
         if (g->known.slot[i].id && g->known.slot[i].tag == KIND_LANTERN)
            px_scene_burst(s, g->known.slot[i].x + 4, g->known.slot[i].y + 4, 0xFFF0B0, 18, 220);
   }

   /* The fuse spits. */
   if (g->sparks && g->frame - g->dynamite_at < 2 && g->frame % 3 == 0)
      px_scene_burst(s, g->dynamite_x, g->dynamite_y - 2, 0xFFD060, 3, 120);

   g->seen0 = v0;
   if (score != ~0u)
      g->score = (int)score;
   g->rescued = rescued;
   g->screen = screen;
}

static void frame(void *state, px_scene *s)
{
   hr *g = (hr*)state;
   bool flash_was, flash_began;

   if (s->frame->height < ROWS || !s->frame->tags)
      return;
   if (s->advance)
   {
      g->frame++;
      px_kit_tia_hear(&g->seen, s->frame);
   }

   keep_display(s);
   flash_was = g->flashing;
   look(g, s);
   flash_began = g->flashing && !flash_was;
   if (s->advance)
   {
      if (g->dark)
         g->dim = g->dim > FADE ? g->dim - FADE : 0;
      else
         g->dim = g->dim + FADE < 256 ? g->dim + FADE : 256;
      g->flash_gap = flash_began ? 0 : g->flash_gap < 1000 ? g->flash_gap + 1 : g->flash_gap;
   }

   /* What was there in the frame before, for what is gone from this one. */
   if (s->advance)
   {
      memcpy(g->were, g->creatures, sizeof(g->were));
      g->were_count = g->creature_count;
      memcpy(g->was_wall_x, g->wall_x, sizeof(g->wall_x));
      memcpy(g->was_wall_top, g->wall_top, sizeof(g->wall_top));
      memcpy(g->was_wall_bottom, g->wall_bottom, sizeof(g->wall_bottom));
      g->was_wall_count = g->wall_count;
   }
   g->lamp_count = 0;
   objects(g, s);
   if (s->advance)
      events(g, s, flash_began && (g->backdrop || g->sparks));

   make_light(g, s);
   if (g->colors == COLORS_ROCK)
      paint_walls(g, s);

   if (g->backdrop)
   {
      show_backdrop(g, s);
      paint_backdrop(g, s);
   }
   else
   {
      g->painted = false;
      /* The game's flash, with light of Proteus's in its place: the border and the display
       * are blank, which it does not fall on. */
      if (g->sparks && flash_began && s->advance)
         px_scene_flash(s, g->seen0 == V0_DEATH ? 0xFF3A20 : 0xFFE0A8, g->flash_gap > 16 ? 150 : 90);
      if (g->sparks && g->flashing)
         for (unsigned y = ROW_CEILING; y < ROW_PANEL; y++)
            for (unsigned x = 0; x < PXC_W; x++)
            {
               const size_t i = (size_t)y * PXC_W + x;
               if (PX_KEY_CLS(s->top[i]) == PX_CLS_BK && !(g->water_top && y >= g->water_top))
               {
                  s->top[i] = PX_KEY(PX_CLS_BK, 0);
                  s->bk[i]  = 0;
               }
            }
   }
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 *   voice 0   the laser, for as long as the button is held: waveform 4 at pitches 28, 31 and
 *             waveform 12 at pitches 12, 15 in turns, two frames each, volume 5
 *   voice 0   the hero dies: waveforms 12 and 4 in turns at pitch 15, volume 9 down to 3 in
 *             about 106 frames
 *   voice 0   the power counted off: waveform 12 at pitch 15 for two frames of every eight,
 *             volume 5
 *   voice 0   silent: waveform 0, at volume 5
 *   voice 1   the propeller: noise (waveform 8) at pitches 16, 23, 19 in turns, a frame each,
 *             volumes 1, 3, 2, for as long as the hero flies
 *   voice 1   the fuse: noise at pitches 1 to 3, volumes 1 to 3
 *   voice 1   the blast: noise at pitch 16, volume 15 down to 1 in 30 frames
 *   voice 1   a stick of dynamite counted: noise at pitch 16, volume 7 down to 1, every 16
 *             frames
 *   voice 1   a level begins: noise at pitch 31 down to 16, volume 4, 81 frames, while the
 *             power bar fills
 *
 * A creature shot makes no sound of its own. No tune was heard in the game.
 * ------------------------------------------------------------------------- */

static unsigned voice1_plays(const px_kit_tia *t, unsigned before)
{
   const unsigned pitch = t->pitch[1], volume = t->volume[1];
   if (!volume || t->wave[1] == 0)
      return V1_NONE;
   if (t->wave[1] != 8)
      return V1_OTHER;
   if (pitch == 16 && volume >= 8 && volume > t->was_volume[1])
      return V1_BLAST;
   if (pitch == 16 && volume == 7 && volume > t->was_volume[1])
      return V1_COUNT;
   if ((before == V1_BLAST || before == V1_COUNT) && pitch == 16 && volume <= t->was_volume[1])
      return before;
   if (pitch <= 3)
      return V1_FUSE;
   if (volume == 4 && (before == V1_FILL || pitch >= 24))
      return V1_FILL;
   if (pitch == 16 || pitch == 19 || pitch == 23)
      return V1_WHIRR;
   return V1_OTHER;
}

static void stop(px_sound *s, unsigned *id, float seconds)
{
   if (*id)
      px_synth_stop(s->synth, *id, seconds);
   *id = 0;
}

/* A sound that goes on: started, or moved. */
static void hold(px_sound *s, unsigned *id, const px_tone *tone, float pan, float gain, float freq)
{
   if (!px_synth_move(s->synth, *id, pan, gain, freq))
   {
      px_tone t = *tone;
      if (freq > 0.0f)
         t.freq = freq;
      *id = px_synth_play(s->synth, &t, pan, gain);
   }
}

/* The laser: a buzzing beam, a bright sizzle over it, and a hum. */
static void play_laser(hr *g, px_sound *s, float pan)
{
   static const px_tone beam = { PX_WAVE_SAW, 660, 0, 0, 0.01f, 0, 0, 0.22f, 2600, 0, 15.0f, 0.03f };
   hold(s, &g->laser, &beam, pan, 1.0f, 0);
   if (g->heard0 != V0_LASER)
   {
      static const px_tone zap[3] = {
         /* wave           freq  to    glide  attack  hold   decay  gain   cutoff to */
         { PX_WAVE_SQUARE, 2400, 900,  0.08f, 0.001f, 0.01f, 0.10f, 0.14f, 6000, 1500, 0, 0 },
         { PX_WAVE_SINE,   1800, 1200, 0.05f, 0.001f, 0,     0.08f, 0.16f, 0, 0, 0, 0 },
         { PX_WAVE_NOISE,  9000, 0,    0.03f, 0,      0,     0.04f, 0.08f, 8000, 3000, 0, 0 }
      };
      px_kit_play(s, zap, 3, pan);
   }
   /* The buzz comes and goes with the game's own. */
   if (g->tia.wave[0] == 4 && g->frame % 4 == 0)
   {
      static const px_tone crackle = { PX_WAVE_SINE, 3600, 2400, 0.03f, 0.001f, 0, 0.04f, 0.05f, 0, 0, 0, 0 };
      px_synth_play(s->synth, &crackle, pan, 1.0f);
   }
}

/* The hero dies: a cry that falls, a crash, a blow that shakes. */
static void play_death(px_sound *s, float pan)
{
   static const px_tone p[5] = {
      { PX_WAVE_SAW,     880, 110, 1.40f, 0.005f, 0.10f, 1.60f, 0.20f, 3000, 400, 7.0f, 0.04f },
      { PX_WAVE_SQUARE,  440,  55, 1.40f, 0.005f, 0.10f, 1.60f, 0.10f, 1800, 300, 7.0f, 0.04f },
      { PX_WAVE_NOISE,  6000, 300, 0.80f, 0,      0.05f, 1.00f, 0.36f, 5000, 200, 0, 0 },
      { PX_WAVE_SINE,    110,  30, 0.60f, 0.002f, 0.08f, 0.90f, 0.70f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 1320, 165, 1.40f, 0.01f, 0.10f, 1.40f, 0.08f, 0, 0, 6.0f, 0.05f }
   };
   px_kit_play(s, p, 5, pan);
   px_sound_rumble(s, 60000, 40000, 40);
}

/* The dynamite: a crack, a roar that falls away, a deep boom, stones that rattle. */
static void play_blast(px_sound *s, float pan)
{
   static const px_tone p[5] = {
      { PX_WAVE_NOISE, 9000,  600, 0.40f, 0,      0.03f, 0.80f, 0.46f, 8000, 400, 0, 0 },
      { PX_WAVE_NOISE, 1800,  120, 1.20f, 0.02f,  0.10f, 1.60f, 0.36f, 1400, 90, 0, 0 },
      { PX_WAVE_SINE,    90,   26, 0.70f, 0.002f, 0.10f, 1.20f, 0.85f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    300,   45, 0.50f, 0.002f, 0.04f, 0.70f, 0.16f, 1500, 200, 11.0f, 0.05f },
      { PX_WAVE_NOISE, 3000, 1200, 1.40f, 0.30f,  0.20f, 1.00f, 0.10f, 3500, 1500, 23.0f, 0.30f }
   };
   px_kit_play(s, p, 5, pan);
   px_sound_rumble(s, 65535, 60000, 36);
}

/* A stick of dynamite counted: a thud, and a spark. */
static void play_count(px_sound *s, float pan)
{
   static const px_tone p[3] = {
      { PX_WAVE_SINE,   140,  45, 0.18f, 0.001f, 0.02f, 0.30f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 4000, 500, 0.20f, 0,      0.01f, 0.26f, 0.22f, 3000, 300, 0, 0 },
      { PX_WAVE_SINE,  1760, 1760, 0,    0.001f, 0,     0.20f, 0.06f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, pan);
   px_sound_rumble(s, 16000, 20000, 6);
}

/* A beep of the power counted off: the game's note, as a bell. */
static void play_beep(hr *g, px_sound *s, float pan)
{
   const float hz = px_kit_tune(px_kit_tia_hz(g->tia.wave[0], g->tia.pitch[0]));
   px_tone p[3] = {
      { PX_WAVE_SINE,     0, 0, 0, 0.001f, 0.01f, 0.35f, 0.26f, 0, 0, 0, 0 },
      { PX_WAVE_TRIANGLE, 0, 0, 0, 0.001f, 0,     0.20f, 0.12f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,     0, 0, 0, 0.001f, 0,     0.10f, 0.06f, 0, 0, 0, 0 }
   };
   if (hz <= 0.0f)
      return;
   p[0].freq = hz;
   p[1].freq = hz * 2.0f;
   p[2].freq = hz * 6.0f;
   px_kit_play(s, p, 3, pan);
}

/* A creature shot: a squelch and a pop. */
static void play_kill(px_sound *s, float pan)
{
   static const px_tone p[3] = {
      { PX_WAVE_SINE,   700, 140, 0.14f, 0.001f, 0.01f, 0.18f, 0.40f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE, 5000, 900, 0.10f, 0,      0.01f, 0.12f, 0.22f, 4000, 700, 0, 0 },
      { PX_WAVE_SQUARE, 1400, 2600, 0.06f, 0.001f, 0,   0.08f, 0.06f, 5000, 5000, 0, 0 }
   };
   px_kit_play(s, p, 3, pan);
   px_sound_rumble(s, 0, 14000, 5);
}

/* The miner reached: a shimmer that rises. */
static void play_rescue(px_sound *s, float pan)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE, 3000, 9000, 0.60f, 0.10f, 0.10f, 0.60f, 0.10f, 4000, 9000, 0, 0 },
      { PX_WAVE_SINE,  1046.5f, 2093.0f, 0.60f, 0.05f, 0.20f, 0.80f, 0.10f, 0, 0, 6.0f, 0.01f },
      { PX_WAVE_SINE,  1318.5f, 2637.0f, 0.60f, 0.05f, 0.20f, 0.80f, 0.08f, 0, 0, 5.0f, 0.01f }
   };
   px_kit_play(s, p, 3, pan);
   px_sound_rumble(s, 0, 16000, 10);
}

static void sound(void *state, px_sound *s)
{
   hr *g = (hr*)state;
   const int rescued = px_kit_ram(s->ram, s->ram_size, RAM_RESCUED);
   unsigned heard0, heard1, score;
   float pan;


   if (s->objects)
      for (unsigned i = 0; i < s->objects->count; i++)
      {
         const px_instance *in = &s->objects->inst[i];
         if (in->role == PX_ROLE_PLAYER && !in->ghost)
            g->at = in->x + 4;
      }
   pan = px_kit_pan(g->at) * 0.7f;

   px_kit_tia_hear(&g->tia, s->frame);
   heard0 = voice0_plays(&g->tia, g->heard0);
   heard1 = voice1_plays(&g->tia, g->heard1);

   /* Voice 0. */
   if (heard0 == V0_LASER && g->own_sound)
      play_laser(g, s, pan);
   else
      stop(s, &g->laser, 0.06f);
   if (heard0 == V0_DEATH && g->heard0 != V0_DEATH)
   {
      if (g->own_sound) play_death(s, pan);
      else              px_sound_rumble(s, 60000, 40000, 40);
   }
   if (heard0 == V0_BEEP && g->heard0 != V0_BEEP && g->own_sound)
      play_beep(g, s, pan * 0.5f);

   /* Voice 1. */
   if (heard1 == V1_WHIRR && g->own_sound)
   {
      /* The rotor: air chopped as the game chops it, and a hum. */
      static const px_tone air = { PX_WAVE_NOISE, 1400, 0, 0, 0.02f, 0, 0, 1.0f, 900, 0, 0, 0 };
      static const px_tone hum = { PX_WAVE_SAW, 58, 0, 0, 0.04f, 0, 0, 1.0f, 300, 0, 0, 0 };
      const float loud = (float)g->tia.volume[1] / 3.0f;
      const float chop = g->tia.pitch[1] == 16 ? 1.0f : g->tia.pitch[1] == 19 ? 0.55f : 0.25f;
      hold(s, &g->whirr[0], &air, pan, 0.20f * loud * chop, 700.0f + 2400.0f / (float)(g->tia.pitch[1] + 1) * 8.0f);
      hold(s, &g->whirr[1], &hum, pan, 0.16f * loud, 31440.0f / 32.0f / (float)(g->tia.pitch[1] + 1) * 1.1f);
   }
   else
   {
      stop(s, &g->whirr[0], 0.08f);
      stop(s, &g->whirr[1], 0.08f);
   }
   if (heard1 == V1_FUSE && g->own_sound)
   {
      static const px_tone hiss = { PX_WAVE_NOISE, 8000, 0, 0, 0.01f, 0, 0, 1.0f, 6500, 0, 0, 0 };
      hold(s, &g->fuse, &hiss, pan, 0.05f + 0.04f * (float)g->tia.volume[1], 0);
      if (px_kit_chance(&g->seed) % 3 == 0)
      {
         static const px_tone crack = { PX_WAVE_NOISE, 6000, 0, 0, 0, 0, 0.015f, 0.18f, 5000, 0, 0, 0 };
         px_synth_play(s->synth, &crack, pan, 1.0f);
      }
   }
   else
      stop(s, &g->fuse, 0.05f);
   if (heard1 == V1_BLAST && g->heard1 != V1_BLAST)
   {
      if (g->own_sound) play_blast(s, pan);
      else              px_sound_rumble(s, 65535, 60000, 36);
   }
   if (heard1 == V1_COUNT && g->heard1 != V1_COUNT)
   {
      if (g->own_sound) play_count(s, pan * 0.5f);
      else              px_sound_rumble(s, 16000, 20000, 6);
   }
   if (heard1 == V1_FILL && g->own_sound)
   {
      /* The power fills: a charge that rises as the game's pitch does. */
      static const px_tone charge = { PX_WAVE_SAW, 110, 0, 0, 0.05f, 0, 0, 1.0f, 1800, 0, 6.0f, 0.01f };
      static const px_tone shine = { PX_WAVE_SINE, 440, 0, 0, 0.05f, 0, 0, 1.0f, 0, 0, 9.0f, 0.02f };
      const float f = 3520.0f / (float)(g->tia.pitch[1] + 1);
      hold(s, &g->fill[0], &charge, 0.0f, 0.12f, f);
      hold(s, &g->fill[1], &shine, 0.0f, 0.06f, f * 4.0f);
   }
   else
   {
      stop(s, &g->fill[0], 0.15f);
      stop(s, &g->fill[1], 0.15f);
   }

   /* What the picture does not hear: a creature shot, the miner reached. */
   {
      const int a = px_kit_ram(s->ram, s->ram_size, RAM_SCORE), b = px_kit_ram(s->ram, s->ram_size, RAM_SCORE + 1),
            c = px_kit_ram(s->ram, s->ram_size, RAM_SCORE + 2);
      score = a < 0 || b < 0 || c < 0 ? ~0u : (unsigned)((a >> 4) * 100000 + (a & 15) * 10000
            + (b >> 4) * 1000 + (b & 15) * 100 + (c >> 4) * 10 + (c & 15));
   }
   if (score != ~0u && g->sound_score >= 0 && score == (unsigned)g->sound_score + 50 && heard1 != V1_BLAST)
   {
      if (g->own_sound) play_kill(s, pan);
      else              px_sound_rumble(s, 0, 14000, 5);
   }
   if (rescued > 0 && g->rescued == 0 && g->own_sound)
      play_rescue(s, pan * 0.5f);
   g->sound_score = score == ~0u ? -1 : (int)score;

   if (g->own_sound)
   {
      if (heard0 == V0_LASER || heard0 == V0_DEATH || heard0 == V0_BEEP)
         s->voice[0] = 0.0f;
      if (heard1 != V1_NONE && heard1 != V1_OTHER)
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
   hr *g = (hr*)state;
   px_kit_tags_reset(&g->known);
   px_kit_tia_reset(&g->seen);
   px_kit_tia_reset(&g->tia);
   g->dark = false;
   g->dim = 256;
   memset(g->row_rgb, 0, sizeof(g->row_rgb));
   memset(g->lava, 0, sizeof(g->lava));
   memset(g->flares, 0, sizeof(g->flares));
   g->water_top = 0;
   g->flashing = false;
   g->flash_gap = 1000;
   g->screen = -1;
   g->hero_x = g->laser_x = -1;
   g->hero_y = 90;
   g->hero_h = 20;
   g->facing = 1;
   g->dynamite_x = g->dynamite_y = g->miner_x = g->miner_y = 0;
   g->dynamite_at = g->miner_at = 0u - 1000u;
   g->creature_count = g->were_count = 0;
   g->wall_count = g->was_wall_count = 0;
   g->lamp_count = 0;
   g->painted = false;
   g->seen0 = V0_NONE;
   g->score = -1;
   g->rescued = -1;
   g->party = 0;
   g->seed = 0x4E120u;
   g->heard0 = V0_NONE;
   g->heard1 = V1_NONE;
   g->laser = g->fuse = 0;
   g->whirr[0] = g->whirr[1] = g->fill[0] = g->fill[1] = 0;
   g->at = -1;
   g->sound_score = -1;
}

static void *create(void)
{
   hr *g = (hr*)calloc(1, sizeof(hr));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = true;
      g->stone_of = ~0u;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   hr *g = (hr*)state;
   if (g)
   {
      px_kit_texture_free(&g->rock);
      px_kit_canvas_free(&g->stone);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   hr *g = (hr*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->vivid_of  = NULL;
   g->painted   = false;
}

const px_game px_game_hero = {
   "H.E.R.O.", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
