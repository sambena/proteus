/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The kit: what game modules have in common.
 *
 * A game module (src/games/, docs/GAME_MODULES.md) says what is particular to its game:
 * which object is the enemy, what colour it is to have, which sound the game makes when it
 * is hit. How that is found out and done is the same from game to game, and is here:
 *
 *   colours     adding, scaling and mixing them
 *   options     reading a module's own
 *   memory      the console's 128 bytes, read safely
 *   tags        what a module knows of an object for as long as it is on the screen
 *   picture     recolouring the playfield and the background, filling an object's holes
 *   sound       following the TIA's two voices, and playing sounds in their place
 *
 * A module that needs something a second module would need too adds it here.
 */
#ifndef PROTEUS_KIT_H
#define PROTEUS_KIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "fx.h"

/* ---------------------------------------------------------------------------
 * Colours: 0xRRGGBB
 * ------------------------------------------------------------------------- */

/* The sum of two colours, no channel beyond 255. */
static inline uint32_t px_rgb_add(uint32_t a, uint32_t b)
{
   unsigned r = ((a >> 16) & 0xFF) + ((b >> 16) & 0xFF);
   unsigned g = ((a >> 8) & 0xFF) + ((b >> 8) & 0xFF);
   unsigned bl = (a & 0xFF) + (b & 0xFF);
   return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (bl > 255 ? 255 : bl);
}

/* A colour at `f256` of 256 of its brightness. */
static inline uint32_t px_rgb_scale(uint32_t rgb, unsigned f256)
{
   return ((((rgb >> 16) & 0xFF) * f256 >> 8) << 16) | ((((rgb >> 8) & 0xFF) * f256 >> 8) << 8)
         | ((rgb & 0xFF) * f256 >> 8);
}

/* From `a` at 0 to `b` at 256. */
static inline uint32_t px_rgb_mix(uint32_t a, uint32_t b, unsigned t256)
{
   return px_rgb_add(px_rgb_scale(a, 256 - t256), px_rgb_scale(b, t256));
}

/* A triangle wave of a counter: up from 0 to 254 and down again, every 256 counts. */
static inline unsigned px_kit_wave(unsigned t)
{
   t &= 255;
   return t < 128 ? t * 2 : (255 - t) * 2;
}

/* The next of a row of numbers that look like chance and are the same every time. */
static inline uint32_t px_kit_chance(uint32_t *seed)
{
   *seed = *seed * 1664525u + 1013904223u;
   return *seed >> 8;
}

/* ---------------------------------------------------------------------------
 * Options
 * ------------------------------------------------------------------------- */

typedef const char *(*px_kit_get)(const char *key);

/* Values and labels for options that are on or off, and for whose the sounds are. */
extern const char *const px_kit_toggle[];   /* "enabled", "disabled" */
extern const char *const px_kit_sounds[];   /* "proteus", "original" */

/* True unless the option is "disabled", "off" or "original". */
bool px_kit_on(px_kit_get get, const char *key);
/* Which of an option's values it has: 0 for the first of `values` (values and labels in
 * turns, NULL at the end, as in px_game_option), which it is too when it has none of them. */
unsigned px_kit_pick(px_kit_get get, const char *key, const char *const *values);

/* ---------------------------------------------------------------------------
 * Memory
 * ------------------------------------------------------------------------- */

/* Byte `index` of the console's memory ($80 is 0), or -1 if it is not to be had. */
static inline int px_kit_ram(const uint8_t *ram, size_t size, unsigned index)
{
   return ram && index < size ? ram[index] : -1;
}

/* ---------------------------------------------------------------------------
 * Tags: what a module knows of an object, kept while the object's track lasts
 *
 * The objects of a frame are found anew every frame; what tells one from the frame before
 * is its track. A module that has found out something about an object that does not show
 * in every frame (the row an invader began in, which ghost it is while all are blue) tags
 * the track with it.
 * ------------------------------------------------------------------------- */

#define PX_KIT_TAGS 64

typedef struct
{
   uint32_t id;         /* of the track; 0: free */
   int16_t x, y;        /* where the object was seen last */
   uint8_t tag;
   uint8_t kept;        /* seen in the frame at hand */
} px_kit_tagged;

typedef struct
{
   px_kit_tagged slot[PX_KIT_TAGS];
} px_kit_tags;

void px_kit_tags_reset(px_kit_tags *t);
/* A frame begins: nothing is seen yet. */
void px_kit_tags_begin(px_kit_tags *t);
/* What is known of an object, or NULL. */
px_kit_tagged *px_kit_tags_find(px_kit_tags *t, const px_instance *in);
/* An object is seen: what is known of it, its place brought up to date. If nothing was
 * known, it is tagged `tag_if_new`. NULL when there is no room. */
px_kit_tagged *px_kit_tags_keep(px_kit_tags *t, const px_instance *in, unsigned tag_if_new);
/* How many are known and were not seen in this frame. */
unsigned px_kit_tags_gone(const px_kit_tags *t);
/* A frame ends: forgets those not seen in it. */
void px_kit_tags_end(px_kit_tags *t);

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static inline bool px_kit_is_player(const px_instance *in)
{
   return in->cls == PXC_L_P0 || in->cls == PXC_L_P1;
}

/* The playfield's colour in rows `from` up to `to`. */
void px_kit_playfield(px_scene *s, unsigned from, unsigned to, uint32_t rgb);
/* The background's colour in rows `from` up to `to`. */
void px_kit_background(px_scene *s, unsigned from, unsigned to, uint32_t rgb);
/* Marks in `small` (an entry a captured pixel, PXC_W a row) what of the playfield in rows
 * `from` up to `to` is no higher than `rows`: dots to eat, among walls. What is on top of
 * the playfield does not hide it. Returns how many pixels it marked. */
unsigned px_kit_small_playfield(const px_scene *s, unsigned from, unsigned to, unsigned rows,
      uint8_t *small);
/* Draws the playfield in rows `from` up to `to` as outlines: `edge` where it ends,
 * `inside` within, and `light` (0: none) glowing from its edges. What `skip` marks (may be
 * NULL) is no part of it. */
void px_kit_outline(px_scene *s, unsigned from, unsigned to, uint32_t edge, uint32_t inside,
      uint32_t light, const uint8_t *skip);
/* Gives what an object encloses and does not cover (a ghost's eyes) a colour, as a part of
 * the object. What is open to the outside (a mouth) stays as it is. */
void px_kit_fill_holes(px_scene *s, const px_instance *in, uint32_t rgb);

/* A picture a module paints once and keeps: the still part of a backdrop. */
typedef struct
{
   uint32_t *pixels;
   unsigned w, h;
} px_kit_canvas;

/* Makes the canvas the size of the scene's picture. True if it is to be painted: it is new,
 * or the size changed. False too if there is no memory, with `pixels` NULL. */
bool px_kit_canvas_fit(px_kit_canvas *c, const px_scene *s);
void px_kit_canvas_free(px_kit_canvas *c);

/* An object of several colours, a row each (a helicopter's rotor, body and skids): every
 * row gets the colour `map` has for its own, which is an index into the palette. `map` has
 * 256 entries; rows whose entry is PX_KIT_KEEP stay as they are. */
#define PX_KIT_KEEP 0xFF000000u
void px_kit_repaint(px_scene *s, const px_instance *in, const uint32_t *map);

/* Scenery that moves. A game that scrolls counts in memory how far, with a counter that
 * goes round (the line of a block of 32 that the river has come to). This follows such a
 * counter and adds up how far the scenery has moved in all. */
typedef struct
{
   int16_t at;       /* the counter as it was last; -1: not known */
   int16_t step;     /* how far it went in the frame at hand */
   int32_t total;    /* and since the reset */
} px_kit_scroll;

void px_kit_scroll_reset(px_kit_scroll *k);
/* The counter is `value` now (-1: not to be had, the scenery stands still) and goes round
 * at `period`. Of the two ways round it is taken to have gone the shorter. Returns the
 * step. Not to be called for a frame that is drawn again. */
int px_kit_scroll_follow(px_kit_scroll *k, int value, unsigned period);

/* A backdrop that moves with the scenery is a canvas that goes round: what leaves it at
 * one edge comes in at the other, so it is painted to fit itself there
 * (px_kit_texture_noise does). This paints columns `from` up to `to` of row `y` of a picture as wide as the
 * canvas, of which `out` is that row, with the canvas moved `dx` to the right and `dy`
 * down. A second canvas, moved by another measure, is what is nearer or further off. */
void px_kit_canvas_roll(const px_kit_canvas *c, uint32_t *out, unsigned y, unsigned from,
      unsigned to, int32_t dx, int32_t dy);

/* A texture: a picture in shades 0..255, of the size of the scene's, painted once. It is
 * what a canvas is made of when the colours are not known until the game shows them, or
 * change: grass in the green the game has for it. It goes round as a canvas does. */
typedef struct
{
   uint8_t *shades;
   unsigned w, h;
} px_kit_texture;

/* Makes the texture the size of the scene's picture. True if it is to be painted: it is
 * new, or the size changed. False too if there is no memory, with `shades` NULL. */
bool px_kit_texture_fit(px_kit_texture *t, const px_scene *s);
void px_kit_texture_free(px_kit_texture *t);
/* Paints a canvas of the texture's size with it, in the colours `colors` has for the
 * shades. */
void px_kit_texture_show(const px_kit_texture *t, px_kit_canvas *c, const uint32_t *colors);
/* As px_kit_canvas_roll, of the texture in the colours of `colors`: for the few rows that
 * are not of the colours a canvas was made with. */
void px_kit_texture_roll(const px_kit_texture *t, uint32_t *out, unsigned y, unsigned from,
      unsigned to, int32_t dx, int32_t dy, const uint32_t *colors);
/* Paints the texture with smooth noise that fits itself at the edges: `across` by `down`
 * cells of chance to the picture, so that it looks alike at every size of it. The same
 * every time for a seed. Textures are made of several of these, coarse and fine. */
void px_kit_texture_noise(px_kit_texture *t, unsigned across, unsigned down, uint32_t seed);
/* A table of colours for the shades: `dark` at 0, `mid` at 128, `light` at 255. */
void px_kit_shades(uint32_t colors[256], uint32_t dark, uint32_t mid, uint32_t light);

/* ---------------------------------------------------------------------------
 * Sound
 * ------------------------------------------------------------------------- */

/* The TIA's two voices as a frame left them, and as the frame before did. Between the
 * writes of one frame a voice may have the waveform of one sound and the pitch of another,
 * so sounds are told by what is there when the frame ends. */
typedef struct
{
   uint8_t wave[2], pitch[2], volume[2];
   uint8_t was_wave[2], was_pitch[2], was_volume[2];
} px_kit_tia;

void px_kit_tia_reset(px_kit_tia *t);
/* Follows the writes of a frame (which may be NULL: nothing changed). */
void px_kit_tia_hear(px_kit_tia *t, const struct pxc_frame *f);

/* A voice is louder than it was: a sound began, or began again. */
static inline bool px_kit_tia_louder(const px_kit_tia *t, unsigned voice)
{
   return t->volume[voice] > t->was_volume[voice];
}
/* A voice sounds and did not. */
static inline bool px_kit_tia_began(const px_kit_tia *t, unsigned voice)
{
   return t->volume[voice] && !t->was_volume[voice];
}

/* The pitch in Hz of a waveform that is a tone, on a console of 60 Hz; 0 for noise. */
float px_kit_tia_hz(unsigned wave, unsigned pitch);

/* The note of the scale that a pitch is nearest to: the TIA's are between them. */
float px_kit_tune(float hz);

/* Where a column of the picture is between left and right; the middle for -1. */
float px_kit_pan(int column);
/* Plays tones together, as one sound. */
void px_kit_play(px_sound *s, const px_tone *tones, unsigned count, float pan);

#ifdef __cplusplus
}
#endif

#endif
