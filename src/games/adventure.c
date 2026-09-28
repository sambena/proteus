/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Adventure (Atari, 1980).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  13..205  the room, one screen at a time: the playfield in one colour on a grey
 *                  background (colour 08). There are 31 rooms, numbered in memory.
 *   thin walls     the two missiles, two pixels wide and as high as the room (rooms 01, 03,
 *                  0C, 0D). They have the colour of whatever their player object shows, so
 *                  a wall is yellow while the yellow dragon is in the room.
 *   the player     the ball, 4 by 8, in the colour of the room's walls
 *   all else       the two player objects, two things a frame: the dragons (8 by 40, 44
 *                  while they bite, 34 dead), the bat (8 by 22), the keys (8 by 6), the
 *                  sword (8 by 10), the chalice (8 by 18, its colour two further every
 *                  frame), the magnet (8 by 16), the bridge (32 by 48), the gates of the
 *                  castles (8 by 32 when shut), the number of the game, the dot, and the
 *                  author's name in room 1E.
 *   flicker        the game does its work in turns of three frames, and what the player
 *                  objects show changes with them. With three things in a room each is
 *                  shown for three frames with player 1, for three with player 0 and for
 *                  three not at all. With four, each is shown for three frames of six,
 *                  always with the same player object and the same one other: the sword and
 *                  the dragon may never be in one frame, and then the sword does not slay
 *                  it.
 *   the dark mazes rooms 09 to 0B and 13 to 16. The walls are there, in the colour of the
 *                  background, and do not show. What shows them is a square of orange
 *                  (colour 28), a player object of 32 by 64 around the ball, which the
 *                  playfield is drawn over. It takes one of the two player objects, so
 *                  the dark mazes flicker sooner. The dot is of the background's colour
 *                  too: it shows over the orange and over walls.
 *
 * Every effect that treats the background and not the playfield shows the walls of the
 * dark mazes, which the game hides. The rooms painted here hide them again.
 *
 * Of its memory ($80 is 0):
 *
 *   10, 11, 12     the player's room, x and y. A thing at x, y is drawn from column x - 1
 *                  (the ball: x - 2) and row 224 - 2 y.
 *   21, 22         what player 0 and player 1 show in this frame: 00 the orange square,
 *                  09 12 1B the gates (yellow, white, black), 24 the author, 2D the number,
 *                  36 3F 48 the dragons (red, yellow, green), 51 the sword, 5A the bridge,
 *                  63 6C 75 the keys (yellow, white, black), 7E the bat, 87 the dot,
 *                  90 the chalice, 99 the magnet, A2 nothing. They agree with what is
 *                  drawn in the same frame.
 *   29             what the player carries, as above; A2 nothing
 *   36.., 41.., 46..  the dragons, red, yellow, green: room, x, y, movement, state. The
 *                  state is 0 alive, 1 dead, 2 having eaten the player, and counts up from
 *                  D0 while the dragon bites.
 *   51, 54, 57, 60, 63, 66, 69, 75, 82   room, x, y of the magnet, the sword, the chalice,
 *                  the bridge, the three keys, the bat and the dot
 *   94             FF once the chalice is in the yellow castle (room 12): the game is won
 *   95, 96         the sound: what is left of it, and which it is (1 bite, 2 eaten,
 *                  3 slain, 4 put down, 5 picked up)
 *
 * The yellow dragon runs from the yellow key, which is why it does not bite in front of
 * the yellow castle.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_av_colors"
#define OPT_BACKDROP "proteus_av_backdrop"
#define OPT_STEADY   "proteus_av_steady"
#define OPT_SPARKS   "proteus_av_sparks"
#define OPT_SOUND    "proteus_av_sound"
#define OPT_AMBIENCE "proteus_av_ambience"

#define RAM_ROOM     10
#define RAM_SHOWN    21    /* and 22 */
#define RAM_CARRIED  29
#define RAM_DRAGONS  36    /* three of five bytes */
#define RAM_WON      94

#define DRAGON_BYTES 5
#define DRAGON_STATE 4
#define STATE_DEAD   1
#define STATE_ATE    2
#define STATE_BITES  0x80  /* and above */

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define ROOM_TOP     13     /* the first row of a room */
#define COLOR_GREY   0x08   /* the background, the dot, and the walls of the dark mazes */
#define COLOR_ORANGE 0x28   /* the square that shows them */
#define DRAGON_HEAD  14     /* rows of a dragon that are its head */
#define SHAPE_ROWS   64     /* the highest thing that is shown in turns: the orange square */
#define TURNS        6      /* frames a thing is not shown for, at most, while it is there */

/* What a player object shows: memory's number for it, by nine. */
enum
{
   OBJ_SQUARE = 0, OBJ_GATE_YELLOW, OBJ_GATE_WHITE, OBJ_GATE_BLACK, OBJ_AUTHOR, OBJ_NUMBER,
   OBJ_RED, OBJ_YELLOW, OBJ_GREEN, OBJ_SWORD, OBJ_BRIDGE, OBJ_KEY_YELLOW, OBJ_KEY_WHITE,
   OBJ_KEY_BLACK, OBJ_BAT, OBJ_DOT, OBJ_CHALICE, OBJ_MAGNET, OBJ_NOTHING, OBJ_UNKNOWN
};

/* A room is painted in blocks of stone and slabs of floor, in pixels of the capture. */
#define BLOCK_W  8
#define BLOCK_H  8
#define SLAB_W   16
#define SLAB_H   16

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "157bddb7192754a45372be196797f284",   /* Adventure (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "medium",
   "width", "50",
   "reverb", "hall",
   NULL
};

static const char *const colors[] = { "proteus", "Proteus's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "stone", "Stone and torchlight", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_PROTEUS = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "Stronger colours for the dragons with eyes that show, steel for the sword, iron for the gates and the black key, or the game's own colours.",
     "proteus", colors },
   { OPT_BACKDROP, "Backdrop",
     "The rooms painted: walls of stone in the colour the game gives them, a dark floor, and in the dark mazes the light of a lantern where the game has a square of orange. Off: the rooms as the game draws them.",
     "stone", backdrop },
   { OPT_STEADY, "Steady objects",
     "With three things in a room the game shows each in six frames of nine, and the light in the dark mazes goes out with them. Draw them in every frame. Needs flicker fusion.",
     "enabled", px_kit_toggle },
   { OPT_SPARKS, "Explosions",
     "Sparks and a flash when a dragon bites, eats or is slain, and a glitter around the chalice.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for picking up and putting down, the dragon's bite, its death and the player's, each where it happens between left and right, and the notes of the game's end as chimes. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_AMBIENCE, "Ambience",
     "Wind around the castles, a low drone in the dark mazes, and a growl while a dragon that lives is in the room. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps
 * ------------------------------------------------------------------------- */

/* The lantern of the dark mazes. */
typedef struct
{
   int x0, y0, x1, y1;   /* in the picture; x1 and y1 are beyond it */
   int cx, cy;           /* the middle of the light */
   unsigned level;       /* how bright it burns, of 256 */
   bool on;
} lantern;

/* A thing as it was seen last. */
typedef struct
{
   px_instance as;
   uint32_t bits[SHAPE_ROWS];
   uint8_t colors[SHAPE_ROWS];
   uint32_t seen;        /* the frame it was seen in; 0: not yet */
   int dx, dy;           /* from the ball, if that was there: `held` */
   bool held;
} shape;

typedef struct
{
   /* The options. */
   unsigned colors;
   bool rooms, steady, sparks, own_sound, ambience;

   uint32_t frame;               /* counts the frames that advance */
   px_kit_tags known;            /* tagged with what each object is: OBJ_* */
   shape shapes[OBJ_NOTHING];    /* what was seen of each thing there is */
   uint64_t walls;               /* of the frame before: other walls are another room */

   /* Memory as the frame before had it; -1 before the first. */
   int dragon[3], carried, won, room;

   /* The room as it is painted. */
   px_kit_canvas lit;            /* walls and floor */
   px_kit_canvas unlit;          /* a dark maze where the lantern is not: the floor alone */
   uint8_t wall[PXC_W * PXC_MAX_H];
   uint64_t painted_walls;       /* what tells one room's walls from another's */
   uint32_t painted_rgb;         /* and the colour the game gave them */
   bool painted, painted_dark;
   uint32_t new_rgb;             /* another colour, and for how many frames it has been there */
   unsigned new_for;
   lantern lamp;                 /* as it is in the backdrop */

   /* For the sounds: what frame() saw. */
   bool dark;                    /* the room is a dark maze */
   bool dragon_near;             /* a dragon that lives is in the picture */
   int player_at, dragon_at;     /* columns; -1: not known */

   px_kit_tia tia;               /* the game's two voices */
   unsigned heard;               /* what voice 0 played in the frame before: SOUND_* */
   bool rising;                  /* of picking up and putting down: which it is */
   unsigned wind, drone_low, drone_high, growl;   /* the ambience's voices at the synth */
} av;

/* ---------------------------------------------------------------------------
 * Colours
 * ------------------------------------------------------------------------- */

/* A colour at `f256` of 256 of its brightness, which may be more than all of it. */
static inline uint32_t lit(uint32_t rgb, unsigned f256)
{
   unsigned r = (((rgb >> 16) & 0xFF) * f256) >> 8;
   unsigned g = (((rgb >> 8) & 0xFF) * f256) >> 8;
   unsigned b = ((rgb & 0xFF) * f256) >> 8;
   return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

/* A colour through a filter: each channel at that much of 256 of the filter's. */
static inline uint32_t through(uint32_t rgb, uint32_t filter)
{
   unsigned r = (((rgb >> 16) & 0xFF) * (((filter >> 16) & 0xFF) + 1)) >> 8;
   unsigned g = (((rgb >> 8) & 0xFF) * (((filter >> 8) & 0xFF) + 1)) >> 8;
   unsigned b = ((rgb & 0xFF) * ((filter & 0xFF) + 1)) >> 8;
   return (r << 16) | (g << 8) | b;
}

static inline unsigned brightest(uint32_t rgb)
{
   unsigned r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
   return r > g ? (r > b ? r : b) : (g > b ? g : b);
}

/* Two numbers made one that looks like chance: the same for the same two. */
static inline uint32_t chance_of(uint32_t a, uint32_t b)
{
   uint32_t h = (a * 0x9E3779B1u) ^ ((b + 0x7F4A7C15u) * 0x85EBCA6Bu);
   h ^= h >> 15;
   h *= 0x2C1B3C6Du;
   return h ^ (h >> 12);
}

/* The stone of a wall the game gives a colour: a little of the colour taken out of it. The
 * black castle's is slate, which shows on a dark floor. */
static uint32_t stone_of(uint32_t rgb)
{
   const unsigned most = brightest(rgb);
   if (most < 0x40)
      return 0x4A4468;
   return px_rgb_mix(rgb, most * 0x010101u, 56);
}

/* The light a wall gives off. */
static uint32_t glow_of(uint32_t rgb)
{
   return brightest(rgb) < 0x40 ? 0x2A1C50 : px_rgb_scale(rgb, 72);
}

/* ---------------------------------------------------------------------------
 * The rooms, painted
 *
 * The playfield of a room stands still, so a room is painted once, when the player comes
 * into it, as large as the picture: blocks of stone where the playfield is, slabs where it
 * is not. It is shown as the backdrop, and the playfield is made background for it to show
 * through. While the game changes the colour of the walls from frame to frame (it does
 * when it is won) the walls are left as the game draws them, over the floor.
 * ------------------------------------------------------------------------- */

/* What a column of the picture is of a slab of the floor or of a block of a wall: which of
 * them it is in, and how it is lit there. */
enum { PART_JOINT = 0, PART_LIGHT, PART_FACE, PART_SHADE };

typedef struct
{
   uint16_t x;               /* the capture's column */
   uint8_t  left, right;     /* at the capture's pixel's left or right edge */
   uint8_t  slab_part[2];    /* in a course of slabs that is even, and in one that is odd */
   uint8_t  block_part[2];
   uint16_t slab[2], block[2];
   int16_t  shade, half_shade;   /* the capture's columns a shadow comes from; -1: none */
   uint16_t away;            /* from the middle of the picture, squared, of 256 */
} column;

static unsigned part_of(unsigned at, unsigned size, unsigned joint)
{
   return at < joint ? PART_JOINT : at < 2 * joint ? PART_LIGHT : at + joint >= size ? PART_SHADE : PART_FACE;
}

/* Paints the room the frame shows: blocks of stone in courses, slabs on the floor, all lit
 * from the upper left, so that the walls' edges are bright where they face the light and
 * dark where they do not, and the walls throw a shadow on the floor to the lower right.
 * `rgb` is the colour the game gives its walls. */
static bool paint_room(av *g, const px_scene *s, uint32_t rgb, bool dark)
{
   static const uint16_t slab_light[4]  = { 96, 226, 200, 200 };
   static const uint16_t block_light[4] = { 150, 262, 232, 198 };
   const unsigned w = s->w, h = s->h, sx = s->sx, sy = s->sy, rows = s->frame->height;
   const unsigned jx = sx >= 4 ? sx / 4 : 1, jy = sy >= 2 ? sy / 2 : 1;
   const unsigned shade_x = sx * 3 / 2, shade_y = sy * 3;
   const uint32_t stone = stone_of(rgb);
   /* The floor has a little of the walls' colour. In the dark it has the lantern's. */
   const uint32_t ground = dark ? 0x3A3630 : px_rgb_add(0x23222A, px_rgb_scale(stone, 14));
   uint32_t stones[512], grounds[512], unlit[512];
   column *columns;

   px_kit_canvas_fit(&g->lit, s);
   if (dark)
      px_kit_canvas_fit(&g->unlit, s);
   else
      px_kit_canvas_free(&g->unlit);
   if (!g->lit.pixels || (dark && !g->unlit.pixels) || !s->backdrop || !sx || !sy || !rows)
      return false;
   columns = (column*)malloc(w * sizeof(column));
   if (!columns)
      return false;

   for (unsigned f = 0; f < 512; f++)
   {
      stones[f]  = lit(stone, f);
      grounds[f] = lit(ground, f);
      unlit[f]   = through(grounds[f], 0x38404C);
   }
   for (unsigned X = 0; X < w; X++)
   {
      column *c = &columns[X];
      const int dx = ((int)(2 * X + 1) - (int)w) * 256 / (int)w;
      const unsigned x = X / sx < PXC_W ? X / sx : PXC_W - 1;
      c->x     = (uint16_t)x;
      c->left  = X % sx < jx;
      c->right = X % sx + jx >= sx;
      c->away  = (uint16_t)((unsigned)(dx * dx) >> 8);
      c->shade      = (int16_t)(X >= shade_x && (X - shade_x) / sx < PXC_W ? (int)((X - shade_x) / sx) : -1);
      c->half_shade = (int16_t)(X >= shade_x / 2 && (X - shade_x / 2) / sx < PXC_W ? (int)((X - shade_x / 2) / sx) : -1);
      for (unsigned odd = 0; odd < 2; odd++)
      {
         const unsigned on_floor = X + (odd ? SLAB_W * sx / 2 : 0);
         const unsigned on_wall  = (x + (odd ? BLOCK_W / 2 : 0)) % BLOCK_W * sx + X % sx;
         c->slab[odd]       = (uint16_t)(on_floor / (SLAB_W * sx));
         c->slab_part[odd]  = (uint8_t)part_of(on_floor % (SLAB_W * sx), SLAB_W * sx, jx);
         c->block[odd]      = (uint16_t)((x + (odd ? BLOCK_W / 2 : 0)) / BLOCK_W);
         c->block_part[odd] = (uint8_t)part_of(on_wall, BLOCK_W * sx, jx);
      }
   }

   for (unsigned Y = 0; Y < h; Y++)
   {
      const unsigned y = Y / sy < rows ? Y / sy : rows - 1, t = Y % sy;
      const uint8_t *wall = g->wall + (size_t)y * PXC_W;
      const uint8_t *above = y ? wall - PXC_W : NULL, *below = y + 1 < rows ? wall + PXC_W : NULL;
      const uint8_t *shade = Y >= shade_y && (Y - shade_y) / sy < rows
            ? g->wall + (size_t)((Y - shade_y) / sy) * PXC_W : NULL;
      const uint8_t *half_shade = Y >= shade_y / 2 && (Y - shade_y / 2) / sy < rows
            ? g->wall + (size_t)((Y - shade_y / 2) / sy) * PXC_W : NULL;
      const bool top = t < jy, bottom = t + jy >= sy;
      /* The courses of the floor and of the walls. */
      const unsigned slabs = Y / (SLAB_H * sy), slab_odd = slabs & 1;
      const unsigned slab_part = part_of(Y % (SLAB_H * sy), SLAB_H * sy, jy);
      const unsigned down = y >= ROOM_TOP ? y - ROOM_TOP : 0;
      const unsigned blocks = down / BLOCK_H, block_odd = blocks & 1;
      const unsigned block_part = part_of(down % BLOCK_H * sy + t, BLOCK_H * sy, jy);
      const int dy = ((int)(2 * Y + 1) - (int)h) * 256 / (int)h;
      const unsigned away = (unsigned)(dy * dy) >> 8;
      uint32_t *out = g->lit.pixels + (size_t)Y * w;
      uint32_t *out_unlit = dark ? g->unlit.pixels + (size_t)Y * w : NULL;
      uint32_t grain = Y * 2654435761u + 12345u;
      unsigned slab = ~0u, slab_is = 0, block = ~0u, block_is = 0;

      for (unsigned X = 0; X < w; X++)
      {
         const column *c = &columns[X];
         /* Darker towards the edges of the picture. */
         const unsigned dim = 256 - (((c->away + away) * 50u) >> 8);
         unsigned part, f, floor;

         grain = grain * 1664525u + 1013904223u;

         /* The floor, which the dark mazes show where the walls are too. */
         if (c->slab[slab_odd] != slab)
         {
            slab    = c->slab[slab_odd];
            slab_is = chance_of(slab, slabs) % 56;
         }
         part  = slab_part == PART_JOINT || c->slab_part[slab_odd] == PART_JOINT ? PART_JOINT
               : slab_part == PART_LIGHT || c->slab_part[slab_odd] == PART_LIGHT ? PART_LIGHT : PART_FACE;
         floor = slab_light[part] + (part ? slab_is : 0) + (grain >> 28);
         if (out_unlit)
            out_unlit[X] = unlit[(floor * dim) >> 8];

         if (wall[c->x])
         {
            if (c->block[block_odd] != block)
            {
               block    = c->block[block_odd];
               block_is = chance_of(block, blocks + 977u) % 46;
            }
            part = block_part == PART_JOINT || c->block_part[block_odd] == PART_JOINT ? PART_JOINT
                  : block_part == PART_LIGHT || c->block_part[block_odd] == PART_LIGHT ? PART_LIGHT
                  : block_part == PART_SHADE || c->block_part[block_odd] == PART_SHADE ? PART_SHADE : PART_FACE;
            f = block_light[part] + (part ? block_is : 0) + (grain >> 29);
            /* The wall's own edges. */
            if (top && !(above && above[c->x]))
               f = f * 3 / 2;
            else if (c->left && !(c->x && wall[c->x - 1]))
               f = f * 5 / 4;
            else if (bottom && !(below && below[c->x]))
               f = f / 2;
            else if (c->right && !(c->x + 1 < PXC_W && wall[c->x + 1]))
               f = f * 5 / 8;
            f = (f * dim) >> 8;
            out[X] = stones[f < 512 ? f : 511];
         }
         else
         {
            if (half_shade && c->half_shade >= 0 && half_shade[c->half_shade])
               floor = floor * 150 >> 8;
            else if (shade && c->shade >= 0 && shade[c->shade])
               floor = floor * 200 >> 8;
            out[X] = grounds[(floor * dim) >> 8];
         }
      }
   }
   free(columns);

   memcpy(s->backdrop, dark ? g->unlit.pixels : g->lit.pixels, (size_t)w * h * sizeof(uint32_t));
   g->lamp.on = false;
   return true;
}

/* How much of the lantern's light falls on a pixel, of 256: all of it around the player,
 * none at the edges of the square the game shows, whose corners it rounds. */
static inline unsigned lamp_light(const lantern *l, int X, int Y)
{
   /* As far as the square goes to either side of the middle. */
   const int across = X < l->cx ? l->cx - l->x0 : l->x1 - l->cx;
   const int down = Y < l->cy ? l->cy - l->y0 : l->y1 - l->cy;
   int nx, ny;
   unsigned a, b, q;
   if (across <= 0 || down <= 0)
      return 0;
   nx = (X - l->cx) * 256 / across;
   ny = (Y - l->cy) * 256 / down;
   if (nx < 0) nx = -nx;
   if (ny < 0) ny = -ny;
   if (nx >= 256 || ny >= 256)
      return 0;
   a = (unsigned)(nx * nx) >> 8;
   b = (unsigned)(ny * ny) >> 8;
   q = (a * a + b * b) >> 8;          /* the distance, to the power of four */
   if (q >= 256)
      return 0;
   /* All of it up to four fifths of the way, and from there less and less. */
   q = (256 - q) * 256 / 150;
   if (q >= 256)
      return 256;
   return (q * q * (768 - 2 * q)) >> 16;
}

/* The lantern in the backdrop: what was lit is dark again, and what is lit now shows the
 * room as it is painted. */
static void draw_lantern(av *g, const px_scene *s, const lantern *now)
{
   const unsigned w = s->w, h = s->h;

   if (g->lamp.on)
      for (int Y = g->lamp.y0; Y < g->lamp.y1; Y++)
         memcpy(s->backdrop + (size_t)Y * w + g->lamp.x0, g->unlit.pixels + (size_t)Y * w + g->lamp.x0,
               (size_t)(g->lamp.x1 - g->lamp.x0) * sizeof(uint32_t));
   g->lamp = *now;
   if (!now->on)
      return;
   if (g->lamp.x0 < 0) g->lamp.x0 = 0;
   if (g->lamp.y0 < 0) g->lamp.y0 = 0;
   if (g->lamp.x1 > (int)w) g->lamp.x1 = (int)w;
   if (g->lamp.y1 > (int)h) g->lamp.y1 = (int)h;
   if (g->lamp.x0 >= g->lamp.x1 || g->lamp.y0 >= g->lamp.y1)
   {
      g->lamp.on = false;
      return;
   }

   for (int Y = g->lamp.y0; Y < g->lamp.y1; Y++)
   {
      uint32_t *out = s->backdrop + (size_t)Y * w;
      const uint32_t *bright = g->lit.pixels + (size_t)Y * w, *dim = g->unlit.pixels + (size_t)Y * w;
      for (int X = g->lamp.x0; X < g->lamp.x1; X++)
      {
         const unsigned light = lamp_light(&g->lamp, X, Y) * g->lamp.level >> 8;
         if (light)
            out[X] = px_rgb_add(px_rgb_scale(dim[X], 256 - light),
                  lit(through(bright[X], 0xFFC078), light * 5 / 4));
      }
   }
}

/* The walls of the frame, and what tells them from another room's. */
static uint64_t read_walls(av *g, const px_scene *s, uint32_t *rgb, bool *dark)
{
   const struct pxc_frame *f = s->frame;
   const size_t n = (size_t)PXC_W * f->height;
   uint64_t hash = 0xCBF29CE484222325ull;
   bool found = false;

   *rgb  = 0;
   *dark = false;
   for (size_t i = 0; i < n; i++)
   {
      const uint8_t tags = f->tags[i];
      const uint8_t wall = !(tags & PXC_BLANK) && (tags & (PXC_PF | PXC_M0 | PXC_M1));
      g->wall[i] = wall;
      hash = (hash ^ wall) * 0x100000001B3ull;
      if (!found && wall && (tags & PXC_PF))
      {
         *rgb  = f->palette[f->color[PXC_L_PF][i]] & 0xFFFFFFu;
         *dark = f->color[PXC_L_PF][i] == f->color[PXC_L_BK][i];
         found = true;
      }
   }
   return hash;
}

/* Sees to it that the room of the frame is the one painted. False if it is not to be had,
 * or not yet: the walls are then drawn as the game draws them. */
static bool have_room(av *g, px_scene *s, uint64_t walls, uint32_t rgb, bool dark)
{
   if (g->painted && !s->backdrop_stale && walls == g->painted_walls && rgb == g->painted_rgb)
   {
      g->new_for = 0;
      return true;
   }
   if (g->painted && !s->backdrop_stale && walls == g->painted_walls)
   {
      /* The same walls in another colour: another room, if the colour stays. */
      if (s->advance)
      {
         g->new_for = rgb == g->new_rgb ? g->new_for + 1 : 1;
         g->new_rgb = rgb;
      }
      if (g->new_for < 4)
         return false;
   }
   g->painted       = paint_room(g, s, rgb, dark);
   g->painted_walls = walls;
   g->painted_rgb   = rgb;
   g->painted_dark  = dark;
   g->new_for       = 0;
   return g->painted;
}

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

/* Takes an object out of the picture where `keep` does not say otherwise: what is behind
 * it shows. */
static void hide(px_scene *s, const px_instance *in, bool (*keep)(const px_scene *s, const av *g, size_t i),
      const av *g, uint32_t rgb)
{
   const px_objects *o = s->objects;
   for (unsigned r = 0; r < in->h; r++)
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
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_BLANK)
            continue;
         if (keep && keep(s, g, i))
         {
            s->sprite[i] = 0xFF000000u | rgb;
            if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE)
               s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
            continue;
         }
         /* Another object may be there too, which the capture has the colour of. */
         {
            static const uint8_t tag[5]   = { PXC_P0, PXC_M0, PXC_P1, PXC_M1, PXC_BL };
            static const uint8_t layer[5] = { PXC_L_P0, PXC_L_M0, PXC_L_P1, PXC_L_M1, PXC_L_BL };
            const uint8_t tags = s->frame->tags[i];
            uint32_t other = 0;
            /* Drawn from its track, it is not in the frame: what is, is another. */
            for (unsigned k = 0; k < 5 && !other; k++)
               if ((tags & tag[k]) && (in->ghost || layer[k] != in->cls) && !g->wall[i])
                  other = 0xFF000000u | (s->frame->palette[s->frame->color[layer[k]][i]] & 0xFFFFFFu);
            s->sprite[i] = other;
            s->energy[i] = 0;
            if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE)
               s->top[i] = other ? PX_KEY(PX_CLS_SPRITE, other) : PX_KEY(PX_CLS_BK, 0);
         }
      }
   }
}

/* The dot has the colour of the game's floor. It shows where the game shows it: over a
 * wall, and in the light of the lantern. */
static bool dot_shows(const px_scene *s, const av *g, size_t i)
{
   const int X = (int)((i % PXC_W) * s->sx), Y = (int)((i / PXC_W) * s->sy);
   if (s->frame->tags[i] & PXC_PF)
      return true;
   return g->lamp.on && X >= g->lamp.x0 && X < g->lamp.x1 && Y >= g->lamp.y0 && Y < g->lamp.y1;
}

/* Gives the rows `from` up to `to` of an object a colour. */
static void tint_rows(px_scene *s, const px_instance *in, unsigned from, unsigned to, uint32_t rgb)
{
   px_instance part = *in;
   if (from >= in->h)
      return;
   part.y    = (int16_t)(in->y + (int)from);
   part.rows = in->rows + from;
   part.h    = (uint16_t)((to < in->h ? to : in->h) - from);
   px_scene_tint(s, &part, rgb);
}

static int byte_of(const px_scene *s, unsigned index)
{
   return px_kit_ram(s->ram, s->ram_size, index);
}

/* What an object is, from what memory says the player objects show. */
static unsigned object_of(av *g, const px_scene *s, const px_instance *in)
{
   unsigned what = OBJ_UNKNOWN;

   if (!px_kit_is_player(in))
      return OBJ_UNKNOWN;
   /* One of the module's own, in a frame that is drawn again: it says what it is. */
   if (in->ghost && !in->track)
      return in->group < OBJ_NOTHING ? in->group : OBJ_UNKNOWN;
   if (in->ghost)
   {
      /* Drawn from its track: it is what it was when it was seen. */
      const px_kit_tagged *seen = px_kit_tags_keep(&g->known, in, OBJ_UNKNOWN);
      return seen ? seen->tag : OBJ_UNKNOWN;
   }
   {
      const int code = byte_of(s, in->cls == PXC_L_P0 ? RAM_SHOWN : RAM_SHOWN + 1);
      px_kit_tagged *seen;
      if (code >= 0 && code % 9 == 0 && code / 9 <= OBJ_NOTHING)
         what = (unsigned)code / 9;
      /* Without memory, what no other thing looks like. */
      else if (code < 0 && in->w == 32 && in->color == COLOR_ORANGE)
         what = OBJ_SQUARE;
      else if (code < 0 && in->h <= 2 && in->color == COLOR_GREY)
         what = OBJ_DOT;
      seen = px_kit_tags_keep(&g->known, in, what);
      if (seen)
         seen->tag = (uint8_t)what;
   }
   return what;
}

/* ---------------------------------------------------------------------------
 * Things shown in turns
 *
 * With three things in a room the game shows each for three frames with one player object,
 * for three with the other, and for three not at all. To the tracks, which are of one
 * object of the TIA each, that is a thing gone for six frames, which is too long to be
 * flicker. The module knows what each player object shows, and so what is missing: it is
 * drawn as it was seen last, for as long as the longest turn takes.
 * ------------------------------------------------------------------------- */

static void remember(av *g, const px_scene *s, const px_instance *in, unsigned what,
      const px_instance *ball)
{
   shape *k = &g->shapes[what];
   const px_objects *o = s->objects;
   if (in->h > SHAPE_ROWS)
      return;
   k->as   = *in;
   k->seen = g->frame;
   k->held = ball != NULL;
   if (ball)
   {
      k->dx = in->x - ball->x;
      k->dy = in->y - ball->y;
   }
   memcpy(k->bits, o->bits + in->rows, in->h * sizeof(uint32_t));
   memcpy(k->colors, o->colors + in->rows, in->h);
}

/* Adds what is missing of `what` to the frame's objects and to the picture. */
static void recall(av *g, px_scene *s, unsigned what, const px_instance *ball, bool with_ball)
{
   const shape *k = &g->shapes[what];
   const struct pxc_frame *f = s->frame;
   px_objects *o = s->objects;
   px_instance *in;

   if (o->count >= PX_MAX_INSTANCES || o->pool_used + k->as.h > o->pool_cap)
      return;
   in  = &o->inst[o->count++];
   *in = k->as;
   in->ghost = 1;
   in->track = 0;
   in->group = (uint8_t)what;
   in->rows  = (uint32_t)o->pool_used;
   /* What the player has with him, and the light around him, are where he is. */
   if (with_ball && ball && k->held)
   {
      in->x = (int16_t)(ball->x + k->dx);
      in->y = (int16_t)(ball->y + k->dy);
   }
   memcpy(o->bits + o->pool_used, k->bits, in->h * sizeof(uint32_t));
   memcpy(o->colors + o->pool_used, k->colors, in->h);
   o->pool_used += in->h;

   for (unsigned r = 0; r < in->h; r++)
   {
      const int y = in->y + (int)r;
      const uint32_t bits = k->bits[r];
      const uint32_t rgb  = f->palette[k->colors[r]] & 0xFFFFFFu;
      if (y < 0 || y >= (int)f->height)
         continue;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         const int x = in->x + (int)b;
         size_t i;
         if (!((bits >> b) & 1) || x < 0 || x >= PXC_W)
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         if (s->sprite[i] || PX_KEY_CLS(s->top[i]) == PX_CLS_BLANK)
            continue;
         /* Behind the playfield where the game draws that over the objects. */
         if ((f->tags[i] & PXC_PF) && PXC_AUX_PRIORITY(f->aux[i]) == PXC_PRI_PFP)
            continue;
         s->sprite[i] = 0xFF000000u | rgb;
         s->top[i]    = PX_KEY(PX_CLS_SPRITE, rgb);
         s->energy[i] = 0;
      }
   }
}

static uint32_t dragon_color(unsigned what)
{
   return what == OBJ_RED ? 0xFF4030 : what == OBJ_YELLOW ? 0xFFD030 : 0x40E860;
}

/* The colour of an object, or `original`. */
static uint32_t color_of(const av *g, unsigned what, int state, uint32_t original)
{
   if (g->colors == COLORS_ORIGINAL)
      /* What is black does not show on a dark floor: it is lit a little. */
      return g->painted && brightest(original) < 0x50 ? 0x5A6274 : original;
   switch (what)
   {
      case OBJ_RED:
      case OBJ_YELLOW:
      case OBJ_GREEN:
         if (state == STATE_DEAD)
            return px_rgb_mix(px_rgb_scale(dragon_color(what), 110), 0x404040, 96);
         if (state >= STATE_BITES || state == STATE_ATE)
            return px_rgb_add(dragon_color(what), 0x302010);
         return dragon_color(what);
      case OBJ_GATE_YELLOW:
      case OBJ_GATE_WHITE:
      case OBJ_GATE_BLACK: return 0x8C94A8;
      case OBJ_SWORD:      return 0xE4F0FF;
      case OBJ_BRIDGE:     return 0xB070FF;
      case OBJ_KEY_YELLOW: return 0xFFD23C;
      case OBJ_KEY_WHITE:  return 0xF4F4FF;
      case OBJ_KEY_BLACK:  return 0x6C84C0;
      case OBJ_BAT:        return 0xA080E0;
      case OBJ_MAGNET:     return 0xA0A8B8;
      case OBJ_CHALICE:    return px_rgb_mix(original, 0xFFE890, 110);
      default:
         return g->painted && brightest(original) < 0x50 ? 0x5A6274 : original;
   }
}

static void frame(void *state, px_scene *s)
{
   av *g = (av*)state;
   px_objects *o = s->objects;
   const struct pxc_frame *f = s->frame;
   const uint32_t *palette = f->palette;
   const size_t n = (size_t)PXC_W * f->height;
   const px_instance *ball = NULL, *square = NULL, *chalice = NULL;
   const px_instance *dragon[3] = { NULL, NULL, NULL };
   const unsigned found = o->count;
   int states[3], carried, won, number;
   bool there[OBJ_UNKNOWN + 1] = { false };
   uint32_t walls_rgb;
   uint64_t walls;
   bool room;

   if (s->advance)
      g->frame++;

   for (unsigned d = 0; d < 3; d++)
      states[d] = byte_of(s, RAM_DRAGONS + d * DRAGON_BYTES + DRAGON_STATE);
   carried = byte_of(s, RAM_CARRIED);
   won     = byte_of(s, RAM_WON);
   number  = byte_of(s, RAM_ROOM);

   /* The room. */
   walls = read_walls(g, s, &walls_rgb, &g->dark);
   room  = g->rooms && have_room(g, s, walls, walls_rgb, g->dark);
   if (!g->rooms)
      g->painted = false;
   if (s->advance && (walls != g->walls || number != g->room))
   {
      /* Another room: what was seen is not in it. */
      for (unsigned k = 0; k < OBJ_NOTHING; k++)
         g->shapes[k].seen = 0;
      g->walls = walls;
      g->room  = number;
   }
   if (g->rooms)
   {
      const uint32_t wall_light = 0xFF000000u | glow_of(g->painted_rgb);
      for (size_t i = 0; i < n; i++)
      {
         const unsigned cls = PX_KEY_CLS(s->top[i]);
         if (cls == PX_CLS_BLANK)
            continue;
         s->bk[i] = 0;
         if (cls == PX_CLS_BK)
            s->top[i] = PX_KEY(PX_CLS_BK, 0);
         else if (!g->painted)
            continue;
         else if (!room)
            /* Walls in a colour that is not the painted one are the game's to draw. */
            continue;
         else if (g->wall[i] && (g->painted_dark || cls == PX_CLS_PF
               || ((f->tags[i] & (PXC_M0 | PXC_M1)) && !(f->tags[i] & (PXC_P0 | PXC_P1 | PXC_BL)))))
         {
            /* A wall, thick or thin: it is in the backdrop, where the dark mazes show it
             * by the lantern alone. */
            s->top[i]    = PX_KEY(PX_CLS_BK, 0);
            s->sprite[i] = 0;
            s->energy[i] = 0;
            if (!g->painted_dark)
               s->light[i] = wall_light;
         }
      }
   }

   /* What the objects are. */
   px_kit_tags_begin(&g->known);
   g->dragon_near = false;
   g->player_at   = g->dragon_at = -1;
   for (unsigned i = 0; i < found; i++)
      if (o->inst[i].cls == PXC_L_BL)
         ball = &o->inst[i];
   for (unsigned i = 0; i < found; i++)
   {
      px_instance *in = &o->inst[i];
      const unsigned what = object_of(g, s, in);
      in->group   = (uint8_t)what;
      there[what] = true;
      if (s->advance && !in->ghost && what < OBJ_NOTHING)
         remember(g, s, in, what, ball);
   }
   /* What is shown in turns and not in this one. A frame drawn again has them already. */
   for (unsigned k = 0; k < OBJ_NOTHING && s->advance && g->steady && s->cfg->flicker; k++)
      if (!there[k] && g->shapes[k].seen && g->frame - g->shapes[k].seen <= TURNS)
         recall(g, s, k, ball, k == OBJ_SQUARE || (int)k * 9 == carried);

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      const unsigned what = in->group;

      in->role = PX_ROLE_NONE;
      if (in->cls == PXC_L_BL)
      {
         in->role = PX_ROLE_PLAYER;
         if (!in->ghost)
            g->player_at = in->x + 2;
         continue;
      }
      switch (what)
      {
         case OBJ_SQUARE:  square = in; break;
         case OBJ_RED:
         case OBJ_YELLOW:
         case OBJ_GREEN:
            in->role = PX_ROLE_ENEMY;
            dragon[what - OBJ_RED] = in;
            if (states[what - OBJ_RED] != STATE_DEAD)
            {
               g->dragon_near = true;
               g->dragon_at   = in->x + 4;
            }
            break;
         case OBJ_BAT:        in->role = PX_ROLE_ENEMY; break;
         case OBJ_SWORD:      in->role = PX_ROLE_SHOT; break;
         case OBJ_CHALICE:    in->role = PX_ROLE_BONUS; chalice = in; break;
         case OBJ_KEY_YELLOW:
         case OBJ_KEY_WHITE:
         case OBJ_KEY_BLACK:  in->role = PX_ROLE_BONUS; break;
         case OBJ_GATE_YELLOW:
         case OBJ_GATE_WHITE:
         case OBJ_GATE_BLACK: in->role = PX_ROLE_SHIELD; break;
         case OBJ_AUTHOR:
         case OBJ_NUMBER:     in->role = PX_ROLE_HUD; break;
         default:             break;
      }
   }

   /* The lantern, before the things it shines on. */
   if (g->painted && g->painted_dark)
   {
      lantern now;
      memset(&now, 0, sizeof(now));
      if (square)
      {
         now.on = true;
         now.x0 = square->x * (int)s->sx;
         now.y0 = square->y * (int)s->sy;
         now.x1 = (square->x + (int)square->w) * (int)s->sx;
         now.y1 = (square->y + (int)square->h) * (int)s->sy;
         if (ball)
         {
            now.cx = (ball->x + 2) * (int)s->sx;
            now.cy = (ball->y + 4) * (int)s->sy;
         }
         else
         {
            now.cx = (now.x0 + now.x1) / 2;
            now.cy = (now.y0 + now.y1) / 2;
         }
         /* It burns unevenly. */
         now.level = 226 + (px_kit_wave(g->frame * 7) * 30 >> 8);
      }
      if (s->advance || s->backdrop_stale || !g->lamp.on != !now.on)
         draw_lantern(g, s, &now);
      if (square)
      {
         hide(s, square, NULL, g, 0);
         /* Its light on what is in it. */
         for (unsigned r = 0; r < square->h; r++)
            for (unsigned b = 0; b < square->w; b++)
            {
               const int x = square->x + (int)b, y = square->y + (int)r;
               const unsigned light = lamp_light(&g->lamp, x * (int)s->sx, y * (int)s->sy);
               if (light > 40 && x >= 0 && x < PXC_W && y >= 0 && y < (int)f->height
                     && PX_KEY_CLS(s->top[(size_t)y * PXC_W + x]) == PX_CLS_BK)
                  s->light[(size_t)y * PXC_W + x] = 0xFF000000u | px_rgb_scale(0x70481C, light);
            }
      }
   }

   /* Their colours. */
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      const uint32_t original = palette[in->color] & 0xFFFFFFu;
      const unsigned what = in->group;
      const int st = what >= OBJ_RED && what <= OBJ_GREEN ? states[what - OBJ_RED] : 0;

      if (in->cls == PXC_L_BL)
      {
         if (g->colors != COLORS_ORIGINAL)
            px_scene_tint(s, in, 0xFFF0C0);
         else if (g->painted && brightest(original) < 0x50)
            px_scene_tint(s, in, 0x5A6274);
         continue;
      }
      /* A thin wall: painted with the room, or else the game's to draw. */
      if (!px_kit_is_player(in))
         continue;
      if (what == OBJ_SQUARE && g->painted && g->painted_dark)
         continue;
      if (what == OBJ_DOT)
      {
         if (g->painted)
            hide(s, in, dot_shows, g, 0x9A9A98);
         continue;
      }
      if (what == OBJ_AUTHOR || what == OBJ_NUMBER || what == OBJ_SQUARE)
         continue;

      px_scene_tint(s, in, color_of(g, what, st, original));
      if (g->colors == COLORS_ORIGINAL)
         continue;
      switch (what)
      {
         case OBJ_RED:
         case OBJ_YELLOW:
         case OBJ_GREEN:
         {
            /* The eye is a hole in the head, and the belly one in the body. */
            px_instance head = *in;
            if (head.h > DRAGON_HEAD)
               head.h = DRAGON_HEAD;
            if (st != STATE_DEAD)
               px_kit_fill_holes(s, &head, st >= STATE_BITES || st == STATE_ATE ? 0xFF2818 : 0xFFF8D0);
            px_kit_fill_holes(s, in, px_rgb_scale(color_of(g, what, st, original), 120));
            break;
         }
         case OBJ_MAGNET:
            /* Its poles. */
            tint_rows(s, in, in->h >= 6 ? in->h - 6u : 0, in->h, 0xF04838);
            break;
         case OBJ_SWORD:
         case OBJ_CHALICE:
            px_scene_energy(s, in, true);
            break;
         default:
            break;
      }
   }

   /* What happens. */
   if (s->advance && g->sparks)
   {
      for (unsigned d = 0; d < 3; d++)
      {
         const int now = states[d], was = g->dragon[d];
         const uint32_t rgb = dragon_color(OBJ_RED + d);
         if (now < 0 || was < 0 || now == was || !dragon[d])
            continue;
         if (now == STATE_DEAD)
         {
            px_scene_burst(s, dragon[d]->x + 4, dragon[d]->y + 16, rgb, 44, 400);
            px_scene_burst(s, dragon[d]->x + 4, dragon[d]->y + 16, 0xFFFFFF, 12, 260);
            px_scene_flash(s, 0xFFFFFF, 70);
         }
         else if (now >= STATE_BITES && was < STATE_BITES)
         {
            px_scene_burst(s, dragon[d]->x + 3, dragon[d]->y + 6, 0xFF5030, 16, 240);
            px_scene_flash(s, 0xFF2010, 60);
         }
         else if (now == STATE_ATE)
         {
            px_scene_burst(s, dragon[d]->x + 4, dragon[d]->y + 24, 0xFF2010, 30, 320);
            px_scene_flash(s, 0xFF1008, 150);
         }
      }
      if (ball && carried >= 0 && g->carried >= 0 && carried != g->carried && carried != OBJ_NOTHING * 9)
         px_scene_burst(s, ball->x + 2, ball->y + 4, 0xFFF0B0, 8, 160);
      if (chalice)
      {
         if (won == 0xFF && g->won >= 0 && g->won != 0xFF)
         {
            px_scene_burst(s, chalice->x + 4, chalice->y + 8, 0xFFE070, 60, 460);
            px_scene_flash(s, 0xFFE080, 130);
         }
         /* It glitters, and more when the game is won. */
         if (g->frame % (won == 0xFF ? 4u : 14u) == 0)
            px_scene_burst(s, chalice->x + 1 + (int)(chance_of(g->frame, 3) % 6),
                  chalice->y + 1 + (int)(chance_of(g->frame, 5) % 8), 0xFFF0A0, won == 0xFF ? 4 : 2, 110);
      }
   }

   if (s->advance)
   {
      px_kit_tags_end(&g->known);
      for (unsigned d = 0; d < 3; d++)
         g->dragon[d] = states[d];
      g->carried = carried;
      g->won     = won;
   }

   if (g->rooms && g->painted)
      s->backdrop_on = true;
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has six, all of them on voice 0. Voice 1 is silent.
 *
 *   picked up   waveform 6 at pitch 3, 2, 1, 0, three frames each, volume 5
 *   put down    waveform 6 at pitch 0, 1, 2, 3, three frames each, volume 5
 *   the bite    waveforms 3 and 8 in turns, three frames each, the pitch from 31 down to 28
 *               and the volume from 15 down to 1: 45 frames
 *   eaten       waveform 6, the pitch from 0 up to 15, three frames each, the volume from
 *               15 down to 8: 48 frames
 *   slain       waveform 4, the pitch from 16 up to 30, three frames each, the volume from
 *               15 down to 1: 45 frames
 *   the end     256 frames, the waveform one less every frame through all sixteen, the
 *               pitch one less every 16 frames from 31 to 16, the volume from 15 down to 1
 *               every 32 frames. What there is of a tune in it are the sixteen pitches,
 *               which rise; of the waveforms that are tones, 4 has them from 491 Hz to
 *               923 Hz.
 *
 * Here each is a sound of several voices where it happens between left and right, and the
 * sixteen pitches of the end are chimes. The game's voice is silent while it plays one of
 * these.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_HANDS, SOUND_BITE, SOUND_EATEN, SOUND_SLAIN, SOUND_END, SOUND_OTHER };

static unsigned voice0_plays(const px_kit_tia *t, int won)
{
   const unsigned wave = t->wave[0], pitch = t->pitch[0], volume = t->volume[0];
   if (!volume)
      return SOUND_NONE;
   /* The end goes through the waveforms of the others. */
   if (won == 0xFF || (t->was_volume[0] && ((t->was_wave[0] + 15u) & 15u) == wave))
      return pitch >= 16 ? SOUND_END : SOUND_OTHER;
   switch (wave)
   {
      case 6:  return volume == 5 && pitch <= 3 ? SOUND_HANDS : volume >= 8 && pitch <= 15 ? SOUND_EATEN : SOUND_OTHER;
      case 3:
      case 8:  return pitch >= 28 ? SOUND_BITE : SOUND_OTHER;
      case 4:  return pitch >= 16 && pitch <= 30 ? SOUND_SLAIN : SOUND_OTHER;
      default: return SOUND_OTHER;
   }
}

static void play_picked(av *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave            freq  to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_TRIANGLE, 506, 1013, 0.10f, 0.002f, 0.06f, 0.22f, 0.42f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,    1013, 2026, 0.10f, 0.002f, 0.04f, 0.30f, 0.20f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE,   9000,    0, 0,     0,      0,     0.04f, 0.10f, 5000, 1500, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->player_at));
   px_sound_rumble(s, 0, 12000, 4);
}

static void play_put(av *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_TRIANGLE, 1013, 253, 0.12f, 0.002f, 0.04f, 0.20f, 0.36f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,      150,  70, 0.08f, 0.001f, 0.02f, 0.18f, 0.55f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE,    5000,   0, 0,     0,      0,     0.05f, 0.12f, 2500, 600, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->player_at));
   px_sound_rumble(s, 9000, 0, 4);
}

static void play_bite(av *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_SAW,     130,   62, 0.60f, 0.010f, 0.20f, 0.55f, 0.50f, 1400, 300, 31.0f, 0.10f },
      { PX_WAVE_SQUARE,   65,   41, 0.60f, 0.010f, 0.20f, 0.55f, 0.36f,  600, 200, 23.0f, 0.08f },
      { PX_WAVE_NOISE,  4000,  900, 0.50f, 0.005f, 0.10f, 0.50f, 0.34f, 3000, 400, 0, 0 },
      { PX_WAVE_NOISE, 12000,    0, 0,     0,      0,     0.05f, 0.30f, 6000, 1500, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->dragon_at >= 0 ? g->dragon_at : g->player_at));
   px_sound_rumble(s, 42000, 30000, 24);
}

static void play_eaten(av *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SQUARE, 1013,  63, 0.80f, 0.002f, 0.30f, 0.60f, 0.26f, 3000, 300, 9.0f, 0.03f },
      { PX_WAVE_SINE,    506,  31, 0.80f, 0.002f, 0.30f, 0.70f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_NOISE,  3000, 200, 0.80f, 0,      0.10f, 0.90f, 0.30f, 1800, 120, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->player_at));
   px_sound_rumble(s, 65535, 40000, 48);
}

static void play_slain(av *g, px_sound *s)
{
   static const px_tone p[4] = {
      /* The sword, */
      { PX_WAVE_SQUARE, 2637, 2610, 0.30f, 0,      0.01f, 0.35f, 0.16f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE, 3951, 3920, 0.30f, 0,      0.01f, 0.25f, 0.10f, 0, 0, 0, 0 },
      /* and the dragon. */
      { PX_WAVE_SAW,     923,  253, 0.75f, 0.005f, 0.15f, 0.65f, 0.40f, 2600, 500, 13.0f, 0.06f },
      { PX_WAVE_NOISE,  5000,  500, 0.75f, 0.005f, 0.10f, 0.70f, 0.28f, 2400, 250, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->dragon_at >= 0 ? g->dragon_at : g->player_at));
   px_sound_rumble(s, 30000, 52000, 26);
}

/* One of the sixteen pitches of the end, as loud as the game plays it. */
static void play_chime(px_sound *s, unsigned pitch, unsigned volume)
{
   const float f = px_kit_tia_hz(4, pitch), loud = 0.35f + 0.65f * (float)volume / 15.0f;
   px_tone p[3] = {
      { PX_WAVE_SINE,     0, 0, 0, 0.002f, 0.02f, 0.90f, 0.40f, 0, 0, 5.0f, 0.002f },
      { PX_WAVE_TRIANGLE, 0, 0, 0, 0.002f, 0.01f, 0.45f, 0.16f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,     0, 0, 0, 0.004f, 0.05f, 1.20f, 0.22f, 0, 0, 0, 0 }
   };
   p[0].freq = f;
   p[1].freq = f * 2.0f;
   p[2].freq = f * 0.5f;
   for (unsigned i = 0; i < 3; i++)
      px_synth_play(s->synth, &p[i], (pitch & 1) ? -0.3f : 0.3f, loud);
   px_sound_rumble(s, 0, 14000, 4);
}

/* What is heard where the game is silent: wind, in the dark mazes a drone in its place,
 * and a growl while a dragon that lives is in the room. */
static void play_ambience(av *g, px_sound *s)
{
   static const px_tone wind  = { PX_WAVE_NOISE, 5000, 0, 0, 1.5f, 0, 0, 1.0f, 380, 0, 0.11f, 0.30f };
   static const px_tone low   = { PX_WAVE_SINE, 55.0f, 0, 0, 1.5f, 0, 0, 1.0f, 0, 0, 0.09f, 0.006f };
   static const px_tone high  = { PX_WAVE_TRIANGLE, 82.4f, 0, 0, 1.5f, 0, 0, 0.5f, 300, 0, 0.13f, 0.005f };
   static const px_tone growl = { PX_WAVE_SAW, 43.0f, 0, 0, 0.4f, 0, 0, 1.0f, 170, 0, 6.5f, 0.05f };
   const int room = px_kit_ram(s->ram, s->ram_size, RAM_ROOM);
   /* The room of the game's number is no place. */
   const bool on = g->ambience && g->own_sound && room != 0 && g->heard != SOUND_END;
   /* The wind comes and goes. */
   const float gust = 0.6f + 0.4f * (float)px_kit_wave(g->frame / 3u) / 254.0f;
   const float blows = on ? (g->dark ? 0.035f : 0.11f) * gust : 0.0f;
   const float drones = on && g->dark ? 0.10f : 0.0f;
   const float growls = on && g->dragon_near ? 0.16f : 0.0f;
   const float at = px_kit_pan(g->dragon_at);

   if (blows <= 0.0f)
   {
      px_synth_stop(s->synth, g->wind, 0.8f);
      g->wind = 0;
   }
   else if (!px_synth_move(s->synth, g->wind, 0.0f, blows, 0.0f))
      g->wind = px_synth_play(s->synth, &wind, 0.0f, blows);

   if (drones <= 0.0f)
   {
      px_synth_stop(s->synth, g->drone_low, 1.0f);
      px_synth_stop(s->synth, g->drone_high, 1.0f);
      g->drone_low = g->drone_high = 0;
   }
   else
   {
      if (!px_synth_move(s->synth, g->drone_low, -0.4f, drones, 0.0f))
         g->drone_low = px_synth_play(s->synth, &low, -0.4f, drones);
      if (!px_synth_move(s->synth, g->drone_high, 0.4f, drones, 0.0f))
         g->drone_high = px_synth_play(s->synth, &high, 0.4f, drones);
   }

   if (growls <= 0.0f)
   {
      px_synth_stop(s->synth, g->growl, 0.5f);
      g->growl = 0;
   }
   else if (!px_synth_move(s->synth, g->growl, at, growls, 0.0f))
      g->growl = px_synth_play(s->synth, &growl, at, growls);
}

static void sound(void *state, px_sound *s)
{
   av *g = (av*)state;
   const int won = px_kit_ram(s->ram, s->ram_size, RAM_WON);
   unsigned heard;
   bool began;

   px_kit_tia_hear(&g->tia, s->frame);
   heard = voice0_plays(&g->tia, won);
   began = heard != g->heard;

   if (heard == SOUND_HANDS)
   {
      /* Picking up goes up and putting down goes down: a step the other way is the other
       * sound, begun before this one was over. */
      const unsigned pitch = g->tia.pitch[0], was = g->tia.was_pitch[0];
      if (began || (pitch != was && (g->rising ? pitch > was : pitch < was)))
      {
         g->rising = pitch >= 2;
         if (!g->own_sound)   px_sound_rumble(s, g->rising ? 0 : 9000, g->rising ? 12000 : 0, 4);
         else if (g->rising)  play_picked(g, s);
         else                 play_put(g, s);
      }
   }
   else if (began && heard == SOUND_BITE)
   {
      if (g->own_sound) play_bite(g, s);
      else              px_sound_rumble(s, 42000, 30000, 24);
   }
   else if (began && heard == SOUND_EATEN)
   {
      if (g->own_sound) play_eaten(g, s);
      else              px_sound_rumble(s, 65535, 40000, 48);
   }
   else if (began && heard == SOUND_SLAIN)
   {
      if (g->own_sound) play_slain(g, s);
      else              px_sound_rumble(s, 30000, 52000, 26);
   }
   else if (heard == SOUND_END && (began || g->tia.pitch[0] != g->tia.was_pitch[0]))
   {
      if (g->own_sound) play_chime(s, g->tia.pitch[0], g->tia.volume[0]);
      else              px_sound_rumble(s, 0, 14000, 4);
   }
   g->heard = heard;

   if (g->own_sound && heard != SOUND_OTHER)
      s->voice[0] = 0.0f;
   play_ambience(g, s);
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

/* The game was reset, or a state was loaded: what was known is not so any more. The room
 * stays painted: it is found again, or another is, by its walls. */
static void reset(void *state)
{
   av *g = (av*)state;
   g->dragon[0] = g->dragon[1] = g->dragon[2] = -1;
   g->carried = g->won = g->room = -1;
   g->new_for = 0;
   g->walls   = 0;
   for (unsigned k = 0; k < OBJ_NOTHING; k++)
      g->shapes[k].seen = 0;
   g->dark = g->dragon_near = false;
   g->player_at = g->dragon_at = -1;
   px_kit_tags_reset(&g->known);
   px_kit_tia_reset(&g->tia);
   g->heard  = SOUND_NONE;
   g->rising = false;
   g->wind = g->drone_low = g->drone_high = g->growl = 0;
}

static void *create(void)
{
   av *g = (av*)calloc(1, sizeof(av));
   if (g)
   {
      g->rooms = g->steady = g->sparks = g->own_sound = g->ambience = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   av *g = (av*)state;
   if (g)
   {
      px_kit_canvas_free(&g->lit);
      px_kit_canvas_free(&g->unlit);
   }
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   av *g = (av*)state;
   const bool rooms = px_kit_on(get, OPT_BACKDROP);
   if (rooms != g->rooms)
      g->painted = false;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->rooms     = rooms;
   g->steady    = px_kit_on(get, OPT_STEADY);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->ambience  = px_kit_on(get, OPT_AMBIENCE);
}

const px_game px_game_adventure = {
   "Adventure", md5, fx, options, create, destroy, reset, configure, frame, sound
};
