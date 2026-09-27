/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Proteus for the Atari 2600: draws a frame again from what the core captured of it
 * (proteus_capture.h) and processes the two audio voices apart. None of it needs to know
 * the game. */
#ifndef PROTEUS_FX_H
#define PROTEUS_FX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proteus_capture.h"

/* Core options. Left at "profile", an option takes the game's profile's [fx] entry of its
 * name without the prefix, and its default without one. */
#define PX_OPT_FX_PREFIX     "proteus_fx_"
#define PX_OPT_FX_PROFILE    "profile"
#define PX_OPT_FX_VIDEO      "proteus_fx_video"
#define PX_OPT_FX_SCALE      "proteus_fx_scale"
#define PX_OPT_FX_VIEW       "proteus_fx_view"
#define PX_OPT_FX_GLOW       "proteus_fx_glow"
#define PX_OPT_FX_SHADOW     "proteus_fx_shadow"
#define PX_OPT_FX_BACKGROUND "proteus_fx_background"
#define PX_OPT_FX_SMOOTH     "proteus_fx_smooth"
#define PX_OPT_FX_FLICKER    "proteus_fx_flicker"
#define PX_OPT_FX_TRAILS     "proteus_fx_trails"
#define PX_OPT_FX_REACTIVE   "proteus_fx_reactive"
#define PX_OPT_FX_SCANLINES  "proteus_fx_scanlines"
#define PX_OPT_FX_AUDIO      "proteus_fx_audio"
#define PX_OPT_FX_WIDTH      "proteus_fx_width"
#define PX_OPT_FX_LOWPASS    "proteus_fx_lowpass"
#define PX_OPT_FX_REVERB     "proteus_fx_reverb"

/* The largest frame Proteus hands to the frontend. */
#define PX_FX_MAX_SX     12u
#define PX_FX_MAX_SY     6u
#define PX_FX_MAX_WIDTH  (PXC_W * PX_FX_MAX_SX)
#define PX_FX_MAX_HEIGHT (PXC_MAX_H * PX_FX_MAX_SY)

enum
{
   PX_VIEW_NORMAL = 0,
   PX_VIEW_LAYERS,      /* every object in a flat colour of its own */
   PX_VIEW_INSTANCES,   /* the picture, with a box around every object found */
   PX_VIEW_LAYER        /* one layer alone: PX_VIEW_LAYER + PXC_L_* */
};

typedef struct
{
   bool video;
   bool audio;

   /* Output pixels for one of the core's, across and down. 1 x 1 is the core's own frame,
    * which takes no effects. */
   unsigned sx, sy;
   unsigned view;
   unsigned glow;         /* 0 off .. 3 high */
   bool shadow;
   bool background;
   bool smooth;
   bool flicker;
   bool trails;
   bool reactive;
   bool scanlines;

   unsigned width;        /* 0..100: how far apart the two voices are panned */
   unsigned lowpass;      /* 0 off, 1 soft, 2 warm */
   unsigned reverb;       /* 0 off .. 3 hall */
} px_fx_config;

/* Reads the options through `get` (a core option's value or NULL) and `profile` (the value
 * of an [fx] entry of the game's profile or NULL; may be NULL itself). What neither has
 * keeps its default. */
void px_fx_config_read(px_fx_config *c, const char *(*get)(const char *key),
      const char *(*profile)(const char *key));

/* ---------------------------------------------------------------------------
 * Objects: what the capture's pixels belong to, and the same object over frames
 * ------------------------------------------------------------------------- */

#define PX_MAX_INSTANCES  192
#define PX_MAX_OBJ_TRACKS 192
#define PX_OBJ_ROWS       48

typedef struct
{
   uint8_t  cls;          /* PXC_L_P0, PXC_L_P1, PXC_L_M0, PXC_L_M1 or PXC_L_BL */
   uint8_t  copy;         /* players: the copy 1..3; else 0 */
   uint8_t  color;        /* of the first row */
   uint8_t  ghost;        /* not in this frame: drawn from its track */
   int16_t  x, y;         /* left edge (players: of the 8 pattern bits), top row */
   uint16_t w, h;
   uint32_t rows;         /* index of the first of h rows in the pool */
   uint32_t hash;         /* of the rows' bits */
   uint32_t track;        /* the track's id, 0 before matching */
} px_instance;

typedef struct
{
   uint32_t id;           /* 0: free */
   px_instance last;      /* as last seen; its rows are in bits[] and colors[] */
   uint32_t bits[PX_OBJ_ROWS];
   uint8_t  colors[PX_OBJ_ROWS];
   uint16_t seen;         /* bit 0: seen this frame, bit 1: the frame before, ... */
   uint8_t  missed;       /* frames since it was seen */
   uint8_t  gap;          /* the longest it was away and came back, while flickering */
   bool     flickers;
   bool     matched;
   int16_t  vx, vy;       /* movement since it was seen before, in pixels */
} px_obj_track;

typedef struct
{
   px_instance inst[PX_MAX_INSTANCES];
   unsigned count;

   /* One entry a row of every instance: bit b is the pixel at x + b. */
   uint32_t *bits;
   uint8_t  *colors;
   size_t pool_used, pool_cap;

   px_obj_track tracks[PX_MAX_OBJ_TRACKS];
   uint32_t next_id;
   uint32_t frame;
} px_objects;

void px_objects_init(px_objects *o);
void px_objects_free(px_objects *o);
/* Forgets the tracks: after a reset or a loaded state. */
void px_objects_reset(px_objects *o);
/* Finds the frame's instances and matches them to the tracks. With `ghosts`, tracks that
 * flicker and are not in this frame are added as instances marked `ghost`. */
void px_objects_update(px_objects *o, const struct pxc_frame *f, bool ghosts);

/* ---------------------------------------------------------------------------
 * Video
 * ------------------------------------------------------------------------- */

typedef struct px_fx_video px_fx_video;

px_fx_video *px_fx_video_new(void);
void px_fx_video_free(px_fx_video *v);
void px_fx_video_reset(px_fx_video *v);

/* The size of the frame px_fx_video_render makes of a picture `height` rows high. */
void px_fx_video_size(const px_fx_config *c, unsigned height, unsigned *w, unsigned *h);

/* Draws the captured frame. Returns XRGB8888 pixels of *w by *h, *w * 4 bytes a row, valid
 * until the next call; NULL if the frame cannot be drawn (the caller shows the core's). */
const uint32_t *px_fx_video_render(px_fx_video *v, const struct pxc_frame *f,
      const px_fx_config *c, unsigned *w, unsigned *h);

/* How long the last frame took to draw, in microseconds. */
unsigned px_fx_video_last_us(const px_fx_video *v);

/* ---------------------------------------------------------------------------
 * Audio
 * ------------------------------------------------------------------------- */

typedef struct px_fx_audio px_fx_audio;

px_fx_audio *px_fx_audio_new(void);
void px_fx_audio_free(px_fx_audio *a);
void px_fx_audio_reset(px_fx_audio *a);
void px_fx_audio_set_rate(px_fx_audio *a, double rate);

/* `frames` stereo frames in place. On entry the left channel is voice 0 and the right one
 * voice 1, as Stella gives them with stereo sound on. */
void px_fx_audio_process(px_fx_audio *a, const px_fx_config *c, int16_t *frames, size_t count);

#ifdef __cplusplus
}
#endif

#endif
