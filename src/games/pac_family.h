/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the games of Pac-Man's family have in common: the arcade's colours, how a ghost
 * looks by what it is about, and the sounds of eating and being eaten. A game of the family
 * tells what happens from its own picture, memory and voices (pac_man.c, ms_pac_man.c), and
 * shows and plays it with these, so that the games look and sound of a kind.
 *
 * Everything here is static: a module includes this file and has its own of it.
 */
#ifndef PROTEUS_PAC_FAMILY_H
#define PROTEUS_PAC_FAMILY_H

#include "../kit.h"

#define PAC_GHOSTS 4

/* The ghosts in the order of the arcade's: red, pink, cyan, orange. */
static const uint32_t pac_ghost_colors[PAC_GHOSTS] = { 0xFF2A1A, 0xFFA8E0, 0x2AE8F0, 0xFFA030 };

#define PAC_RGB_PAC     0xFFE81Au
#define PAC_RGB_SCARED  0x2438FFu
#define PAC_RGB_PALE    0xF2F2F8u
#define PAC_RGB_EYES    0xE4EAFFu
#define PAC_RGB_WAFER   0xFFB897u
#define PAC_RGB_WALL    0x2A48FFu
#define PAC_RGB_WITHIN  0x05082Cu
#define PAC_RGB_SCORE   0xF0F0F0u

/* What the ghosts are about. */
enum { PAC_IDLE = 0, PAC_CHASE, PAC_SCARED, PAC_WARNING };

/* The colour of a ghost, and in `holes` that of its eyes (0: it has none to fill): its own
 * in the chase, blue while it can be eaten, pale and blue in turns while that is about to
 * end, and what is left of it when it was. */
static inline uint32_t pac_ghost_color(unsigned mode, uint32_t frame, unsigned which, bool eaten,
      uint32_t *holes)
{
   if (eaten)
   {
      *holes = 0;
      return PAC_RGB_EYES;
   }
   switch (mode)
   {
      case PAC_SCARED:
         *holes = 0xFFC4A8;
         return PAC_RGB_SCARED;
      case PAC_WARNING:
         /* Faster than the eye follows the game's own change of colour. */
         if ((frame >> 3) & 1)
         {
            *holes = 0xFF3030;
            return PAC_RGB_PALE;
         }
         *holes = 0xFFC4A8;
         return PAC_RGB_SCARED;
      default:
         *holes = 0xF8F8FF;
         return pac_ghost_colors[which & 3];
   }
}

/* The power pills beat. */
static inline uint32_t pac_pill_color(uint32_t frame)
{
   return px_rgb_scale(PAC_RGB_WAFER, 150 + (px_kit_wave(frame * 10) * 105 >> 8));
}

/* ---------------------------------------------------------------------------
 * The sounds
 * ------------------------------------------------------------------------- */

/* What of the sounds goes on from frame to frame. */
typedef struct
{
   uint32_t ticks;          /* counts the frames heard */
   bool waka;               /* which of the two sounds of eating is next */
   unsigned warble, hum;    /* the voices at the synth that go on: 0 for none */
} pac_voices;

static inline void pac_voices_reset(pac_voices *v)
{
   v->waka   = false;
   v->warble = v->hum = 0;
}

/* Eating: up and down in turns, over a low note that goes the same way and carries it. */
static inline void pac_play_wafer(pac_voices *v, px_sound *s, int column)
{
   static const px_tone up[3] = {
      /* wave            freq  to   glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SQUARE,   300, 640, 0.07f, 0.002f, 0.03f, 0.08f, 0.22f, 2400, 1200, 0, 0 },
      { PX_WAVE_TRIANGLE, 150, 320, 0.07f, 0.002f, 0.03f, 0.09f, 0.46f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,      62, 124, 0.07f, 0.003f, 0.04f, 0.13f, 0.76f, 0, 0, 0, 0 }
   };
   static const px_tone down[3] = {
      { PX_WAVE_SQUARE,   640, 300, 0.07f, 0.002f, 0.03f, 0.08f, 0.22f, 2400, 1200, 0, 0 },
      { PX_WAVE_TRIANGLE, 320, 150, 0.07f, 0.002f, 0.03f, 0.09f, 0.46f, 0, 0, 0, 0 },
      { PX_WAVE_SINE,     124,  62, 0.07f, 0.003f, 0.04f, 0.13f, 0.76f, 0, 0, 0, 0 }
   };
   px_kit_play(s, v->waka ? down : up, 3, px_kit_pan(column));
   v->waka = !v->waka;
}
#define PAC_RUMBLE_WAFER(s)  px_sound_rumble(s, 9000, 7000, 3)

static inline void pac_play_pill(px_sound *s, int column)
{
   static const px_tone p[3] = {
      { PX_WAVE_SAW,      196, 784, 0.22f, 0.003f, 0.10f, 0.30f, 0.30f, 1200, 5000, 0, 0 },
      { PX_WAVE_SQUARE,   392, 1568, 0.22f, 0.003f, 0.08f, 0.24f, 0.14f, 2000, 6000, 0, 0 },
      { PX_WAVE_SINE,      98,  49, 0.30f, 0.002f, 0.10f, 0.40f, 0.70f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(column));
}
#define PAC_RUMBLE_PILL(s)   px_sound_rumble(s, 26000, 20000, 14)

static inline void pac_play_eaten(px_sound *s, int column)
{
   static const px_tone p[3] = {
      { PX_WAVE_SQUARE,   330, 1760, 0.40f, 0.002f, 0.30f, 0.22f, 0.24f, 1500, 6000, 14.0f, 0.03f },
      { PX_WAVE_TRIANGLE, 165,  880, 0.40f, 0.002f, 0.30f, 0.22f, 0.44f, 0, 0, 14.0f, 0.03f },
      { PX_WAVE_NOISE,   6000,  900, 0.10f, 0,      0.01f, 0.12f, 0.24f, 5000, 800, 0, 0 }
   };
   px_kit_play(s, p, 3, px_kit_pan(column));
}
#define PAC_RUMBLE_EATEN(s)  px_sound_rumble(s, 30000, 36000, 12)

/* Caught: down and away. */
static inline void pac_play_caught(px_sound *s, int column)
{
   static const px_tone p[2] = {
      { PX_WAVE_TRIANGLE, 988, 110, 1.45f, 0.004f, 1.30f, 0.30f, 0.60f, 0, 0, 7.0f, 0.06f },
      { PX_WAVE_SQUARE,   494,  55, 1.45f, 0.004f, 1.30f, 0.30f, 0.16f, 1800, 300, 7.0f, 0.06f }
   };
   px_kit_play(s, p, 2, px_kit_pan(column));
}
#define PAC_RUMBLE_CAUGHT(s) px_sound_rumble(s, 52000, 30000, 30)

/* The burst at the end of it. */
static inline void pac_play_burst(px_sound *s, int column)
{
   static const px_tone p[2] = {
      { PX_WAVE_NOISE, 5000, 500, 0.20f, 0,      0.02f, 0.28f, 0.40f, 4000, 400, 0, 0 },
      { PX_WAVE_SINE,   160,  50, 0.14f, 0.001f, 0.02f, 0.24f, 0.70f, 0, 0, 0, 0 }
   };
   px_kit_play(s, p, 2, px_kit_pan(column));
}
#define PAC_RUMBLE_BURST(s)  px_sound_rumble(s, 65535, 40000, 16)

/* A note of a game's tune in softer voices: the melody, or what goes below it. */
static inline void pac_play_note(px_sound *s, bool melody, float hz)
{
   px_tone lead[2] = {
      /* wave            freq  to  glide  attack  hold   decay  gain   cutoff to */
      { PX_WAVE_SQUARE,   0,   0,  0,     0.004f, 0.10f, 0.30f, 0.20f, 2600, 900, 5.5f, 0.004f },
      { PX_WAVE_TRIANGLE, 0,   0,  0,     0.004f, 0.12f, 0.36f, 0.42f, 0,    0,   0,    0 }
   };
   px_tone bass[2] = {
      { PX_WAVE_TRIANGLE, 0,   0,  0,     0.006f, 0.14f, 0.34f, 0.50f, 0,    0,   0,    0 },
      { PX_WAVE_SAW,      0,   0,  0,     0.006f, 0.10f, 0.26f, 0.12f, 700,  300, 0,    0 }
   };
   const float f = px_kit_tune(hz);
   if (f <= 0.0f)
      return;
   if (melody)
   {
      lead[0].freq = f;
      lead[1].freq = f * 2.0f;
      px_kit_play(s, lead, 2, 0.25f);
   }
   else
   {
      bass[0].freq = f;
      bass[1].freq = f;
      px_kit_play(s, bass, 2, -0.25f);
   }
}

static inline void pac_stop(px_sound *s, unsigned *voice, float seconds)
{
   if (*voice)
      px_synth_stop(s->synth, *voice, seconds);
   *voice = 0;
}

/* While the ghosts can be eaten: a note that climbs over and over, by `climb` of 0 to 1. */
static inline void pac_warble(pac_voices *v, px_sound *s, bool on, float climb)
{
   static const px_tone p = { PX_WAVE_SQUARE, 220, 0, 0, 0.02f, 0, 0, 1.0f, 1500, 0, 0, 0 };
   if (!on)
   {
      pac_stop(s, &v->warble, 0.08f);
      return;
   }
   if (!px_synth_move(s->synth, v->warble, 0.0f, 0.13f, 196.0f * (1.0f + climb)))
      v->warble = px_synth_play(s->synth, &p, 0.0f, 0.13f);
}

/* The chase: a siren that goes up and down, higher and faster as the wafers get fewer:
 * `few` is 0 with all of them there and 1 with none. */
static inline void pac_siren(pac_voices *v, px_sound *s, bool on, float few)
{
   static const px_tone p = { PX_WAVE_TRIANGLE, 330, 0, 0, 0.25f, 0, 0, 1.0f, 1400, 0, 0, 0 };
   float swing, hz;
   if (!on)
   {
      pac_stop(s, &v->hum, 0.25f);
      return;
   }
   swing = (float)px_kit_wave(v->ticks * (unsigned)(5 + (int)(few * 6.0f))) / 254.0f;
   hz    = (300.0f + 220.0f * few) * (1.0f + 0.45f * swing);
   if (!px_synth_move(s->synth, v->hum, 0.0f, 0.085f, hz))
      v->hum = px_synth_play(s->synth, &p, 0.0f, 0.085f);
}

#endif
