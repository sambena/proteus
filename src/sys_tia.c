/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Atari 2600 as a source of pictures (fx.h): its capture's tags, told as what the
 * effects draw from. The TIA's objects are found by fx_track.c. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")
#endif

#include "fx.h"

#include <string.h>
#include <strings.h>

#define CLS_BK     PX_CLS_BK
#define CLS_PF     PX_CLS_PF
#define CLS_SPRITE PX_CLS_SPRITE
#define CLS_BLANK  PX_CLS_BLANK
#define KEY(cls, rgb) PX_KEY(cls, rgb)
#define KEY_CLS(k)    PX_KEY_CLS(k)

#define FRAME(s) ((const struct pxc_frame*)(s)->capture)

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
static void fill_bars(uint32_t *top_plane, uint32_t *bk_plane, unsigned height)
{
   for (unsigned y = 0; y < height; y++)
   {
      uint32_t *top = top_plane + (size_t)y * PXC_W;
      uint32_t *bk  = bk_plane + (size_t)y * PXC_W;
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

static void classify(const px_source *s, uint32_t *top, uint32_t *bk, uint32_t *sprite,
      uint8_t *energy, bool tidy)
{
   static const uint8_t sprite_tag[5]   = { PXC_P0, PXC_M0, PXC_P1, PXC_M1, PXC_BL };
   static const uint8_t sprite_layer[5] = { PXC_L_P0, PXC_L_M0, PXC_L_P1, PXC_L_M1, PXC_L_BL };
   const struct pxc_frame *f = FRAME(s);
   const size_t n = (size_t)PXC_W * f->height;

   for (size_t i = 0; i < n; i++)
   {
      uint8_t tags = f->tags[i];
      unsigned layer;

      sprite[i] = 0;
      energy[i] = 0;
      if (tags & PXC_BLANK)
      {
         top[i] = KEY(CLS_BLANK, f->palette[f->winner[i]]);
         bk[i]  = 0;
         continue;
      }
      top[i] = KEY(top_of(tags, PXC_AUX_PRIORITY(f->aux[i]), &layer), f->palette[f->winner[i]]);
      bk[i]  = f->palette[f->color[PXC_L_BK][i]] & 0xFFFFFFu;
      if (tags & PXC_SPRITES)
      {
         for (unsigned k = 0; k < 5; k++)
            if (tags & sprite_tag[k])
            {
               sprite[i] = 0xFF000000u | (f->palette[f->color[sprite_layer[k]][i]] & 0xFFFFFFu);
               break;
            }
         energy[i] = (tags & (PXC_M0 | PXC_M1 | PXC_BL)) != 0;
      }
   }
   if (tidy)
      fill_bars(top, bk, f->height);
}

static uint32_t pixel(const px_source *s, size_t i)
{
   const struct pxc_frame *f = FRAME(s);
   return f->palette[f->winner[i]] & 0xFFFFFFu;
}

static const uint32_t layer_tint[PXC_LAYERS] = {
   0x101018, 0x2060C0, 0xF0F0F0, 0xF04040, 0xF0A020, 0x40D040, 0x20D0D0
};
static const uint8_t layer_tag[PXC_LAYERS] = { 0, PXC_PF, PXC_BL, PXC_P0, PXC_M0, PXC_P1, PXC_M1 };

static uint32_t view(const px_source *s, size_t i, unsigned which)
{
   const struct pxc_frame *f = FRAME(s);
   uint8_t tags = f->tags[i];
   if (which == PX_VIEW_LAYERS)
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
      unsigned l = which - PX_VIEW_LAYER;
      if (l >= PXC_LAYERS || (tags & PXC_BLANK) || (l != PXC_L_BK && !(tags & layer_tag[l])))
         return 0x400040;
      return f->palette[f->color[l][i]] & 0xFFFFFFu;
   }
}

/* How loud the two voices are. */
static int loudness(const px_source *s)
{
   const struct pxc_frame *f = FRAME(s);
   unsigned sum = 0, level;
   if (!f->audio.count || !f->audio.v0 || !f->audio.v1)
      return -1;
   for (uint32_t i = 0; i < f->audio.count; i++)
      sum += f->audio.v0[i] + f->audio.v1[i];
   /* Sound is a square wave between 0 and the volume, so its mean is about half of it. */
   level = sum * 256u / (f->audio.count * 15u);
   return level > 256 ? 256 : (int)level;
}

static void objects(const px_source *s, px_objects *o, bool ghosts)
{
   px_objects_update(o, FRAME(s), ghosts);
}

static uint32_t object_rgb(const px_source *s, uint8_t color)
{
   return FRAME(s)->palette[color] & 0xFFFFFFu;
}

void px_tia_source(px_source *s, const struct pxc_frame *f)
{
   memset(s, 0, sizeof(*s));
   s->height     = f ? f->height : 0;
   s->tia        = f;
   s->capture    = f;
   s->classify   = classify;
   s->pixel      = pixel;
   s->view       = view;
   s->loudness   = loudness;
   s->objects    = objects;
   s->object_rgb = object_rgb;
}

/* Stella gives the voices apart, left and right, with stereo sound on, and the picture as
 * the TIA made it with its filters, phosphor and cropping off. */
static const char *tia_hold(const char *key, bool video, bool audio)
{
   if (audio && !strcmp(key, "stella_stereo"))
      return "on";
   if (video)
   {
      if (!strcmp(key, "stella_filter"))         return "disabled";
      if (!strcmp(key, "stella_phosphor"))       return "off";
      if (!strcmp(key, "stella_crop_hoverscan")) return "disabled";
      if (!strcmp(key, "stella_crop_voverscan")) return "0";
   }
   return NULL;
}

static bool tia_source(px_source *s, const void *capture, unsigned w, unsigned h)
{
   const struct pxc_frame *f = (const struct pxc_frame*)capture;
   if (!f || f->struct_size < sizeof(*f) || (w && f->width != w) || (h && f->height != h) || !f->tags
         || !f->winner || !f->aux || !f->palette || f->width != PXC_W)
      return false;
   px_tia_source(s, f);
   return true;
}

static const px_system tia = {
   "stella", "2600", "stellapx", PXC_ABI_VERSION, tia_hold, tia_source
};

const px_system *px_system_for(const char *core)
{
   static const px_system *const systems[] = { &tia };
   for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); i++)
      if (core && !strncasecmp(core, systems[i]->core, strlen(systems[i]->core)))
         return systems[i];
   return NULL;
}
