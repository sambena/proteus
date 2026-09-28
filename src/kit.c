/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The kit: what game modules have in common (kit.h). */
#include "kit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Options
 * ------------------------------------------------------------------------- */

const char *const px_kit_toggle[] = { "enabled", "Enabled", "disabled", "Disabled", NULL };
const char *const px_kit_sounds[] = { "proteus", "Proteus's", "original", "The game's own", NULL };

bool px_kit_on(px_kit_get get, const char *key)
{
   const char *v = get ? get(key) : NULL;
   return !v || (strcmp(v, "disabled") && strcmp(v, "off") && strcmp(v, "original"));
}

unsigned px_kit_pick(px_kit_get get, const char *key, const char *const *values)
{
   const char *v = get ? get(key) : NULL;
   for (unsigned i = 0; v && values && values[i * 2]; i++)
      if (!strcmp(v, values[i * 2]))
         return i;
   return 0;
}

/* ---------------------------------------------------------------------------
 * Tags
 * ------------------------------------------------------------------------- */

void px_kit_tags_reset(px_kit_tags *t)
{
   memset(t, 0, sizeof(*t));
}

void px_kit_tags_begin(px_kit_tags *t)
{
   for (unsigned i = 0; i < PX_KIT_TAGS; i++)
      t->slot[i].kept = 0;
}

px_kit_tagged *px_kit_tags_find(px_kit_tags *t, const px_instance *in)
{
   if (!in->track)
      return NULL;
   for (unsigned i = 0; i < PX_KIT_TAGS; i++)
      if (t->slot[i].id == in->track)
         return &t->slot[i];
   return NULL;
}

px_kit_tagged *px_kit_tags_keep(px_kit_tags *t, const px_instance *in, unsigned tag_if_new)
{
   px_kit_tagged *slot = px_kit_tags_find(t, in);
   if (!slot && in->track)
      for (unsigned i = 0; i < PX_KIT_TAGS && !slot; i++)
         if (!t->slot[i].id)
         {
            slot      = &t->slot[i];
            slot->id  = in->track;
            slot->tag = (uint8_t)tag_if_new;
         }
   if (slot)
   {
      slot->x    = in->x;
      slot->y    = in->y;
      slot->kept = 1;
   }
   return slot;
}

unsigned px_kit_tags_gone(const px_kit_tags *t)
{
   unsigned n = 0;
   for (unsigned i = 0; i < PX_KIT_TAGS; i++)
      if (t->slot[i].id && !t->slot[i].kept)
         n++;
   return n;
}

void px_kit_tags_end(px_kit_tags *t)
{
   for (unsigned i = 0; i < PX_KIT_TAGS; i++)
      if (!t->slot[i].kept)
         t->slot[i].id = 0;
}

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static unsigned rows_of(const px_scene *s, unsigned to)
{
   return to < s->frame->height ? to : s->frame->height;
}

void px_kit_playfield(px_scene *s, unsigned from, unsigned to, uint32_t rgb)
{
   to = rows_of(s, to);
   for (size_t i = (size_t)from * PXC_W; i < (size_t)to * PXC_W; i++)
      if (PX_KEY_CLS(s->top[i]) == PX_CLS_PF)
         s->top[i] = PX_KEY(PX_CLS_PF, rgb);
}

void px_kit_background(px_scene *s, unsigned from, unsigned to, uint32_t rgb)
{
   to = rows_of(s, to);
   for (size_t i = (size_t)from * PXC_W; i < (size_t)to * PXC_W; i++)
   {
      if (PX_KEY_CLS(s->top[i]) == PX_CLS_BLANK)
         continue;
      s->bk[i] = rgb & 0xFFFFFFu;
      if (PX_KEY_CLS(s->top[i]) == PX_CLS_BK)
         s->top[i] = PX_KEY(PX_CLS_BK, rgb);
   }
}

unsigned px_kit_small_playfield(const px_scene *s, unsigned from, unsigned to, unsigned rows,
      uint8_t *small)
{
   const uint8_t *tags = s->frame->tags;
   unsigned marked = 0;

   to = rows_of(s, to);
   memset(small, 0, (size_t)PXC_W * s->frame->height);
   for (unsigned x = 0; x < PXC_W; x++)
      for (unsigned y = from; y < to; )
      {
         unsigned end = y;
         while (end < to && (tags[(size_t)end * PXC_W + x] & (PXC_PF | PXC_BLANK)) == PXC_PF)
            end++;
         if (end == y)
         {
            y++;
            continue;
         }
         /* What goes on beyond the rows looked at is not small for all that is known. */
         if (end - y <= rows && y > from && end < to)
            for (unsigned r = y; r < end; r++, marked++)
               small[(size_t)r * PXC_W + x] = 1;
         y = end;
      }
   return marked;
}

void px_kit_dots(px_scene *s, const uint8_t *small, unsigned from, unsigned to, bool own,
      uint32_t rgb, bool round)
{
   const struct pxc_frame *f = s->frame;

   to = rows_of(s, to);
   for (unsigned y = from; y < to; y++)
   {
      const uint8_t *row = small + (size_t)y * PXC_W;
      for (unsigned x = 0; x < PXC_W; )
      {
         unsigned end = x;
         uint32_t color;
         while (end < PXC_W && row[end])
            end++;
         if (end == x)
         {
            x++;
            continue;
         }
         color = own ? rgb & 0xFFFFFFu
               : f->palette[f->color[PXC_L_PF][(size_t)y * PXC_W + x]] & 0xFFFFFFu;

         for (unsigned u = x; u < end; u++)
         {
            const size_t i = (size_t)y * PXC_W + u;
            /* In the middle of what there is of it, or of each four pixels of it. */
            const unsigned len = end - x, cell = len == 8 ? 8 : 4, at = (u - x) % cell;
            const bool dot = at == cell / 2 - 1 || at == cell / 2;
            if (PX_KEY_CLS(s->top[i]) != PX_CLS_PF)
               continue;
            if (!round)
               s->top[i] = PX_KEY(PX_CLS_PF, color);
            else if (dot)
            {
               s->top[i]    = PX_KEY(PX_CLS_SPRITE, color);
               s->sprite[i] = 0xFF000000u | color;
            }
            else
               s->top[i] = PX_KEY(PX_CLS_BK, s->bk[i]);
         }
         x = end;
      }
   }
}

void px_kit_outline(px_scene *s, unsigned from, unsigned to, uint32_t edge, uint32_t inside,
      uint32_t light, const uint8_t *skip)
{
   const uint8_t *tags = s->frame->tags;
   const int h = (int)s->frame->height;

   to = rows_of(s, to);
   for (unsigned y = from; y < to; y++)
      for (unsigned x = 0; x < PXC_W; x++)
      {
         static const int step[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
         const size_t i = (size_t)y * PXC_W + x;
         bool within = true;
         if (!(tags[i] & PXC_PF) || (tags[i] & PXC_BLANK) || (skip && skip[i]))
            continue;
         for (unsigned d = 0; d < 4 && within; d++)
         {
            const int nx = (int)x + step[d][0], ny = (int)y + step[d][1];
            size_t n;
            /* The picture's sides end nothing: a wall goes on beyond them. */
            if (nx < 0 || nx >= PXC_W || ny < 0 || ny >= h)
               continue;
            n = (size_t)ny * PXC_W + (size_t)nx;
            within = (tags[n] & PXC_BLANK) || ((tags[n] & PXC_PF) && !(skip && skip[n]));
         }
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_PF)
            s->top[i] = PX_KEY(PX_CLS_PF, within ? inside : edge);
         if (light && !within)
            s->light[i] = 0xFF000000u | (light & 0xFFFFFFu);
      }
}

void px_kit_fill_holes(px_scene *s, const px_instance *in, uint32_t rgb)
{
   /* The object with a pixel of nothing around it. What of the nothing cannot be reached
    * from that rim is a hole. */
   enum { W = 34, H = PX_OBJ_ROWS + 2 };
   uint8_t open[H][W];
   uint16_t todo[H * W];
   unsigned count = 0;
   const px_objects *o = s->objects;
   const unsigned w = in->w < 32 ? in->w : 32, h = in->h < PX_OBJ_ROWS ? in->h : PX_OBJ_ROWS;

   memset(open, 0, sizeof(open));
   open[0][0] = 1;
   todo[count++] = 0;
   while (count)
   {
      static const int step[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
      const unsigned at = todo[--count], x = at % W, y = at / W;
      for (unsigned d = 0; d < 4; d++)
      {
         const int nx = (int)x + step[d][0], ny = (int)y + step[d][1];
         if (nx < 0 || ny < 0 || nx > (int)w + 1 || ny > (int)h + 1 || open[ny][nx])
            continue;
         if (nx >= 1 && nx <= (int)w && ny >= 1 && ny <= (int)h
               && ((o->bits[in->rows + (unsigned)ny - 1] >> (nx - 1)) & 1))
            continue;
         open[ny][nx] = 1;
         todo[count++] = (uint16_t)(ny * W + nx);
      }
   }

   rgb &= 0xFFFFFFu;
   for (unsigned r = 0; r < h; r++)
      for (unsigned b = 0; b < w; b++)
      {
         const int x = in->x + (int)b, y = in->y + (int)r;
         size_t i;
         if (open[r + 1][b + 1] || ((o->bits[in->rows + r] >> b) & 1))
            continue;
         if (x < 0 || x >= PXC_W || y < 0 || y >= (int)s->frame->height)
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_BLANK || PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE)
            continue;
         s->top[i]    = PX_KEY(PX_CLS_SPRITE, rgb);
         s->sprite[i] = 0xFF000000u | rgb;
      }
}

bool px_kit_canvas_fit(px_kit_canvas *c, const px_scene *s)
{
   uint32_t *p;
   if (c->pixels && c->w == s->w && c->h == s->h && !s->backdrop_stale)
      return false;
   p = (uint32_t*)realloc(c->pixels, (size_t)s->w * s->h * sizeof(uint32_t));
   if (!p)
   {
      px_kit_canvas_free(c);
      return false;
   }
   c->pixels = p;
   c->w      = s->w;
   c->h      = s->h;
   return true;
}

void px_kit_canvas_free(px_kit_canvas *c)
{
   free(c->pixels);
   c->pixels = NULL;
   c->w = c->h = 0;
}

void px_kit_repaint(px_scene *s, const px_instance *in, const uint32_t *map)
{
   const px_objects *o = s->objects;
   for (unsigned r = 0; r < in->h; r++)
   {
      const int y = in->y + (int)r;
      const uint32_t bits = o->bits[in->rows + r];
      const uint32_t rgb = map[o->colors[in->rows + r]];
      if (rgb == PX_KIT_KEEP || y < 0 || y >= (int)s->frame->height)
         continue;
      for (unsigned b = 0; b < 32 && bits >> b; b++)
      {
         const int x = in->x + (int)b;
         size_t i;
         if (!((bits >> b) & 1) || x < 0 || x >= PXC_W)
            continue;
         i = (size_t)y * PXC_W + (size_t)x;
         if (s->sprite[i])
            s->sprite[i] = 0xFF000000u | (rgb & 0xFFFFFFu);
         if (PX_KEY_CLS(s->top[i]) == PX_CLS_SPRITE)
            s->top[i] = PX_KEY(PX_CLS_SPRITE, rgb);
      }
   }
}

void px_kit_scroll_reset(px_kit_scroll *k)
{
   k->at    = -1;
   k->step  = 0;
   k->total = 0;
}

int px_kit_scroll_follow(px_kit_scroll *k, int value, unsigned period)
{
   int step = 0;
   if (value >= 0 && k->at >= 0 && period)
   {
      step = (value - k->at) % (int)period;
      if (step < 0)
         step += (int)period;
      if ((unsigned)step * 2 > period)
         step -= (int)period;
   }
   k->at     = (int16_t)(value < 0 ? -1 : value);
   k->step   = (int16_t)step;
   k->total += step;
   return step;
}

bool px_kit_texture_fit(px_kit_texture *t, const px_scene *s)
{
   uint8_t *p;
   if (t->shades && t->w == s->w && t->h == s->h)
      return false;
   p = (uint8_t*)realloc(t->shades, (size_t)s->w * s->h);
   if (!p)
   {
      px_kit_texture_free(t);
      return false;
   }
   t->shades = p;
   t->w      = s->w;
   t->h      = s->h;
   return true;
}

void px_kit_texture_free(px_kit_texture *t)
{
   free(t->shades);
   t->shades = NULL;
   t->w = t->h = 0;
}

/* Where column `x` of a picture is in a texture `size` wide that was moved by `by`. */
static unsigned rolled(unsigned x, int32_t by, unsigned size)
{
   int32_t at = ((int32_t)x - by) % (int32_t)size;
   return (unsigned)(at < 0 ? at + (int32_t)size : at);
}

void px_kit_canvas_roll(const px_kit_canvas *c, uint32_t *out, unsigned y, unsigned from,
      unsigned to, int32_t dx, int32_t dy)
{
   const uint32_t *row;
   unsigned at;

   if (!c->pixels || !c->w || !c->h)
      return;
   if (to > c->w)
      to = c->w;
   if (from >= to)
      return;
   row = c->pixels + (size_t)rolled(y, dy, c->h) * c->w;
   at  = rolled(from, dx, c->w);
   /* In stretches in which it does not go round. */
   while (from < to)
   {
      const unsigned n = to - from < c->w - at ? to - from : c->w - at;
      memcpy(out + from, row + at, n * sizeof(uint32_t));
      from += n;
      at    = at + n == c->w ? 0 : at + n;
   }
}

void px_kit_texture_show(const px_kit_texture *t, px_kit_canvas *c, const uint32_t *colors)
{
   if (!t->shades || !c->pixels || c->w != t->w || c->h != t->h)
      return;
   for (size_t i = 0; i < (size_t)t->w * t->h; i++)
      c->pixels[i] = colors[t->shades[i]];
}

void px_kit_texture_roll(const px_kit_texture *t, uint32_t *out, unsigned y, unsigned from,
      unsigned to, int32_t dx, int32_t dy, const uint32_t *colors)
{
   const uint8_t *row;
   unsigned at;

   if (!t->shades || !t->w || !t->h)
      return;
   if (to > t->w)
      to = t->w;
   if (from >= to)
      return;
   row = t->shades + (size_t)rolled(y, dy, t->h) * t->w;
   at  = rolled(from, dx, t->w);
   while (from < to)
   {
      const unsigned n = to - from < t->w - at ? to - from : t->w - at;
      for (unsigned i = 0; i < n; i++)
         out[from + i] = colors[row[at + i]];
      from += n;
      at    = at + n == t->w ? 0 : at + n;
   }
}

static unsigned chance_at(unsigned x, unsigned y, uint32_t seed)
{
   uint32_t n = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ seed * 0xC2B2AE3Du;
   n ^= n >> 15;
   n *= 0x2C1B3C6Du;
   n ^= n >> 12;
   n *= 0x297A2D39u;
   n ^= n >> 15;
   return n & 255;
}

/* Where in its cell, of 256, eased so that the cells do not show as squares. */
static unsigned eased(unsigned f)
{
   return (f * f * (768 - 2 * f)) >> 16;
}

void px_kit_texture_noise(px_kit_texture *t, unsigned across, unsigned down, uint32_t seed)
{
   const unsigned w = t->w, h = t->h;
   uint16_t *cell, *part, *line;

   if (!t->shades || !w || !h || !across || !down)
      return;
   /* For every column its cell and where in it; for a row, what the cells' corners come
    * to at its height. */
   cell = (uint16_t*)malloc(((size_t)w * 2 + across + 1) * sizeof(uint16_t));
   if (!cell)
      return;
   part = cell + w;
   line = part + w;
   for (unsigned x = 0; x < w; x++)
   {
      const unsigned u = (unsigned)((uint64_t)x * across * 256u / w);
      cell[x] = (uint16_t)(u >> 8);
      part[x] = (uint16_t)eased(u & 255);
   }

   for (unsigned y = 0; y < h; y++)
   {
      const unsigned v = (unsigned)((uint64_t)y * down * 256u / h);
      const unsigned cy = v >> 8, ny = (cy + 1) % down, fy = eased(v & 255);
      uint8_t *out = t->shades + (size_t)y * w;
      for (unsigned c = 0; c <= across; c++)
         line[c] = (uint16_t)((chance_at(c % across, cy, seed) * (256 - fy)
               + chance_at(c % across, ny, seed) * fy) >> 8);
      for (unsigned x = 0; x < w; x++)
         out[x] = (uint8_t)((line[cell[x]] * (256u - part[x]) + line[cell[x] + 1] * part[x]) >> 8);
   }
   free(cell);
}

void px_kit_shades(uint32_t colors[256], uint32_t dark, uint32_t mid, uint32_t light)
{
   for (unsigned i = 0; i < 256; i++)
      colors[i] = i < 128 ? px_rgb_mix(dark, mid, i * 2) : px_rgb_mix(mid, light, (i - 128) * 256 / 127);
}

/* ---------------------------------------------------------------------------
 * A road into the distance
 * ------------------------------------------------------------------------- */

#define ROW_SAMPLES 18

/* The background's colour along a row: that of three in four of its samples, or false. */
static bool row_color(const px_scene *s, unsigned y, uint32_t *rgb)
{
   const uint32_t *bk = s->bk + (size_t)y * PXC_W;
   uint32_t seen[ROW_SAMPLES];
   unsigned count[ROW_SAMPLES], kinds = 0, best = 0;

   for (unsigned k = 0; k < ROW_SAMPLES; k++)
   {
      const uint32_t c = bk[12 + k * 8] & 0xFFFFFFu;
      unsigned i = 0;
      while (i < kinds && seen[i] != c)
         i++;
      if (i == kinds)
      {
         seen[kinds]  = c;
         count[kinds] = 0;
         kinds++;
      }
      if (++count[i] > count[best])
         best = i;
   }
   *rgb = seen[best];
   return count[best] * 4 >= ROW_SAMPLES * 3;
}

bool px_kit_horizon_find(const px_scene *s, unsigned from, unsigned to, px_kit_horizon *h)
{
   uint32_t c;
   unsigned y, last_sky = from;

   memset(h, 0, sizeof(*h));
   if (!s->frame || !s->bk || to > s->frame->height)
      to = s->frame ? s->frame->height : 0;
   if (from + 32 > to || !row_color(s, from, &h->sky))
      return false;

   /* The sky ends where no row of its colour follows. */
   for (y = from; y < to; y++)
      if (row_color(s, y, &c) && c == h->sky)
         last_sky = y;
   h->top     = from;
   h->horizon = last_sky + 1;
   if (h->horizon - h->top < 16 || to - h->horizon < 16)
      return false;

   /* A band at the horizon, then the ground; or the ground from the horizon on. */
   for (y = h->horizon; y < to && !row_color(s, y, &h->band); y++)
      ;
   if (y >= to)
      return false;
   h->ground     = y;
   h->ground_rgb = h->band;
   for (; y < to; y++)
      if (row_color(s, y, &c) && c != h->band)
      {
         h->ground     = y;
         h->ground_rgb = c;
         break;
      }
   /* The ground goes on to the last row of its colour. */
   h->bottom = h->ground;
   for (y = h->ground; y < to; y++)
      if (row_color(s, y, &c) && c == h->ground_rgb)
         h->bottom = y + 1;
   if (h->bottom - h->ground < 16)
   {
      /* What looked like a band is the ground. */
      h->ground     = h->horizon;
      h->ground_rgb = h->band;
      for (y = h->horizon; y < to; y++)
         if (row_color(s, y, &c) && c == h->band)
            h->bottom = y + 1;
   }
   return h->bottom - h->ground >= 16 && h->ground_rgb != h->sky;
}

void px_kit_sky_shades(uint32_t colors[256], uint32_t sky)
{
   const unsigned r = (sky >> 16) & 0xFF, g = (sky >> 8) & 0xFF, b = sky & 0xFF;
   const unsigned light = (r * 77 + g * 150 + b * 29) >> 8;
   /* Above: deeper. At the horizon: the colour and the light of the air, which is as bright
    * as the sky is and a little warm. */
   const uint32_t zenith = px_rgb_add(px_rgb_scale(sky, 150), px_rgb_scale(0x000814, 256 - light));
   const uint32_t haze = px_rgb_add(px_rgb_scale(sky, 140), px_rgb_scale(0xFFF0DC, light * 3 / 4));
   px_kit_shades(colors, zenith, sky, px_rgb_mix(sky, haze, 190));
}

/* A run of pixels of an edge on a row, and what drew it. */
typedef struct
{
   int16_t a, b;
   uint8_t tags;
   uint8_t color;
} edge_run;

/* Carries a side's edge from the rows it is known on to those it is not: between two, along
 * the line from one to the other; beyond the last, as the last rows run. */
static void carry_edge(int16_t *at, const uint8_t *known, unsigned from, unsigned to, int off)
{
   int prev = -1;
   for (unsigned y = from; y < to; y++)
   {
      if (!known[y])
         continue;
      if (prev >= 0 && (int)y - prev > 1)
         for (int k = prev + 1; k < (int)y; k++)
            at[k] = (int16_t)(at[prev] + (at[y] - at[prev]) * (k - prev) / ((int)y - prev));
      prev = (int)y;
   }
   if (prev < 0)
   {
      for (unsigned y = from; y < to; y++)
         at[y] = (int16_t)off;
      return;
   }
   /* Beyond the known rows at either end: as the eight rows nearest run. */
   {
      unsigned first = from, last = (unsigned)prev, n;
      int slope;
      while (!known[first])
         first++;
      for (n = first + 8 < last ? first + 8 : last; n > first && !known[n]; n--)
         ;
      slope = n > first ? (at[n] - at[first]) * 256 / (int)(n - first) : 0;
      for (unsigned y = from; y < first; y++)
         at[y] = (int16_t)(at[first] - slope * (int)(first - y) / 256);
      for (n = last >= first + 8 ? last - 8 : first; n < last && !known[n]; n++)
         ;
      slope = n < last ? (at[last] - at[n]) * 256 / (int)(last - n) : 0;
      for (unsigned y = last + 1; y < to; y++)
      {
         const int v = at[last] + slope * (int)(y - last) / 256;
         at[y] = (int16_t)(v < -1000 ? -1000 : v > 1160 ? 1160 : v);
      }
   }
}

bool px_kit_road_find(px_kit_road *r, const px_scene *s, unsigned from, unsigned to, unsigned tags)
{
   const struct pxc_frame *f = s->frame;
   static const uint8_t bits[5] = { PXC_P0, PXC_M0, PXC_P1, PXC_M1, PXC_BL };
   int votes[5] = { 0 };
   edge_run first[PXC_MAX_H], last[PXC_MAX_H];
   uint8_t runs[PXC_MAX_H], known_l[PXC_MAX_H], known_r[PXC_MAX_H];
   bool any = false;

   memset(r->seen, 0, sizeof(r->seen));
   memset(r->edge, 0, sizeof(r->edge));
   if (!f || !f->tags || !f->winner)
      return false;
   if (to > f->height)
      to = f->height;
   if (from >= to)
      return false;
   memset(runs, 0, sizeof(runs));
   memset(known_l, 0, sizeof(known_l));
   memset(known_r, 0, sizeof(known_r));

   /* The runs of edge on every row: the first and the last. */
   for (unsigned y = from; y < to; y++)
   {
      const uint8_t *t = f->tags + (size_t)y * PXC_W;
      unsigned x = 0;
      while (x < PXC_W)
      {
         edge_run run;
         if (!(t[x] & tags) || (t[x] & PXC_BLANK))
         {
            x++;
            continue;
         }
         run.a = run.b = (int16_t)x;
         run.tags  = 0;
         run.color = f->winner[(size_t)y * PXC_W + x];
         while (x < PXC_W && (t[x] & tags))
         {
            run.tags |= (uint8_t)(t[x] & tags);
            run.b = (int16_t)x++;
         }
         if (!runs[y])
            first[y] = run;
         last[y] = run;
         runs[y]++;
      }
      /* Which object draws which edge: from rows that show both, drawn by one each. */
      if (runs[y] >= 2 && first[y].tags != last[y].tags)
         for (unsigned k = 0; k < 5; k++)
         {
            if (first[y].tags == bits[k]) votes[k]++;
            if (last[y].tags == bits[k])  votes[k]--;
         }
   }

   for (unsigned y = from; y < to; y++)
   {
      if (runs[y] >= 2)
      {
         r->left[y]  = first[y].b;
         r->right[y] = last[y].a;
         known_l[y] = known_r[y] = 1;
      }
      else if (runs[y] == 1)
      {
         int side = 0;
         for (unsigned k = 0; k < 5; k++)
            if (first[y].tags & bits[k])
               side += votes[k];
         if (side > 0)
         {
            r->left[y] = first[y].b;
            known_l[y] = 1;
         }
         else if (side < 0)
         {
            r->right[y] = first[y].a;
            known_r[y] = 1;
         }
      }
      if (runs[y])
      {
         r->edge[y] = first[y].color;
         any = true;
      }
      else if (y > from)
         r->edge[y] = r->edge[y - 1];
   }
   if (!any)
      return false;

   carry_edge(r->left, known_l, from, to, -1000);
   carry_edge(r->right, known_r, from, to, 1160);

   /* A lone run that no object told the side of: the side it is nearer to. */
   {
      bool more = false;
      for (unsigned y = from; y < to; y++)
         if (runs[y] == 1 && !known_l[y] && !known_r[y])
         {
            const int m = (first[y].a + first[y].b) / 2;
            if (abs(m - r->left[y]) <= abs(m - r->right[y]))
            {
               r->left[y] = first[y].b;
               known_l[y] = 1;
            }
            else
            {
               r->right[y] = first[y].a;
               known_r[y] = 1;
            }
            more = true;
         }
      if (more)
      {
         carry_edge(r->left, known_l, from, to, -1000);
         carry_edge(r->right, known_r, from, to, 1160);
      }
   }
   for (unsigned y = from; y < to; y++)
      r->seen[y] = (uint8_t)(known_l[y] | known_r[y] << 1);
   return true;
}

/* ---------------------------------------------------------------------------
 * Sound
 * ------------------------------------------------------------------------- */

#define REG_AUDC0 0x15
#define REG_AUDV1 0x1A

void px_kit_tia_reset(px_kit_tia *t)
{
   memset(t, 0, sizeof(*t));
}

void px_kit_tia_hear(px_kit_tia *t, const struct pxc_frame *f)
{
   memcpy(t->was_wave, t->wave, sizeof(t->wave));
   memcpy(t->was_pitch, t->pitch, sizeof(t->pitch));
   memcpy(t->was_volume, t->volume, sizeof(t->volume));

   for (uint32_t i = 0; f && i < f->write_count; i++)
   {
      const struct pxc_regwrite *w = &f->writes[i];
      if (w->reg < REG_AUDC0 || w->reg > REG_AUDV1)
         continue;
      switch ((w->reg - REG_AUDC0) / 2)
      {
         case 0:  t->wave[(w->reg - REG_AUDC0) & 1]   = w->value & 0x0F; break;
         case 1:  t->pitch[(w->reg - REG_AUDC0) & 1]  = w->value & 0x1F; break;
         default: t->volume[(w->reg - REG_AUDC0) & 1] = w->value & 0x0F; break;
      }
   }
}

float px_kit_tia_hz(unsigned wave, unsigned pitch)
{
   /* The TIA counts two clocks a line: 31399.5 of them a second. A waveform divides that
    * by the length of its pattern, and the pitch divides it again. */
   static const uint16_t pattern[16] = {
      0, 15, 465, 465, 2, 2, 31, 31, 0, 31, 31, 0, 6, 6, 93, 93
   };
   const unsigned n = pattern[wave & 15];
   return n ? 31399.5f / (float)(n * ((pitch & 31) + 1)) : 0.0f;
}

float px_kit_tune(float hz)
{
   if (hz <= 0.0f)
      return 0.0f;
   return 440.0f * powf(2.0f, roundf(12.0f * log2f(hz / 440.0f)) / 12.0f);
}

float px_kit_pan(int column)
{
   return column < 0 ? 0.0f : ((float)column - 80.0f) / 80.0f * 0.85f;
}

void px_kit_play(px_sound *s, const px_tone *tones, unsigned count, float pan)
{
   for (unsigned i = 0; i < count; i++)
      px_synth_play(s->synth, &tones[i], pan, 1.0f);
}
