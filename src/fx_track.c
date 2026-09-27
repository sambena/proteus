/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Objects in a captured Atari 2600 frame.
 *
 * The TIA has two players, two missiles and a ball, and games draw many more things than
 * that by moving them between scanlines and by showing them in turns from frame to frame.
 * An instance is one thing on the screen: consecutive rows of one object at one place. A
 * track is an instance followed over frames, which is what tells flicker (there, away,
 * there again) from an object that went away.
 */
#include "fx.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OPEN 8

void px_objects_init(px_objects *o)
{
   memset(o, 0, sizeof(*o));
   o->next_id = 1;
}

void px_objects_free(px_objects *o)
{
   free(o->bits);
   free(o->colors);
   memset(o, 0, sizeof(*o));
}

void px_objects_reset(px_objects *o)
{
   memset(o->tracks, 0, sizeof(o->tracks));
   o->count = 0;
}

static bool pool_reserve(px_objects *o, size_t rows)
{
   uint32_t *bits;
   uint8_t *colors;
   size_t cap;
   if (o->pool_used + rows <= o->pool_cap)
      return true;
   cap = (o->pool_used + rows) * 2 + 1024;
   if (!(bits = (uint32_t*)realloc(o->bits, cap * sizeof(*bits))))
      return false;
   o->bits = bits;
   if (!(colors = (uint8_t*)realloc(o->colors, cap)))
      return false;
   o->colors   = colors;
   o->pool_cap = cap;
   return true;
}

/* An instance being built. Its rows are at the pool's end only while it is the one that was
 * started last, so rows are collected here and copied to the pool when it is closed. */
typedef struct
{
   bool used;
   uint8_t cls, copy;
   int16_t x, y;
   uint16_t w;
   unsigned h;
   unsigned empty;                      /* rows without a pixel at the end */
   uint32_t bits[PX_OBJ_ROWS * 4];
   uint8_t  colors[PX_OBJ_ROWS * 4];
} open_instance;

#define OPEN_ROWS (PX_OBJ_ROWS * 4)

static void close_instance(px_objects *o, open_instance *b)
{
   px_instance *in;
   uint32_t hash = 2166136261u;

   if (!b->used)
      return;
   b->used = false;
   b->h   -= b->empty;
   if (!b->h || o->count >= PX_MAX_INSTANCES || !pool_reserve(o, b->h))
      return;

   in = &o->inst[o->count++];
   memset(in, 0, sizeof(*in));
   in->cls   = b->cls;
   in->copy  = b->copy;
   in->x     = b->x;
   in->y     = b->y;
   in->w     = b->w;
   in->h     = (uint16_t)b->h;
   in->color = b->colors[0];
   in->rows  = (uint32_t)o->pool_used;
   memcpy(o->bits + o->pool_used, b->bits, b->h * sizeof(uint32_t));
   memcpy(o->colors + o->pool_used, b->colors, b->h);
   o->pool_used += b->h;

   for (unsigned r = 0; r < b->h; r++)
   {
      hash ^= b->bits[r];
      hash *= 16777619u;
   }
   in->hash = hash;
}

static void start_instance(open_instance *b, uint8_t cls, uint8_t copy, int x, int y, unsigned w)
{
   b->used  = true;
   b->cls   = cls;
   b->copy  = copy;
   b->x     = (int16_t)x;
   b->y     = (int16_t)y;
   b->w     = (uint16_t)w;
   b->h     = 0;
   b->empty = 0;
}

static void add_row(open_instance *b, uint32_t bits, uint8_t color)
{
   if (b->h >= OPEN_ROWS)
      return;
   b->bits[b->h]   = bits;
   b->colors[b->h] = color;
   b->h++;
   b->empty = bits ? 0 : b->empty + 1;
}

/* A player's copy scans its eight pattern bits on every line whether they are set or not,
 * so its instances are cut where the pattern is empty for more than a line. */
static void find_players(px_objects *o, const struct pxc_frame *f, unsigned player)
{
   const uint8_t tag = player ? PXC_P1 : PXC_P0;
   const uint8_t cls = player ? PXC_L_P1 : PXC_L_P0;
   const uint8_t *color = f->color[cls];
   open_instance *open = (open_instance*)calloc(4, sizeof(*open));   /* by copy */

   if (!open)
      return;

   for (unsigned y = 0; y < f->height; y++)
   {
      const uint8_t *tags = f->tags + (size_t)y * PXC_W;
      const uint8_t *aux  = f->aux + (size_t)y * PXC_W;
      const uint8_t *col  = color + (size_t)y * PXC_W;
      bool scanned[4] = { false, false, false, false };
      unsigned x = 0;

      while (x < PXC_W)
      {
         unsigned copy = player ? PXC_AUX_P1_COPY(aux[x]) : PXC_AUX_P0_COPY(aux[x]);
         unsigned x0 = x, w;
         uint32_t bits = 0;
         uint8_t c = 0;
         open_instance *b;

         if (!copy || (tags[x] & PXC_BLANK))
         {
            x++;
            continue;
         }
         while (x < PXC_W && x - x0 < 32 && !(tags[x] & PXC_BLANK)
               && (player ? PXC_AUX_P1_COPY(aux[x]) : PXC_AUX_P0_COPY(aux[x])) == copy)
         {
            if (tags[x] & tag)
            {
               if (!bits)
                  c = col[x];
               bits |= 1u << (x - x0);
            }
            x++;
         }
         w = x - x0;
         b = &open[copy];

         /* The same copy twice on a line: the player was placed again within it. */
         if (scanned[copy])
            continue;
         scanned[copy] = true;

         if (b->used && (b->x != (int)x0 || b->w != w || b->empty > 1))
            close_instance(o, b);
         if (!b->used)
         {
            if (!bits)
               continue;
            start_instance(b, cls, (uint8_t)copy, (int)x0, (int)y, w);
         }
         add_row(b, bits, c);
      }

      for (unsigned copy = 1; copy < 4; copy++)
         if (open[copy].used && !scanned[copy])
            close_instance(o, &open[copy]);
   }
   for (unsigned copy = 1; copy < 4; copy++)
      close_instance(o, &open[copy]);
   free(open);
}

/* Missiles and the ball are only there where they are enabled: their instances are the runs
 * of pixels that touch from line to line. */
static void find_small(px_objects *o, const struct pxc_frame *f, uint8_t cls, uint8_t tag)
{
   const uint8_t *color = f->color[cls];
   open_instance *open = (open_instance*)calloc(MAX_OPEN, sizeof(*open));
   int last_x0[MAX_OPEN], last_x1[MAX_OPEN];

   if (!open)
      return;

   for (unsigned y = 0; y < f->height; y++)
   {
      const uint8_t *tags = f->tags + (size_t)y * PXC_W;
      bool grew[MAX_OPEN] = { false };
      unsigned x = 0;

      while (x < PXC_W)
      {
         unsigned x0 = x;
         int slot = -1;

         if (!(tags[x] & tag) || (tags[x] & PXC_BLANK))
         {
            x++;
            continue;
         }
         while (x < PXC_W && x - x0 < 32 && (tags[x] & tag) && !(tags[x] & PXC_BLANK))
            x++;

         for (int i = 0; i < MAX_OPEN && slot < 0; i++)
            if (open[i].used && !grew[i] && (int)x0 <= last_x1[i] + 1 && (int)x >= last_x0[i] - 1
                  && (int)x0 >= open[i].x && x - (unsigned)open[i].x <= 32)
               slot = i;

         if (slot < 0)
         {
            for (int i = 0; i < MAX_OPEN && slot < 0; i++)
               if (!open[i].used)
                  slot = i;
            if (slot < 0)
               continue;
            start_instance(&open[slot], cls, 0, (int)x0, (int)y, x - x0);
         }

         {
            open_instance *b = &open[slot];
            unsigned shift = x0 - (unsigned)b->x;
            uint32_t run = (x - x0 >= 32 ? 0xFFFFFFFFu : ((1u << (x - x0)) - 1u)) << shift;
            add_row(b, run, color[(size_t)y * PXC_W + x0]);
            if (x - (unsigned)b->x > b->w)
               b->w = (uint16_t)(x - (unsigned)b->x);
            last_x0[slot] = (int)x0;
            last_x1[slot] = (int)x;
            grew[slot]    = true;
         }
      }

      for (int i = 0; i < MAX_OPEN; i++)
         if (open[i].used && !grew[i])
            close_instance(o, &open[i]);
   }
   for (int i = 0; i < MAX_OPEN; i++)
      close_instance(o, &open[i]);
   free(open);
}

const px_obj_track *px_objects_track(const px_objects *o, uint32_t id)
{
   if (!o || !id)
      return NULL;
   for (unsigned t = 0; t < PX_MAX_OBJ_TRACKS; t++)
      if (o->tracks[t].id == id)
         return &o->tracks[t];
   return NULL;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static void match_tracks(px_objects *o, bool ghosts)
{
   unsigned found = o->count;

   for (unsigned t = 0; t < PX_MAX_OBJ_TRACKS; t++)
   {
      o->tracks[t].matched = false;
      o->tracks[t].seen  <<= 1;
   }

   for (unsigned i = 0; i < found; i++)
   {
      px_instance *in = &o->inst[i];
      px_obj_track *best = NULL;
      int best_cost = 1 << 30;
      unsigned rows;

      for (unsigned t = 0; t < PX_MAX_OBJ_TRACKS; t++)
      {
         px_obj_track *tr = &o->tracks[t];
         int dx, dy, cost;
         if (!tr->id || tr->matched || tr->last.cls != in->cls)
            continue;
         dx = in->x - tr->last.x;
         dy = in->y - tr->last.y;
         /* Far enough for a row of invaders that steps down, near enough to tell rows. */
         if (iabs(dx) > 10 || iabs(dy) > 12)
            continue;
         cost = iabs(dx) + iabs(dy) + (tr->last.hash != in->hash ? 4 : 0)
               + (tr->last.copy != in->copy ? 2 : 0) + (tr->last.color != in->color ? 2 : 0);
         if (cost < best_cost)
         {
            best_cost = cost;
            best      = tr;
         }
      }

      if (!best)
      {
         for (unsigned t = 0; t < PX_MAX_OBJ_TRACKS && !best; t++)
            if (!o->tracks[t].id)
               best = &o->tracks[t];
         if (!best)
            continue;
         memset(best, 0, sizeof(*best));
         best->id = o->next_id++;
         if (!o->next_id)
            o->next_id = 1;
         best->last = *in;
      }
      else
      {
         /* Back after being away, and it had been there before that: flicker. */
         if (best->missed && best->missed <= 4 && (best->seen & 0x3E))
         {
            best->flickers = true;
            if (best->missed > best->gap)
               best->gap = best->missed;
         }
         best->vx = (int16_t)(in->x - best->last.x);
         best->vy = (int16_t)(in->y - best->last.y);
      }

      best->matched = true;
      best->seen   |= 1;
      best->missed  = 0;
      if ((best->seen & 0xFF) == 0xFF)
      {
         best->flickers = false;
         best->gap      = 0;
      }

      in->track  = best->id;
      best->last = *in;
      rows = in->h < PX_OBJ_ROWS ? in->h : PX_OBJ_ROWS;
      memcpy(best->bits, o->bits + in->rows, rows * sizeof(uint32_t));
      memcpy(best->colors, o->colors + in->rows, rows);
      best->last.h = (uint16_t)rows;
   }

   for (unsigned t = 0; t < PX_MAX_OBJ_TRACKS; t++)
   {
      px_obj_track *tr = &o->tracks[t];
      if (!tr->id || tr->matched)
         continue;
      if (++tr->missed > 8)
      {
         tr->id = 0;
         continue;
      }
      if (ghosts && tr->flickers && tr->missed <= tr->gap && o->count < PX_MAX_INSTANCES
            && pool_reserve(o, tr->last.h))
      {
         px_instance *in = &o->inst[o->count++];
         *in       = tr->last;
         in->ghost = 1;
         in->track = tr->id;
         in->rows  = (uint32_t)o->pool_used;
         memcpy(o->bits + o->pool_used, tr->bits, in->h * sizeof(uint32_t));
         memcpy(o->colors + o->pool_used, tr->colors, in->h);
         o->pool_used += in->h;
      }
   }
}

void px_objects_update(px_objects *o, const struct pxc_frame *f, bool ghosts)
{
   o->count     = 0;
   o->pool_used = 0;
   o->frame++;
   if (!f || !f->tags || !f->aux || f->width != PXC_W || f->height > PXC_MAX_H)
      return;

   find_players(o, f, 0);
   find_players(o, f, 1);
   find_small(o, f, PXC_L_M0, PXC_M0);
   find_small(o, f, PXC_L_M1, PXC_M1);
   find_small(o, f, PXC_L_BL, PXC_BL);
   match_tracks(o, ghosts);
}
