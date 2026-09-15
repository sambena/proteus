/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"
#include "util.h"

#define STATE_MAGIC 0x4E545250u /* "PRTN" */
#define STATE_VER   2u

static void elog(px_engine *e, enum retro_log_level level, const char *fmt, ...)
{
   char msg[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   if (e->host.log)
      e->host.log(e->host.userdata, level, msg);
}

static void enotify(px_engine *e, const char *fmt, ...)
{
   char msg[256];
   va_list ap;
   if (!e->host.notify)
      return;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   e->host.notify(e->host.userdata, msg);
}

static const char *option(px_engine *e, const char *key)
{
   return e->host.option ? e->host.option(e->host.userdata, key) : NULL;
}

/* The core's cheat: the profile's [tap] for it while a profile is loaded, and its [patch], which
 * stops the game's music code, while the original is muted. */
static void apply_patches(px_engine *e)
{
   const px_profile *p = &e->profile;
   const char *core, *tap, *patch;
   char code[2 * PX_PATCH_CODE_MAX + 1];

   if (!e->host.patch || !e->host.core_file)
      return;
   core  = e->host.core_file(e->host.userdata);
   tap   = p->loaded ? px_profile_patch_for(p->tap, p->tap_count, core) : NULL;
   patch = p->loaded && e->muted ? px_profile_patch_for(p->patch, p->patch_count, core) : NULL;
   if (!tap && !patch)
   {
      if (e->patched)
         e->host.patch(e->host.userdata, NULL);
      e->patched = false;
      return;
   }
   snprintf(code, sizeof(code), "%s%s%s", tap ? tap : "", tap && patch ? "+" : "", patch ? patch : "");
   e->host.patch(e->host.userdata, code);
   e->patched = true;
}

static void set_muted(px_engine *e, bool muted)
{
   if (e->muted == muted)
      return;
   e->muted = muted;
   if (e->host.mute)
      e->host.mute(e->host.userdata, muted);
   apply_patches(e);
}

static void reset_state(px_engine *e)
{
   px_mixer_free(&e->mixer);
   {
      double rate = e->mixer.out_rate;
      px_mixer_init(&e->mixer);
      px_mixer_set_rate(&e->mixer, rate);
   }
   set_muted(e, false);
   e->have_candidate = false;
   e->stable_frames  = 0;
   e->have_applied   = false;
   e->warned_memory  = false;
   e->silencing      = 0;
   e->have_silenced  = false;
   e->idle           = false;
   /* The game (or its memory) is going away: nothing is written back. */
   e->holding        = false;
}

/* Where `address` is in the core's memory: N64 addresses are masked to physical RDRAM, and each
 * 32-bit word is in the host's (little endian) byte order. */
static size_t byte_index(const px_profile *p, uint32_t address)
{
   if (!p->n64)
      return address;
   address &= 0x1FFFFFFFu;
   return (address & ~3u) + (3 - (address & 3));
}

/* Whether `n` bytes at `address` are inside the core's `size` bytes. */
static bool in_range(const px_profile *p, size_t size, uint32_t address, unsigned n)
{
   if (p->n64)
      return ((uint64_t)(address & 0x1FFFFFFFu) + n + 3) / 4 * 4 <= size;
   return (uint64_t)address + n <= size;
}

/* `n` (1 to 4) bytes at `address` (little endian, or big endian for N64), false when outside `size`. */
static bool read_value(const px_profile *p, const uint8_t *data, size_t size, uint32_t address, unsigned n, uint32_t *out)
{
   uint32_t v = 0;
   if (!in_range(p, size, address, n))
      return false;
   for (unsigned i = 0; i < n; i++)
   {
      uint8_t b = data[byte_index(p, address + i)];
      v = p->n64 ? v << 8 | b : v | (uint32_t)b << (8 * i);
   }
   *out = v;
   return true;
}

static void write_value(const px_profile *p, uint8_t *data, uint32_t address, unsigned n, uint32_t v)
{
   for (unsigned i = 0; i < n; i++)
      data[byte_index(p, address + i)] = (uint8_t)(p->n64 ? v >> (8 * (n - 1 - i)) : v >> (8 * i));
}

/* Applies the profile's [hold] writes while the original music is muted, and writes back what was
 * there once the mute lifts. Called before each frame the core runs. */
static void apply_holds(px_engine *e)
{
   const px_profile *p = &e->profile;
   size_t size = 0;
   uint8_t *data;

   if (!p->hold_count || !e->host.memory || (!e->muted && !e->holding))
      return;
   if (!(data = (uint8_t*)e->host.memory(e->host.userdata, p->memory_id, &size)))
      return;
   for (unsigned i = 0; i < p->hold_count; i++)
   {
      const px_hold *h = &p->hold[i];
      uint32_t v;
      if (!read_value(p, data, size, h->address, h->size, &v))
         continue;
      if (e->muted)
      {
         if (!e->holding)
            e->hold_saved[i] = v;
         write_value(p, data, h->address, h->size, h->or_bits ? v | h->value : h->value);
      }
      /* |= sets flags (a "recalculate the volume" request): set once more so the restored volume applies. */
      else if (h->or_bits)
         write_value(p, data, h->address, h->size, v | h->value);
      else
         write_value(p, data, h->address, h->size, h->has_release ? h->release : e->hold_saved[i]);
   }
   e->holding = e->muted;
}

/* Frames in which the game reacts to the silence request (its music code runs once a frame). */
#define PX_SILENCE_FRAMES 30

/* Asks the game to stop its own music: the profile's [silence] request, written to its RAM. */
static void stop_game_music(px_engine *e)
{
   const px_profile *p = &e->profile;
   size_t size = 0;
   uint8_t *data;

   if (!p->silence || !e->host.memory)
      return;
   /* retro_get_memory_data gives the core's own, writable memory. */
   data = (uint8_t*)e->host.memory(e->host.userdata, p->silence_memory, &size);
   if (!data || byte_index(p, p->silence_address) >= size)
      return;
   data[byte_index(p, p->silence_address)] = p->silence_value;
   e->silencing     = PX_SILENCE_FRAMES;
   e->have_silenced = false;
}

void px_engine_init(px_engine *e, const px_host *host)
{
   memset(e, 0, sizeof(*e));
   e->host = *host;
   px_mixer_init(&e->mixer);
   e->cfg.enabled = true;
}

/* A profile named `stem`, next to the content in `dir` or in the system folder. */
static bool profile_named(const char *dir, const char *stem, const char *system_dir, char *out, size_t n)
{
   snprintf(out, n, "%s/%s.proteus.ini", dir, stem);
   if (px_file_exists(out))
      return true;
   if (system_dir && *system_dir)
   {
      snprintf(out, n, "%s/proteus/%s.ini", system_dir, stem);
      if (px_file_exists(out))
         return true;
   }
   return false;
}

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Looks for a profile named after each file inside a zip archive (from its central directory). */
static bool profile_in_zip(const char *zip_path, const char *dir, const char *system_dir, char *out, size_t n)
{
   FILE *f = px_fopen(zip_path, "rb");
   unsigned char *buf = NULL;
   long size, tail_start;
   size_t tail;
   bool found = false;

   if (!f)
      return false;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   /* The end record is in the last 22 bytes plus a comment of up to 65535. */
   tail_start = size > 65557 ? size - 65557 : 0;
   tail = (size_t)(size - tail_start);
   if (size < 22 || !(buf = (unsigned char*)malloc(tail)))
      goto done;
   fseek(f, tail_start, SEEK_SET);
   if (fread(buf, 1, tail, f) != tail)
      goto done;
   for (size_t at = tail - 22 + 1; at-- > 0;)
   {
      uint32_t cd_size, cd_offset;
      unsigned count;
      unsigned char *cd;
      size_t pos = 0;

      if (le32(buf + at) != 0x06054b50)
         continue;
      count = le16(buf + at + 10);
      cd_size = le32(buf + at + 12);
      cd_offset = le32(buf + at + 16);
      if ((long)cd_offset + (long)cd_size > size || !(cd = (unsigned char*)malloc(cd_size ? cd_size : 1)))
         break;
      fseek(f, (long)cd_offset, SEEK_SET);
      if (fread(cd, 1, cd_size, f) == cd_size)
         for (unsigned i = 0; i < count && pos + 46 <= cd_size && !found; i++)
         {
            char name[512], stem[256];
            unsigned name_len = le16(cd + pos + 28);
            size_t next = pos + 46 + name_len + le16(cd + pos + 30) + le16(cd + pos + 32);
            if (le32(cd + pos) != 0x02014b50 || pos + 46 + name_len > cd_size)
               break;
            if (name_len && name_len < sizeof(name) && cd[pos + 46 + name_len - 1] != '/')
            {
               memcpy(name, cd + pos + 46, name_len);
               name[name_len] = '\0';
               px_path_stem(name, stem, sizeof(stem));
               found = profile_named(dir, stem, system_dir, out, n);
            }
            pos = next;
         }
      free(cd);
      break;
   }
done:
   free(buf);
   fclose(f);
   return found;
}

bool px_engine_find_profile(const char *content_path, const char *system_dir, char *out, size_t n)
{
   char path[PX_PATH_MAX];
   char dir[PX_PATH_MAX];
   char stem[256];
   const char *hash, *ext;

   if (!content_path || !*content_path)
      return false;

   snprintf(path, sizeof(path), "%s", content_path);
   hash = strchr(path, '#');
   if (hash)
   {
      /* "archive.zip#game.nes": named after the game, or failing that the archive. */
      px_path_stem(hash + 1, stem, sizeof(stem));
      path[hash - path] = '\0';
      px_path_dir(path, dir, sizeof(dir));
      if (profile_named(dir, stem, system_dir, out, n))
         return true;
   }
   px_path_dir(path, dir, sizeof(dir));
   px_path_stem(path, stem, sizeof(stem));
   if (profile_named(dir, stem, system_dir, out, n))
      return true;

   /* RetroArch's history names only the archive (the DSP plugin's view): try the games inside. */
   ext = strrchr(path, '.');
   return !hash && ext && !strcasecmp(ext, ".zip") && profile_in_zip(path, dir, system_dir, out, n);
}

bool px_engine_load(px_engine *e, const char *profile_path)
{
   char err[PX_PATH_MAX + 128];

   reset_state(e);
   e->profile.loaded = false;
   if (!px_profile_load(&e->profile, profile_path, err, sizeof(err)))
   {
      elog(e, RETRO_LOG_ERROR, "%s: %s", profile_path, err);
      enotify(e, "Proteus: profile error, see log");
      return false;
   }
   elog(e, RETRO_LOG_INFO, "loaded profile %s (%u tracks)", profile_path, e->profile.track_count);
   px_engine_read_config(e);
   apply_patches(e);
   return true;
}

void px_engine_unload(px_engine *e)
{
   reset_state(e);
   e->profile.loaded = false;
   apply_patches(e);
}

bool px_engine_loaded(const px_engine *e)
{
   return e->profile.loaded;
}

void px_engine_set_rate(px_engine *e, double rate)
{
   if (rate > 0.0)
      px_mixer_set_rate(&e->mixer, rate);
}

static float option_percent(px_engine *e, const char *key, float fallback)
{
   const char *v = option(e, key);
   return (!v || !strcmp(v, PX_OPT_PROFILE)) ? fallback : (float)atoi(v) / 100.0f;
}

void px_engine_read_config(px_engine *e)
{
   const px_profile *p = &e->profile;
   const char *v;

   e->cfg.enabled      = !((v = option(e, PX_OPT_ENABLED)) && !strcmp(v, "disabled"));
   e->cfg.music_volume = option_percent(e, PX_OPT_MUSIC_VOLUME, p->music_volume);
   e->cfg.game_volume  = option_percent(e, PX_OPT_GAME_VOLUME, p->game_volume);

   v = option(e, PX_OPT_CROSSFADE);
   e->cfg.crossfade_ms = (!v || !strcmp(v, PX_OPT_PROFILE)) ? p->crossfade_ms : (unsigned)atoi(v);

   v = option(e, PX_OPT_NOTIFY);
   e->cfg.notify = (!v || !strcmp(v, PX_OPT_PROFILE)) ? p->log_songs : !strcmp(v, "enabled");
}

static unsigned fade_frames(const px_engine *e)
{
   return (unsigned)(e->mixer.out_rate * e->cfg.crossfade_ms / 1000.0);
}

/* What to do for one song value, after applying the song picker core option. */
typedef struct
{
   px_action action;
   char path[PX_PATH_MAX];
   unsigned subtrack;
   bool loop;
   uint64_t loop_start;
   float volume;
   bool mapped;
} px_choice;

static void resolve_song(px_engine *e, uint32_t value, px_choice *c)
{
   const px_profile *p = &e->profile;
   int index = px_profile_find(p, value);
   char key[32], rel[PX_PATH_MAX], dir[PX_PATH_MAX], full[PX_PATH_MAX];
   unsigned subtrack = 0;
   const char *v;
   char *hash;

   memset(c, 0, sizeof(*c));
   c->loop   = true;
   c->volume = 1.0f;
   if (value == PX_SONG_STOPPED && p->active)
   {
      c->action = p->stopped;
      return;
   }
   if (index < 0)
   {
      c->action = p->unmapped;
      return;
   }

   {
      const px_track *t = &p->tracks[index];
      c->mapped     = true;
      c->action     = t->action;
      c->subtrack   = t->subtrack;
      c->loop       = t->loop;
      c->loop_start = t->loop_start;
      c->volume     = t->volume;
      snprintf(c->path, sizeof(c->path), "%s", t->path);
   }

   snprintf(key, sizeof(key), PX_OPT_SONG_FMT, (unsigned)value);
   v = option(e, key);
   if (!v || !strcmp(v, PX_OPT_PROFILE))
      return;
   if (!strcmp(v, "original"))
   {
      c->action = PX_ACTION_ORIGINAL;
      return;
   }
   if (!strcmp(v, "silence"))
   {
      c->action = PX_ACTION_SILENCE;
      return;
   }

   /* "music/dungeon.nsf#3" picks song 3 of a multi-song file. */
   snprintf(rel, sizeof(rel), "%s", v);
   hash = strrchr(rel, '#');
   if (hash && hash[1] && strspn(hash + 1, "0123456789") == strlen(hash + 1) && atoi(hash + 1) > 0)
   {
      subtrack = (unsigned)atoi(hash + 1) - 1;
      *hash    = '\0';
   }
   px_path_dir(p->path, dir, sizeof(dir));
   px_path_join(dir, rel, full, sizeof(full));

   /* Picking the profile's own file keeps its loop point and volume. */
   if (c->action == PX_ACTION_FILE && !strcmp(full, c->path) && subtrack == c->subtrack)
      return;
   c->action     = PX_ACTION_FILE;
   c->subtrack   = subtrack;
   c->loop       = true;
   c->loop_start = 0;
   c->volume     = 1.0f;
   snprintf(c->path, sizeof(c->path), "%s", full);
}

static void start_choice(px_engine *e, const px_choice *c, unsigned fade, uint64_t start_frame)
{
   char err[PX_PATH_MAX + 64];

   /* Several song values may share a file; keep it playing without a restart. */
   if (px_mixer_is_playing(&e->mixer, c->path, c->subtrack))
   {
      e->mixer.current.volume = c->volume;
      e->mixer.current.loop   = c->loop;
      set_muted(e, true);
      return;
   }

   if (!px_mixer_play(&e->mixer, c->path, c->subtrack, c->loop, c->loop_start, c->volume,
            fade, start_frame, err, sizeof(err)))
   {
      elog(e, RETRO_LOG_ERROR, "%s", err);
      enotify(e, "Proteus: cannot play %s", c->path);
      px_mixer_stop(&e->mixer, fade);
      set_muted(e, false);
      return;
   }
   set_muted(e, true);
}

/* `announce` is false when re-applying the current song after an option change. */
static void apply_song(px_engine *e, uint32_t value, bool announce)
{
   px_choice c;
   resolve_song(e, value, &c);

   e->have_applied = true;
   e->applied      = value;
   e->idle         = false;

   /* Song changes are always logged; notifications only shows them on screen. */
   if (announce && value == PX_SONG_STOPPED && e->profile.active)
   {
      static const char *names[] = { "", "silence", "original music", "keep playing" };
      elog(e, RETRO_LOG_INFO, "song stopped -> %s", names[c.action]);
   }
   else if (announce)
   {
      static const char *names[] = { "", "silence", "original music", "keep playing" };
      const char *what = c.action == PX_ACTION_FILE ? c.path : names[c.action];
      const char *name = c.action == PX_ACTION_FILE ? what + strlen(what) : what;
      while (name > what && name[-1] != '/' && name[-1] != '\\')
         name--;
      int index = px_profile_find(&e->profile, value);
      const char *song = index >= 0 ? e->profile.tracks[index].name : "";
      elog(e, RETRO_LOG_INFO, "song 0x%X%s%s%s -> %s%s", (unsigned)value, *song ? " (" : "", song, *song ? ")" : "",
            what, c.mapped ? "" : " (unmapped)");
      if (e->cfg.notify)
         enotify(e, "Proteus: song 0x%X%s%s%s: %s", (unsigned)value, c.mapped ? "" : " (unmapped)",
               *song ? " " : "", song, name);
   }

   e->have_silenced = false;
   switch (c.action)
   {
      case PX_ACTION_FILE:
         start_choice(e, &c, fade_frames(e), 0);
         stop_game_music(e);
         break;
      case PX_ACTION_SILENCE:
         px_mixer_stop(&e->mixer, fade_frames(e));
         set_muted(e, true);
         stop_game_music(e);
         break;
      case PX_ACTION_ORIGINAL:
         px_mixer_stop(&e->mixer, fade_frames(e));
         set_muted(e, false);
         e->silencing = 0;
         break;
      case PX_ACTION_KEEP:
         break;
   }
}

void px_engine_options_changed(px_engine *e)
{
   bool was_enabled = e->cfg.enabled;

   if (!e->profile.loaded)
      return;
   px_engine_read_config(e);
   if (!e->cfg.enabled && was_enabled)
   {
      px_mixer_stop(&e->mixer, fade_frames(e));
      set_muted(e, false);
      e->have_applied   = false;
      e->have_candidate = false;
   }
   else if (e->cfg.enabled && e->have_applied)
      apply_song(e, e->applied, false);
}

static void follow_song(px_engine *e);

static bool read_song_value(px_engine *e, uint32_t *out)
{
   const px_profile *p = &e->profile;
   size_t size = 0;
   const uint8_t *data = e->host.memory ? e->host.memory(e->host.userdata, p->memory_id, &size) : NULL;
   uint32_t v = 0, flags;

   /* N64 cores hand out their RAM once the game is running. */
   if (!data)
      return false;
   if (!in_range(p, size, p->address, p->pattern_length ? p->pattern_length : p->size)
         || (!p->pattern_length && !read_value(p, data, size, p->address, p->size, &v)))
   {
      if (!e->warned_memory)
      {
         e->warned_memory = true;
         elog(e, RETRO_LOG_WARN, "song address 0x%X is outside the core's memory (size 0x%X)",
               (unsigned)p->address, (unsigned)size);
      }
      return false;
   }

   if (p->active && read_value(p, data, size, p->active_address, 1, &flags) && !(flags & p->active_mask))
   {
      *out = PX_SONG_STOPPED;
      return true;
   }

   if (p->events && byte_index(p, p->events_address) < size && data[byte_index(p, p->events_address)])
   {
      *out = 0x100u | data[byte_index(p, p->events_address)];
      return true;
   }

   if (p->pattern_length > 0)
   {
      for (unsigned i = 0; i < p->pattern_length; i++)
      {
         if ((data[byte_index(p, p->address + i)] & p->pattern_mask[i]) != (p->pattern[i] & p->pattern_mask[i]))
            return false;
      }
      *out = (uint32_t)data[byte_index(p, p->address + p->pattern_offset)];
      return true;
   }

   *out = v & p->mask;
   return true;
}

void px_engine_frame(px_engine *e)
{
   if (!e->profile.loaded)
      return;
   if (e->cfg.enabled)
      follow_song(e);
   /* After following the song, so a mute holds from this frame; with replacement turned off this
    * writes back what the holds replaced. */
   apply_holds(e);
}

static void follow_song(px_engine *e)
{
   uint32_t v;

   /* Frames pass whether or not the song address reads anything. */
   bool silencing = e->silencing > 0;
   if (silencing)
      e->silencing--;
   if (!read_song_value(e, &v))
      return;
   /* A command register reads zero between commands; the last song keeps playing.
    * Pattern matches return false when idle, so a zero song value is valid. */
   if (e->profile.latch && e->profile.pattern_length == 0 && v == 0)
   {
      e->idle = e->have_applied;
      return;
   }

   /* Stopping the game's music changes what it reads as its song: that is still the song applied. */
   if (silencing && e->have_applied && v != e->applied)
   {
      e->have_silenced = true;
      e->silenced      = v;
   }
   if (e->have_silenced && v == e->silenced)
   {
      e->have_candidate = false;
      return;
   }

   if (!e->have_candidate || v != e->candidate)
   {
      e->have_candidate = true;
      e->candidate      = v;
      e->stable_frames  = 0;
   }
   if (e->stable_frames < 0xFFFF)
      e->stable_frames++;

   /* The song applied, read again after its music was stopped or its request cleared, means the game
    * started it again (a life lost): applied again, the replacement starts over and the game's music
    * is stopped again, however the profile stops it. */
   if (e->stable_frames >= e->profile.debounce &&
         (!e->have_applied || v != e->applied || e->have_silenced || e->idle))
      apply_song(e, v, true);
}

void px_engine_reset(px_engine *e)
{
   e->have_candidate = false;
   e->have_applied   = false;
}

bool px_engine_mixing(const px_engine *e)
{
   return e->profile.loaded && e->cfg.enabled;
}

void px_engine_mix_s16(px_engine *e, int16_t *frames, size_t count)
{
   px_mixer_mix(&e->mixer, frames, count, e->cfg.game_volume, e->cfg.music_volume);
}

void px_engine_mix_float(px_engine *e, float *frames, size_t count)
{
   px_mixer_mix_float(&e->mixer, frames, count, e->cfg.game_volume, e->cfg.music_volume);
}

const char *px_engine_mute_override(const px_engine *e, const char *key)
{
   if (!e->muted || !key)
      return NULL;
   for (unsigned i = 0; i < e->profile.mute_count; i++)
      if (!strcmp(e->profile.mute[i].key, key))
         return e->profile.mute[i].value;
   return NULL;
}

/* ---------------------------------------------------------------------------
 * Save states
 * ------------------------------------------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void px_engine_save_state(const px_engine *e, uint8_t *block)
{
   uint64_t pos   = px_mixer_position(&e->mixer);
   uint32_t flags = 0;

   if (e->have_applied)    flags |= 1;
   if (e->muted)           flags |= 2;
   if (e->have_candidate)  flags |= 4;
   if (e->mixer.current.src) flags |= 8;

   memset(block, 0, PX_ENGINE_STATE_SIZE);
   put_u32(block + 0,  STATE_MAGIC);
   put_u32(block + 4,  STATE_VER);
   put_u32(block + 8,  flags);
   put_u32(block + 12, e->applied);
   put_u32(block + 20, (uint32_t)pos);
   put_u32(block + 24, (uint32_t)(pos >> 32));
   put_u32(block + 28, e->candidate);
   put_u32(block + 32, e->stable_frames);
}

bool px_engine_is_state(const uint8_t *block)
{
   return get_u32(block) == STATE_MAGIC;
}

static void restore_music(px_engine *e, bool playing, uint64_t pos)
{
   px_choice c;

   if (!playing || !e->have_applied)
   {
      px_mixer_stop(&e->mixer, 0);
      return;
   }
   resolve_song(e, e->applied, &c);
   if (c.action != PX_ACTION_FILE)
   {
      px_mixer_stop(&e->mixer, 0);
      return;
   }

   if (px_mixer_is_playing(&e->mixer, c.path, c.subtrack))
   {
      /* Rewind and run-ahead load states every frame; only seek on a real jump so
       * the music doesn't stutter. */
      uint64_t cur  = px_mixer_position(&e->mixer);
      uint64_t diff = cur > pos ? cur - pos : pos - cur;
      if (diff > px_mixer_source_rate(&e->mixer) / 2)
         px_mixer_seek(&e->mixer, pos);
      return;
   }

   start_choice(e, &c, 0, pos);
}

bool px_engine_load_state(px_engine *e, const uint8_t *block)
{
   uint32_t flags;

   if (!px_engine_is_state(block) || get_u32(block + 4) != STATE_VER)
      return false;

   flags             = get_u32(block + 8);
   e->have_applied   = flags & 1;
   e->have_candidate = flags & 4;
   e->applied        = get_u32(block + 12);
   e->candidate      = get_u32(block + 28);
   e->stable_frames  = get_u32(block + 32);
   restore_music(e, flags & 8,
         (uint64_t)get_u32(block + 20) | (uint64_t)get_u32(block + 24) << 32);
   set_muted(e, flags & 2);
   return true;
}
