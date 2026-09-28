/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Warlords (Atari, 1981).
 *
 * Four warlords, one in each corner, each behind a castle wall, each with a shield turned
 * by a paddle. A fireball bounces between them and breaks the walls; a warlord it reaches
 * falls. Castles no one plays are played by the console.
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  14..23   the scores of the two top warlords: playfield in score mode, in the
 *                  colours of players 0 and 1, columns 52..63 and 96..107
 *   rows  26..34   the top warlords: missile 0 on the left (column 12), missile 1 on the
 *                  right (column 138), eight wide, drawn in the warlord's colour
 *   rows  27..66   the top castles: playfield, columns 0..47 and 112..159. A keep of 16
 *                  rows beside the warlord in the darkest red ($40), then three rows of
 *                  bricks of 8 lines, $42, $44 and $46, the lightest nearest the middle
 *   rows 153..192  the bottom castles, the same upside down
 *   rows 185..193  the bottom warlords, missiles again
 *   rows 194..203  the scores of the two bottom warlords
 *   shields        player 0 for the two on the left, player 1 for the two on the right,
 *                  set again between the top and the bottom half, so all four are drawn in
 *                  every frame. They go round the castle as the paddle turns: their shape
 *                  changes on the way, and a top shield is never below row 110 nor a bottom
 *                  one above it
 *   the fireball   the ball, 2 by 4. It takes the playfield's colour of its row, $40..$46,
 *                  since the ball has no colour of its own. A shield that catches it holds
 *                  it until the button is let go
 *
 * The warlords' colours: top left $28 (orange), top right $A4 (blue), bottom left $D4
 * (green), bottom right $62 (purple). A warlord who falls, his shield and his score are
 * drawn black ($00) until the next round. Before the game begins the missiles are $20 and
 * $A4, and no shields are drawn.
 *
 * Of its memory ($80 is 0): the walls are bytes 15..78, four blocks of 16, whose bits are
 * bricks: a brick broken clears four bits of one block. Which block is which castle was
 * not made out, and the module does not need it: it tells a broken brick from the
 * picture, where playfield that was there is background now.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

#define OPT_COLORS   "proteus_wl_colors"
#define OPT_BACKDROP "proteus_wl_backdrop"
#define OPT_SPARKS   "proteus_wl_sparks"
#define OPT_SOUND    "proteus_wl_sound"
#define OPT_ROAR     "proteus_wl_roar"

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define MIDDLE_ROW   110    /* between the top half and the bottom one */
#define MIDDLE_COL   80
#define SCORE_TOP    12     /* the rows of the scores, with a little to spare */
#define SCORE_TOP_END 25
#define SCORE_BOT    193
#define SCORE_BOT_END 206
#define WALL_TOP     26     /* the rows of the castles */
#define WALL_TOP_END 67
#define WALL_BOT     152
#define WALL_BOT_END 193

#define LORDS   4           /* top left, top right, bottom left, bottom right */
#define EMBERS  90

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "cbe5a166550a8129a5e6d374901dffad",   /* Warlords (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "70",
   "reverb", "room",
   NULL
};

static const char *const colors[] = { "heraldic", "Walls in their colours",
   "stone", "Walls of stone", "original", "The game's own", NULL };
static const char *const backdrop[] = { "courtyard", "Courtyard", "off", "Off", NULL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "The warlords, their shields and their castles in the colours of each, the castles of grey stone, or the game's own colours.",
     "heraldic", colors },
   { OPT_BACKDROP, "Backdrop",
     "What is behind the game where its background is black.", "courtyard", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks where a brick breaks, and fire where a warlord falls.", "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for a brick breaking, the fireball thrown and a warlord falling, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_ROAR, "Fireball roar",
     "The fireball roars as it flies, heard where it is. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* In the order of the option's values. */
enum { COLORS_HERALDIC = 0, COLORS_STONE, COLORS_ORIGINAL };

typedef struct
{
   uint16_t x, y;          /* in the picture, in 16ths of a pixel */
   uint16_t drawn_x, drawn_y;
   uint8_t phase, pace, size;
   bool drawn;
} ember;

typedef struct
{
   unsigned colors;
   bool backdrop, sparks, own_sound, roar;

   uint32_t frame;

   /* The walls as the frame before had them: 1 where there was playfield. */
   uint8_t wall[PXC_MAX_H][PXC_W];
   bool walls_known;
   int fallen[LORDS];      /* 1: fallen, 0: stands, -1: not known */
   int fell_at;            /* the column of the warlord who fell last, for the sound */

   px_kit_tia tia;
   unsigned heard1;        /* what voice 1 played in the frame before: SOUND_* */
   unsigned roar_noise, roar_hum;   /* the roar's voices at the synth */
   int ball_x, ball_y;     /* where the fireball was, -1: not seen */

   px_kit_canvas yard;     /* the backdrop without its embers */
   unsigned yard_fallen;   /* the fallen warlords it was painted with, a bit each */
   ember embers[EMBERS];
   uint32_t seed;
} wl;

/* The colours of the four: gold, azure, emerald and violet. */
static const uint32_t lord_colors[LORDS] = { 0xF6B838, 0x4AA2FF, 0x4FE07A, 0xC070FF };

static unsigned lord_of(int x, int y)
{
   return (x >= MIDDLE_COL ? 1u : 0u) + (y >= MIDDLE_ROW ? 2u : 0u);
}

/* ---------------------------------------------------------------------------
 * The backdrop
 * ------------------------------------------------------------------------- */

/* The flagstones of a courtyard at night, lit at each corner by its warlord's torches
 * while he stands. */
static void paint_yard(wl *g, unsigned fallen)
{
   const unsigned w = g->yard.w, h = g->yard.h;
   uint32_t *base = g->yard.pixels;
   const unsigned tile_w = w / 10 ? w / 10 : 1, tile_h = h / 9 ? h / 9 : 1;

   for (unsigned y = 0; y < h; y++)
   {
      const unsigned row = y / tile_h, in_y = y % tile_h;
      const int dy = ((int)(2 * y + 1) - (int)h) * 256 / (int)h;
      for (unsigned x = 0; x < w; x++)
      {
         /* The stones are laid in courses, every other one half a stone along. */
         const unsigned shifted = x + (row & 1) * (tile_w / 2);
         const unsigned col = shifted / tile_w, in_x = shifted % tile_w;
         const int dx = ((int)(2 * x + 1) - (int)w) * 256 / (int)w;
         uint32_t cell = (row * 31u + col * 17u) * 2654435761u;
         unsigned shade = 13 + ((cell >> 13) % 7);
         uint32_t rgb;
         unsigned lit;

         if (in_x < 2 || in_y < 2)
            shade = 5;                   /* the mortar */
         else if (in_x < 4 || in_y < 4)
            shade += 3;                  /* the edge of a stone, catching light */
         rgb = (shade << 16) | (shade << 8) | (shade + 5);

         /* Torchlight from the corners of those who stand. */
         for (unsigned k = 0; k < LORDS; k++)
         {
            int cx = (k & 1) ? 256 : -256, cy = (k & 2) ? 256 : -256;
            int ex = dx - cx, ey = dy - cy;
            unsigned d2 = (unsigned)(ex * ex + ey * ey) >> 8;
            if (fallen & (1u << k) || d2 >= 300)
               continue;
            lit = (300 - d2) * 70 / 300;
            rgb = px_rgb_add(rgb, px_rgb_scale(lord_colors[k], lit * lit / 70));
         }
         /* Darker towards the middle, where the fireball flies. */
         lit = (unsigned)(dx * dx + dy * dy) >> 9;
         lit = 180 + (lit > 256 ? 256 : lit) * 76 / 256;
         base[(size_t)y * w + x] = px_rgb_scale(rgb, lit);
      }
   }
}

static void seed_embers(wl *g)
{
   g->seed = 0xF12Eu;
   for (unsigned i = 0; i < EMBERS; i++)
   {
      ember *e = &g->embers[i];
      e->x     = (uint16_t)(px_kit_chance(&g->seed) % (g->yard.w * 16u));
      e->y     = (uint16_t)(px_kit_chance(&g->seed) % (g->yard.h * 16u));
      e->phase = (uint8_t)px_kit_chance(&g->seed);
      e->pace  = (uint8_t)(3 + px_kit_chance(&g->seed) % 6);
      e->size  = (uint8_t)(g->yard.w >= 1200 ? 2 + px_kit_chance(&g->seed) % 2 : 1 + px_kit_chance(&g->seed) % 2);
      e->drawn = false;
   }
}

static void put_ember(const wl *g, uint32_t *out, const ember *e, unsigned x, unsigned y, uint32_t rgb)
{
   for (unsigned t = 0; t < e->size; t++)
      for (unsigned u = 0; u < e->size; u++)
      {
         unsigned X = x + u, Y = y + t;
         if (X < g->yard.w && Y < g->yard.h)
         {
            size_t i = (size_t)Y * g->yard.w + X;
            out[i] = rgb ? px_rgb_add(g->yard.pixels[i], rgb) : g->yard.pixels[i];
         }
      }
}

static void paint_backdrop(wl *g, px_scene *s)
{
   unsigned fallen = 0;
   for (unsigned k = 0; k < LORDS; k++)
      if (g->fallen[k] == 1)
         fallen |= 1u << k;

   if (px_kit_canvas_fit(&g->yard, s))
   {
      paint_yard(g, fallen);
      g->yard_fallen = fallen;
      seed_embers(g);
      memcpy(s->backdrop, g->yard.pixels, (size_t)s->w * s->h * sizeof(uint32_t));
   }
   else if (!g->yard.pixels)
      return;
   else if (fallen != g->yard_fallen)
   {
      /* A torch went out, or they were lit again for a new round. */
      paint_yard(g, fallen);
      g->yard_fallen = fallen;
      memcpy(s->backdrop, g->yard.pixels, (size_t)s->w * s->h * sizeof(uint32_t));
      for (unsigned i = 0; i < EMBERS; i++)
         g->embers[i].drawn = false;
   }
   if (!s->advance)
   {
      s->backdrop_on = true;
      return;
   }

   for (unsigned i = 0; i < EMBERS; i++)
   {
      ember *e = &g->embers[i];
      /* Embers rise, sway and flicker. */
      unsigned wave = px_kit_wave(e->phase + g->frame * e->pace);
      unsigned sway = px_kit_wave(e->phase * 3u + g->frame * 2u);
      unsigned x, y;
      uint32_t rgb = px_rgb_mix(0x802000, 0xFFA040, wave);

      if (e->drawn)
         put_ember(g, s->backdrop, e, e->drawn_x, e->drawn_y, 0);
      e->y = (uint16_t)((e->y + s->h * 16u - e->pace) % (s->h * 16u));
      x = (e->x / 16u + sway * s->sx / 256u) % s->w;
      y = e->y / 16u;
      put_ember(g, s->backdrop, e, x, y, px_rgb_scale(rgb, 60 + (wave * 120 >> 8)));
      e->drawn_x = (uint16_t)x;
      e->drawn_y = (uint16_t)y;
      e->drawn   = true;
   }
   s->backdrop_on = true;
}

/* ---------------------------------------------------------------------------
 * The objects and the walls
 * ------------------------------------------------------------------------- */

static bool in_walls(unsigned y)
{
   return (y >= WALL_TOP && y < WALL_TOP_END) || (y >= WALL_BOT && y < WALL_BOT_END);
}

static bool in_scores(unsigned y)
{
   return (y >= SCORE_TOP && y < SCORE_TOP_END) || (y >= SCORE_BOT && y < SCORE_BOT_END);
}

/* The walls and the scores in the colours of their warlords: the game's shades of red are
 * kept as shades, the darkest by the warlord and the lightest to the middle. */
static void color_walls(const wl *g, px_scene *s)
{
   const unsigned h = s->frame->height < PXC_MAX_H ? s->frame->height : PXC_MAX_H;
   for (unsigned y = 0; y < h; y++)
   {
      const bool wall = in_walls(y), score = in_scores(y);
      uint32_t *row = s->top + (size_t)y * PXC_W;
      if (!wall && !score)
         continue;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const uint32_t k = row[x];
         const unsigned lord = lord_of((int)x, (int)y);
         uint32_t rgb, red;
         if (PX_KEY_CLS(k) != PX_CLS_PF || !(k & 0xFFFFFFu))
            continue;   /* not playfield, or a fallen warlord's black score */
         if (score)
         {
            row[x] = PX_KEY(PX_CLS_PF, lord_colors[lord]);
            continue;
         }
         red = (k >> 16) & 0xFF;
         if (g->colors == COLORS_STONE)
            rgb = g->fallen[lord] == 1 ? 0x585450 : 0x9C958A;
         else
            rgb = g->fallen[lord] == 1 ? 0x6A6460 : px_rgb_mix(lord_colors[lord], 0x807870, 70);
         /* $40..$46 is 0x8A..0xC2 of red: from 60 to 100 of a hundred. */
         red = red < 0x8A ? 0x8A : red > 0xC2 ? 0xC2 : red;
         row[x] = PX_KEY(PX_CLS_PF, px_rgb_scale(rgb, 150 + (red - 0x8A) * 106 / 56));
      }
   }
}

/* A brick is broken where there was playfield and now is background. Sprites on top hide
 * what is under them: the wall is taken to be as it was there. */
static void find_broken(wl *g, px_scene *s)
{
   const unsigned h = s->frame->height < PXC_MAX_H ? s->frame->height : PXC_MAX_H;
   unsigned count[LORDS] = { 0 }, sum_x[LORDS] = { 0 }, sum_y[LORDS] = { 0 };
   uint32_t shade[LORDS] = { 0 };

   for (unsigned y = 0; y < h; y++)
   {
      const uint32_t *row = s->top + (size_t)y * PXC_W;
      if (!in_walls(y))
         continue;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const unsigned cls = PX_KEY_CLS(row[x]);
         uint8_t now;
         if (cls == PX_CLS_SPRITE || cls == PX_CLS_BLANK)
            continue;
         now = cls == PX_CLS_PF;
         if (g->walls_known && g->wall[y][x] && !now)
         {
            const unsigned lord = lord_of((int)x, (int)y);
            count[lord]++;
            sum_x[lord] += x;
            sum_y[lord] += y;
         }
         g->wall[y][x] = now;
      }
   }
   /* What a colour the sparks have: the wall's, as it is drawn. */
   for (unsigned k = 0; k < LORDS; k++)
      shade[k] = g->colors == COLORS_ORIGINAL ? 0xC23D3C
            : g->colors == COLORS_STONE ? 0xC8C0B0 : px_rgb_mix(lord_colors[k], 0xFFFFFF, 60);

   /* A brick is a few pixels; more is a round that ended. */
   if (g->walls_known && g->sparks)
      for (unsigned k = 0; k < LORDS; k++)
         if (count[k] && count[k] <= 96)
         {
            px_scene_burst(s, (int)(sum_x[k] / count[k]), (int)(sum_y[k] / count[k]),
                  shade[k], 14 + count[k] / 4, 260);
            px_scene_burst(s, (int)(sum_x[k] / count[k]), (int)(sum_y[k] / count[k]),
                  0xFFC060, 6, 180);
         }
   g->walls_known = true;
}

static void frame(void *state, px_scene *s)
{
   wl *g = (wl*)state;
   px_objects *o = s->objects;
   const uint32_t *palette = s->frame->palette;
   int fallen[LORDS] = { -1, -1, -1, -1 };
   int king_x[LORDS] = { 0 }, king_y[LORDS] = { 0 };

   if (s->advance)
      g->frame++;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      const uint32_t original = palette[in->color] & 0xFFFFFFu;
      unsigned lord;
      in->role = PX_ROLE_NONE;
      if (in->cls == PXC_L_M0 || in->cls == PXC_L_M1)
      {
         lord = lord_of(in->x, in->y);
         in->role  = PX_ROLE_PLAYER;
         in->group = (uint8_t)lord;
         if (!in->ghost)
         {
            fallen[lord] = original == 0;
            king_x[lord] = in->x + 4;
            king_y[lord] = in->y + 4;
         }
      }
      else if (px_kit_is_player(in))
      {
         in->role  = PX_ROLE_SHIELD;
         in->group = (uint8_t)lord_of(in->cls == PXC_L_P1 ? MIDDLE_COL : 0, in->y + in->h / 2);
      }
      else if (in->cls == PXC_L_BL)
         in->role = PX_ROLE_SHOT;
   }

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      const uint32_t original = palette[in->color] & 0xFFFFFFu;
      if (g->colors == COLORS_ORIGINAL)
         break;
      switch (in->role)
      {
         case PX_ROLE_PLAYER:
         case PX_ROLE_SHIELD:
            /* A fallen warlord is black, and stays so. */
            if (original)
               px_scene_tint(s, in, lord_colors[in->group & 3]);
            break;
         case PX_ROLE_SHOT:
         {
            /* The fireball flickers from yellow to red. */
            unsigned wave = px_kit_wave(g->frame * 23);
            px_scene_tint(s, in, px_rgb_mix(0xFFE070, 0xFF5018, wave));
            px_scene_energy(s, in, true);
            break;
         }
         default:
            break;
      }
   }

   if (s->advance)
   {
      find_broken(g, s);
      for (unsigned k = 0; k < LORDS; k++)
      {
         if (fallen[k] < 0)
            continue;
         if (fallen[k] == 1 && g->fallen[k] == 0)
         {
            /* A warlord falls: his castle goes up in fire. */
            g->fell_at = king_x[k];
            if (g->sparks)
            {
               px_scene_burst(s, king_x[k], king_y[k], lord_colors[k], 50, 460);
               px_scene_burst(s, king_x[k], king_y[k], 0xFFB040, 40, 340);
               px_scene_flash(s, lord_colors[k], 60);
            }
         }
         g->fallen[k] = fallen[k];
      }
   }
   if (g->colors != COLORS_ORIGINAL)
      color_walls(g, s);

   if (g->backdrop)
      paint_backdrop(g, s);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has three, and nothing else: no sound when the fireball bounces off a shield.
 *
 *   voice 0   a brick breaks: noise (waveform 8), the pitch in turns 12, 3, 12, 8, 11, 4,
 *             15, 3, 8, the volume from 15 down by one a frame. A brick broken before it
 *             ends begins it again at 15.
 *   voice 1   the fireball is thrown from a shield that held it: pitch 15, the waveform in
 *             turns 11, 3, 2, 8, 12, the volume from 15 down to 1 in 13 frames
 *   voice 1   a warlord falls: noise (waveform 8) at pitch 31, volume 15 for 16 frames,
 *             then a frame of it every four, then waveform 12 at volume 1
 *
 * Each is a sound of several voices of Proteus's own, where it happens between left and
 * right, and the game's voice is silent while it plays what is known.
 * ------------------------------------------------------------------------- */

enum { SOUND_NONE = 0, SOUND_THROW, SOUND_FALL, SOUND_OTHER };

static unsigned voice1_plays(unsigned wave, unsigned pitch, unsigned volume)
{
   if (!volume)
      return SOUND_NONE;
   if (pitch == 15 && (wave == 11 || wave == 3 || wave == 2 || wave == 8 || wave == 12))
      return SOUND_THROW;
   if (pitch == 31 && (wave == 8 || wave == 12))
      return SOUND_FALL;
   return SOUND_OTHER;
}

static bool voice0_breaks(unsigned wave, unsigned pitch)
{
   return wave == 8 && (pitch == 12 || pitch == 3 || pitch == 8 || pitch == 11 || pitch == 4
         || pitch == 15);
}

static void play_break(wl *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave          freq  to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_NOISE, 5200, 1200, 0.20f, 0,      0.01f, 0.28f, 0.30f, 4200, 600, 0, 0 },
      { PX_WAVE_NOISE, 1600,  500, 0.30f, 0.002f, 0.02f, 0.40f, 0.26f, 1400, 250, 0, 0 },
      { PX_WAVE_SINE,   120,   48, 0.10f, 0.001f, 0.02f, 0.22f, 0.50f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->ball_x));
   px_sound_rumble(s, 16000, 22000, 6);
}

static void play_throw(wl *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_NOISE,  900, 5000, 0.18f, 0.01f,  0.03f, 0.30f, 0.40f, 800, 6000, 0, 0 },
      { PX_WAVE_SAW,    180,  520, 0.16f, 0.005f, 0.02f, 0.22f, 0.20f, 1500, 3500, 0, 0 },
      { PX_WAVE_SQUARE, 1320, 990, 0.05f, 0.001f, 0.01f, 0.25f, 0.10f, 5000, 2000, 6.0f, 0.02f }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->ball_x));
   px_sound_rumble(s, 0, 16000, 4);
}

static void play_fall(wl *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 4500, 200, 1.4f, 0,      0.20f, 1.8f, 0.34f, 3800, 120, 0, 0 },
      { PX_WAVE_SINE,    70,  24, 1.0f, 0.002f, 0.15f, 1.5f, 0.45f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    330,  55, 1.2f, 0.002f, 0.05f, 1.2f, 0.20f, 2400, 180, 9.0f, 0.05f },
      { PX_WAVE_NOISE,  800, 150, 2.0f, 0.3f,   0.20f, 2.2f, 0.22f, 900, 100, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->fell_at >= 0 ? g->fell_at : g->ball_x));
   px_sound_rumble(s, 65535, 45000, 50);
}

/* The fireball roars where it flies: a rush of noise and a low hum under it. */
static void play_roar(wl *g, px_sound *s)
{
   static const px_tone noise = { PX_WAVE_NOISE, 2400, 0, 0, 0.4f, 0, 0, 1.0f, 700, 0, 0, 0 };
   static const px_tone hum   = { PX_WAVE_SAW, 62.0f, 0, 0, 0.4f, 0, 0, 0.7f, 240, 0, 5.5f, 0.03f };
   const float pan = px_kit_pan(g->ball_x);

   if (!g->roar || !g->own_sound || g->ball_x < 0)
   {
      px_synth_stop(s->synth, g->roar_noise, 0.4f);
      px_synth_stop(s->synth, g->roar_hum, 0.4f);
      g->roar_noise = g->roar_hum = 0;
      return;
   }
   if (!px_synth_move(s->synth, g->roar_noise, pan, 0.07f, 0))
      g->roar_noise = px_synth_play(s->synth, &noise, pan, 0.07f);
   if (!px_synth_move(s->synth, g->roar_hum, pan, 0.06f, 0))
      g->roar_hum = px_synth_play(s->synth, &hum, pan, 0.06f);
}

static void sound(void *state, px_sound *s)
{
   wl *g = (wl*)state;
   unsigned heard1;

   /* Where the fireball is, from the picture before. */
   g->ball_x = g->ball_y = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_SHOT && !in->ghost)
      {
         g->ball_x = in->x + 1;
         g->ball_y = in->y + 2;
      }
   }

   px_kit_tia_hear(&g->tia, s->frame);

   if (voice0_breaks(g->tia.wave[0], g->tia.pitch[0]) && px_kit_tia_louder(&g->tia, 0)
         && g->tia.volume[0] >= 14)
   {
      if (g->own_sound) play_break(g, s);
      else              px_sound_rumble(s, 16000, 22000, 6);
   }

   heard1 = voice1_plays(g->tia.wave[1], g->tia.pitch[1], g->tia.volume[1]);
   if (heard1 != g->heard1)
   {
      if (heard1 == SOUND_THROW && g->tia.volume[1] >= 14)
      {
         if (g->own_sound) play_throw(g, s);
         else              px_sound_rumble(s, 0, 16000, 4);
      }
      else if (heard1 == SOUND_FALL)
      {
         if (g->own_sound) play_fall(g, s);
         else              px_sound_rumble(s, 65535, 45000, 50);
      }
   }
   g->heard1 = heard1;

   if (g->own_sound)
   {
      if (!g->tia.volume[0] || voice0_breaks(g->tia.wave[0], g->tia.pitch[0]))
         s->voice[0] = 0.0f;
      if (heard1 != SOUND_OTHER)
         s->voice[1] = 0.0f;
   }
   play_roar(g, s);
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   wl *g = (wl*)state;
   g->walls_known = false;
   for (unsigned k = 0; k < LORDS; k++)
      g->fallen[k] = -1;
   g->fell_at = -1;
   g->ball_x = g->ball_y = -1;
   px_kit_tia_reset(&g->tia);
   g->heard1 = SOUND_NONE;
   g->roar_noise = g->roar_hum = 0;
}

static void *create(void)
{
   wl *g = (wl*)calloc(1, sizeof(wl));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = g->roar = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   wl *g = (wl*)state;
   if (g)
      px_kit_canvas_free(&g->yard);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   wl *g = (wl*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->roar      = px_kit_on(get, OPT_ROAR);
}

const px_game px_game_warlords = {
   "Warlords", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
