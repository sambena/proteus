/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "profile.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

static char *trim(char *s)
{
   char *end;
   while (isspace((unsigned char)*s))
      s++;
   end = s + strlen(s);
   while (end > s && isspace((unsigned char)end[-1]))
      *--end = '\0';
   return s;
}

/* Comments start with ';' or '#' at the start of a line or after whitespace,
 * so paths like "music/track#2.ogg" survive. */
static void strip_comment(char *s)
{
   for (char *p = s; *p; p++)
   {
      if ((*p == ';' || *p == '#') && (p == s || isspace((unsigned char)p[-1])))
      {
         *p = '\0';
         return;
      }
   }
}

static bool parse_uint(const char *s, uint64_t *out)
{
   char *end;
   if (!*s)
      return false;
   if (s[0] == '$')
      *out = strtoull(s + 1, &end, 16);
   else
      *out = strtoull(s, &end, 0);
   return *end == '\0';
}

static bool parse_bool(const char *s, bool *out)
{
   if (!strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "yes") || !strcmp(s, "on"))
      *out = true;
   else if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "no") || !strcmp(s, "off"))
      *out = false;
   else
      return false;
   return true;
}

/* Volumes are written as percentages (0-200). */
static bool parse_volume(const char *s, float *out)
{
   uint64_t v;
   if (!parse_uint(s, &v) || v > 200)
      return false;
   *out = (float)v / 100.0f;
   return true;
}

static bool parse_action(const char *s, px_action *out)
{
   if (!strcmp(s, "original"))
      *out = PX_ACTION_ORIGINAL;
   else if (!strcmp(s, "silence"))
      *out = PX_ACTION_SILENCE;
   else if (!strcmp(s, "keep"))
      *out = PX_ACTION_KEEP;
   else
      return false;
   return true;
}

static bool parse_memory(const char *s, unsigned *out)
{
   if (!strcmp(s, "system_ram"))
      *out = RETRO_MEMORY_SYSTEM_RAM;
   else if (!strcmp(s, "save_ram"))
      *out = RETRO_MEMORY_SAVE_RAM;
   else if (!strcmp(s, "video_ram"))
      *out = RETRO_MEMORY_VIDEO_RAM;
   else if (!strcmp(s, "rtc"))
      *out = RETRO_MEMORY_RTC;
   else
      return false;
   return true;
}

/* "music/overworld.ogg | loop=0 | loop_start=12345 | volume=80" */
static bool parse_track(px_profile *p, const char *dir, uint32_t value, char *spec,
      char *err, size_t errlen)
{
   px_track *t;
   char *save = NULL;
   char *part;
   bool first = true;

   if (p->track_count >= PX_MAX_TRACKS)
   {
      snprintf(err, errlen, "too many tracks (max %d)", PX_MAX_TRACKS);
      return false;
   }

   for (unsigned i = 0; i < p->track_count; i++)
   {
      if (p->tracks[i].value == value)
      {
         snprintf(err, errlen, "song value 0x%X is mapped twice", (unsigned)value);
         return false;
      }
   }

   t = &p->tracks[p->track_count];
   memset(t, 0, sizeof(*t));
   t->value  = value;
   t->loop   = true;
   t->volume = 1.0f;

   for (part = strtok_r(spec, "|", &save); part; part = strtok_r(NULL, "|", &save))
   {
      char *item = trim(part);
      if (first)
      {
         first = false;
         if (!parse_action(item, &t->action))
         {
            t->action = PX_ACTION_FILE;
            px_path_join(dir, item, t->path, sizeof(t->path));
         }
         continue;
      }

      {
         char *eq = strchr(item, '=');
         char *key, *val;
         uint64_t n;
         if (!eq)
         {
            snprintf(err, errlen, "expected key=value, got '%s'", item);
            return false;
         }
         *eq = '\0';
         key = trim(item);
         val = trim(eq + 1);
         if (!strcmp(key, "loop"))
         {
            if (!parse_bool(val, &t->loop))
               goto bad_value;
         }
         else if (!strcmp(key, "loop_start"))
         {
            if (!parse_uint(val, &n))
               goto bad_value;
            t->loop_start = n;
         }
         else if (!strcmp(key, "volume"))
         {
            if (!parse_volume(val, &t->volume))
               goto bad_value;
         }
         else if (!strcmp(key, "track"))
         {
            if (!parse_uint(val, &n) || n < 1 || n > 255)
               goto bad_value;
            t->subtrack = (unsigned)(n - 1);
         }
         else
         {
            snprintf(err, errlen, "unknown track option '%s'", key);
            return false;
         }
         continue;
bad_value:
         snprintf(err, errlen, "bad value '%s' for '%s'", val, key);
         return false;
      }
   }

   if (first)
   {
      snprintf(err, errlen, "empty track entry");
      return false;
   }

   p->track_count++;
   return true;
}

static bool parse_pattern(const char *s, uint8_t *pattern, uint8_t *mask,
      unsigned *length, unsigned *offset)
{
   char buf[128];
   char *token, *save = NULL;
   unsigned count = 0;
   int song_pos = -1;

   snprintf(buf, sizeof(buf), "%s", s);
   for (token = strtok_r(buf, " \t", &save); token; token = strtok_r(NULL, " \t", &save))
   {
      if (count >= PX_MAX_PATTERN)
         return false;
      if ((token[0] == 'x' || token[0] == 'X') &&
          (token[1] == 'x' || token[1] == 'X') && token[2] == '\0')
      {
         pattern[count] = 0;
         mask[count]    = 0x00;
         song_pos       = (int)count;
      }
      else if (!strcmp(token, "..") || !strcmp(token, "??"))
      {
         /* any value: bytes a command block varies from song to song */
         pattern[count] = 0;
         mask[count]    = 0x00;
      }
      else
      {
         char *end;
         unsigned long b = strtoul(token, &end, 16);
         if (*end != '\0' || b > 0xFF)
            return false;
         pattern[count] = (uint8_t)b;
         mask[count]    = 0xFF;
      }
      count++;
   }
   if (count == 0 || song_pos < 0)
      return false;
   if (count == 1)
   {
      *length = 0;
      *offset = 0;
   }
   else
   {
      *length = count;
      *offset = (unsigned)song_pos;
   }
   return true;
}

static bool handle_entry(px_profile *p, const char *dir, const char *section,
      char *key, char *val, char *err, size_t errlen)
{
   uint64_t n;

   if (!strcmp(section, "song"))
   {
      if (!strcmp(key, "memory"))
      {
         if (!parse_memory(val, &p->memory_id))
            goto bad_value;
      }
      else if (!strcmp(key, "address"))
      {
         if (!parse_uint(val, &n) || n > 0xFFFFFFFFu)
            goto bad_value;
         p->address = (uint32_t)n;
      }
      else if (!strcmp(key, "size"))
      {
         if (!parse_uint(val, &n) || (n != 1 && n != 2 && n != 4))
            goto bad_value;
         p->size = (unsigned)n;
      }
      else if (!strcmp(key, "mask"))
      {
         if (!parse_uint(val, &n) || n > 0xFFFFFFFFu)
            goto bad_value;
         p->mask = (uint32_t)n;
      }
      else if (!strcmp(key, "debounce"))
      {
         if (!parse_uint(val, &n) || n > 600)
            goto bad_value;
         p->debounce = (unsigned)n;
      }
      else if (!strcmp(key, "unmapped"))
      {
         if (!parse_action(val, &p->unmapped))
            goto bad_value;
      }
      else if (!strcmp(key, "events"))
      {
         if (!parse_uint(val, &n) || n > 0xFFFFFFFFu)
            goto bad_value;
         p->events_address = (uint32_t)n;
         p->events = true;
      }
      else if (!strcmp(key, "latch"))
      {
         if (!parse_bool(val, &p->latch))
            goto bad_value;
      }
      else if (!strcmp(key, "bytes"))
      {
         if (!parse_pattern(val, p->pattern, p->pattern_mask, &p->pattern_length, &p->pattern_offset))
            goto bad_value;
         p->size = p->pattern_length ? p->pattern_length : 1;
      }
      else
         goto bad_key;
   }
   else if (!strcmp(section, "mute"))
   {
      px_option *o;
      if (p->mute_count >= PX_MAX_MUTE)
      {
         snprintf(err, errlen, "too many mute options (max %d)", PX_MAX_MUTE);
         return false;
      }
      o = &p->mute[p->mute_count++];
      snprintf(o->key, sizeof(o->key), "%s", key);
      snprintf(o->value, sizeof(o->value), "%s", val);
   }
   else if (!strcmp(section, "silence"))
   {
      if (!strcmp(key, "memory"))
      {
         if (!parse_memory(val, &p->silence_memory))
            goto bad_value;
      }
      else if (!strcmp(key, "address"))
      {
         if (!parse_uint(val, &n) || n > 0xFFFFFFFFu)
            goto bad_value;
         p->silence_address = (uint32_t)n;
         p->silence = true;
      }
      else if (!strcmp(key, "value"))
      {
         if (!parse_uint(val, &n) || n > 0xFF)
            goto bad_value;
         p->silence_value = (uint8_t)n;
      }
      else
         goto bad_key;
   }
   else if (!strcmp(section, "mix"))
   {
      if (!strcmp(key, "music_volume"))
      {
         if (!parse_volume(val, &p->music_volume))
            goto bad_value;
      }
      else if (!strcmp(key, "game_volume"))
      {
         if (!parse_volume(val, &p->game_volume))
            goto bad_value;
      }
      else if (!strcmp(key, "crossfade_ms"))
      {
         if (!parse_uint(val, &n) || n > 60000)
            goto bad_value;
         p->crossfade_ms = (unsigned)n;
      }
      else
         goto bad_key;
   }
   else if (!strcmp(section, "tracks"))
   {
      if (!parse_uint(key, &n) || n > 0xFFFFFFFFu)
      {
         snprintf(err, errlen, "track key '%s' is not a number", key);
         return false;
      }
      return parse_track(p, dir, (uint32_t)n, val, err, errlen);
   }
   else if (!strcmp(section, "library"))
   {
      if (strcmp(key, "dir"))
         goto bad_key;
      if (p->library_count >= PX_MAX_LIBRARY)
      {
         snprintf(err, errlen, "too many library folders (max %d)", PX_MAX_LIBRARY);
         return false;
      }
      px_path_join(dir, val, p->library[p->library_count++], PX_PATH_MAX);
   }
   else if (!strcmp(section, "debug"))
   {
      if (!strcmp(key, "log_songs"))
      {
         if (!parse_bool(val, &p->log_songs))
            goto bad_value;
      }
      else
         goto bad_key;
   }
   else
   {
      snprintf(err, errlen, "unknown section [%s]", section);
      return false;
   }
   return true;

bad_key:
   snprintf(err, errlen, "unknown key '%s' in [%s]", key, section);
   return false;
bad_value:
   snprintf(err, errlen, "bad value '%s' for '%s'", val, key);
   return false;
}

bool px_profile_load(px_profile *p, const char *path, char *err, size_t errlen)
{
   char line[2048];
   char section[64] = "";
   char dir[PX_PATH_MAX];
   bool have_address = false;
   unsigned line_no = 0;
   FILE *f;

   memset(p, 0, sizeof(*p));
   p->memory_id    = RETRO_MEMORY_SYSTEM_RAM;
   p->silence_memory = RETRO_MEMORY_SYSTEM_RAM;
   p->size         = 1;
   p->mask         = 0xFFFFFFFFu;
   p->debounce     = 1;
   p->unmapped     = PX_ACTION_ORIGINAL;
   p->music_volume = 1.0f;
   p->game_volume  = 1.0f;
   p->crossfade_ms = 400;
   snprintf(p->path, sizeof(p->path), "%s", path);
   px_path_dir(path, dir, sizeof(dir));

   f = px_fopen(path, "rb");
   if (!f)
   {
      snprintf(err, errlen, "cannot open %s", path);
      return false;
   }

   while (fgets(line, sizeof(line), f))
   {
      char *s = line;
      line_no++;
      /* Skip a UTF-8 byte order mark. */
      if (line_no == 1 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB
            && (unsigned char)s[2] == 0xBF)
         s += 3;
      strip_comment(s);
      s = trim(s);
      if (!*s)
         continue;

      if (*s == '[')
      {
         char *close = strchr(s, ']');
         if (!close)
         {
            snprintf(err, errlen, "line %u: unterminated section", line_no);
            goto fail;
         }
         *close = '\0';
         snprintf(section, sizeof(section), "%s", trim(s + 1));
         for (char *c = section; *c; c++)
            *c = (char)tolower((unsigned char)*c);
         continue;
      }

      {
         char *eq = strchr(s, '=');
         char msg[256];
         char *key, *val;
         if (!eq)
         {
            snprintf(err, errlen, "line %u: expected key = value", line_no);
            goto fail;
         }
         if (!section[0])
         {
            snprintf(err, errlen, "line %u: entry outside of a section", line_no);
            goto fail;
         }
         *eq = '\0';
         key = trim(s);
         val = trim(eq + 1);
         if (!strcmp(section, "song") && !strcmp(key, "address"))
            have_address = true;
         if (!handle_entry(p, dir, section, key, val, msg, sizeof(msg)))
         {
            snprintf(err, errlen, "line %u: %s", line_no, msg);
            goto fail;
         }
      }
   }
   fclose(f);

   if (!have_address)
   {
      snprintf(err, errlen, "[song] address is required");
      return false;
   }

   p->loaded = true;
   return true;

fail:
   fclose(f);
   return false;
}

int px_profile_find(const px_profile *p, uint32_t value)
{
   for (unsigned i = 0; i < p->track_count; i++)
      if (p->tracks[i].value == value)
         return (int)i;
   return -1;
}
