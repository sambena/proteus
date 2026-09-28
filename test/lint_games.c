/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Holds the game modules to what docs/GAME_MODULES.md asks of them, without a game:
 *
 *   - a name, and the MD5 of at least one ROM, which no other module has
 *   - defaults for options that there are
 *   - options of its own named proteus_<game>_<what>, with a default among their values
 *   - a module does not fall over a frame without objects, memory or sound, nor over being
 *     reset, reconfigured and asked again
 *
 * The template is held to it too, so that it stays a fit beginning.
 */
#include "../src/kit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const px_game px_game_template;

static unsigned failures;
static const char *who;

static void fail(const char *what, const char *detail)
{
   printf("  FAIL %s: %s%s%s\n", who, what, detail ? ": " : "", detail ? detail : "");
   failures++;
}

static bool is_md5(const char *s)
{
   if (strlen(s) != 32)
      return false;
   for (unsigned i = 0; i < 32; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return false;
   return true;
}

static const char *const fx_keys[] = {
   "video", "scale", "view", "glow", "shadow", "background", "smooth", "flicker", "trails",
   "reactive", "scanlines", "bars", "game", "rumble", "audio", "width", "lowpass", "reverb", NULL
};

static void check_tables(const px_game *g, const px_game *const *all, unsigned count)
{
   char prefix[64] = "";

   if (!g->name || !*g->name)
      fail("no name", NULL);
   if (!g->md5 || !g->md5[0])
      fail("no ROM", NULL);
   for (const char *const *m = g->md5; m && *m; m++)
   {
      if (!is_md5(*m))
         fail("not an MD5 in lower case", *m);
      for (unsigned i = 0; i < count; i++)
         for (const char *const *n = all[i]->md5; all[i] != g && n && *n; n++)
            if (!strcmp(*m, *n))
               fail("a ROM another module has too", *m);
   }

   for (const char *const *p = g->fx; p && p[0]; p += 2)
   {
      bool known = false;
      if (!p[1])
      {
         fail("a default without a value", p[0]);
         break;
      }
      for (unsigned i = 0; fx_keys[i]; i++)
         known = known || !strcmp(fx_keys[i], p[0]);
      if (!known)
         fail("a default for an option there is not", p[0]);
   }

   for (const px_game_option *o = g->options; o && o->key; o++)
   {
      const char *second = strncmp(o->key, "proteus_", 8) ? NULL : strchr(o->key + 8, '_');
      unsigned values = 0;
      bool has_default = false;

      if (!second || second == o->key + 8 || !second[1] || !strncmp(o->key, PX_OPT_FX_PREFIX, sizeof(PX_OPT_FX_PREFIX) - 1))
      {
         fail("an option not named proteus_<game>_<what>", o->key);
         continue;
      }
      if (!*prefix)
         snprintf(prefix, sizeof(prefix), "%.*s", (int)(second - o->key + 1), o->key);
      else if (strncmp(o->key, prefix, strlen(prefix)))
         fail("an option with another game's letters", o->key);
      if (strlen(o->key) > 40)
         fail("an option's key is longer than 40", o->key);
      /* The panel shows name and value on a line of 52. */
      if (!o->desc || !*o->desc || strlen(o->desc) > 24)
         fail("an option's name is missing or longer than 24", o->key);
      if (!o->info || !*o->info)
         fail("an option without a description", o->key);

      for (const char *const *v = o->values; v && v[0]; v += 2, values++)
      {
         if (!v[1])
         {
            fail("a value without a label", o->key);
            break;
         }
         if (strlen(v[1]) > 24)
            fail("a label longer than 24", v[1]);
         has_default = has_default || (o->default_value && !strcmp(o->default_value, v[0]));
      }
      if (values < 2)
         fail("an option with fewer than two values", o->key);
      if (!has_default)
         fail("an option whose default is not one of its values", o->key);

      for (unsigned i = 0; i < count; i++)
         for (const px_game_option *p = all[i]->options; p && p->key; p++)
            if (p != o && !strcmp(p->key, o->key))
               fail("an option there is twice", o->key);
   }

   if (!g->frame && !g->sound)
      fail("neither picture nor sound", NULL);
   if ((g->create != NULL) != (g->destroy != NULL))
      fail("create without destroy, or the other way round", NULL);
   if ((g->frame || g->sound) && g->options && !g->configure)
      fail("options and no configure", NULL);
}

/* ---------------------------------------------------------------------------
 * A frame of nothing: background, and blanking at the top
 * ------------------------------------------------------------------------- */

#define ROWS 228

static uint8_t plane_tags[PXC_W * ROWS], plane_zero[PXC_W * ROWS];
static uint32_t palette[256];
static struct pxc_frame nothing;

static void make_nothing(void)
{
   memset(&nothing, 0, sizeof(nothing));
   for (unsigned i = 0; i < 256; i++)
      palette[i] = i * 0x010101u;
   for (unsigned i = 0; i < PXC_W * 20; i++)
      plane_tags[i] = PXC_BLANK;
   nothing.abi_version     = PXC_ABI_VERSION;
   nothing.struct_size     = sizeof(nothing);
   nothing.frame_serial    = 1;
   nothing.width           = PXC_W;
   nothing.height          = ROWS;
   nothing.scanlines_total = 262;
   nothing.flags           = PXC_ENABLE_VIDEO | PXC_ENABLE_WRITES;
   nothing.tags            = plane_tags;
   nothing.winner          = plane_zero;
   nothing.aux             = plane_zero;
   nothing.palette         = palette;
   for (unsigned l = 0; l < PXC_LAYERS; l++)
      nothing.color[l] = plane_zero;
}

static const char *no_option(const char *key)
{
   (void)key;
   return NULL;
}

/* Every option at its last value: the one furthest from the default. */
static const px_game *asked;
static const char *last_option(const char *key)
{
   for (const px_game_option *o = asked->options; o && o->key; o++)
      if (!strcmp(o->key, key))
      {
         const char *last = NULL;
         for (const char *const *v = o->values; v && v[0] && v[1]; v += 2)
            last = v[0];
         return last;
      }
   return NULL;
}

static void check_run(const px_game *g)
{
   px_fx_video *video = px_fx_video_new();
   px_synth *synth = px_synth_new();
   px_fx_config cfg;
   px_fx_extra extra;
   void *state = g->create ? g->create() : NULL;
   static const uint8_t ram[128];
   unsigned w = 0, h = 0;

   if (!video || !synth || (g->create && !state))
   {
      fail("no memory", NULL);
      goto out;
   }
   px_fx_config_read(&cfg, no_option, NULL);
   px_synth_set_rate(synth, 31440.0);
   asked = g;

   for (unsigned pass = 0; pass < 4; pass++)
   {
      /* Without memory and with, with the defaults and with every option at its other
       * end; and after a reset. */
      const bool with_ram = pass & 1;
      if (g->configure)
         g->configure(state, pass < 2 ? no_option : last_option);
      if (g->reset)
         g->reset(state);

      for (unsigned n = 0; n < 8; n++)
      {
         nothing.frame_serial++;
         memset(&extra, 0, sizeof(extra));
         extra.game       = g;
         extra.game_state = state;
         extra.ram        = with_ram ? ram : NULL;
         extra.ram_size   = with_ram ? sizeof(ram) : 0;
         extra.advance    = n != 3;   /* one frame is drawn again */
         if (!px_fx_video_render(video, &nothing, &cfg, &extra, &w, &h) || w != PXC_W * cfg.sx)
         {
            fail("the frame was not drawn", NULL);
            goto out;
         }
         if (g->sound)
         {
            px_sound snd;
            memset(&snd, 0, sizeof(snd));
            snd.frame    = n & 1 ? &nothing : NULL;
            snd.ram      = extra.ram;
            snd.ram_size = extra.ram_size;
            snd.objects  = n & 2 ? px_fx_video_objects(video) : NULL;
            snd.synth    = synth;
            snd.voice[0] = snd.voice[1] = 1.0f;
            g->sound(state, &snd);
            if (snd.voice[0] < 0.0f || snd.voice[0] > 1.0f || snd.voice[1] < 0.0f || snd.voice[1] > 1.0f)
               fail("a voice louder than 1 or softer than 0", NULL);
         }
      }
   }

out:
   if (g->destroy)
      g->destroy(state);
   px_synth_free(synth);
   px_fx_video_free(video);
}

int main(void)
{
   const px_game *all[64];
   unsigned count = 0;

   setvbuf(stdout, NULL, _IONBF, 0);
   for (unsigned i = 0; i < px_game_count() && count < 63; i++)
      all[count++] = px_game_at(i);
   all[count++] = &px_game_template;

   make_nothing();
   for (unsigned i = 0; i < count; i++)
   {
      const unsigned before = failures;
      unsigned options = 0;
      who = all[i]->name && *all[i]->name ? all[i]->name : "(a module without a name)";
      check_tables(all[i], all, count);
      check_run(all[i]);
      for (const px_game_option *o = all[i]->options; o && o->key; o++)
         options++;
      printf("  %s %-24s %u options, %s\n", failures == before ? "ok  " : "FAIL", who, options,
            all[i]->sound ? "picture and sound" : "picture");
      if (all[i] != &px_game_template && !px_game_find(all[i]->md5[0]))
      {
         fail("not found by its ROM", all[i]->md5[0]);
      }
   }

   printf("%u game modules and the template: %s\n", count - 1, failures ? "FAILED" : "PASSED");
   return failures ? 1 : 0;
}
