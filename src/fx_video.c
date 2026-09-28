/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Draws an Atari 2600 frame again from its captured parts, larger and with effects that
 * need to know what is an object and what is scenery:
 *
 *   - objects glow, and shots leave a trail
 *   - objects throw a shadow on the playfield and the background behind them
 *   - objects get their stair steps cut; the playfield stays blocky
 *   - objects that the game shows in turns (flicker) are drawn in every frame
 *   - the background gets a vignette, and its colour bands blend into each other
 *
 * The picture is made of three classes (background, playfield, objects), from the capture's
 * tags and the priority the TIA used at each pixel.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")
#endif

#include "fx.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define CLS_BK     PX_CLS_BK
#define CLS_PF     PX_CLS_PF
#define CLS_SPRITE PX_CLS_SPRITE
#define CLS_BLANK  PX_CLS_BLANK

#define GLOW_W (PXC_W * 2)
/* Parts a picture is drawn in, each by a thread. */
#define BANDS_MAX (PX_POOL_MAX + 1)
#define KEY(cls, rgb) PX_KEY(cls, rgb)
#define KEY_CLS(k)    PX_KEY_CLS(k)

#define SPARKS_MAX 384

/* A spark: where it is among the glow's pixels, in 256ths. */
typedef struct
{
   int32_t x, y, vx, vy;
   uint32_t rgb;
   uint16_t life, span;
} spark;

struct px_fx_video
{
   px_objects objects;

   uint32_t *out;
   size_t out_cap;

   /* One entry a captured pixel. */
   uint32_t *top;        /* KEY(class, colour) of what is on top */
   uint32_t *bk;         /* the background's colour there */
   uint32_t *sprite;     /* an object's colour there, 0xFF000000 set; 0 where there is none */
   uint8_t  *energy;     /* a missile or the ball is there */
   uint8_t  *crisp;      /* no light from what is there */
   uint32_t *light;      /* scenery that glows: its light, 0 where there is none */
   bool lit;             /* a game module put light there in this frame */

   /* GLOW_W by height, three channels. */
   uint16_t *glow;
   uint16_t *glow_tmp;
   uint16_t *trail;
   uint16_t *glow_row;   /* a row for every band, blended between two of glow's */
   uint8_t  *row_sprite; /* a row of the capture has an object */
   uint8_t  *row_glow;   /* a row of the glow has light */

   /* For the columns of a picture col_w wide. */
   unsigned col_w, col_sx;
   uint16_t *col_vx;     /* how far from the middle, squared */
   uint16_t *col_shade;  /* the capture's column a shadow comes from, 0xFFFF for none */
   uint16_t *col_near;
   uint16_t *col_g0, *col_g1;   /* the glow's two columns next to it */
   uint8_t  *col_gf;     /* how far from the first to the second, of 256 */
   uint32_t *keys;       /* a row of the picture before the effects, for every band */

   px_pool *pool;

   /* A game module's. */
   const px_game *who_game;   /* the module that tells who an object is, while it is asked */
   void *who_state;
   px_glance glance;
   uint32_t *backdrop;   /* a picture for where the background is */
   size_t backdrop_cap;
   unsigned backdrop_w, backdrop_h;
   bool backdrop_on;
   spark sparks[SPARKS_MAX];
   unsigned spark_count;
   uint32_t spark_seed;
   unsigned flash;       /* of 256 */
   uint32_t flash_rgb;

   unsigned height;      /* of the frame before, to tell when the buffers are stale */
   unsigned level;       /* how loud the voices are, smoothed, 0..256 */
   unsigned last_us;
};

/* ---------------------------------------------------------------------------
 * Options
 * ------------------------------------------------------------------------- */

static bool is_on(const char *v, bool def)
{
   if (!v)
      return def;
   return !strcmp(v, "enabled") || !strcmp(v, "on");
}

static unsigned pick(const char *v, const char *const *names, unsigned count, unsigned def)
{
   for (unsigned i = 0; v && i < count; i++)
      if (!strcmp(v, names[i]))
         return i;
   return def;
}

/* An option's value: the core option, the game's profile where that is left at "profile",
 * or NULL for the default. */
static const char *value_of(const char *(*get)(const char *key),
      const char *(*profile)(const char *key), const char *key)
{
   const char *v = get ? get(key) : NULL;
   if (v && strcmp(v, PX_OPT_FX_PROFILE))
      return v;
   return profile ? profile(key + sizeof(PX_OPT_FX_PREFIX) - 1) : NULL;
}

void px_fx_config_read(px_fx_config *c, const char *(*get)(const char *key),
      const char *(*profile)(const char *key))
{
   static const char *const levels[]  = { "off", "low", "medium", "high" };
   static const char *const lowpass[] = { "off", "soft", "warm" };
   static const char *const reverb[]  = { "off", "small", "room", "hall" };
   static const char *const views[]   = { "normal", "layers", "instances",
      "bk", "pf", "bl", "p0", "m0", "p1", "m1" };
   const char *v;
#define V(key) value_of(get, profile, key)

   memset(c, 0, sizeof(*c));
   c->video      = is_on(V(PX_OPT_FX_VIDEO), true);
   c->audio      = is_on(V(PX_OPT_FX_AUDIO), true);
   c->glow       = pick(V(PX_OPT_FX_GLOW), levels, 4, 2);
   c->shadow     = is_on(V(PX_OPT_FX_SHADOW), true);
   c->background = is_on(V(PX_OPT_FX_BACKGROUND), true);
   c->smooth     = is_on(V(PX_OPT_FX_SMOOTH), true);
   c->flicker    = is_on(V(PX_OPT_FX_FLICKER), true);
   c->trails     = is_on(V(PX_OPT_FX_TRAILS), true);
   c->reactive   = is_on(V(PX_OPT_FX_REACTIVE), true);
   c->scanlines  = is_on(V(PX_OPT_FX_SCANLINES), false);
   c->bars       = is_on(V(PX_OPT_FX_BARS), true);
   c->game       = is_on(V(PX_OPT_FX_GAME), true);
   c->rumble     = pick(V(PX_OPT_FX_RUMBLE), levels, 4, 2);
   c->lowpass    = pick(V(PX_OPT_FX_LOWPASS), lowpass, 3, 1);
   c->reverb     = pick(V(PX_OPT_FX_REVERB), reverb, 4, 1);
   c->view       = pick(V(PX_OPT_FX_VIEW), views, 10, PX_VIEW_NORMAL);

   v = V(PX_OPT_FX_WIDTH);
   c->width = v ? (unsigned)atoi(v) : 30;
   if (c->width > 100)
      c->width = 100;

   v = V(PX_OPT_FX_SCALE);
#undef V
   c->sx = 8;
   if (v && !strcmp(v, "native"))    c->sx = 1;
   else if (v && !strcmp(v, "640"))  c->sx = 4;
   else if (v && !strcmp(v, "1920")) c->sx = 12;
   c->sy = c->sx > 1 ? c->sx / 2 : 1;
}

void px_fx_video_size(const px_fx_config *c, unsigned height, unsigned *w, unsigned *h)
{
   *w = PXC_W * c->sx;
   *h = height * c->sy;
}

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

px_fx_video *px_fx_video_new(void)
{
   const size_t n = (size_t)PXC_W * PXC_MAX_H, g = (size_t)GLOW_W * PXC_MAX_H * 3;
   px_fx_video *v = (px_fx_video*)calloc(1, sizeof(*v));
   if (!v)
      return NULL;
   px_objects_init(&v->objects);
   v->top      = (uint32_t*)calloc(n, sizeof(uint32_t));
   v->bk       = (uint32_t*)calloc(n, sizeof(uint32_t));
   v->sprite   = (uint32_t*)calloc(n, sizeof(uint32_t));
   v->energy   = (uint8_t*)calloc(n, 1);
   v->crisp    = (uint8_t*)calloc(n, 1);
   v->light    = (uint32_t*)calloc(n, sizeof(uint32_t));
   v->glow     = (uint16_t*)calloc(g, sizeof(uint16_t));
   v->glow_tmp = (uint16_t*)calloc(g, sizeof(uint16_t));
   v->trail    = (uint16_t*)calloc(g, sizeof(uint16_t));
   v->glow_row = (uint16_t*)calloc((size_t)GLOW_W * 3 * BANDS_MAX, sizeof(uint16_t));
   v->pool     = px_pool_new();
   v->row_sprite = (uint8_t*)calloc(PXC_MAX_H, 1);
   v->row_glow   = (uint8_t*)calloc(PXC_MAX_H, 1);
   if (!v->top || !v->bk || !v->sprite || !v->energy || !v->crisp || !v->light || !v->glow || !v->glow_tmp || !v->trail
         || !v->glow_row || !v->row_sprite || !v->row_glow)
   {
      px_fx_video_free(v);
      return NULL;
   }
   return v;
}

void px_fx_video_free(px_fx_video *v)
{
   if (!v)
      return;
   px_pool_free(v->pool);
   px_objects_free(&v->objects);
   free(v->out);
   free(v->top);
   free(v->bk);
   free(v->sprite);
   free(v->energy);
   free(v->crisp);
   free(v->light);
   free(v->glow);
   free(v->glow_tmp);
   free(v->trail);
   free(v->glow_row);
   free(v->row_sprite);
   free(v->row_glow);
   free(v->col_vx);
   free(v->col_shade);
   free(v->col_near);
   free(v->col_g0);
   free(v->col_g1);
   free(v->col_gf);
   free(v->keys);
   free(v->backdrop);
   free(v);
}

void px_fx_video_reset(px_fx_video *v)
{
   if (!v)
      return;
   px_objects_reset(&v->objects);
   memset(v->trail, 0, (size_t)GLOW_W * PXC_MAX_H * 3 * sizeof(uint16_t));
   v->level       = 0;
   v->spark_count = 0;
   v->flash       = 0;
}

/* ---------------------------------------------------------------------------
 * For game modules
 * ------------------------------------------------------------------------- */

void px_scene_tint(px_scene *s, const px_instance *in, uint32_t rgb)
{
   const px_objects *o = s->objects;
   rgb &= 0xFFFFFFu;
   for (unsigned r = 0; r < in->h; r++)
   {
      int y = in->y + (int)r;
      uint32_t bits = o->bits[in->rows + r];
      if (y < 0 || y >= (int)s->frame->height)
         continue;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         int x = in->x + (int)b;
         size_t i;
         if (!((bits >> b) & 1) || x < 0 || x >= PXC_W)
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         if (s->sprite[i])
            s->sprite[i] = 0xFF000000u | rgb;
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE)
            s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
      }
   }
}

void px_scene_energy(px_scene *s, const px_instance *in, bool on)
{
   const px_objects *o = s->objects;
   for (unsigned r = 0; r < in->h; r++)
   {
      int y = in->y + (int)r;
      uint32_t bits = o->bits[in->rows + r];
      if (y < 0 || y >= (int)s->frame->height)
         continue;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         int x = in->x + (int)b;
         if (((bits >> b) & 1) && x >= 0 && x < PXC_W)
            s->energy[(size_t)y * PXC_W + (size_t)x] = on;
      }
   }
}

void px_scene_crisp(px_scene *s, const px_instance *in, bool on)
{
   const px_objects *o = s->objects;
   for (unsigned r = 0; r < in->h; r++)
   {
      int y = in->y + (int)r;
      uint32_t bits = o->bits[in->rows + r];
      if (y < 0 || y >= (int)s->frame->height)
         continue;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         int x = in->x + (int)b;
         if (((bits >> b) & 1) && x >= 0 && x < PXC_W)
            s->crisp[(size_t)y * PXC_W + (size_t)x] = on;
      }
   }
}

static uint32_t chance(px_fx_video *v)
{
   v->spark_seed = v->spark_seed * 1664525u + 1013904223u;
   return v->spark_seed >> 8;
}

void px_scene_burst(px_scene *s, int x, int y, uint32_t rgb, unsigned count, unsigned speed)
{
   /* Sixteen directions of a circle, in 256ths. */
   static const int16_t dir[16][2] = {
      { 256, 0 }, { 237, 98 }, { 181, 181 }, { 98, 237 }, { 0, 256 }, { -98, 237 },
      { -181, 181 }, { -237, 98 }, { -256, 0 }, { -237, -98 }, { -181, -181 }, { -98, -237 },
      { 0, -256 }, { 98, -237 }, { 181, -181 }, { 237, -98 }
   };
   px_fx_video *v = s->video;
   if (!s->advance)
      return;
   for (unsigned i = 0; i < count && v->spark_count < SPARKS_MAX; i++)
   {
      spark *k = &v->sparks[v->spark_count++];
      unsigned d = (i * 16 / (count ? count : 1) + chance(v)) & 15;
      int pace = (int)(speed / 2 + chance(v) % (speed ? speed : 1));
      k->x    = (x * 2 + 1) * 256;
      k->y    = y * 256 + 128;
      k->vx   = dir[d][0] * pace / 256;
      k->vy   = dir[d][1] * pace / 256;
      k->rgb  = rgb & 0xFFFFFFu;
      k->span = (uint16_t)(14 + chance(v) % 14);
      k->life = k->span;
   }
}

void px_scene_flash(px_scene *s, uint32_t rgb, unsigned strength)
{
   if (!s->advance)
      return;
   s->video->flash     = strength > 256 ? 256 : strength;
   s->video->flash_rgb = rgb & 0xFFFFFFu;
}

static void move_sparks(px_fx_video *v, unsigned height)
{
   unsigned kept = 0;
   for (unsigned i = 0; i < v->spark_count; i++)
   {
      spark *k = &v->sparks[i];
      k->x += k->vx;
      k->y += k->vy;
      /* They slow down, and fall a little. */
      k->vx = k->vx * 236 / 256;
      k->vy = k->vy * 236 / 256 + 6;
      if (--k->life == 0 || k->x < 0 || k->y < 0 || k->x >= GLOW_W * 256
            || k->y >= (int32_t)height * 256)
         continue;
      v->sparks[kept++] = *k;
   }
   v->spark_count = kept;
   v->flash = v->flash * 200 / 256;
   if (v->flash < 6)
      v->flash = 0;
}

unsigned px_fx_video_last_us(const px_fx_video *v)
{
   return v ? v->last_us : 0;
}

const px_objects *px_fx_video_objects(const px_fx_video *v)
{
   return v ? &v->objects : NULL;
}

static uint64_t now_us(void)
{
#ifdef _WIN32
   static LARGE_INTEGER freq;
   LARGE_INTEGER t;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&t);
   return (uint64_t)(t.QuadPart * 1000000 / freq.QuadPart);
#else
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}

/* ---------------------------------------------------------------------------
 * The picture's classes
 * ------------------------------------------------------------------------- */

/* What the TIA puts on top, as a class and the layer it takes the colour from. */
static unsigned top_of(uint8_t tags, unsigned priority, unsigned *layer)
{
   static const struct { uint8_t tag, layer, cls; } order[3][6] = {
      { { PXC_P0, PXC_L_P0, CLS_SPRITE }, { PXC_M0, PXC_L_M0, CLS_SPRITE },
        { PXC_P1, PXC_L_P1, CLS_SPRITE }, { PXC_M1, PXC_L_M1, CLS_SPRITE },
        { PXC_PF, PXC_L_PF, CLS_PF },     { PXC_BL, PXC_L_BL, CLS_SPRITE } },
      { { PXC_PF, PXC_L_PF, CLS_PF },     { PXC_BL, PXC_L_BL, CLS_SPRITE },
        { PXC_P0, PXC_L_P0, CLS_SPRITE }, { PXC_M0, PXC_L_M0, CLS_SPRITE },
        { PXC_P1, PXC_L_P1, CLS_SPRITE }, { PXC_M1, PXC_L_M1, CLS_SPRITE } },
      { { PXC_P0, PXC_L_P0, CLS_SPRITE }, { PXC_M0, PXC_L_M0, CLS_SPRITE },
        { PXC_PF, PXC_L_PF, CLS_PF },     { PXC_P1, PXC_L_P1, CLS_SPRITE },
        { PXC_M1, PXC_L_M1, CLS_SPRITE }, { PXC_BL, PXC_L_BL, CLS_SPRITE } },
   };
   if (priority > 2)
      priority = 0;
   for (unsigned i = 0; i < 6; i++)
      if (tags & order[priority][i].tag)
      {
         *layer = order[priority][i].layer;
         return order[priority][i].cls;
      }
   *layer = PXC_L_BK;
   return CLS_BK;
}

/* The bars at the left of lines on which a game moved its objects (HMOVE): the TIA blanks
 * eight pixels there. Black on black they do not show, on a background that is lit they
 * would, so they are given what is next to them of the scenery. */
static void fill_bars(px_fx_video *v, const struct pxc_frame *f)
{
   for (unsigned y = 0; y < f->height; y++)
   {
      uint32_t *top = v->top + (size_t)y * PXC_W;
      uint32_t *bk  = v->bk + (size_t)y * PXC_W;
      unsigned n = 0;
      uint32_t key;
      while (n < PXC_W && KEY_CLS(top[n]) == CLS_BLANK)
         n++;
      if (!n || n > 16)
         continue;
      key = KEY_CLS(top[n]) == CLS_SPRITE ? KEY(CLS_BK, bk[n]) : top[n];
      for (unsigned x = 0; x < n; x++)
      {
         top[x] = key;
         bk[x]  = bk[n];
      }
   }
}

static void classify(px_fx_video *v, const struct pxc_frame *f)
{
   static const uint8_t sprite_tag[5]   = { PXC_P0, PXC_M0, PXC_P1, PXC_M1, PXC_BL };
   static const uint8_t sprite_layer[5] = { PXC_L_P0, PXC_L_M0, PXC_L_P1, PXC_L_M1, PXC_L_BL };
   const size_t n = (size_t)PXC_W * f->height;

   for (size_t i = 0; i < n; i++)
   {
      uint8_t tags = f->tags[i];
      unsigned layer;

      v->sprite[i] = 0;
      v->energy[i] = 0;
      v->crisp[i]  = 0;
      if (tags & PXC_BLANK)
      {
         v->top[i] = KEY(CLS_BLANK, f->palette[f->winner[i]]);
         v->bk[i]  = 0;
         continue;
      }
      v->top[i] = KEY(top_of(tags, PXC_AUX_PRIORITY(f->aux[i]), &layer), f->palette[f->winner[i]]);
      v->bk[i]  = f->palette[f->color[PXC_L_BK][i]] & 0xFFFFFFu;
      if (tags & PXC_SPRITES)
      {
         for (unsigned s = 0; s < 5; s++)
            if (tags & sprite_tag[s])
            {
               v->sprite[i] = 0xFF000000u | (f->palette[f->color[sprite_layer[s]][i]] & 0xFFFFFFu);
               break;
            }
         v->energy[i] = (tags & (PXC_M0 | PXC_M1 | PXC_BL)) != 0;
      }
   }
}

/* Asks the game module who an object is. */
static unsigned ask_who(void *ctx, const px_instance *in)
{
   px_fx_video *v = (px_fx_video*)ctx;
   return v->who_game->who(v->who_state, &v->glance, in);
}

/* Objects that are in turn not to be drawn in this frame, drawn from their tracks. */
static void draw_ghosts(px_fx_video *v, const struct pxc_frame *f)
{
   const px_objects *o = &v->objects;

   for (unsigned k = 0; k < o->count; k++)
   {
      const px_instance *in = &o->inst[k];
      if (!in->ghost)
         continue;
      for (unsigned r = 0; r < in->h; r++)
      {
         int y = in->y + (int)r;
         uint32_t bits = o->bits[in->rows + r];
         uint32_t rgb  = f->palette[o->colors[in->rows + r]] & 0xFFFFFFu;
         if (y < 0 || y >= (int)f->height)
            continue;
         for (unsigned b = 0; b < 32 && bits >> b; b++)
         {
            int x = in->x + (int)b;
            size_t i;
            if (!((bits >> b) & 1) || x < 0 || x >= PXC_W)
               continue;
            i = (size_t)y * PXC_W + (size_t)x;
            if (v->sprite[i] || KEY_CLS(v->top[i]) == CLS_BLANK)
               continue;
            v->sprite[i] = 0xFF000000u | rgb;
            v->top[i]    = KEY(CLS_SPRITE, rgb);
            v->energy[i] = in->cls != PXC_L_P0 && in->cls != PXC_L_P1;
         }
      }
   }
}

/* ---------------------------------------------------------------------------
 * Glow
 * ------------------------------------------------------------------------- */

/* sum / div for the sums of a box, by a multiplication: exact while sum * div < 2^32. */
#define BOX_RECIPROCAL(div) ((uint64_t)((0x100000000ull + (div) - 1) / (div)))
#define BOX_DIVIDE(sum, m)  ((uint16_t)(((uint64_t)(sum) * (m)) >> 32))

/* Rows `from` to `to` of a box blur across. */
static void box_blur_h(const uint16_t *src, uint16_t *dst, unsigned w, unsigned from, unsigned to,
      unsigned r)
{
   const uint64_t m = BOX_RECIPROCAL(2 * r + 1);
   for (unsigned y = from; y < to; y++)
   {
      const uint16_t *s = src + (size_t)y * w * 3;
      uint16_t *d = dst + (size_t)y * w * 3;
      uint32_t sum[3] = { 0, 0, 0 };
      for (unsigned x = 0; x <= r && x < w; x++)
         for (unsigned c = 0; c < 3; c++)
            sum[c] += s[x * 3 + c];
      for (unsigned x = 0; x < w; x++)
      {
         for (unsigned c = 0; c < 3; c++)
            d[x * 3 + c] = BOX_DIVIDE(sum[c], m);
         if (x + r + 1 < w)
            for (unsigned c = 0; c < 3; c++)
               sum[c] += s[(x + r + 1) * 3 + c];
         if (x >= r)
            for (unsigned c = 0; c < 3; c++)
               sum[c] -= s[(x - r) * 3 + c];
      }
   }
}

/* A box blur down, of rows `from` to `to` of `src` into the same rows and `r` more to
 * either side; the rows of `src` outside `from` to `to` count as dark. */
static void box_blur_v(const uint16_t *src, uint16_t *dst, unsigned w, unsigned h, unsigned from,
      unsigned to, unsigned r, uint32_t *sums)
{
   const uint64_t m = BOX_RECIPROCAL(2 * r + 1);
   const unsigned n = w * 3;
   const unsigned first = from > r ? from - r : 0, last = to + r < h ? to + r : h;

   memset(sums, 0, n * sizeof(uint32_t));
   for (unsigned y = first; y <= first + r && y < to; y++)
      if (y >= from)
      {
         const uint16_t *s = src + (size_t)y * n;
         for (unsigned i = 0; i < n; i++)
            sums[i] += s[i];
      }
   for (unsigned y = first; y < last; y++)
   {
      uint16_t *d = dst + (size_t)y * n;
      for (unsigned i = 0; i < n; i++)
         d[i] = BOX_DIVIDE(sums[i], m);
      if (y + r + 1 >= from && y + r + 1 < to)
      {
         const uint16_t *s = src + (size_t)(y + r + 1) * n;
         for (unsigned i = 0; i < n; i++)
            sums[i] += s[i];
      }
      if (y >= r && y - r >= from && y - r < to)
      {
         const uint16_t *s = src + (size_t)(y - r) * n;
         for (unsigned i = 0; i < n; i++)
            sums[i] -= s[i];
      }
   }
}

static void make_glow(px_fx_video *v, const struct pxc_frame *f, const px_fx_config *c,
      bool advance)
{
   const unsigned h = f->height;
   const size_t n = (size_t)GLOW_W * h * 3;
   const unsigned radius = c->glow == 1 ? 2 : c->glow == 2 ? 3 : 5;

   /* Shots stay in the trail and fade from it. */
   if (!c->trails)
      memset(v->trail, 0, n * sizeof(uint16_t));
   else if (advance)
   {
      for (size_t i = 0; i < n; i++)
         v->trail[i] = (uint16_t)((v->trail[i] * 215u) >> 8);
   }

   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         size_t i = (size_t)y * PXC_W + x;
         uint16_t *g = v->glow + ((size_t)y * GLOW_W + x * 2) * 3;
         /* An object's light, or the scenery's where a game module lit it. */
         uint32_t s = v->sprite[i] ? v->sprite[i] : v->lit ? v->light[i] : 0;
         /* Lines and digits stay crisp, unless the module lit them itself. */
         if (v->crisp[i] && v->sprite[i])
            s = 0;
         unsigned weight = v->energy[i] ? 448 : 256;
         unsigned rgb[3];

         if (!s)
         {
            memset(g, 0, 6 * sizeof(uint16_t));
            continue;
         }
         rgb[0] = (((s >> 16) & 0xFF) * weight) >> 8;
         rgb[1] = (((s >> 8) & 0xFF) * weight) >> 8;
         rgb[2] = ((s & 0xFF) * weight) >> 8;
         for (unsigned k = 0; k < 3; k++)
            g[k] = g[3 + k] = (uint16_t)rgb[k];

         if (c->trails && advance && v->energy[i])
         {
            uint16_t *t = v->trail + ((size_t)y * GLOW_W + x * 2) * 3;
            for (unsigned k = 0; k < 3; k++)
               if (t[k] < rgb[k])
                  t[k] = t[3 + k] = (uint16_t)rgb[k];
         }
      }

   /* Sparks light the glow where they are, and the trail behind them. */
   for (unsigned i = 0; i < v->spark_count; i++)
   {
      const spark *k = &v->sparks[i];
      unsigned x = (unsigned)(k->x >> 8), y = (unsigned)(k->y >> 8);
      unsigned bright = 640u * k->life / k->span;
      uint16_t *g, *t;
      if (x >= GLOW_W || y >= h)
         continue;
      g = v->glow + ((size_t)y * GLOW_W + x) * 3;
      t = v->trail + ((size_t)y * GLOW_W + x) * 3;
      for (unsigned ch = 0; ch < 3; ch++)
      {
         unsigned level = (((k->rgb >> (16 - 8 * ch)) & 0xFF) * bright) >> 8;
         if (g[ch] < level)
            g[ch] = (uint16_t)level;
         if (c->trails && advance && t[ch] < level / 2)
            t[ch] = (uint16_t)(level / 2);
      }
   }

   if (c->trails)
      for (size_t i = 0; i < n; i++)
      {
         unsigned s = v->glow[i] + v->trail[i];
         v->glow[i] = (uint16_t)(s > 1023 ? 1023 : s);
      }

   /* Three boxes after each other come close to a Gaussian. */
   for (unsigned pass = 0; pass < 3; pass++)
   {
      uint32_t sums[GLOW_W * 3];
      box_blur_h(v->glow, v->glow_tmp, GLOW_W, 0, h, radius);
      box_blur_v(v->glow_tmp, v->glow, GLOW_W, h, 0, h, radius, sums);
   }
}

/* How loud the two voices are in this frame, 0..256, following up fast and down slowly. */
static void follow_audio(px_fx_video *v, const struct pxc_frame *f)
{
   unsigned sum = 0, level;
   if (!f->audio.count || !f->audio.v0 || !f->audio.v1)
   {
      v->level = (v->level * 230u) >> 8;
      return;
   }
   for (uint32_t i = 0; i < f->audio.count; i++)
      sum += f->audio.v0[i] + f->audio.v1[i];
   /* Sound is a square wave between 0 and the volume, so its mean is about half of it. */
   level = sum * 256u / (f->audio.count * 15u);
   if (level > 256)
      level = 256;
   v->level = level > v->level ? (v->level + level * 3u) / 4u : (v->level * 7u + level) / 8u;
}

/* ---------------------------------------------------------------------------
 * Debug views
 * ------------------------------------------------------------------------- */

static const uint32_t layer_tint[PXC_LAYERS] = {
   0x101018, 0x2060C0, 0xF0F0F0, 0xF04040, 0xF0A020, 0x40D040, 0x20D0D0
};
static const uint8_t layer_tag[PXC_LAYERS] = { 0, PXC_PF, PXC_BL, PXC_P0, PXC_M0, PXC_P1, PXC_M1 };

static uint32_t view_pixel(const struct pxc_frame *f, size_t i, unsigned view)
{
   uint8_t tags = f->tags[i];
   if (view == PX_VIEW_LAYERS)
   {
      unsigned r = 0x10, g = 0x10, b = 0x18;
      if (tags & PXC_BLANK)
         return 0;
      for (unsigned l = 1; l < PXC_LAYERS; l++)
         if (tags & layer_tag[l])
         {
            r += (layer_tint[l] >> 16) & 0xFF;
            g += (layer_tint[l] >> 8) & 0xFF;
            b += layer_tint[l] & 0xFF;
         }
      return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
   }
   else
   {
      unsigned l = view - PX_VIEW_LAYER;
      if (l >= PXC_LAYERS || (tags & PXC_BLANK) || (l != PXC_L_BK && !(tags & layer_tag[l])))
         return 0x400040;
      return f->palette[f->color[l][i]] & 0xFFFFFFu;
   }
}

static void draw_views(px_fx_video *v, const struct pxc_frame *f, const px_fx_config *c,
      unsigned w, unsigned h)
{
   (void)h;
   for (unsigned y = 0; y < f->height; y++)
   {
      uint32_t *row = v->out + (size_t)y * c->sy * w;
      for (unsigned x = 0; x < PXC_W; x++)
      {
         uint32_t p = view_pixel(f, (size_t)y * PXC_W + x, c->view);
         for (unsigned u = 0; u < c->sx; u++)
            row[x * c->sx + u] = p;
      }
      for (unsigned s = 1; s < c->sy; s++)
         memcpy(row + (size_t)s * w, row, (size_t)w * sizeof(uint32_t));
   }
}

static void draw_boxes(px_fx_video *v, const px_fx_config *c, unsigned w, unsigned h)
{
   const px_objects *o = &v->objects;
   for (unsigned k = 0; k < o->count; k++)
   {
      const px_instance *in = &o->inst[k];
      uint32_t id = in->track * 2654435761u;
      uint32_t color = in->ghost ? 0xFFFFFFu
            : (0x808080u | ((id >> 8) & 0x7F7F7Fu));
      int x0 = in->x * (int)c->sx - 1, x1 = (in->x + in->w) * (int)c->sx;
      int y0 = in->y * (int)c->sy - 1, y1 = (in->y + in->h) * (int)c->sy;

      for (int x = x0; x <= x1; x++)
      {
         if (x < 0 || x >= (int)w || (in->ghost && ((x >> 1) & 1)))
            continue;
         if (y0 >= 0 && y0 < (int)h) v->out[(size_t)y0 * w + (size_t)x] = color;
         if (y1 >= 0 && y1 < (int)h) v->out[(size_t)y1 * w + (size_t)x] = color;
      }
      for (int y = y0; y <= y1; y++)
      {
         if (y < 0 || y >= (int)h || (in->ghost && ((y >> 1) & 1)))
            continue;
         if (x0 >= 0 && x0 < (int)w) v->out[(size_t)y * w + (size_t)x0] = color;
         if (x1 >= 0 && x1 < (int)w) v->out[(size_t)y * w + (size_t)x1] = color;
      }
   }
}

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static inline uint32_t scale_rgb(uint32_t rgb, unsigned f256)
{
   unsigned r = (((rgb >> 16) & 0xFF) * f256) >> 8;
   unsigned g = (((rgb >> 8) & 0xFF) * f256) >> 8;
   unsigned b = ((rgb & 0xFF) * f256) >> 8;
   return (r << 16) | (g << 8) | b;
}

static inline uint32_t add_rgb(uint32_t a, uint32_t b)
{
   unsigned r = ((a >> 16) & 0xFF) + ((b >> 16) & 0xFF);
   unsigned g = ((a >> 8) & 0xFF) + ((b >> 8) & 0xFF);
   unsigned bl = (a & 0xFF) + (b & 0xFF);
   return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (bl > 255 ? 255 : bl);
}

static inline uint32_t mix_rgb(uint32_t a, uint32_t b, unsigned t256)
{
   unsigned s = 256 - t256;
   unsigned r = (((a >> 16) & 0xFF) * s + ((b >> 16) & 0xFF) * t256) >> 8;
   unsigned g = (((a >> 8) & 0xFF) * s + ((b >> 8) & 0xFF) * t256) >> 8;
   unsigned bl = ((a & 0xFF) * s + (b & 0xFF) * t256) >> 8;
   return (r << 16) | (g << 8) | bl;
}

/* What is done to the background: darker towards the edges, and black lit a little, more so
 * in the middle. `dim` is 256 in the middle of the picture. */
static inline uint32_t treat_bk(uint32_t rgb, unsigned dim)
{
   unsigned r, g, b, dark, lift;
   rgb  = scale_rgb(rgb, dim);
   r    = (rgb >> 16) & 0xFF;
   g    = (rgb >> 8) & 0xFF;
   b    = rgb & 0xFF;
   dark = 255 - (r > g ? (r > b ? r : b) : (g > b ? g : b));
   lift = (dark * dim) >> 8;
   r   += (lift * 5) >> 8;
   g   += (lift * 7) >> 8;
   b   += (lift * 18) >> 8;
   return (r << 16) | (g << 8) | b;
}

/* The tables for the columns of a picture `w` wide, made when the size changes. */
static bool make_columns(px_fx_video *v, unsigned w, unsigned sx, unsigned shadow_x)
{
   if (v->col_w == w && v->col_sx == sx && v->col_vx)
      return true;

   free(v->col_vx);    free(v->col_shade); free(v->col_near);
   free(v->col_g0);    free(v->col_g1);    free(v->col_gf);
   free(v->keys);
   v->col_vx    = (uint16_t*)malloc(w * sizeof(uint16_t));
   v->col_shade = (uint16_t*)malloc(w * sizeof(uint16_t));
   v->col_near  = (uint16_t*)malloc(w * sizeof(uint16_t));
   v->col_g0    = (uint16_t*)malloc(w * sizeof(uint16_t));
   v->col_g1    = (uint16_t*)malloc(w * sizeof(uint16_t));
   v->col_gf    = (uint8_t*)malloc(w);
   v->keys      = (uint32_t*)malloc((size_t)w * BANDS_MAX * sizeof(uint32_t));
   if (!v->col_vx || !v->col_shade || !v->col_near || !v->col_g0 || !v->col_g1 || !v->col_gf
         || !v->keys)
   {
      v->col_w = 0;
      return false;
   }

   for (unsigned X = 0; X < w; X++)
   {
      /* Across the screen: 0 in the middle, 256 at the sides. */
      const int dx = ((int)(2 * X + 1) - (int)w) * 256 / (int)w;
      int gx = (int)((2 * X + 1) * 256 / sx) - 128;
      unsigned g0;

      v->col_vx[X]    = (uint16_t)((unsigned)(dx * dx) >> 8);
      v->col_shade[X] = X >= shadow_x ? (uint16_t)((X - shadow_x) / sx) : 0xFFFF;
      v->col_near[X]  = X >= shadow_x / 2 ? (uint16_t)((X - shadow_x / 2) / sx) : 0xFFFF;

      if (gx < 0)
         gx = 0;
      g0 = (unsigned)gx >> 8;
      if (g0 >= GLOW_W)
         g0 = GLOW_W - 1;
      v->col_g0[X] = (uint16_t)g0;
      v->col_g1[X] = (uint16_t)(g0 + 1 < GLOW_W ? g0 + 1 : g0);
      v->col_gf[X] = (uint8_t)((unsigned)gx & 0xFF);
   }
   v->col_w  = w;
   v->col_sx = sx;
   return true;
}

/* What the bands of a picture have in common. */
typedef struct
{
   px_fx_video *v;
   const struct pxc_frame *f;
   const px_fx_config *c;
   unsigned w, h;
   bool smooth;
   unsigned shadow_x, shadow_y;
   unsigned gain, gain_on_sprites;
   const uint32_t *backdrop;   /* a game's, or NULL */
   uint32_t flash;             /* what a flash adds to every pixel */
   /* Which corner of a captured pixel an output pixel is in: 1..4, 0 for the middle. */
   uint8_t corner[PX_FX_MAX_SY][PX_FX_MAX_SX];
} picture;

/* Band `index` of `count`: the rows of the capture it stands for, drawn. Bands share nothing
 * that is written to but the picture, of which each has its own rows. */
static void draw_band(void *ctx, unsigned index, unsigned count)
{
   static const uint32_t none[PXC_W];
   const picture *p = (const picture*)ctx;
   px_fx_video *v = p->v;
   const px_fx_config *c = p->c;
   const unsigned sx = c->sx, sy = c->sy, sh = p->f->height, w = p->w, h = p->h;
   const unsigned shadow_y = p->shadow_y, gain = p->gain, gain_on_sprites = p->gain_on_sprites;
   const uint32_t *backdrop = p->backdrop;
   const uint32_t flash = p->flash;
   const bool smooth = p->smooth;
   const unsigned y_from = sh * index / count, y_to = sh * (index + 1) / count;
   uint32_t *keys = v->keys + (size_t)index * w;
   uint16_t *glow_row = v->glow_row + (size_t)index * GLOW_W * 3;
   uint8_t glow_any[GLOW_W];
   uint32_t quad[5][PXC_W];
   uint8_t band[PXC_W], cornered[PXC_W];
   uint32_t lut[257], lut_rgb = 0;
   bool lut_valid = false;
   unsigned lut_misses = 0;

   for (unsigned y = y_from; y < y_to; y++)
   {
      const uint32_t *top = v->top + (size_t)y * PXC_W;
      const uint32_t *up  = y ? top - PXC_W : top;
      const uint32_t *dn  = y + 1 < sh ? top + PXC_W : top;
      const uint32_t *bk  = v->bk + (size_t)y * PXC_W;
      const uint32_t *bkd = y + 1 < sh ? bk + PXC_W : bk;
      bool bands = false;

      /* What each corner of each pixel of this row shows. */
      for (unsigned x = 0; x < PXC_W; x++)
      {
         const uint32_t px = top[x];
         quad[0][x] = quad[1][x] = quad[2][x] = quad[3][x] = quad[4][x] = px;
         if (smooth && KEY_CLS(px) != CLS_BLANK)
         {
            const uint32_t a = up[x], d = dn[x];
            const uint32_t l = x ? top[x - 1] : px, r = x + 1 < PXC_W ? top[x + 1] : px;
            /* Only where an object is on one side of the step. */
            if (KEY_CLS(px) == CLS_SPRITE || KEY_CLS(a) == CLS_SPRITE || KEY_CLS(d) == CLS_SPRITE
                  || KEY_CLS(l) == CLS_SPRITE || KEY_CLS(r) == CLS_SPRITE)
            {
               if (l == a && l != d && a != r && KEY_CLS(a) != CLS_BLANK) quad[1][x] = a;
               if (a == r && a != l && r != d && KEY_CLS(r) != CLS_BLANK) quad[2][x] = r;
               if (d == l && d != r && l != a && KEY_CLS(l) != CLS_BLANK) quad[3][x] = l;
               if (r == d && r != a && d != l && KEY_CLS(d) != CLS_BLANK) quad[4][x] = d;
            }
         }
         cornered[x] = quad[1][x] != px || quad[2][x] != px || quad[3][x] != px || quad[4][x] != px;
         /* Bands of the background's colour blend into the one below. */
         band[x] = c->background && KEY_CLS(px) == CLS_BK && KEY_CLS(dn[x]) == CLS_BK
               && bk[x] != bkd[x];
         bands = bands || band[x];
      }

      for (unsigned t = 0; t < sy; t++)
      {
         const unsigned Y = y * sy + t;
         uint32_t *out = v->out + (size_t)Y * w;
         const unsigned shadow_row = Y >= shadow_y ? (Y - shadow_y) / sy : sh;
         const unsigned near_row   = Y >= shadow_y / 2 ? (Y - shadow_y / 2) / sy : sh;
         const bool far_on  = c->shadow && shadow_row < sh && v->row_sprite[shadow_row];
         const bool near_on = c->shadow && near_row < sh && v->row_sprite[near_row];
         const uint32_t *shade = far_on ? v->sprite + (size_t)shadow_row * PXC_W : none;
         const uint32_t *shade_near = near_on ? v->sprite + (size_t)near_row * PXC_W : none;
         const bool shadows = far_on || near_on;
         /* Down the screen: 0 in the middle, 256 at the top and the bottom. */
         const int dy = ((int)(2 * Y + 1) - (int)h) * 256 / (int)h;
         const unsigned vy = (unsigned)(dy * dy) >> 8;
         const bool dark_row = c->scanlines && t + 1 == sy && sy > 1;
         bool glows = false;

         if (gain)
         {
            /* This row of the glow, between the two of its rows next to it. */
            int gy = (int)((2 * Y + 1) * 256 / (2 * sy)) - 128;
            unsigned g0, g1, fy;
            if (gy < 0)
               gy = 0;
            g0 = (unsigned)gy >> 8;
            fy = (unsigned)gy & 0xFF;
            if (g0 >= sh)
               g0 = sh - 1;
            g1 = g0 + 1 < sh ? g0 + 1 : g0;
            if (v->row_glow[g0] || v->row_glow[g1])
            {
               const uint16_t *a = v->glow + (size_t)g0 * GLOW_W * 3;
               const uint16_t *b = v->glow + (size_t)g1 * GLOW_W * 3;
               for (unsigned i = 0; i < GLOW_W * 3; i++)
                  glow_row[i] = (uint16_t)((a[i] * (256 - fy) + b[i] * fy) >> 8);
               for (unsigned i = 0; i < GLOW_W; i++)
                  glow_any[i] = (glow_row[i * 3] | glow_row[i * 3 + 1] | glow_row[i * 3 + 2]) != 0;
               glows = true;
            }
         }

         /* The row before the effects. */
         {
            const uint8_t *cr = p->corner[t];
            for (unsigned x = 0; x < PXC_W; x++)
            {
               uint32_t *k = keys + x * sx;
               if (!cornered[x])
               {
                  const uint32_t px = top[x];
                  for (unsigned u = 0; u < sx; u++)
                     k[u] = px;
               }
               else
                  for (unsigned u = 0; u < sx; u++)
                     k[u] = quad[cr[u]][x];
            }
         }
         if (bands)
         {
            const unsigned blend = t * 256 / sy;
            for (unsigned x = 0; x < PXC_W; x++)
            {
               uint32_t mixed;
               if (!band[x])
                  continue;
               mixed = KEY(CLS_BK, mix_rgb(bk[x], bkd[x], blend));
               for (unsigned u = 0; u < sx; u++)
                  if (KEY_CLS(keys[x * sx + u]) == CLS_BK)
                     keys[x * sx + u] = mixed;
            }
         }

         /* The colours, the background's treated. */
         if (c->background)
         {
            const uint16_t *vx = v->col_vx;
            for (unsigned X = 0; X < w; X++)
            {
               const uint32_t key = keys[X];
               uint32_t rgb = key & 0xFFFFFFu;
               if (KEY_CLS(key) == CLS_BK)
               {
                  const unsigned dim = 256 - (((vx[X] + vy) * 80u) >> 8);
                  if (lut_valid && rgb == lut_rgb)
                     rgb = lut[dim];
                  else
                  {
                     /* A table for the colour once it is seen to stay. */
                     if (++lut_misses >= 8)
                     {
                        for (unsigned d = 0; d <= 256; d++)
                           lut[d] = treat_bk(rgb, d);
                        lut_rgb    = rgb;
                        lut_valid  = true;
                        lut_misses = 0;
                     }
                     rgb = treat_bk(rgb, dim);
                  }
               }
               out[X] = rgb;
            }
         }
         else
            for (unsigned X = 0; X < w; X++)
               out[X] = keys[X] & 0xFFFFFFu;

         /* A game's backdrop shows where the background is dark. */
         if (backdrop)
         {
            const uint32_t *b = backdrop + (size_t)Y * w;
            for (unsigned X = 0; X < w; X++)
            {
               const uint32_t key = keys[X];
               if (KEY_CLS(key) == CLS_BK && !(key & 0xE0E0E0u))
                  out[X] = add_rgb(b[X], key & 0xFFFFFFu);
            }
         }

         /* Shadows fall on the background and the playfield. */
         if (shadows)
         {
            const uint16_t *far_x = v->col_shade, *near_x = v->col_near;
            for (unsigned X = 0; X < w; X++)
            {
               unsigned dark = 0;
               if (far_x[X] != 0xFFFF && shade[far_x[X]])
                  dark += 60;
               if (near_x[X] != 0xFFFF && shade_near[near_x[X]])
                  dark += 50;
               if (dark && KEY_CLS(keys[X]) < CLS_SPRITE)
                  out[X] = scale_rgb(out[X], 256 - dark);
            }
         }

         if (glows)
         {
            const uint16_t *c0 = v->col_g0, *c1 = v->col_g1;
            const uint8_t *cf = v->col_gf;
            for (unsigned X = 0; X < w; X++)
            {
               const unsigned g0 = c0[X], g1 = c1[X];
               if (glow_any[g0] | glow_any[g1])
               {
                  const unsigned cls = KEY_CLS(keys[X]);
                  const unsigned fx = cf[X], k = cls == CLS_SPRITE ? gain_on_sprites : gain;
                  const uint16_t *a = glow_row + g0 * 3, *b = glow_row + g1 * 3;
                  const uint32_t rgb = out[X];
                  unsigned r, g, bl;
                  if (cls == CLS_BLANK)
                     continue;
                  r  = ((rgb >> 16) & 0xFF) + ((((a[0] * (256 - fx) + b[0] * fx) >> 8) * k) >> 8);
                  g  = ((rgb >> 8) & 0xFF) + ((((a[1] * (256 - fx) + b[1] * fx) >> 8) * k) >> 8);
                  bl = (rgb & 0xFF) + ((((a[2] * (256 - fx) + b[2] * fx) >> 8) * k) >> 8);
                  out[X] = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8)
                        | (bl > 255 ? 255 : bl);
               }
            }
         }

         if (dark_row)
            for (unsigned X = 0; X < w; X++)
               if (KEY_CLS(keys[X]) != CLS_BLANK)
                  out[X] = scale_rgb(out[X], 176);

         if (flash)
            for (unsigned X = 0; X < w; X++)
               if (KEY_CLS(keys[X]) != CLS_BLANK)
                  out[X] = add_rgb(out[X], flash);
      }
   }
}

static void draw_picture(px_fx_video *v, const struct pxc_frame *f, const px_fx_config *c,
      unsigned w, unsigned h)
{
   static const unsigned glow_gain[4] = { 0, 150, 215, 280 };
   const unsigned sx = c->sx, sy = c->sy, sh = f->height;
   picture p;
   unsigned parts;

   p.v      = v;
   p.f      = f;
   p.c      = c;
   p.w      = w;
   p.h      = h;
   p.smooth = c->smooth && sx >= 4;
   /* The shadow falls to the lower right, further from objects than their stair steps. */
   p.shadow_x = sx * 3 / 4;
   p.shadow_y = sy * 3 / 2;
   p.gain     = glow_gain[c->glow & 3];
   if (c->reactive)
      p.gain += (p.gain * v->level) >> 9;
   p.gain_on_sprites = p.gain * 90u / 256u;
   p.backdrop = v->backdrop_on && v->backdrop && v->backdrop_w == w && v->backdrop_h == h
         ? v->backdrop : NULL;
   p.flash    = v->flash ? scale_rgb(v->flash_rgb, v->flash) : 0;

   if (!make_columns(v, w, sx, p.shadow_x))
      return;

   for (unsigned t = 0; t < sy; t++)
      for (unsigned u = 0; u < sx; u++)
      {
         unsigned fu = (2 * u + 1) * 256 / (2 * sx), fv = (2 * t + 1) * 256 / (2 * sy);
         unsigned gu = 256 - fu, gv = 256 - fv;
         p.corner[t][u] = !p.smooth ? 0 : fu + fv < 128 ? 1 : gu + fv < 128 ? 2
               : fu + gv < 128 ? 3 : gu + gv < 128 ? 4 : 0;
      }

   /* Rows of the capture with an object, and rows of the glow with any light. */
   for (unsigned y = 0; y < sh; y++)
   {
      const uint32_t *s = v->sprite + (size_t)y * PXC_W;
      const uint16_t *g = v->glow + (size_t)y * GLOW_W * 3;
      uint32_t any = 0;
      unsigned lit = 0;
      for (unsigned x = 0; x < PXC_W; x++)
         any |= s[x];
      v->row_sprite[y] = any != 0;
      if (p.gain)
         for (unsigned i = 0; i < GLOW_W * 3; i++)
            lit |= g[i];
      v->row_glow[y] = lit != 0;
   }

   parts = px_pool_parts(v->pool);
   if (parts > BANDS_MAX)
      parts = BANDS_MAX;
   /* Small pictures are drawn sooner than threads are woken. */
   if ((size_t)w * h < 200000 || parts > sh)
      parts = 1;
   px_pool_run(v->pool, draw_band, &p, parts);
}

/* The bright middle of every spark. */
static void draw_sparks(px_fx_video *v, const px_fx_config *c, unsigned w, unsigned h)
{
   const unsigned bw = c->sx > 1 ? c->sx / 2 : 1, bh = c->sy > 1 ? c->sy / 2 : 1;
   for (unsigned i = 0; i < v->spark_count; i++)
   {
      const spark *k = &v->sparks[i];
      const uint32_t rgb = scale_rgb(k->rgb, 80u + 176u * k->life / k->span);
      /* A pixel of the glow is half a captured pixel wide. */
      unsigned X = (unsigned)((int64_t)k->x * c->sx / 512), Y = (unsigned)((int64_t)k->y * c->sy / 256);
      for (unsigned t = 0; t < bh; t++)
         for (unsigned u = 0; u < bw; u++)
            if (X + u < w && Y + t < h)
            {
               uint32_t *o = v->out + (size_t)(Y + t) * w + X + u;
               /* Not on what has no picture, nor on what a game module keeps as the game
                * drew it (a cockpit): blank. */
               const size_t at = (size_t)((Y + t) / c->sy) * PXC_W + (X + u) / c->sx;
               if ((Y + t) / c->sy < v->height && KEY_CLS(v->top[at]) == CLS_BLANK)
                  continue;
               *o = add_rgb(*o, rgb);
            }
   }
}

/* Lets the game module at the frame. */
static void run_game(px_fx_video *v, const struct pxc_frame *f, const px_fx_config *c,
      const px_fx_extra *extra, unsigned w, unsigned h, bool advance)
{
   const size_t need = (size_t)w * h;
   px_scene s;

   memset(&s, 0, sizeof(s));
   if (need > v->backdrop_cap)
   {
      uint32_t *b = (uint32_t*)realloc(v->backdrop, need * sizeof(uint32_t));
      if (!b)
         return;
      v->backdrop     = b;
      v->backdrop_cap = need;
   }
   s.backdrop_stale = v->backdrop_w != w || v->backdrop_h != h;
   v->backdrop_w    = w;
   v->backdrop_h    = h;

   s.frame    = f;
   s.cfg      = c;
   s.objects  = &v->objects;
   s.ram      = extra->ram;
   s.ram_size = extra->ram_size;
   s.advance  = advance;
   s.w        = w;
   s.h        = h;
   s.sx       = c->sx;
   s.sy       = c->sy;
   s.top      = v->top;
   s.bk       = v->bk;
   s.sprite   = v->sprite;
   s.energy   = v->energy;
   s.crisp    = v->crisp;
   s.light    = v->light;
   s.backdrop = v->backdrop;
   s.video    = v;

   memset(v->light, 0, (size_t)PXC_W * f->height * sizeof(uint32_t));
   v->lit = true;
   extra->game->frame(extra->game_state, &s);
   /* The score and what else is read and not played with. */
   for (unsigned i = 0; i < v->objects.count; i++)
      if (v->objects.inst[i].role == PX_ROLE_HUD)
         px_scene_crisp(&s, &v->objects.inst[i], true);
   v->backdrop_on = s.backdrop_on;
}

const uint32_t *px_fx_video_render(px_fx_video *v, const struct pxc_frame *f,
      const px_fx_config *c, const px_fx_extra *extra, unsigned *w, unsigned *h)
{
   static const px_fx_extra nothing = { NULL, NULL, NULL, 0, true, NULL };
   uint64_t start = now_us();
   const bool plain = c->sx == 1;
   size_t need;
   bool advance;

   if (!v || !f || !f->tags || !f->winner || !f->aux || !f->palette || f->width != PXC_W
         || !f->height || f->height > PXC_MAX_H || !c->sx || c->sx > PX_FX_MAX_SX || !c->sy
         || c->sy > PX_FX_MAX_SY)
      return NULL;
   if (!extra)
      extra = &nothing;
   advance = extra->advance;

   px_fx_video_size(c, f->height, w, h);
   need = (size_t)*w * *h;
   if (need > v->out_cap)
   {
      uint32_t *out = (uint32_t*)realloc(v->out, need * sizeof(uint32_t));
      if (!out)
         return NULL;
      v->out     = out;
      v->out_cap = need;
   }

   if (f->height != v->height)
   {
      v->height = f->height;
      px_fx_video_reset(v);
   }

   if (c->view == PX_VIEW_LAYERS || c->view >= PX_VIEW_LAYER)
      draw_views(v, f, c, *w, *h);
   else if (plain && c->view == PX_VIEW_NORMAL)
   {
      /* The core's own frame. */
      for (size_t i = 0; i < need; i++)
         v->out[i] = f->palette[f->winner[i]] & 0xFFFFFFu;
   }
   else
   {
      const bool fuse = c->flicker && !plain;
      const bool game = c->game && extra->game && extra->game->frame && !plain;

      classify(v, f);
      if (c->bars && !plain)
         fill_bars(v, f);
      if (fuse || game || c->view == PX_VIEW_INSTANCES)
      {
         /* A frame drawn again has the objects it had. */
         if (advance)
         {
            const bool tells = game && extra->game->who;
            v->who_game        = extra->game;
            v->who_state       = extra->game_state;
            v->glance.frame    = f;
            v->glance.ram      = extra->ram;
            v->glance.ram_size = extra->ram_size;
            v->objects.who     = tells ? ask_who : NULL;
            v->objects.who_ctx = v;
            px_objects_update(&v->objects, f, fuse);
         }
         if (fuse)
            draw_ghosts(v, f);
      }
      if (advance)
         move_sparks(v, f->height);
      v->backdrop_on = false;
      v->lit = false;
      if (game)
         run_game(v, f, c, extra, *w, *h, advance);
      else
         v->spark_count = 0;

      if (plain)
      {
         px_fx_config none = *c;
         none.glow = 0;
         none.shadow = none.background = none.smooth = none.scanlines = false;
         draw_picture(v, f, &none, *w, *h);
      }
      else
      {
         if (advance)
            follow_audio(v, f);
         if (c->glow)
            make_glow(v, f, c, advance);
         draw_picture(v, f, c, *w, *h);
         draw_sparks(v, c, *w, *h);
      }
      if (c->view == PX_VIEW_INSTANCES)
         draw_boxes(v, c, *w, *h);
   }

   if (extra->panel)
      px_panel_draw(v->out, *w, *h, extra->panel);

   v->last_us = (unsigned)(now_us() - start);
   return v->out;
}
