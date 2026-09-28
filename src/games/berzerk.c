/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Berzerk (Atari, 1982).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  15..190  the room: a maze of the playfield in blue on black, with gaps in its
 *                  outer wall for the ways out. The room is cut into bands of 20 rows
 *                  (robots at rows 25, 45, 65 ... 165): player 1 is set again for every
 *                  band, so the robots, one to a band, are all drawn in every frame. The
 *                  HMOVE of every band leaves its notch in the left wall.
 *   the robots     player 1, 8 by 16 (a shape of 8 rows drawn twice each), 8 by 18 while
 *                  one walks from band to band. Their colour is the level's: yellow (1A)
 *                  in the first rooms, orange (36) later. A robot's eye is a gap that runs
 *                  along the second row. A robot that is hit becomes a ring of dots that
 *                  grows for four frames (player 1 in two or three pieces of 4 to 8 rows)
 *                  and is gone.
 *   the humanoid   player 0 in colour 3C, a head of 4 rows and a body of 14 below a gap;
 *                  running, it is one piece of 11 to 23 rows. Electrocuted, it is drawn in
 *                  turns as itself and as a hollow figure of 24 rows, for 128 frames.
 *   Evil Otto      player 0 too, in the humanoid's colour: a smiling face of 8 rows drawn
 *                  twice each (3C 7E DB FF BD C3 7E 3C in the ROM), in the frames the
 *                  humanoid is not drawn in. Only games with Otto (not game 1, the one a
 *                  reset gives) have him: he comes out where the humanoid came in when the
 *                  room has lasted about 720 frames (memory 7 counts 256 frames to 4).
 *                  He is told by the row DB (his eyes), which no other shape has.
 *   shots          missile 0 is the humanoid's laser (1 by 6 or 4 by 2), in its colour;
 *                  missile 1 the robots' (4 by 2), in theirs.
 *   rows 190..     the score, or "(c)1982 ATARI" before the first points, and the men
 *                  left: copies of both players, in yellow (1C).
 *
 * Of its memory ($80 is 0): 0 the game's number (in BCD), 1 is 02 while a room is played
 * and FF between rooms, 90 the men left (FF: the game is over), 91 the robots destroyed in
 * the room, 93..95 the score in BCD, and 104 is 7F while the humanoid is alive, FF from
 * the frame it is electrocuted, 87 for the frame a room begins. When a robot is destroyed
 * byte 91 counts one up in the frame after its ring of dots has gone.
 *
 * The game zeroes voice 1's volume near the end of the frame (line 230) in many frames of
 * its sounds, after playing it from near the top: what the voice sounded is read from the
 * loudest it was during the frame, not from where the frame left it.
 *
 * Found with the tools of tools/2600 from runs of the game from a state saved at frame 30,
 * the stick in four directions and the button pressed now and then; Evil Otto in game 10
 * (Select nine times, then Reset), with the humanoid standing near the way he came in.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "136f75c4dd02c29283752b7e5799f978",   /* Berzerk (Japan, USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "reverb", "room",
   "width", "50",
   NULL
};

#define OPT_COLORS   "proteus_bz_colors"
#define OPT_WALLS    "proteus_bz_walls"
#define OPT_BACKDROP "proteus_bz_backdrop"
#define OPT_SPARKS   "proteus_bz_sparks"
#define OPT_SOUND    "proteus_bz_sound"
#define OPT_HUM      "proteus_bz_hum"
#define OPT_OTTO     "proteus_bz_otto"

static const char *const colors[] = { "arcade", "The arcade's", "original", "The game's own", NULL };
static const char *const walls[] = { "electric", "Electrified", "neon", "Glowing",
   "original", "The game's own", NULL };
static const char *const backdrop[] = { "floor", "A steel floor", "off", "Off", NULL };
/* In the order of the options' values. */
enum { COLORS_ARCADE = 0, COLORS_ORIGINAL };
enum { WALLS_ELECTRIC = 0, WALLS_NEON, WALLS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "The humanoid in the arcade's green, the robots in the colour the level gives them with a red eye, Evil Otto a burning face, and lasers that glow; or the game's own colours.",
     "arcade", colors },
   { OPT_WALLS, "Walls",
     "The maze's walls electrified: glowing blue, with current running along them and flaring when the humanoid touches them; glowing and steady; or as the game draws them.",
     "electric", walls },
   { OPT_BACKDROP, "Backdrop", "What is behind the game where its background is black.", "floor", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks where a robot is destroyed, and a flash and sparks when the humanoid is electrocuted.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the lasers, the robots destroyed, the humanoid electrocuted and a room coming on, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_HUM, "Electric hum",
     "The low hum of the electrified walls while a room is played. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { OPT_OTTO, "Evil Otto's sound",
     "A throb that follows Evil Otto while he is in the room: the game has no sound for him. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define HUD_TOP      190   /* the score and the men left are below */

#define RAM_ROOM      1    /* 02 while a room is played */
#define RAM_KILLED   91    /* robots destroyed in the room */
#define RAM_STATE   104    /* 7F alive, FF electrocuted, 87 a room begins */

#define STATE_ALIVE  0x7F
#define STATE_DEAD   0xFF

#define SHOCK_FRAMES 128   /* how long the humanoid is electrocuted */
#define RINGS        8     /* explosions kept from one frame to the next */

/* Groups of PX_ROLE_ENEMY. */
enum { GROUP_ROBOT = 0, GROUP_RING, GROUP_OTTO };

/* The colours of Proteus's. */
#define RGB_HUMANOID  0x3CE85Au
#define RGB_SHOCK_A   0x90B4F0u
#define RGB_SHOCK_B   0x3450D8u
#define RGB_EYE       0xFF2A18u
#define RGB_LASER     0xE6FFD8u
#define RGB_BOLT      0xFF4A2Cu
#define RGB_OTTO      0xFFD23Cu
#define RGB_WALL      0x2C4CFFu
#define RGB_CORE      0xB4D4FFu
#define RGB_ARC       0xF4F8FFu

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct { int16_t x, y; } spot;

typedef struct
{
   /* The options. */
   unsigned colors, walls;
   bool backdrop, sparks, own_sound, hum, otto_sound;

   uint32_t frame;          /* counts the frames that advance */
   int state;               /* memory 104 as it was; -1 before the first frame */
   unsigned shock;          /* frames of electrocution left */
   unsigned surge;          /* of 256: the walls flare, and fade */
   unsigned otto_gone;      /* frames since Otto was seen */
   spot rings[RINGS];       /* the explosions of the frame before */
   unsigned ring_count;

   px_kit_canvas floor;     /* the backdrop */

   /* The sound. */
   px_kit_tia tia;          /* the game's two voices, as frames leave them */
   uint8_t peak[2], was_peak[2];   /* the loudest each voice was in the frame */
   int killed;              /* memory 91 as the sound heard it; -1: not known */
   unsigned killed_age;     /* frames since it counted up */
   unsigned dying;          /* frames of the electrocution sound left */
   unsigned hum_id[2], otto_id;
   uint32_t beat;           /* counts the frames the sound hears */
   int player_at, shot_at, bolt_at, ring_at, otto_at;   /* columns; -1: not known */
   uint32_t seed;
} bz;

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static uint32_t mix3(unsigned a, unsigned b, unsigned c)
{
   uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
   h ^= h >> 15;
   h *= 0x2C1B3C6Du;
   h ^= h >> 12;
   return h;
}

/* A row of an object's shape, as the ROM has it (the bits of an 8 wide player). */
static bool has_row(const px_objects *o, const px_instance *in, uint32_t row)
{
   for (unsigned r = 0; r < in->h && r < PX_OBJ_ROWS; r++)
      if (o->bits[in->rows + r] == row)
         return true;
   return false;
}

static unsigned role_of(const px_objects *o, px_instance *in)
{
   if (in->y >= HUD_TOP)
      return PX_ROLE_HUD;
   switch (in->cls)
   {
      case PXC_L_P0:
         /* Otto's eyes, 11011011: the same both ways round. */
         if (in->w == 8 && has_row(o, in, 0xDB))
         {
            in->group = GROUP_OTTO;
            return PX_ROLE_ENEMY;
         }
         return PX_ROLE_PLAYER;
      case PXC_L_P1:
         in->group = in->h >= 12 ? GROUP_ROBOT : GROUP_RING;
         return PX_ROLE_ENEMY;
      case PXC_L_M0:
         return PX_ROLE_SHOT;
      case PXC_L_M1:
         return PX_ROLE_BOMB;
      default:
         return PX_ROLE_NONE;
   }
}

/* The robots' colour of the level, brighter and fuller. */
static uint32_t robot_rgb(uint32_t original)
{
   return px_rgb_add(original, px_rgb_scale(original, 64));
}

/* A wall: the playfield, or a notch the HMOVE of a band cut in the left wall (blank, with
 * the playfield above and below). */
static bool wall_at(const px_scene *s, int x, int y)
{
   const uint8_t *tags = s->frame->tags;
   const int h = (int)s->frame->height;
   size_t i;
   if (x < 0 || x >= PXC_W || y < 0 || y >= h)
      return false;
   i = (size_t)y * PXC_W + (size_t)x;
   if (!(tags[i] & PXC_BLANK))
      return (tags[i] & PXC_PF) != 0;
   return x < 8 && y > 0 && y + 1 < h
         && (tags[i - PXC_W] & (PXC_PF | PXC_BLANK)) == PXC_PF
         && (tags[i + PXC_W] & (PXC_PF | PXC_BLANK)) == PXC_PF;
}

/* The walls: blue with a white core, current running along them, and arcs. */
static void paint_walls(bz *g, px_scene *s)
{
   const uint8_t *tags = s->frame->tags;
   const unsigned h = s->frame->height < HUD_TOP ? s->frame->height : HUD_TOP;
   const bool electric = g->walls == WALLS_ELECTRIC;
   const unsigned surge = g->surge;
   const uint32_t light = 0xFF000000u | px_rgb_scale(RGB_WALL, 100 + (surge * 150 >> 8));
   /* Arcs: of 4096 pixel pairs, how many flare in a frame. */
   const unsigned arcs = electric ? 5 + (surge >> 3) : 0;

   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         static const int step[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
         const size_t i = (size_t)y * PXC_W + x;
         const unsigned cls = PX_KEY_CLS(s->top[i]);
         bool within = true;
         uint32_t rgb;
         if (!(tags[i] & (PXC_PF | PXC_BLANK)) || !wall_at(s, (int)x, (int)y) || cls == PX_CLS_SPRITE
               || (cls != PX_CLS_PF && !(tags[i] & PXC_BLANK)))
            continue;
         for (unsigned d = 0; d < 4 && within; d++)
         {
            const int nx = (int)x + step[d][0], ny = (int)y + step[d][1];
            if (nx < 0 || nx >= PXC_W || ny < 0 || ny >= (int)s->frame->height)
               continue;
            within = wall_at(s, nx, ny) || (tags[(size_t)ny * PXC_W + (size_t)nx] & PXC_BLANK);
         }
         rgb = within ? RGB_CORE : RGB_WALL;
         if (electric)
         {
            /* Current: bright bands that run along the walls. */
            const unsigned band = px_kit_wave(x * 5 + y * 3 + g->frame * 6);
            if (band > 190)
               rgb = px_rgb_mix(rgb, RGB_ARC, (band - 190) * (within ? 3 : 2));
            if ((mix3(x >> 1, y >> 1, g->frame >> 1) & 4095) < arcs)
            {
               rgb = RGB_ARC;
               s->light[i] = 0xFF000000u | 0xB0D0FF;
               s->top[i] = PX_KEY(PX_CLS_PF, rgb);
               continue;
            }
         }
         if (surge)
            rgb = px_rgb_mix(rgb, RGB_ARC, surge >> 1);
         s->top[i] = PX_KEY(PX_CLS_PF, rgb);
         if (!within)
            s->light[i] = light;
      }
}

/* A floor of dark steel plates, a band of the game's (20 rows) high. */
static void paint_backdrop(bz *g, px_scene *s)
{
   if (px_kit_canvas_fit(&g->floor, s) || (g->floor.pixels && s->backdrop_stale))
   {
      const unsigned w = g->floor.w, h = g->floor.h, sx = s->sx, sy = s->sy;
      uint32_t *p = g->floor.pixels;
      for (unsigned y = 0; y < h; y++)
      {
         const unsigned row = y / sy, in_row = y % sy;
         const int dy = ((int)(2 * y + 1) - (int)h) * 256 / (int)h;
         for (unsigned x = 0; x < w; x++)
         {
            const unsigned col = x / sx;
            const int dx = ((int)(2 * x + 1) - (int)w) * 256 / (int)w;
            const unsigned dim = 256 - ((((unsigned)(dx * dx) >> 8) + ((unsigned)(dy * dy) >> 8)) * 70u >> 8);
            uint32_t rgb;
            if (row >= HUD_TOP)
               rgb = 0x020306;
            else
            {
               const unsigned plate_x = (col + 4) % 20, plate_y = (row + 5) % 20;
               const uint32_t grain = mix3(x >> 2, y >> 1, 77) & 7;
               rgb = 0x07090F + grain * 0x010101u;
               /* The seams between plates, lit a little from above. */
               if (plate_x == 0 && x % sx == 0)
                  rgb = 0x0F1522;
               else if (plate_y == 0 && in_row == 0)
                  rgb = 0x111828;
               else if (plate_y == 1 && in_row == 0)
                  rgb = 0x040508;
               /* Rivets in the corners of the plates. */
               else if ((plate_x == 2 || plate_x == 18) && (plate_y == 2 || plate_y == 18))
                  rgb = 0x161C2A;
            }
            p[(size_t)y * w + x] = px_rgb_scale(rgb, dim);
         }
      }
      memcpy(s->backdrop, p, (size_t)w * h * sizeof(uint32_t));
   }
   if (g->floor.pixels)
      s->backdrop_on = true;
}

static void frame(void *state, px_scene *s)
{
   bz *g = (bz*)state;
   px_objects *o = s->objects;
   const uint32_t *palette = s->frame->palette;
   const int now = px_kit_ram(s->ram, s->ram_size, RAM_STATE);
   int player_x = -1, player_y = -1;
   spot rings[RINGS] = { { 0, 0 } };
   unsigned ring_count = 0;
   bool otto = false;

   if (s->advance)
      g->frame++;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      const uint32_t original = palette[in->color] & 0xFFFFFFu;
      in->role = (uint8_t)role_of(o, in);
      switch (in->role)
      {
         case PX_ROLE_PLAYER:
            if (!in->ghost && (player_y < 0 || in->y < player_y))
            {
               player_x = in->x + 4;
               player_y = in->y;
            }
            if (g->colors == COLORS_ORIGINAL)
               break;
            if (g->shock)
            {
               /* Electrocuted: pale and blue in turns, burning in the first frames. */
               px_scene_tint(s, in, (g->frame >> 1) & 1 ? RGB_SHOCK_A : RGB_SHOCK_B);
               px_scene_energy(s, in, g->shock > SHOCK_FRAMES - 12 && ((g->frame >> 2) & 1));
            }
            else
               px_scene_tint(s, in, RGB_HUMANOID);
            break;
         case PX_ROLE_ENEMY:
            if (in->group == GROUP_OTTO)
            {
               otto = true;
               if (g->colors != COLORS_ORIGINAL)
               {
                  /* He burns, and throbs; his eyes and his grin are dark. */
                  const unsigned beat = px_kit_wave(g->frame * 9);
                  px_scene_tint(s, in, px_rgb_mix(RGB_OTTO, 0xFF3A10, beat * 3 / 4));
                  px_kit_fill_holes(s, in, 0x3A0800);
               }
            }
            else if (in->group == GROUP_RING)
            {
               if (!in->ghost && ring_count < RINGS)
               {
                  rings[ring_count].x = (int16_t)(in->x + 4);
                  rings[ring_count].y = (int16_t)(in->y + in->h / 2);
                  ring_count++;
               }
               if (g->colors != COLORS_ORIGINAL)
               {
                  px_scene_tint(s, in, px_rgb_mix(robot_rgb(original), 0xFFFFFF, 140));
                  px_scene_energy(s, in, true);
               }
            }
            else if (g->colors != COLORS_ORIGINAL)
            {
               px_scene_tint(s, in, robot_rgb(original));
               px_kit_fill_holes(s, in, RGB_EYE);
            }
            break;
         case PX_ROLE_SHOT:
            if (g->colors != COLORS_ORIGINAL)
            {
               px_scene_tint(s, in, RGB_LASER);
               px_scene_energy(s, in, true);
            }
            break;
         case PX_ROLE_BOMB:
            if (g->colors != COLORS_ORIGINAL)
            {
               px_scene_tint(s, in, RGB_BOLT);
               px_scene_energy(s, in, true);
            }
            break;
         default:
            break;
      }
   }

   if (s->advance)
   {
      /* The humanoid touched a wall, a robot, a bolt or Otto. */
      const bool shocked = now == STATE_DEAD && g->state == STATE_ALIVE;
      if (shocked)
      {
         g->shock = SHOCK_FRAMES;
         g->surge = 256;
      }
      if (g->sparks)
      {
         if (shocked && player_x >= 0)
         {
            px_scene_burst(s, player_x, player_y + 10, RGB_ARC, 40, 380);
            px_scene_burst(s, player_x, player_y + 10, RGB_SHOCK_B, 26, 220);
            px_scene_flash(s, 0x90C0FF, 150);
         }
         else if (g->shock && player_x >= 0 && (mix3(g->frame, 3, 5) & 7) == 0)
         {
            /* The current still runs through him. */
            const int dx = (int)(mix3(g->frame, 1, 2) % 9) - 4;
            const int dy = (int)(mix3(g->frame, 2, 3) % 20);
            px_scene_burst(s, player_x + dx, player_y + dy, RGB_ARC, 5, 160);
         }
         /* A ring of dots where there was none: a robot is destroyed. */
         for (unsigned r = 0; r < ring_count; r++)
         {
            bool seen = false;
            for (unsigned q = 0; q < g->ring_count && !seen; q++)
               seen = abs(g->rings[q].x - rings[r].x) < 12 && abs(g->rings[q].y - rings[r].y) < 16;
            for (unsigned q = 0; q < r && !seen; q++)
               seen = abs(rings[q].x - rings[r].x) < 12 && abs(rings[q].y - rings[r].y) < 16;
            if (!seen)
            {
               px_scene_burst(s, rings[r].x, rings[r].y, 0xFFE8A0, 30, 340);
               px_scene_burst(s, rings[r].x, rings[r].y, 0xFF6020, 18, 200);
               px_scene_burst(s, rings[r].x, rings[r].y, RGB_ARC, 8, 440);
            }
         }
         /* Otto comes into the room. */
         if (otto && g->otto_gone > 60)
            px_scene_flash(s, 0xFF4010, 60);
      }
      g->otto_gone = otto ? 0 : g->otto_gone < 1000 ? g->otto_gone + 1 : 1000;
      memcpy(g->rings, rings, sizeof(rings));
      g->ring_count = ring_count;
      if (g->shock)
         g->shock--;
      if (now != STATE_DEAD)
         g->shock = 0;
      g->surge = g->surge > 6 ? g->surge - 6 : 0;
      g->state = now;
   }

   if (g->walls != WALLS_ORIGINAL)
      paint_walls(g, s);
   if (g->backdrop)
      paint_backdrop(g, s);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game has five, the loudest the voice is in the frame counted (see the top):
 *
 *   voice 0   the humanoid's laser: waveform 10, the pitch from 0 up by one every other
 *             frame while the volume goes down from 15, up to 30 frames; begun again
 *             (at 15 and pitch 0) by every shot, and cut short when a shot ends
 *   voice 0   the humanoid electrocuted: waveform 8 at pitch 0 and 1 in turns, volume 14,
 *             128 frames
 *   voice 1   a robot destroyed, and a room coming on: waveform 8, the pitch from 0 up to
 *             14 while the volume goes down from 15, 8 to 16 frames. A robot destroyed is
 *             told from a room by memory 91, which counts it in the same frame.
 *   voice 1   a robot's bolt: waveform 14 from pitch 0 and volume 15, the pitch up by one
 *             and the volume down by one every 8 frames for as long as the bolt flies
 *             (up to 120 frames), or a single frame of it in every 8
 *
 * Each is played here with voices of Proteus's, where it happens between left and right,
 * and the game's voice is silent while it plays what is known. Otto has no sound in the
 * game; the walls hum in the arcade's spirit, not the game's. Both are options.
 * ------------------------------------------------------------------------- */

#define TIA_AUDV0 0x19
#define TIA_AUDV1 0x1A

/* The loudest each voice is in the frame: what it was when the frame began, and every
 * volume written in it. */
static void hear_peaks(bz *g, const struct pxc_frame *f)
{
   uint8_t at[2] = { g->tia.volume[0], g->tia.volume[1] };   /* before this frame's */
   g->was_peak[0] = g->peak[0];
   g->was_peak[1] = g->peak[1];
   g->peak[0] = at[0];
   g->peak[1] = at[1];
   for (uint32_t i = 0; f && i < f->write_count; i++)
   {
      const struct pxc_regwrite *w = &f->writes[i];
      if (w->reg == TIA_AUDV0 || w->reg == TIA_AUDV1)
      {
         const unsigned v = w->reg - TIA_AUDV0;
         if ((w->value & 15) > g->peak[v])
            g->peak[v] = w->value & 15;
      }
   }
}

/* A sound began: loud from its first pitch, as it was not in the frame before. */
static bool began(const bz *g, unsigned v, unsigned wave, unsigned volume)
{
   return g->tia.wave[v] == wave && g->tia.pitch[v] == 0 && g->peak[v] >= volume
         && (g->was_peak[v] < volume || g->tia.was_pitch[v] != 0 || g->tia.was_wave[v] != wave);
}

static void play_laser(bz *g, px_sound *s)
{
   static const px_tone p[3] = {
      /* wave            freq   to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SAW,     2300,  380,  0.15f, 0.001f, 0.02f, 0.20f, 0.26f, 7500, 1400, 0, 0 },
      { PX_WAVE_SQUARE,  1150,  190,  0.15f, 0.001f, 0.01f, 0.18f, 0.14f, 3500, 700, 0, 0 },
      { PX_WAVE_SINE,    3400,  700,  0.10f, 0,      0,     0.10f, 0.10f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->player_at));
   px_sound_rumble(s, 0, 12000, 3);
}

static void play_bolt(bz *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_SQUARE,  820,  150,  0.28f, 0.002f, 0.03f, 0.32f, 0.20f, 2600, 500, 28.0f, 0.06f },
      { PX_WAVE_SAW,     410,   75,  0.28f, 0.002f, 0.02f, 0.30f, 0.18f, 1800, 300, 0, 0 },
      { PX_WAVE_NOISE,  5000, 1500,  0.10f, 0,      0,     0.08f, 0.06f, 4000, 1000, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(g->bolt_at >= 0 ? g->bolt_at : -1));
   px_sound_rumble(s, 0, 9000, 3);
}

static void play_destroyed(bz *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE,  8000,  700,  0.30f, 0,      0.02f, 0.42f, 0.30f, 6000, 400, 0, 0 },
      { PX_WAVE_SINE,    170,   40,  0.20f, 0.001f, 0.03f, 0.32f, 0.42f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE, 1400,  180,  0.22f, 0.001f, 0.01f, 0.22f, 0.10f, 4000, 600, 45.0f, 0.08f },
      { PX_WAVE_NOISE, 12000, 6000,  0.05f, 0,      0,     0.06f, 0.14f, 11000, 7000, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->ring_at >= 0 ? g->ring_at : g->shot_at));
   px_sound_rumble(s, 24000, 30000, 9);
}

static void play_room(bz *g, px_sound *s)
{
   static const px_tone p[3] = {
      { PX_WAVE_TRIANGLE, 150,  620,  0.35f, 0.02f, 0.05f, 0.45f, 0.24f, 0, 0, 11.0f, 0.03f },
      { PX_WAVE_SAW,       75,  310,  0.35f, 0.02f, 0.05f, 0.40f, 0.10f, 1200, 3000, 0, 0 },
      { PX_WAVE_NOISE,   3000, 9000,  0.30f, 0.05f, 0,     0.30f, 0.05f, 3500, 9000, 0, 0 }
   };
   (void)g;
   px_kit_play(s, p, 3, 0.0f);
   px_sound_rumble(s, 8000, 0, 4);
}

static void play_shock(bz *g, px_sound *s)
{
   static const px_tone p[4] = {
      { PX_WAVE_SAW,     118,  100,  1.8f, 0.002f, 0.90f, 1.20f, 0.22f, 3200, 500, 57.0f, 0.18f },
      { PX_WAVE_SQUARE,   59,   50,  1.8f, 0.002f, 0.90f, 1.20f, 0.20f, 900, 250, 7.0f, 0.25f },
      { PX_WAVE_NOISE,  9000, 2500,  1.6f, 0,      0.80f, 1.20f, 0.15f, 8000, 1200, 0, 0 },
      { PX_WAVE_SINE,    110,   30,  0.4f, 0.001f, 0.05f, 0.50f, 0.40f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->player_at));
   px_sound_rumble(s, 60000, 45000, 40);
}

/* A crack of the current while the humanoid is electrocuted. */
static void play_crack(bz *g, px_sound *s)
{
   px_tone p = { PX_WAVE_NOISE, 12000, 5000, 0.04f, 0, 0, 0.05f, 0.22f, 10000, 4000, 0, 0 };
   p.freq = 7000.0f + (float)(px_kit_chance(&g->seed) % 6000);
   px_kit_play(s, &p, 1, px_kit_pan(g->player_at));
}

/* The walls' hum while a room is played; and Otto's throb while he is in it. */
static void play_ambience(bz *g, px_sound *s, bool room)
{
   static const px_tone low  = { PX_WAVE_SAW,    60.0f, 0, 0, 0.6f, 0, 0, 1.0f, 260, 0, 0.4f, 0.004f };
   static const px_tone high = { PX_WAVE_SQUARE, 120.0f, 0, 0, 0.6f, 0, 0, 0.5f, 520, 0, 0.3f, 0.003f };
   static const px_tone otto = { PX_WAVE_SQUARE, 55.0f, 0, 0, 0.3f, 0, 0, 1.0f, 420, 0, 3.5f, 0.10f };

   if (g->hum && g->own_sound && room)
   {
      const float gain = 0.045f + 0.08f * (float)g->surge / 256.0f;
      if (!px_synth_move(s->synth, g->hum_id[0], -0.4f, gain, 0))
         g->hum_id[0] = px_synth_play(s->synth, &low, -0.4f, gain);
      if (!px_synth_move(s->synth, g->hum_id[1], 0.4f, gain * 0.6f, 0))
         g->hum_id[1] = px_synth_play(s->synth, &high, 0.4f, gain * 0.6f);
   }
   else
   {
      px_synth_stop(s->synth, g->hum_id[0], 0.5f);
      px_synth_stop(s->synth, g->hum_id[1], 0.5f);
      g->hum_id[0] = g->hum_id[1] = 0;
   }

   if (g->otto_sound && g->own_sound && g->otto_at >= 0)
   {
      /* A throb, faster than a heart. */
      const float beat = (float)px_kit_wave(g->beat * 10) / 254.0f;
      const float gain = 0.06f + 0.14f * beat, pan = px_kit_pan(g->otto_at);
      if (!px_synth_move(s->synth, g->otto_id, pan, gain, 55.0f + 20.0f * beat))
         g->otto_id = px_synth_play(s->synth, &otto, pan, gain);
   }
   else
   {
      px_synth_stop(s->synth, g->otto_id, 0.3f);
      g->otto_id = 0;
   }
}

static void sound(void *state, px_sound *s)
{
   bz *g = (bz*)state;
   const int killed = px_kit_ram(s->ram, s->ram_size, RAM_KILLED);
   const bool room = px_kit_ram(s->ram, s->ram_size, RAM_ROOM) == 0x02;
   bool destroyed;

   /* Where things are, from the picture before. */
   g->player_at = g->shot_at = g->bolt_at = g->ring_at = g->otto_at = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      switch (in->role)
      {
         case PX_ROLE_PLAYER: if (!in->ghost) g->player_at = in->x + 4; break;
         case PX_ROLE_SHOT:   g->shot_at = in->x; break;
         case PX_ROLE_BOMB:   g->bolt_at = in->x; break;
         case PX_ROLE_ENEMY:
            if (in->group == GROUP_RING)
               g->ring_at = in->x + 4;
            else if (in->group == GROUP_OTTO)
               g->otto_at = in->x + 4;
            break;
         default: break;
      }
   }

   g->beat++;
   hear_peaks(g, s->frame);
   px_kit_tia_hear(&g->tia, s->frame);

   /* Memory 91 counts a robot destroyed in the frame its sound begins. */
   if (killed >= 0 && g->killed >= 0 && killed > g->killed)
      g->killed_age = 0;
   else if (g->killed_age < 255)
      g->killed_age++;
   g->killed = killed;
   destroyed = g->killed_age <= 1;

   /* Voice 0: the laser and the electrocution. */
   if (began(g, 0, 10, 15))
   {
      if (g->own_sound) play_laser(g, s);
      else              px_sound_rumble(s, 0, 12000, 3);
   }
   if (g->tia.wave[0] == 8 && g->tia.pitch[0] <= 1 && g->peak[0] >= 14
         && !(g->tia.was_wave[0] == 8 && g->was_peak[0] >= 14))
   {
      g->dying = SHOCK_FRAMES;
      if (g->own_sound) play_shock(g, s);
      else              px_sound_rumble(s, 60000, 45000, 40);
   }
   else if (g->dying)
   {
      g->dying--;
      if (g->own_sound && (px_kit_chance(&g->seed) & 7) == 0)
         play_crack(g, s);
   }

   /* Voice 1: a robot destroyed or a room, and the robots' bolts. */
   if (began(g, 1, 8, 15))
   {
      if (destroyed)
      {
         if (g->own_sound) play_destroyed(g, s);
         else              px_sound_rumble(s, 24000, 30000, 9);
      }
      else if (g->own_sound)
         play_room(g, s);
   }
   if (began(g, 1, 14, 15))
   {
      if (g->own_sound) play_bolt(g, s);
      else              px_sound_rumble(s, 0, 9000, 3);
   }

   if (g->own_sound)
   {
      if (g->peak[0] && (g->tia.wave[0] == 10 || (g->tia.wave[0] == 8 && g->tia.pitch[0] <= 1)))
         s->voice[0] = 0.0f;
      if (g->peak[1] && (g->tia.wave[1] == 8 || g->tia.wave[1] == 14))
         s->voice[1] = 0.0f;
   }
   play_ambience(g, s, room);
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   bz *g = (bz*)state;
   g->state = -1;
   g->shock = g->surge = 0;
   g->otto_gone = 1000;
   g->ring_count = 0;
   px_kit_tia_reset(&g->tia);
   g->peak[0] = g->peak[1] = g->was_peak[0] = g->was_peak[1] = 0;
   g->killed = -1;
   g->killed_age = 255;
   g->dying = 0;
   g->hum_id[0] = g->hum_id[1] = g->otto_id = 0;
   g->beat = 0;
   g->seed = 0xB3E2u;
}

static void *create(void)
{
   bz *g = (bz*)calloc(1, sizeof(bz));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = g->hum = g->otto_sound = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   bz *g = (bz*)state;
   if (g)
      px_kit_canvas_free(&g->floor);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   bz *g = (bz*)state;
   g->colors     = px_kit_pick(get, OPT_COLORS, colors);
   g->walls      = px_kit_pick(get, OPT_WALLS, walls);
   g->backdrop   = px_kit_on(get, OPT_BACKDROP);
   g->sparks     = px_kit_on(get, OPT_SPARKS);
   g->own_sound  = px_kit_on(get, OPT_SOUND);
   g->hum        = px_kit_on(get, OPT_HUM);
   g->otto_sound = px_kit_on(get, OPT_OTTO);
}

const px_game px_game_berzerk = {
   "Berzerk", md5, fx, options, create, destroy, reset, configure, frame, sound, NULL
};
