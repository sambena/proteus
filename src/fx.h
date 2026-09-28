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
#define PX_OPT_FX_BARS       "proteus_fx_bars"
#define PX_OPT_FX_GAME       "proteus_fx_game"
#define PX_OPT_FX_BUTTON     "proteus_fx_button"
#define PX_OPT_FX_KEYS       "proteus_fx_keys"
#define PX_OPT_FX_RUMBLE     "proteus_fx_rumble"
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
   bool bars;             /* fill the bars HMOVE leaves at the left */
   bool game;             /* what Proteus knows of this game in particular */

   unsigned width;        /* 0..100: how far apart the two voices are panned */
   unsigned lowpass;      /* 0 off, 1 soft, 2 warm */
   unsigned reverb;       /* 0 off .. 3 hall */
   unsigned rumble;       /* 0 off .. 3 high */
} px_fx_config;

/* Reads the options through `get` (a core option's value or NULL) and `profile` (the value
 * of an [fx] entry of the game's profile or NULL; may be NULL itself). What neither has
 * keeps its default. */
void px_fx_config_read(px_fx_config *c, const char *(*get)(const char *key),
      const char *(*profile)(const char *key));

/* ---------------------------------------------------------------------------
 * Objects: what the capture's pixels belong to, and the same object over frames
 * ------------------------------------------------------------------------- */

/* Who an object is, as a game module tells it (px_game.who): 1 to 127, and this with it if
 * the object may be anywhere in the next frame it is seen in. */
#define PX_WHO_ANYWHERE 0x80u

#define PX_MAX_INSTANCES  192
#define PX_MAX_OBJ_TRACKS 192
#define PX_OBJ_ROWS       48

typedef struct
{
   uint8_t  cls;          /* PXC_L_P0, PXC_L_P1, PXC_L_M0, PXC_L_M1 or PXC_L_BL */
   uint8_t  copy;         /* players: the copy 1..3; else 0 */
   uint8_t  color;        /* of the first row */
   uint8_t  ghost;        /* not in this frame: drawn from its track */
   uint8_t  role;         /* PX_ROLE_*: what it is in the game, if a game module said */
   uint8_t  group;        /* which of its kind: the row of an invader */
   uint8_t  who;          /* who it is, if a game module told (px_game.who); 0: not told */
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

   /* Tells who an object is, before it is matched to a track; NULL: nobody does. An object
    * that is told apart is matched to the track of who it is and to no other, and may be
    * away for twice as long as one that is not. */
   unsigned (*who)(void *ctx, const px_instance *in);
   void *who_ctx;
} px_objects;

void px_objects_init(px_objects *o);
void px_objects_free(px_objects *o);
/* Forgets the tracks: after a reset or a loaded state. */
void px_objects_reset(px_objects *o);
/* Finds the frame's instances and matches them to the tracks. With `ghosts`, tracks that
 * flicker and are not in this frame are added as instances marked `ghost`. */
void px_objects_update(px_objects *o, const struct pxc_frame *f, bool ghosts);

/* The track of an instance, or NULL. */
const px_obj_track *px_objects_track(const px_objects *o, uint32_t id);

/* ---------------------------------------------------------------------------
 * Games: what Proteus knows of one game in particular
 * ------------------------------------------------------------------------- */

enum
{
   PX_ROLE_NONE = 0,
   PX_ROLE_ENEMY,
   PX_ROLE_PLAYER,
   PX_ROLE_SHIELD,
   PX_ROLE_SHOT,      /* the player's */
   PX_ROLE_BOMB,      /* an enemy's */
   PX_ROLE_BONUS,     /* the saucer */
   PX_ROLE_HUD
};

typedef struct px_fx_video px_fx_video;

/* A frame as a game module sees it, between the finding of its objects and its drawing.
 * The planes have an entry a captured pixel, PXC_W a row. */
typedef struct
{
   const struct pxc_frame *frame;
   const px_fx_config *cfg;
   px_objects *objects;
   const uint8_t *ram;        /* the console's 128 bytes, or NULL */
   size_t ram_size;
   /* False when a frame is drawn again that was drawn before (the game stands still):
    * nothing is to happen then, only to look as it did. */
   bool advance;
   unsigned w, h;             /* of the picture */
   unsigned sx, sy;           /* its pixels for one captured */

   uint32_t *top;             /* what is on top: PX_KEY(class, colour) */
   uint32_t *bk;              /* the background's colour */
   uint32_t *sprite;          /* an object's colour with 0xFF000000 set, or 0 */
   uint8_t  *energy;          /* the object there glows brighter and leaves a trail */
   uint8_t  *crisp;           /* what is there gives off no light: lines, digits */
   /* Scenery that glows (neon walls, lava): the colour of its light with 0xFF000000 set,
    * 0 where there is none, which it is everywhere when the module gets the frame. It is
    * drawn as the scenery it is, and the glow is what is added. */
   uint32_t *light;

   /* A picture of w by h to show where the background is, if the module sets backdrop_on.
    * backdrop_stale is set when it has to be painted anew (the size changed). */
   uint32_t *backdrop;
   bool backdrop_on;
   bool backdrop_stale;

   px_fx_video *video;
} px_scene;

enum { PX_CLS_BK = 0, PX_CLS_PF, PX_CLS_SPRITE, PX_CLS_BLANK };
#define PX_KEY(cls, rgb) (((uint32_t)(cls) << 24) | ((rgb) & 0xFFFFFFu))
#define PX_KEY_CLS(k)    ((k) >> 24)

/* Gives an object another colour. */
void px_scene_tint(px_scene *s, const px_instance *in, uint32_t rgb);
/* Marks an object's pixels as glowing brighter and leaving a trail, or not. */
void px_scene_energy(px_scene *s, const px_instance *in, bool on);
/* Marks an object's pixels as giving off no light, or not: for what is to stay crisp, as
 * lines and digits are. What a module gives the role PX_ROLE_HUD is marked so for it. */
void px_scene_crisp(px_scene *s, const px_instance *in, bool on);
/* Sparks from a captured pixel outwards. */
void px_scene_burst(px_scene *s, int x, int y, uint32_t rgb, unsigned count, unsigned speed);
/* Lights the whole picture up for a moment; strength of 256. */
void px_scene_flash(px_scene *s, uint32_t rgb, unsigned strength);

/* ---------------------------------------------------------------------------
 * Sounds of Proteus's own: as many voices as it takes, where the TIA has two
 * ------------------------------------------------------------------------- */

enum { PX_WAVE_SINE = 0, PX_WAVE_TRIANGLE, PX_WAVE_SQUARE, PX_WAVE_SAW, PX_WAVE_NOISE };

/* A sound: a wave whose pitch glides, through a filter that closes, under an envelope. */
typedef struct
{
   uint8_t wave;
   float freq, freq_end;     /* Hz: from one to the other in `glide` seconds, then held */
   float glide;
   float attack;             /* seconds to full volume */
   float hold;               /* seconds at full volume */
   float decay;              /* seconds to a thousandth of it; 0: until it is stopped */
   float gain;
   float cutoff, cutoff_end; /* Hz of a low-pass, gliding as the pitch does; 0: none */
   float vibrato_hz, vibrato; /* and how far, as a part of the pitch */
} px_tone;

#define PX_SYNTH_VOICES 32

typedef struct px_synth px_synth;

px_synth *px_synth_new(void);
void px_synth_free(px_synth *s);
void px_synth_reset(px_synth *s);
void px_synth_set_rate(px_synth *s, double rate);
/* Starts a sound at `pan` (-1 left .. 1 right). Returns what to name it by, never 0. */
unsigned px_synth_play(px_synth *s, const px_tone *p, float pan, float gain);
/* A sound that goes on (decay 0): moves it, makes it louder or softer, gives it another
 * pitch (0: as it is). False when it is no more. */
bool px_synth_move(px_synth *s, unsigned id, float pan, float gain, float freq);
/* Lets a sound fade in `seconds`. */
void px_synth_stop(px_synth *s, unsigned id, float seconds);
/* Adds what sounds to `count` stereo frames. */
void px_synth_render(px_synth *s, float *frames, size_t count);
/* How many voices sound. */
unsigned px_synth_sounding(const px_synth *s);

/* A frame's sound as a game module hears it, before it is mixed. */
typedef struct
{
   const struct pxc_frame *frame;   /* for its writes to the audio registers; may be NULL */
   const uint8_t *ram;
   size_t ram_size;
   const px_objects *objects;       /* as of the picture before, with their roles; or NULL */
   px_synth *synth;
   /* How much is heard of each of the TIA's voices: 1 at first; 0 where the module plays
    * a sound of its own for what the voice plays. */
   float voice[2];
   /* What the controller is to do: strength of 65535, and for how many frames. */
   unsigned rumble_strong, rumble_weak, rumble_frames;
} px_sound;

/* Shakes the controller, if stronger than what it does already. */
void px_sound_rumble(px_sound *s, unsigned strong, unsigned weak, unsigned frames);

/* An option of a game module's own: values and their labels in turns, NULL at the end. */
typedef struct
{
   const char *key;           /* "proteus_si_colors" */
   const char *desc;
   const char *info;
   const char *default_value;
   const char *const *values;
} px_game_option;

/* What a module is given to tell who an object is. */
typedef struct
{
   const struct pxc_frame *frame;
   const uint8_t *ram;        /* the console's 128 bytes, or NULL */
   size_t ram_size;
} px_glance;

typedef struct
{
   const char *name;
   const char *const *md5;          /* of the ROMs it is for, lower case; NULL at the end */
   /* Defaults for the options of fx.h: names without the prefix and values in turns, NULL
    * at the end. A profile's [fx] goes before them. */
   const char *const *fx;
   const px_game_option *options;   /* key NULL at the end; may be NULL */

   void *(*create)(void);
   void (*destroy)(void *state);
   void (*reset)(void *state);
   /* The options changed: `get` gives the value of one of the module's own. */
   void (*configure)(void *state, const char *(*get)(const char *key));
   void (*frame)(void *state, px_scene *s);
   /* Once a frame, before its sound is mixed; may be NULL. */
   void (*sound)(void *state, px_sound *s);
   /* Who an object is: for games that show several things with one object in turns, so
    * near each other that where they are does not tell them apart. 0 for what the module
    * does not tell apart, which is then told apart by where it is; may be NULL. It is asked
    * before the frame's objects are matched to those of the frames before. */
   unsigned (*who)(void *state, const px_glance *g, const px_instance *in);
} px_game;

/* The module for a ROM, or NULL. */
const px_game *px_game_find(const char *md5);
/* The modules there are: src/games/games.h lists them. */
unsigned px_game_count(void);
const px_game *px_game_at(unsigned index);
/* A module's default for an option of fx.h ("glow"), or NULL. */
const char *px_game_fx(const px_game *g, const char *key);
/* The MD5 of `size` bytes as 32 lower case digits and a zero. */
void px_md5(const void *data, size_t size, char out[33]);

/* ---------------------------------------------------------------------------
 * The panel: the options on the picture
 * ------------------------------------------------------------------------- */

#define PX_PANEL_LINES 24

typedef struct
{
   bool open;
   const char *title;
   const char *hint;                     /* the keys, at the bottom */
   const char *names[PX_PANEL_LINES];
   const char *values[PX_PANEL_LINES];
   unsigned count;
   unsigned selected;
   /* One line shown for a while with the panel closed. */
   const char *toast;
} px_panel;

/* Draws the panel, or its toast, on a picture of w by h. */
void px_panel_draw(uint32_t *out, unsigned w, unsigned h, const px_panel *p);

/* ---------------------------------------------------------------------------
 * Threads that share a frame's work
 * ------------------------------------------------------------------------- */

#define PX_POOL_MAX 7   /* threads besides the one that asks */

typedef struct px_pool px_pool;
/* Part `index` of `count` of a job. Parts run at once and in no order. */
typedef void (*px_pool_job)(void *ctx, unsigned index, unsigned count);

/* Half as many threads as the machine has cores, the asking one counted. */
px_pool *px_pool_new(void);
void px_pool_free(px_pool *p);
/* How many parts to make of a job for every thread to have one. */
unsigned px_pool_parts(const px_pool *p);
/* Runs all parts of a job and returns when they are done. */
void px_pool_run(px_pool *p, px_pool_job job, void *ctx, unsigned count);

/* ---------------------------------------------------------------------------
 * Video
 * ------------------------------------------------------------------------- */

px_fx_video *px_fx_video_new(void);
void px_fx_video_free(px_fx_video *v);
void px_fx_video_reset(px_fx_video *v);

/* What a frame is drawn with besides its capture. */
typedef struct
{
   const px_game *game;       /* NULL: none */
   void *game_state;
   const uint8_t *ram;
   size_t ram_size;
   bool advance;              /* false: the frame before, drawn again */
   const px_panel *panel;     /* NULL: none */
} px_fx_extra;

/* The size of the frame px_fx_video_render makes of a picture `height` rows high. */
void px_fx_video_size(const px_fx_config *c, unsigned height, unsigned *w, unsigned *h);

/* Draws the captured frame. Returns XRGB8888 pixels of *w by *h, *w * 4 bytes a row, valid
 * until the next call; NULL if the frame cannot be drawn (the caller shows the core's). */
const uint32_t *px_fx_video_render(px_fx_video *v, const struct pxc_frame *f,
      const px_fx_config *c, const px_fx_extra *extra, unsigned *w, unsigned *h);

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
 * voice 1, as Stella gives them with stereo sound on. `voice` is how much is heard of each
 * (NULL: all of both), `synth` what sounds with them (may be NULL). */
void px_fx_audio_process(px_fx_audio *a, const px_fx_config *c, int16_t *frames, size_t count,
      const float *voice, px_synth *synth);

/* The objects of the picture drawn last, with the roles a game module gave them. */
const px_objects *px_fx_video_objects(const px_fx_video *v);

#ifdef __cplusplus
}
#endif

#endif
