/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"

#define MAX_VALUES   RETRO_NUM_CORE_OPTION_VALUES_MAX
#define MAX_SUBSONGS 32

typedef struct
{
   char *value;
   char *label;
} opt_value;

typedef struct
{
   char *key, *desc, *desc_categorized, *info, *info_categorized, *category, *default_value;
   opt_value values[MAX_VALUES];
   unsigned value_count;
} opt_def;

typedef struct
{
   char *key, *desc, *info;
} opt_category;

typedef struct
{
   opt_def *defs;
   unsigned def_count;
   opt_category *cats;
   unsigned cat_count;
} opt_set;

static retro_environment_t fe_env;
static opt_set inner_set;
static bool inner_declared;
static opt_set own_set;

/* Buffers handed to the frontend; they stay valid until the next publish. */
static struct retro_core_option_v2_category *out_cats;
static struct retro_core_option_v2_definition *out_v2;
static struct retro_core_option_definition *out_v1;
static struct retro_variable *out_v0;
static char **out_v0_strings;
static unsigned out_v0_count;

static char *dup_str(const char *s)
{
   char *d;
   size_t n;
   if (!s)
      return NULL;
   n = strlen(s) + 1;
   if ((d = (char*)malloc(n)))
      memcpy(d, s, n);
   return d;
}

static opt_def *add_def(opt_set *set)
{
   opt_def *defs = (opt_def*)realloc(set->defs, (set->def_count + 1) * sizeof(*defs));
   if (!defs)
      return NULL;
   set->defs = defs;
   memset(&defs[set->def_count], 0, sizeof(*defs));
   return &defs[set->def_count++];
}

static void add_value(opt_def *d, const char *value, const char *label)
{
   if (d->value_count >= MAX_VALUES - 1 || !value)
      return;
   d->values[d->value_count].value = dup_str(value);
   d->values[d->value_count].label = dup_str(label);
   d->value_count++;
}

static void add_category(opt_set *set, const char *key, const char *desc, const char *info)
{
   opt_category *cats = (opt_category*)realloc(set->cats, (set->cat_count + 1) * sizeof(*cats));
   if (!cats)
      return;
   set->cats = cats;
   cats[set->cat_count].key  = dup_str(key);
   cats[set->cat_count].desc = dup_str(desc);
   cats[set->cat_count].info = dup_str(info);
   set->cat_count++;
}

static void clear_set(opt_set *set)
{
   for (unsigned i = 0; i < set->def_count; i++)
   {
      opt_def *d = &set->defs[i];
      free(d->key); free(d->desc); free(d->desc_categorized); free(d->info);
      free(d->info_categorized); free(d->category); free(d->default_value);
      for (unsigned v = 0; v < d->value_count; v++)
      {
         free(d->values[v].value);
         free(d->values[v].label);
      }
   }
   for (unsigned i = 0; i < set->cat_count; i++)
   {
      free(set->cats[i].key); free(set->cats[i].desc); free(set->cats[i].info);
   }
   free(set->defs);
   free(set->cats);
   memset(set, 0, sizeof(*set));
}

/* ---------------------------------------------------------------------------
 * Copying the inner core's declarations (any of the libretro option APIs)
 * ------------------------------------------------------------------------- */

static void copy_v2(const struct retro_core_options_v2 *o)
{
   if (!o)
      return;
   for (const struct retro_core_option_v2_category *c = o->categories; c && c->key; c++)
      add_category(&inner_set, c->key, c->desc, c->info);
   for (const struct retro_core_option_v2_definition *s = o->definitions; s && s->key; s++)
   {
      opt_def *d = add_def(&inner_set);
      if (!d)
         return;
      d->key              = dup_str(s->key);
      d->desc             = dup_str(s->desc);
      d->desc_categorized = dup_str(s->desc_categorized);
      d->info             = dup_str(s->info);
      d->info_categorized = dup_str(s->info_categorized);
      d->category         = dup_str(s->category_key);
      d->default_value    = dup_str(s->default_value);
      for (unsigned v = 0; v < MAX_VALUES && s->values[v].value; v++)
         add_value(d, s->values[v].value, s->values[v].label);
   }
}

static void copy_v1(const struct retro_core_option_definition *s)
{
   for (; s && s->key; s++)
   {
      opt_def *d = add_def(&inner_set);
      if (!d)
         return;
      d->key           = dup_str(s->key);
      d->desc          = dup_str(s->desc);
      d->info          = dup_str(s->info);
      d->default_value = dup_str(s->default_value);
      for (unsigned v = 0; v < MAX_VALUES && s->values[v].value; v++)
         add_value(d, s->values[v].value, s->values[v].label);
   }
}

/* Legacy "Description; first|second|third", where the first value is the default. */
static void copy_v0(const struct retro_variable *s)
{
   for (; s && s->key; s++)
   {
      const char *semi = s->value ? strstr(s->value, "; ") : NULL;
      char *list, *save = NULL, *tok;
      opt_def *d;

      if (!semi || !(d = add_def(&inner_set)))
         continue;
      d->key  = dup_str(s->key);
      d->desc = (char*)malloc((size_t)(semi - s->value) + 1);
      if (d->desc)
      {
         memcpy(d->desc, s->value, (size_t)(semi - s->value));
         d->desc[semi - s->value] = '\0';
      }
      list = dup_str(semi + 2);
      for (tok = list ? strtok_r(list, "|", &save) : NULL; tok; tok = strtok_r(NULL, "|", &save))
         add_value(d, tok, NULL);
      free(list);
      if (d->value_count)
         d->default_value = dup_str(d->values[0].value);
   }
}

bool px_options_intercept(unsigned cmd, void *data, bool *result)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_SET_VARIABLES:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
         break;
      default:
         return false;
   }

   clear_set(&inner_set);
   inner_declared = true;

   /* Translations are dropped: the merged options are published in English. */
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         copy_v0((const struct retro_variable*)data);
         break;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
         copy_v1((const struct retro_core_option_definition*)data);
         break;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
         if (data)
            copy_v1(((const struct retro_core_options_intl*)data)->us);
         break;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
         copy_v2((const struct retro_core_options_v2*)data);
         break;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
         if (data)
            copy_v2(((const struct retro_core_options_v2_intl*)data)->us);
         break;
   }

   *result = px_options_publish();
   return true;
}

bool px_options_inner_declared(void)
{
   return inner_declared;
}

/* ---------------------------------------------------------------------------
 * Proteus's own options
 * ------------------------------------------------------------------------- */

static opt_def *own_def(const char *key, const char *desc, const char *desc_categorized,
      const char *info, const char *default_value)
{
   char full_desc[256];
   opt_def *d = add_def(&own_set);
   if (!d)
      return NULL;
   snprintf(full_desc, sizeof(full_desc), "Proteus: %s", desc);
   d->key              = dup_str(key);
   d->desc             = dup_str(full_desc);
   d->desc_categorized = dup_str(desc_categorized ? desc_categorized : desc);
   d->info             = dup_str(info);
   d->category         = dup_str("proteus");
   d->default_value    = dup_str(default_value);
   return d;
}

static void add_percent_values(opt_def *d)
{
   if (!d)
      return;
   add_value(d, PX_OPT_PROFILE, "Profile default");
   for (unsigned pct = 0; pct <= 200; pct += 10)
   {
      char value[8], label[8];
      snprintf(value, sizeof(value), "%u", pct);
      snprintf(label, sizeof(label), "%u%%", pct);
      add_value(d, value, label);
   }
}

typedef struct
{
   char value[PX_PATH_MAX + 8];
   char label[256];
} music_entry;

typedef struct
{
   const char *dir;
   char **names;
   unsigned count;
} dir_listing;

static void collect_name(const char *name, void *userdata)
{
   dir_listing *l = (dir_listing*)userdata;
   char **names;
   if (!px_source_supported(name))
      return;
   if (!(names = (char**)realloc(l->names, (l->count + 1) * sizeof(*names))))
      return;
   l->names = names;
   l->names[l->count++] = dup_str(name);
}

static int compare_names(const void *a, const void *b)
{
   return strcmp(*(char* const*)a, *(char* const*)b);
}

/* Lists playable files from the profile's [library] folders and the folders its
 * tracks come from, as option values relative to the profile. */
static unsigned scan_music(const px_profile *p, music_entry *out, unsigned max)
{
   const char *dirs[PX_MAX_LIBRARY + PX_MAX_TRACKS];
   char profile_dir[PX_PATH_MAX];
   char (*track_dirs)[PX_PATH_MAX] = malloc(PX_MAX_TRACKS * sizeof(*track_dirs));
   unsigned dir_count = 0, count = 0;
   size_t prefix;

   if (!track_dirs)
      return 0;
   px_path_dir(p->path, profile_dir, sizeof(profile_dir));
   prefix = strlen(profile_dir);

   for (unsigned i = 0; i < p->library_count; i++)
      dirs[dir_count++] = p->library[i];
   for (unsigned i = 0; i < p->track_count; i++)
   {
      if (p->tracks[i].action != PX_ACTION_FILE)
         continue;
      px_path_dir(p->tracks[i].path, track_dirs[i], PX_PATH_MAX);
      dirs[dir_count++] = track_dirs[i];
   }

   for (unsigned i = 0; i < dir_count && count < max; i++)
   {
      dir_listing l = { dirs[i], NULL, 0 };
      bool seen = false;
      for (unsigned j = 0; j < i; j++)
         seen = seen || !strcmp(dirs[i], dirs[j]);
      if (seen || !px_list_files(dirs[i], collect_name, &l))
         continue;
      qsort(l.names, l.count, sizeof(*l.names), compare_names);

      /* Counting a file's songs opens it, so a full list stops looking. */
      for (unsigned n = 0; n < l.count && count < max; n++)
      {
         char full[PX_PATH_MAX];
         const char *rel;
         unsigned songs;

         px_path_join(dirs[i], l.names[n], full, sizeof(full));
         rel = (!strncmp(full, profile_dir, prefix) && (full[prefix] == '/' || full[prefix] == '\\'))
               ? full + prefix + 1 : full;
         songs = px_source_song_count(full);
         if (songs > MAX_SUBSONGS)
            songs = MAX_SUBSONGS;

         for (unsigned s = 0; s < songs && count < max; s++)
         {
            if (songs == 1)
            {
               snprintf(out[count].value, sizeof(out[count].value), "%s", rel);
               snprintf(out[count].label, sizeof(out[count].label), "%s", l.names[n]);
            }
            else
            {
               snprintf(out[count].value, sizeof(out[count].value), "%s#%u", rel, s + 1);
               snprintf(out[count].label, sizeof(out[count].label), "%s #%u", l.names[n], s + 1);
            }
            count++;
         }
      }
      for (unsigned n = 0; n < l.count; n++)
         free(l.names[n]);
      free(l.names);
   }
   free(track_dirs);
   return count;
}

static void track_label(const px_track *t, char *out, size_t n)
{
   const char *name;
   switch (t->action)
   {
      case PX_ACTION_ORIGINAL: snprintf(out, n, "original music"); return;
      case PX_ACTION_SILENCE:  snprintf(out, n, "silence"); return;
      case PX_ACTION_KEEP:     snprintf(out, n, "keep playing"); return;
      case PX_ACTION_FILE:     break;
   }
   name = strrchr(t->path, '/');
   if (!name || strrchr(t->path, '\\') > name)
      name = strrchr(t->path, '\\');
   name = name ? name + 1 : t->path;
   if (t->subtrack)
      snprintf(out, n, "%s #%u", name, t->subtrack + 1);
   else
      snprintf(out, n, "%s", name);
}

static void build_own(const px_profile *p)
{
   opt_def *d;

   clear_set(&own_set);
   add_category(&own_set, "proteus", "Proteus Retune",
         "Replace this game's music while keeping its sound effects.");

   if ((d = own_def(PX_OPT_ENABLED, "Music replacement", NULL,
            "Swap the game's music using its Proteus profile. Games without a profile are unaffected.",
            "enabled")))
   {
      add_value(d, "enabled", "Enabled");
      add_value(d, "disabled", "Disabled");
   }
   d = own_def(PX_OPT_MUSIC_VOLUME, "Replacement music volume", NULL,
         "Volume of the replacement music.", PX_OPT_PROFILE);
   add_percent_values(d);
   d = own_def(PX_OPT_GAME_VOLUME, "Game audio volume", NULL,
         "Volume of everything the game itself plays, including sound effects.", PX_OPT_PROFILE);
   add_percent_values(d);
   if ((d = own_def(PX_OPT_CROSSFADE, "Crossfade", NULL,
            "How long songs fade into each other.", PX_OPT_PROFILE)))
   {
      static const unsigned ms[] = { 100, 250, 400, 750, 1000, 2000 };
      add_value(d, PX_OPT_PROFILE, "Profile default");
      add_value(d, "0", "Off");
      for (unsigned i = 0; i < sizeof(ms) / sizeof(ms[0]); i++)
      {
         char value[8], label[16];
         snprintf(value, sizeof(value), "%u", ms[i]);
         snprintf(label, sizeof(label), "%u ms", ms[i]);
         add_value(d, value, label);
      }
   }
   if ((d = own_def(PX_OPT_NOTIFY, "Song change notifications", NULL,
            "Show the song value on screen whenever the game changes music. Useful for building profiles.",
            PX_OPT_PROFILE)))
   {
      add_value(d, PX_OPT_PROFILE, "Profile default");
      add_value(d, "enabled", "Enabled");
      add_value(d, "disabled", "Disabled");
   }

   if (p && p->loaded && p->track_count)
   {
      unsigned entry_max = MAX_VALUES - 4;
      music_entry *entries = (music_entry*)malloc(entry_max * sizeof(*entries));
      unsigned entry_count = entries ? scan_music(p, entries, entry_max) : 0;

      for (unsigned i = 0; i < p->track_count; i++)
      {
         const px_track *t = &p->tracks[i];
         char key[32], desc[96], info[512], label[300], def_label[320];

         snprintf(key, sizeof(key), PX_OPT_SONG_FMT, (unsigned)t->value);
         if (t->name[0])
            snprintf(desc, sizeof(desc), "Song 0x%X: %s", (unsigned)t->value, t->name);
         else
            snprintf(desc, sizeof(desc), "Song 0x%X", (unsigned)t->value);
         track_label(t, label, sizeof(label));
         snprintf(info, sizeof(info),
               "What plays when the game selects song 0x%X. The profile plays %s.",
               (unsigned)t->value, label);
         if (!(d = own_def(key, desc, NULL, info, PX_OPT_PROFILE)))
            break;
         snprintf(def_label, sizeof(def_label), "Profile (%s)", label);
         add_value(d, PX_OPT_PROFILE, def_label);
         add_value(d, "original", "Original music");
         add_value(d, "silence", "Silence");
         for (unsigned e = 0; e < entry_count; e++)
            add_value(d, entries[e].value, entries[e].label);
      }
      free(entries);
   }
}

/* ---------------------------------------------------------------------------
 * Publishing
 * ------------------------------------------------------------------------- */

static void free_outputs(void)
{
   for (unsigned i = 0; i < out_v0_count; i++)
      free(out_v0_strings[i]);
   free(out_v0_strings);
   free(out_cats);
   free(out_v2);
   free(out_v1);
   free(out_v0);
   out_cats = NULL; out_v2 = NULL; out_v1 = NULL; out_v0 = NULL;
   out_v0_strings = NULL; out_v0_count = 0;
}

static const opt_def *merged_def(unsigned i)
{
   return i < inner_set.def_count ? &inner_set.defs[i] : &own_set.defs[i - inner_set.def_count];
}

static bool publish_v2(unsigned total)
{
   struct retro_core_options_v2 opts;
   unsigned cats = inner_set.cat_count + own_set.cat_count;

   out_cats = (struct retro_core_option_v2_category*)calloc(cats + 1, sizeof(*out_cats));
   out_v2   = (struct retro_core_option_v2_definition*)calloc(total + 1, sizeof(*out_v2));
   if (!out_cats || !out_v2)
      return false;

   for (unsigned i = 0; i < cats; i++)
   {
      const opt_category *c = i < inner_set.cat_count
            ? &inner_set.cats[i] : &own_set.cats[i - inner_set.cat_count];
      out_cats[i].key  = c->key;
      out_cats[i].desc = c->desc;
      out_cats[i].info = c->info;
   }
   for (unsigned i = 0; i < total; i++)
   {
      const opt_def *d = merged_def(i);
      struct retro_core_option_v2_definition *o = &out_v2[i];
      o->key              = d->key;
      o->desc             = d->desc;
      o->desc_categorized = d->desc_categorized;
      o->info             = d->info;
      o->info_categorized = d->info_categorized;
      o->category_key     = d->category;
      o->default_value    = d->default_value;
      for (unsigned v = 0; v < d->value_count; v++)
      {
         o->values[v].value = d->values[v].value;
         o->values[v].label = d->values[v].label;
      }
   }
   opts.categories  = out_cats;
   opts.definitions = out_v2;
   return fe_env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts);
}

static bool publish_v1(unsigned total)
{
   out_v1 = (struct retro_core_option_definition*)calloc(total + 1, sizeof(*out_v1));
   if (!out_v1)
      return false;
   for (unsigned i = 0; i < total; i++)
   {
      const opt_def *d = merged_def(i);
      out_v1[i].key           = d->key;
      out_v1[i].desc          = d->desc;
      out_v1[i].info          = d->info;
      out_v1[i].default_value = d->default_value;
      for (unsigned v = 0; v < d->value_count; v++)
      {
         out_v1[i].values[v].value = d->values[v].value;
         out_v1[i].values[v].label = d->values[v].label;
      }
   }
   return fe_env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, out_v1);
}

static bool publish_v0(unsigned total)
{
   out_v0         = (struct retro_variable*)calloc(total + 1, sizeof(*out_v0));
   out_v0_strings = (char**)calloc(total, sizeof(*out_v0_strings));
   if (!out_v0 || !out_v0_strings)
      return false;

   for (unsigned i = 0; i < total; i++)
   {
      const opt_def *d = merged_def(i);
      size_t len = strlen(d->desc ? d->desc : d->key) + 3;
      char *s;
      int def = 0;

      for (unsigned v = 0; v < d->value_count; v++)
      {
         len += strlen(d->values[v].value) + 1;
         if (d->default_value && !strcmp(d->values[v].value, d->default_value))
            def = (int)v;
      }
      if (!(s = (char*)malloc(len)))
         return false;
      out_v0_strings[out_v0_count++] = s;

      /* The legacy format takes the first value as the default. */
      s += sprintf(s, "%s; ", d->desc ? d->desc : d->key);
      if (d->value_count)
         s += sprintf(s, "%s", d->values[def].value);
      for (unsigned v = 0; v < d->value_count; v++)
         if ((int)v != def)
            s += sprintf(s, "|%s", d->values[v].value);

      out_v0[i].key   = d->key;
      out_v0[i].value = out_v0_strings[i];
   }
   return fe_env(RETRO_ENVIRONMENT_SET_VARIABLES, out_v0);
}

void px_options_set_frontend(retro_environment_t env)
{
   fe_env = env;
}

void px_options_set_profile(const px_profile *profile)
{
   build_own(profile);
}

bool px_options_publish(void)
{
   unsigned version = 0;
   unsigned total;

   if (!fe_env)
      return false;
   if (!own_set.def_count)
      build_own(NULL);

   free_outputs();
   total = inner_set.def_count + own_set.def_count;
   if (!fe_env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;
   if (version >= 2)
      return publish_v2(total);
   if (version == 1)
      return publish_v1(total);
   return publish_v0(total);
}

const char *px_options_get(const char *key)
{
   struct retro_variable var = { key, NULL };
   if (fe_env && fe_env(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      return var.value;
   return NULL;
}

void px_options_free(void)
{
   free_outputs();
   clear_set(&inner_set);
   clear_set(&own_set);
   inner_declared = false;
}
