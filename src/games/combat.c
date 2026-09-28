/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Combat (Atari, 1977).
 *
 * Two players, each with a tank, a biplane or a jet, shoot at each other. The game has 27
 * variations; the difficulty switches and the Select button choose among them. What the
 * game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows   0..12   nothing: the background is black above the score
 *   rows  17..27   the two scores: the playfield in its score mode, the left half in the
 *                  colour of the left player, the right half in that of the right
 *   rows  30..38   tanks: the wall around the field, playfield; below it the field, and in
 *                  the variations with a maze the maze, playfield too. Planes: no wall,
 *                  and two clouds of playfield in the middle
 *   anywhere       the left player's tank or plane: player 0; the right player's: player 1.
 *                  8 by 14 or 16, one pattern a heading. When one is hit it spins.
 *   shots          missile 0 is the left player's shell or bullet, missile 1 the right's,
 *                  2 pixels high, there only while they fly
 *
 * The colours change from variation to variation: red and blue on olive for the first
 * games of tanks, green and blue on brown later, orange and green on blue for biplanes,
 * on purple for jets. So the module finds the field's colour in every frame and asks
 * nothing of it.
 *
 * Of its memory ($80 is 0): 0 has the variation less one (0..26), 1 the same in BCD;
 * 5 what fights: 0 tanks, 1 biplanes, 2 jets; 8 is $FF while a game is played and 0 in
 * the attract mode and while a variation is chosen; 33 and 34 the scores of the left and
 * the right player in BCD. While a variation is chosen, 33 shows its number, so a score is
 * a score only while 8 says a game is on.
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
   "4c8832ed387bbafc055320c05205bc08",   /* Combat ~ Tank-Plus (Japan, USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "70",
   "reverb", "room",
   NULL
};

#define OPT_COLORS   "proteus_cb_colors"
#define OPT_BACKDROP "proteus_cb_backdrop"
#define OPT_SPARKS   "proteus_cb_sparks"
#define OPT_SOUND    "proteus_cb_sound"

static const char *const colors[] = { "teams", "Red and blue", "vivid", "The game's, brighter",
   "original", "The game's own", NULL };
static const char *const backdrop[] = { "terrain", "Ground and sky", "off", "Off", NULL };

/* In the order of the option's values. */
enum { COLORS_TEAMS = 0, COLORS_VIVID, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "The two sides in red and blue in every variation, the game's own colours made brighter, or the game's own.",
     "teams", colors },
   { OPT_BACKDROP, "Backdrop",
     "Ground under the tanks and sky around the planes, in the game's own colour of the field.",
     "terrain", backdrop },
   { OPT_SPARKS, "Explosions",
     "Sparks where a tank or plane is hit, and a flash from the gun when it fires.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for the engines, the shots and the hits, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define RAM_KIND    5
#define RAM_PLAYING 8
#define RAM_SCORE   33     /* and 34 */

enum { KIND_TANKS = 0, KIND_BIPLANES, KIND_JETS };

#define SCORE_TOP    12    /* the score's rows, and the first of the field's colour */
#define SCORE_END    30

/* The two sides in the colours option "teams": the left red, the right blue. */
static const uint32_t team[2] = { 0xFF4A3A, 0x3F8CFF };

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   /* The options. */
   unsigned colors;
   bool backdrop, sparks, own_sound;

   uint32_t frame;
   int score[2];            /* as memory had them while a game was on; -1: not known */
   int side_x[2], side_y[2];/* the middle of each side's tank or plane; -1: not seen */
   bool shell[2];           /* a side's shell was in the frame before */

   /* The backdrop: painted again when the field's colour, where it begins or the size
    * of the picture changes. */
   px_kit_canvas ground;    /* of 256: how light the ground is, pixel by pixel */
   uint32_t painted_rgb;
   unsigned painted_top, painted_kind;
   bool painted;

   /* The sounds. */
   px_kit_tia tia;
   unsigned burst[2];       /* a voice began a burst of noise in the frame before */
   unsigned engine[2];      /* each side's engine at the synth; 0: none */
   int heard_x[2];          /* where each side is, for the sounds */
   int heard_score[2];      /* the scores as the sound hook last read them; -1: not known */
   unsigned since_score;    /* frames since a score went up, up to 255 */
   unsigned kind;           /* what fights, for the engines */
} game;

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

/* The game's colour made brighter and more of its hue. */
static uint32_t vivid(uint32_t rgb)
{
   int c[3] = { (int)(rgb >> 16) & 255, (int)(rgb >> 8) & 255, (int)rgb & 255 };
   int grey = (c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8;
   uint32_t out = 0;
   for (unsigned k = 0; k < 3; k++)
   {
      int v = grey + (c[k] - grey) * 3 / 2 + 36;
      out = (out << 8) | (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
   }
   return out;
}

static uint32_t side_color(const game *g, unsigned side, uint32_t original)
{
   switch (g->colors)
   {
      case COLORS_TEAMS: return team[side & 1];
      case COLORS_VIVID: return vivid(original);
      default:           return original;
   }
}

/* A number of noise that is the same for the same place every time. */
static unsigned noise_at(unsigned x, unsigned y, unsigned seed)
{
   uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
   h = (h ^ (h >> 13)) * 1274126177u;
   return (h ^ (h >> 16)) & 255;
}

/* Smooth noise: `cell` pixels between the points it passes through. */
static unsigned smooth_at(unsigned x, unsigned y, unsigned cell, unsigned seed)
{
   unsigned cx = x / cell, cy = y / cell, fx = (x % cell) * 256 / cell, fy = (y % cell) * 256 / cell;
   unsigned a = noise_at(cx, cy, seed), b = noise_at(cx + 1, cy, seed);
   unsigned c = noise_at(cx, cy + 1, seed), d = noise_at(cx + 1, cy + 1, seed);
   /* Eased, so the cells do not show. */
   fx = fx * fx * (768 - 2 * fx) >> 16;
   fy = fy * fy * (768 - 2 * fy) >> 16;
   unsigned top = a * (256 - fx) + b * fx, bottom = c * (256 - fx) + d * fx;
   return (top * (256 - fy) + bottom * fy) >> 16;
}

/* How light the backdrop is, of 256, pixel by pixel: trodden ground for the tanks, a sky
 * lighter towards the horizon for the planes. */
static void paint_ground(game *g, unsigned kind, unsigned top, unsigned sx, unsigned sy)
{
   const unsigned w = g->ground.w, h = g->ground.h;
   uint32_t *out = g->ground.pixels;
   const unsigned from = top * sy;

   for (unsigned y = 0; y < h; y++)
   {
      /* In the pixels of the game, so that the texture is alike at every scale. */
      const unsigned gy = y * 4 / sy;
      for (unsigned x = 0; x < w; x++)
      {
         const unsigned gx = x * 4 / sx;
         unsigned light;
         if (y < from)
            light = 0;
         else if (kind == KIND_TANKS)
         {
            unsigned n = (smooth_at(gx, gy, 64, 1) * 2 + smooth_at(gx, gy, 20, 2)
                  + smooth_at(gx, gy, 6, 3)) / 4;
            light = 150 + (n * 95 >> 8);
            /* Stones and tufts. */
            if (noise_at(gx / 2, gy / 2, 4) > 252)
               light -= 40;
            else if (noise_at(gx / 2, gy / 2, 5) > 252)
               light += 22;
         }
         else
         {
            /* Of 256: how far down from the top of the field. */
            unsigned down = h > from ? (y - from) * 256 / (h - from) : 0;
            unsigned n = smooth_at(gx, gy / 2, 90, 6);
            light = 150 + (down * 80 >> 8) + (n * 30 >> 8);
         }
         out[(size_t)y * w + x] = light > 256 ? 256 : light;
      }
   }
}

/* Makes the field's colour dark, which is where the backdrop shows, and paints the
 * backdrop in that colour. */
static void paint_backdrop(game *g, px_scene *s, unsigned kind)
{
   const unsigned height = s->frame->height;
   uint32_t field = 0, top = SCORE_TOP;
   bool fresh;

   if (height <= SCORE_END + 10)
      return;
   field = s->bk[(size_t)(height / 2) * PXC_W + PXC_W / 2] & 0xFFFFFFu;
   if (!(field & 0xE0E0E0u))
      return;   /* dark as it is: the game's own black */
   while (top < height && (s->bk[(size_t)top * PXC_W + PXC_W / 2] & 0xFFFFFFu) != field)
      top++;
   if (top >= height)
      return;

   fresh = px_kit_canvas_fit(&g->ground, s);
   if (!g->ground.pixels)
      return;
   if (fresh || !g->painted || g->painted_kind != kind || g->painted_top != top)
   {
      paint_ground(g, kind, top, s->sx, s->sy);
      g->painted = false;
   }
   if (!g->painted || s->backdrop_stale || g->painted_rgb != field || g->painted_top != top
         || g->painted_kind != kind)
   {
      const size_t n = (size_t)s->w * s->h;
      for (size_t i = 0; i < n; i++)
         s->backdrop[i] = px_rgb_scale(field, g->ground.pixels[i]);
      g->painted      = true;
      g->painted_rgb  = field;
      g->painted_top  = top;
      g->painted_kind = kind;
   }

   for (size_t i = (size_t)top * PXC_W; i < (size_t)height * PXC_W; i++)
      if ((s->bk[i] & 0xFFFFFFu) == field)
      {
         s->bk[i] = 0;
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_BK)
            s->top[i] = PX_KEY(PX_CLS_BK, 0);
      }
   s->backdrop_on = true;
}

/* The score's halves in the colours of their sides. */
static void color_score(const game *g, px_scene *s, const uint32_t original[2])
{
   const unsigned end = s->frame->height < SCORE_END ? s->frame->height : SCORE_END;
   for (unsigned y = SCORE_TOP; y < end; y++)
   {
      uint32_t *row = s->top + (size_t)y * PXC_W;
      for (unsigned x = 0; x < PXC_W; x++)
         if (PX_KEY_CLS(row[x]) == PX_CLS_PF)
         {
            const unsigned side = x < PXC_W / 2 ? 0 : 1;
            const uint32_t was = row[x] & 0xFFFFFFu;
            row[x] = PX_KEY(PX_CLS_PF, side_color(g, side, original[side] ? original[side] : was));
         }
   }
}

/* The planes' clouds: the playfield below the score, whiter, with sunlight on them. */
static void light_clouds(px_scene *s)
{
   for (size_t i = (size_t)SCORE_END * PXC_W; i < (size_t)s->frame->height * PXC_W; i++)
      if (PX_KEY_CLS(s->top[i]) == PX_CLS_PF)
      {
         const uint32_t rgb = px_rgb_mix(s->top[i] & 0xFFFFFFu, 0xF4F8FF, 150);
         s->top[i]   = PX_KEY(PX_CLS_PF, rgb);
         s->light[i] = 0xFF000000u | px_rgb_scale(rgb, 90);
      }
}

static void frame(void *state, px_scene *s)
{
   game *g = (game*)state;
   px_objects *o = s->objects;
   const uint32_t *palette = s->frame->palette;
   const int playing = px_kit_ram(s->ram, s->ram_size, RAM_PLAYING);
   const int kind_ram = px_kit_ram(s->ram, s->ram_size, RAM_KIND);
   const unsigned kind = kind_ram == KIND_BIPLANES || kind_ram == KIND_JETS
         ? (unsigned)kind_ram : KIND_TANKS;
   uint32_t side_rgb[2] = { 0, 0 };
   bool shell[2] = { false, false };
   int score[2];

   if (s->advance)
      g->frame++;
   g->side_x[0] = g->side_x[1] = -1;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      const uint32_t original = palette[in->color] & 0xFFFFFFu;
      in->role = PX_ROLE_NONE;
      if (in->y < SCORE_END)
         continue;
      if (px_kit_is_player(in))
      {
         const unsigned side = in->cls == PXC_L_P0 ? 0 : 1;
         in->role  = PX_ROLE_PLAYER;
         in->group = (uint8_t)side;
         if (!in->ghost)
         {
            side_rgb[side]  = original;
            g->side_x[side] = in->x + in->w / 2;
            g->side_y[side] = in->y + in->h / 2;
         }
         if (g->colors != COLORS_ORIGINAL)
            px_scene_tint(s, in, side_color(g, side, original));
      }
      else if (in->cls == PXC_L_M0 || in->cls == PXC_L_M1)
      {
         const unsigned side = in->cls == PXC_L_M0 ? 0 : 1;
         in->role  = PX_ROLE_SHOT;
         in->group = (uint8_t)side;
         shell[side] = true;
         if (g->colors != COLORS_ORIGINAL)
         {
            /* White hot, with a little of its side's colour. */
            px_scene_tint(s, in, px_rgb_mix(side_color(g, side, original), 0xFFFFF0, 150));
            px_scene_energy(s, in, true);
         }
      }
   }

   if (g->colors != COLORS_ORIGINAL)
   {
      color_score(g, s, side_rgb);
      if (kind != KIND_TANKS)
         light_clouds(s);
   }

   score[0] = score[1] = -1;
   if (playing == 0xFF)
   {
      score[0] = px_kit_ram(s->ram, s->ram_size, RAM_SCORE);
      score[1] = px_kit_ram(s->ram, s->ram_size, RAM_SCORE + 1);
   }

   if (s->advance && g->sparks)
   {
      for (unsigned side = 0; side < 2; side++)
      {
         /* A side scored: the other was hit. The score is BCD, and goes up by one. */
         const unsigned hit = side ^ 1;
         if (score[side] >= 0 && g->score[side] >= 0 && score[side] > g->score[side]
               && score[side] - g->score[side] <= 7 && g->side_x[hit] >= 0)
         {
            const uint32_t by = side_color(g, side, side_rgb[side] ? side_rgb[side] : 0xFFC040);
            px_scene_burst(s, g->side_x[hit], g->side_y[hit], 0xFFD890, 36, 380);
            px_scene_burst(s, g->side_x[hit], g->side_y[hit], by, 18, 220);
            px_scene_flash(s, px_rgb_mix(by, 0xFFE0B0, 128), 70);
         }
         /* A shell that was not there flies: the gun flashes. */
         if (shell[side] && !g->shell[side] && playing == 0xFF && g->side_x[side] >= 0)
            px_scene_burst(s, g->side_x[side], g->side_y[side], 0xFFF0C0, 6, 120);
      }
   }

   if (s->advance)
   {
      g->score[0] = score[0];
      g->score[1] = score[1];
      g->shell[0] = shell[0];
      g->shell[1] = shell[1];
   }

   if (g->backdrop)
      paint_backdrop(g, s, kind);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * Each player has a voice of the TIA: voice 0 the left, voice 1 the right. On it are:
 *
 *   the engine   tanks: waveform 2 at volume 8, pitch 29 (left) or 31 (right) standing
 *                and 5 or 7 moving. Biplanes: waveform 3 at volume 2, pitch 22 to 29.
 *                Jets: waveform 8 (noise) at volume 2, pitch 7 to 18. It sounds all game.
 *   a shot       waveform 8 at pitch 15 (left) or 17 (right), volume 15 falling by one a
 *                frame: 15 frames. Fired again, it begins again at 15.
 *   a hit        waveform 8 as for a shot, but falling by one every four frames: about 58
 *                frames, on the voice of the one that was hit. The other's engine is
 *                silent until the hit one has done spinning.
 *   a ricochet   in the variations whose shells bounce: waveform 4 at volume 7, pitch 27
 *                to 30, four frames. Not known to the module: heard as the game plays it.
 *
 * A shot and a hit begin alike. They are told apart a frame later: a shot is quieter by
 * then, a hit is not. Here the engines are voices of their own that go where the tank or
 * plane goes, and the shots and hits are sounds of several voices each.
 * ------------------------------------------------------------------------- */

enum { HEARD_NONE = 0, HEARD_ENGINE, HEARD_BURST, HEARD_OTHER };

static unsigned heard(const px_kit_tia *t, unsigned v)
{
   const unsigned wave = t->wave[v], volume = t->volume[v];
   if (!volume)
      return HEARD_NONE;
   if ((wave == 2 && volume == 8) || (wave == 3 && volume == 2) || (wave == 8 && volume == 2))
      return HEARD_ENGINE;
   if (wave == 8)
      return HEARD_BURST;
   return HEARD_OTHER;
}

static void play_shot(game *g, px_sound *s, unsigned side)
{
   static const px_tone tank[3] = {
      /* wave            freq  to    glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_NOISE,   8000, 1500, 0.12f, 0,      0.01f, 0.22f, 0.32f, 6000, 900, 0, 0 },
      { PX_WAVE_SINE,     160,   48, 0.10f, 0.001f, 0.02f, 0.20f, 0.48f, 0, 0, 0, 0 },
      { PX_WAVE_SQUARE,   420,   90, 0.08f, 0.001f, 0.01f, 0.12f, 0.16f, 2200, 400, 0, 0 }
   };
   static const px_tone gun[3] = {
      { PX_WAVE_NOISE,  11000, 4000, 0.05f, 0,      0.00f, 0.10f, 0.30f, 8000, 2500, 0, 0 },
      { PX_WAVE_SQUARE,  1300,  500, 0.06f, 0.001f, 0.00f, 0.08f, 0.14f, 5000, 1500, 0, 0 },
      { PX_WAVE_SINE,     220,   90, 0.05f, 0.001f, 0.00f, 0.08f, 0.35f, 0, 0, 0, 0 }
   };
   px_kit_play(s, g->kind == KIND_TANKS ? tank : gun, 3, px_kit_pan(g->heard_x[side]));
   px_sound_rumble(s, g->kind == KIND_TANKS ? 12000 : 0, 16000, 4);
}

static void play_hit(game *g, px_sound *s, unsigned side)
{
   static const px_tone p[4] = {
      { PX_WAVE_NOISE, 6000, 250, 0.9f, 0,      0.08f, 1.10f, 0.40f, 4500, 160, 0, 0 },
      { PX_WAVE_SINE,    90,  30, 0.6f, 0.002f, 0.08f, 0.90f, 0.60f, 0, 0, 0, 0 },
      { PX_WAVE_SAW,    600,  60, 0.7f, 0.002f, 0.04f, 0.80f, 0.18f, 2400, 180, 9.0f, 0.05f },
      { PX_WAVE_NOISE, 2500, 400, 0.4f, 0.02f,  0.20f, 0.90f, 0.22f, 1200, 200, 0, 0 }
   };
   px_kit_play(s, p, 4, px_kit_pan(g->heard_x[side]));
   px_sound_rumble(s, 65535, 42000, 40);
}

/* A side's engine: a voice that goes on while the game's does, and goes where its tank or
 * plane goes. It is higher and louder while the game's is: when it moves. */
static void play_engine(game *g, px_sound *s, unsigned side, bool on)
{
   static const px_tone tank  = { PX_WAVE_SAW,    40.0f, 0, 0, 0.10f, 0, 0, 1.0f, 180, 0, 7.0f, 0.06f };
   static const px_tone prop  = { PX_WAVE_SQUARE, 90.0f, 0, 0, 0.10f, 0, 0, 1.0f, 600, 0, 23.0f, 0.10f };
   static const px_tone jet   = { PX_WAVE_NOISE, 2400.0f, 0, 0, 0.10f, 0, 0, 1.0f, 1800, 0, 0, 0 };
   const unsigned pitch = g->tia.pitch[side] & 31;
   const float pan = px_kit_pan(g->heard_x[side]);
   float freq, gain;

   if (!on)
   {
      px_synth_stop(s->synth, g->engine[side], 0.15f);
      g->engine[side] = 0;
      return;
   }
   switch (g->kind)
   {
      case KIND_BIPLANES:
         freq = 60.0f + px_kit_tia_hz(3, pitch) * 12.0f;
         gain = 0.07f;
         break;
      case KIND_JETS:
         freq = 1200.0f + (float)(31 - pitch) * 90.0f;
         gain = 0.06f;
         break;
      default:
      {
         const bool moving = pitch < 16;
         freq = moving ? 58.0f : 38.0f;
         gain = moving ? 0.13f : 0.08f;
         break;
      }
   }
   if (!px_synth_move(s->synth, g->engine[side], pan, gain, freq))
   {
      const px_tone *p = g->kind == KIND_BIPLANES ? &prop : g->kind == KIND_JETS ? &jet : &tank;
      px_tone t = *p;
      t.freq = freq;
      g->engine[side] = px_synth_play(s->synth, &t, pan, gain);
   }
}

static void sound(void *state, px_sound *s)
{
   game *g = (game*)state;
   const int kind = px_kit_ram(s->ram, s->ram_size, RAM_KIND);

   g->kind = kind == KIND_BIPLANES || kind == KIND_JETS ? (unsigned)kind : KIND_TANKS;
   /* Where the two sides are, from the picture before; the left and the right of the field
    * when they are not seen. */
   g->heard_x[0] = 40;
   g->heard_x[1] = 120;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
   {
      const px_instance *in = &s->objects->inst[i];
      if (in->role == PX_ROLE_PLAYER && !in->ghost && in->group < 2)
         g->heard_x[in->group] = in->x + in->w / 2;
   }

   /* A score that went up: someone was hit, and the game says so a frame later. */
   {
      const bool playing = px_kit_ram(s->ram, s->ram_size, RAM_PLAYING) == 0xFF;
      if (g->since_score < 255)
         g->since_score++;
      for (unsigned side = 0; side < 2; side++)
      {
         const int now = playing ? px_kit_ram(s->ram, s->ram_size, RAM_SCORE + side) : -1;
         if (now >= 0 && g->heard_score[side] >= 0 && now > g->heard_score[side])
            g->since_score = 0;
         g->heard_score[side] = now;
      }
   }

   px_kit_tia_hear(&g->tia, s->frame);
   for (unsigned v = 0; v < 2; v++)
   {
      const unsigned now = heard(&g->tia, v);
      const bool began = now == HEARD_BURST && g->tia.volume[v] >= 12
            && g->tia.volume[v] > g->tia.was_volume[v];

      if (g->burst[v] && now == HEARD_BURST)
      {
         /* A burst began the frame before: a shot has become quieter by now, a hit has
          * not. The noise that starts a game is as loud as long, and is not a hit: where
          * memory is to be had, a hit is one that a score went up for. */
         const bool held = g->tia.volume[v] >= g->tia.was_volume[v];
         const bool hit = held && (!s->ram || g->since_score <= 4);
         if (hit)
         {
            if (g->own_sound) play_hit(g, s, v);
            else             px_sound_rumble(s, 65535, 42000, 40);
         }
         else
         {
            if (g->own_sound) play_shot(g, s, v);
            else             px_sound_rumble(s, g->kind == KIND_TANKS ? 12000 : 0, 16000, 4);
         }
      }
      else if (g->burst[v])
      {
         /* Over before it was told: a shot. */
         if (g->own_sound) play_shot(g, s, v);
      }
      g->burst[v] = began;

      if (g->own_sound)
      {
         play_engine(g, s, v, now == HEARD_ENGINE || now == HEARD_BURST);
         if (now == HEARD_ENGINE || now == HEARD_BURST)
            s->voice[v] = 0.0f;
      }
      else
         play_engine(g, s, v, false);
   }
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   game *g = (game*)state;
   g->score[0] = g->score[1] = -1;
   g->side_x[0] = g->side_x[1] = -1;
   g->shell[0] = g->shell[1] = false;
   g->burst[0] = g->burst[1] = 0;
   g->heard_score[0] = g->heard_score[1] = -1;
   g->since_score = 255;
   g->engine[0] = g->engine[1] = 0;
   g->painted = false;
   px_kit_tia_reset(&g->tia);
}

static void *create(void)
{
   game *g = (game*)calloc(1, sizeof(game));
   if (g)
   {
      g->backdrop = g->sparks = g->own_sound = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   game *g = (game*)state;
   if (g)
      px_kit_canvas_free(&g->ground);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   game *g = (game*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
}

const px_game px_game_combat = {
   "Combat", md5, fx, options, create, destroy, reset, configure, frame, sound
};
