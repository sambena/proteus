/* Proteus Retune: a libretro core that wraps another core and swaps its music.
 *
 * The wrapper is installed as proteus_<core>_libretro.<ext> next to
 * <core>_libretro.<ext>. It forwards the libretro API to that core while it
 *   - watches a RAM address named by a per-game profile to detect song changes,
 *   - forces core options (per-channel volumes) that mute the original music,
 *   - mixes replacement music into the core's audio output.
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#include "libretro.h"
#include "music.h"
#include "profile.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
typedef HMODULE px_lib;
#else
#include <dlfcn.h>
typedef void *px_lib;
#endif

#define PX_NAME        "Proteus Retune"
#define PX_PREFIX      "proteus_"
#define PX_STATE_MAGIC 0x4E545250u /* "PRTN" */
#define PX_STATE_VER   1u
#define PX_STATE_SIZE  48u

struct inner_api
{
   void (*set_environment)(retro_environment_t);
   void (*set_video_refresh)(retro_video_refresh_t);
   void (*set_audio_sample)(retro_audio_sample_t);
   void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*set_input_poll)(retro_input_poll_t);
   void (*set_input_state)(retro_input_state_t);
   void (*init)(void);
   void (*deinit)(void);
   unsigned (*api_version)(void);
   void (*get_system_info)(struct retro_system_info*);
   void (*get_system_av_info)(struct retro_system_av_info*);
   void (*set_controller_port_device)(unsigned, unsigned);
   void (*reset)(void);
   void (*run)(void);
   size_t (*serialize_size)(void);
   bool (*serialize)(void*, size_t);
   bool (*unserialize)(const void*, size_t);
   void (*cheat_reset)(void);
   void (*cheat_set)(unsigned, bool, const char*);
   bool (*load_game)(const struct retro_game_info*);
   bool (*load_game_special)(unsigned, const struct retro_game_info*, size_t);
   void (*unload_game)(void);
   unsigned (*get_region)(void);
   void *(*get_memory_data)(unsigned);
   size_t (*get_memory_size)(unsigned);
};

static struct
{
   bool tried;
   bool ok;
   px_lib lib;
   char path[PX_PATH_MAX];
   char error[PX_PATH_MAX + 128];
   char library_name[256];
   struct inner_api api;
} inner;

/* Frontend callbacks, kept so they can be re-applied if the inner core reloads. */
static retro_environment_t        fe_env;
static retro_video_refresh_t      fe_video;
static retro_audio_sample_t       fe_audio_sample;
static retro_audio_sample_batch_t fe_audio_batch;
static retro_input_poll_t         fe_input_poll;
static retro_input_state_t        fe_input_state;
static retro_log_printf_t         fe_log;

static px_profile profile;
static px_mixer   mixer;

static struct
{
   bool muted;           /* mute options are being forced */
   bool options_dirty;   /* the inner core has not seen the latest mute state */
   bool audio_enabled;   /* false while the frontend runs hidden frames (run-ahead) */
   bool have_candidate;
   uint32_t candidate;
   unsigned stable_frames;
   bool have_applied;
   uint32_t applied;
   bool warned_memory;
   int16_t *scratch;
   size_t scratch_frames;
} st;

static void px_log(enum retro_log_level level, const char *fmt, ...)
{
   char msg[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   if (fe_log)
      fe_log(level, "[Proteus] %s\n", msg);
   else
      fprintf(stderr, "[Proteus] %s\n", msg);
}

static void px_notify(const char *fmt, ...)
{
   char msg[256];
   struct retro_message m;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   m.msg    = msg;
   m.frames = 180;
   if (fe_env)
      fe_env(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
}

/* ---------------------------------------------------------------------------
 * Inner core loading
 * ------------------------------------------------------------------------- */

static bool self_path(char *out, size_t n)
{
#ifdef _WIN32
   HMODULE self = NULL;
   wchar_t wpath[PX_PATH_MAX];
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void*)&self_path, &self))
      return false;
   if (!GetModuleFileNameW(self, wpath, PX_PATH_MAX))
      return false;
   return WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out, (int)n, NULL, NULL) > 0;
#else
   Dl_info info;
   if (!dladdr((void*)&self_path, &info) || !info.dli_fname)
      return false;
   snprintf(out, n, "%s", info.dli_fname);
   return true;
#endif
}

static bool load_symbol(void **dst, const char *name)
{
#ifdef _WIN32
   *dst = (void*)GetProcAddress(inner.lib, name);
#else
   *dst = dlsym(inner.lib, name);
#endif
   if (!*dst)
      snprintf(inner.error, sizeof(inner.error), "%s is missing %s", inner.path, name);
   return *dst != NULL;
}

static void apply_callbacks(void);

static bool ensure_inner(void)
{
   char self[PX_PATH_MAX];
   char dir[PX_PATH_MAX];
   const char *base;
   bool ok = true;

   if (inner.tried)
      return inner.ok;
   inner.tried = true;
   inner.ok    = false;

   if (!self_path(self, sizeof(self)))
   {
      snprintf(inner.error, sizeof(inner.error), "cannot determine own path");
      goto fail;
   }

   px_path_dir(self, dir, sizeof(dir));
   base = self + strlen(dir);
   if (*base == '/' || *base == '\\')
      base++;

   /* proteus_snes9x_libretro.dll -> snes9x_libretro.dll */
   if (strncasecmp(base, PX_PREFIX, strlen(PX_PREFIX)) != 0
         || strncasecmp(base + strlen(PX_PREFIX), "libretro", 8) == 0)
   {
      snprintf(inner.error, sizeof(inner.error),
            "rename %s to " PX_PREFIX "<core>_libretro so Proteus knows which core to wrap", base);
      goto fail;
   }
   px_path_join(dir, base + strlen(PX_PREFIX), inner.path, sizeof(inner.path));

#ifdef _WIN32
   {
      wchar_t *w = px_utf8_to_wide(inner.path);
      inner.lib = w ? LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
      free(w);
   }
#else
   inner.lib = dlopen(inner.path, RTLD_LAZY | RTLD_LOCAL);
#endif
   if (!inner.lib)
   {
      snprintf(inner.error, sizeof(inner.error), "cannot load inner core %s", inner.path);
      goto fail;
   }

#define SYM(field, name) ok = ok && load_symbol((void**)&inner.api.field, name)
   SYM(set_environment, "retro_set_environment");
   SYM(set_video_refresh, "retro_set_video_refresh");
   SYM(set_audio_sample, "retro_set_audio_sample");
   SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
   SYM(set_input_poll, "retro_set_input_poll");
   SYM(set_input_state, "retro_set_input_state");
   SYM(init, "retro_init");
   SYM(deinit, "retro_deinit");
   SYM(api_version, "retro_api_version");
   SYM(get_system_info, "retro_get_system_info");
   SYM(get_system_av_info, "retro_get_system_av_info");
   SYM(set_controller_port_device, "retro_set_controller_port_device");
   SYM(reset, "retro_reset");
   SYM(run, "retro_run");
   SYM(serialize_size, "retro_serialize_size");
   SYM(serialize, "retro_serialize");
   SYM(unserialize, "retro_unserialize");
   SYM(cheat_reset, "retro_cheat_reset");
   SYM(cheat_set, "retro_cheat_set");
   SYM(load_game, "retro_load_game");
   SYM(load_game_special, "retro_load_game_special");
   SYM(unload_game, "retro_unload_game");
   SYM(get_region, "retro_get_region");
   SYM(get_memory_data, "retro_get_memory_data");
   SYM(get_memory_size, "retro_get_memory_size");
#undef SYM

   if (!ok)
   {
#ifdef _WIN32
      FreeLibrary(inner.lib);
#else
      dlclose(inner.lib);
#endif
      inner.lib = NULL;
      goto fail;
   }

   inner.ok = true;
   apply_callbacks();
   return true;

fail:
   px_log(RETRO_LOG_ERROR, "%s", inner.error);
   return false;
}

static void unload_inner(void)
{
   if (inner.lib)
   {
#ifdef _WIN32
      FreeLibrary(inner.lib);
#else
      dlclose(inner.lib);
#endif
   }
   memset(&inner, 0, sizeof(inner));
}

/* ---------------------------------------------------------------------------
 * Callbacks handed to the inner core
 * ------------------------------------------------------------------------- */

static const char *mute_override(const char *key)
{
   if (!st.muted || !key)
      return NULL;
   for (unsigned i = 0; i < profile.mute_count; i++)
      if (!strcmp(profile.mute[i].key, key))
         return profile.mute[i].value;
   return NULL;
}

static void update_sample_rate(double rate)
{
   if (rate > 0.0)
      px_mixer_set_rate(&mixer, rate);
}

static bool RETRO_CALLCONV env_wrap(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable*)data;
         bool ret = fe_env(cmd, data);
         const char *forced = var ? mute_override(var->key) : NULL;
         if (forced)
         {
            var->value = forced;
            return true;
         }
         return ret;
      }

      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      {
         bool updated = false;
         bool ret     = fe_env(cmd, &updated);
         bool ours    = st.options_dirty;
         st.options_dirty = false;
         if (data)
            *(bool*)data = (ret && updated) || ours;
         return ret || ours;
      }

      case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
         if (data)
            *(const char**)data = inner.path;
         return true;

      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      {
         bool ret = fe_env(cmd, data);
         if (ret && data)
            update_sample_rate(((const struct retro_system_av_info*)data)->timing.sample_rate);
         return ret;
      }

      case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
         if (profile.loaded)
            px_log(RETRO_LOG_WARN, "this core renders audio asynchronously; music replacement may glitch");
         return fe_env(cmd, data);

      default:
         return fe_env ? fe_env(cmd, data) : false;
   }
}

static int16_t *scratch_buffer(size_t frames)
{
   if (st.scratch_frames < frames)
   {
      int16_t *p = (int16_t*)realloc(st.scratch, frames * 2 * sizeof(int16_t));
      if (!p)
         return NULL;
      st.scratch        = p;
      st.scratch_frames = frames;
   }
   return st.scratch;
}

static bool mixing_active(void)
{
   return profile.loaded && st.audio_enabled;
}

static void RETRO_CALLCONV audio_sample_wrap(int16_t left, int16_t right)
{
   int16_t frame[2] = { left, right };
   if (mixing_active())
      px_mixer_mix(&mixer, frame, 1, profile.game_volume, profile.music_volume);
   if (fe_audio_sample)
      fe_audio_sample(frame[0], frame[1]);
}

static size_t RETRO_CALLCONV audio_batch_wrap(const int16_t *data, size_t frames)
{
   int16_t *buf;
   if (!fe_audio_batch)
      return frames;
   if (!mixing_active() || !(buf = scratch_buffer(frames)))
      return fe_audio_batch(data, frames);
   memcpy(buf, data, frames * 2 * sizeof(int16_t));
   px_mixer_mix(&mixer, buf, frames, profile.game_volume, profile.music_volume);
   return fe_audio_batch(buf, frames);
}

static void apply_callbacks(void)
{
   if (!inner.ok)
      return;
   if (fe_env)
      inner.api.set_environment(env_wrap);
   if (fe_video)
      inner.api.set_video_refresh(fe_video);
   if (fe_audio_sample)
      inner.api.set_audio_sample(audio_sample_wrap);
   if (fe_audio_batch)
      inner.api.set_audio_sample_batch(audio_batch_wrap);
   if (fe_input_poll)
      inner.api.set_input_poll(fe_input_poll);
   if (fe_input_state)
      inner.api.set_input_state(fe_input_state);
}

/* ---------------------------------------------------------------------------
 * Song detection and music control
 * ------------------------------------------------------------------------- */

static void set_muted(bool muted)
{
   if (st.muted == muted)
      return;
   st.muted         = muted;
   st.options_dirty = true;
}

static unsigned fade_frames(void)
{
   return (unsigned)(mixer.out_rate * profile.crossfade_ms / 1000.0);
}

static void start_track(int index, unsigned fade, uint64_t start_frame)
{
   const px_track *t = &profile.tracks[index];
   char err[PX_PATH_MAX + 64];

   /* Several song values may share a file; keep it playing without a restart. */
   if (px_mixer_current_id(&mixer) >= 0
         && !strcmp(profile.tracks[px_mixer_current_id(&mixer)].path, t->path))
   {
      mixer.current.id = index;
      set_muted(true);
      return;
   }

   if (!px_mixer_play(&mixer, index, t->path, t->loop, t->loop_start, t->volume,
            fade, start_frame, err, sizeof(err)))
   {
      px_log(RETRO_LOG_ERROR, "%s", err);
      px_mixer_stop(&mixer, fade);
      set_muted(false);
      return;
   }
   set_muted(true);
}

static void apply_song(uint32_t value)
{
   int index = px_profile_find(&profile, value);
   px_action action = index >= 0 ? profile.tracks[index].action : profile.unmapped;

   st.have_applied = true;
   st.applied      = value;

   if (profile.log_songs)
   {
      static const char *names[] = { "", "silence", "original", "keep" };
      px_log(RETRO_LOG_INFO, "song value 0x%X -> %s%s", (unsigned)value,
            action == PX_ACTION_FILE ? profile.tracks[index].path : names[action],
            index >= 0 ? "" : " (unmapped)");
      px_notify("Proteus: song 0x%X%s", (unsigned)value, index >= 0 ? "" : " (unmapped)");
   }

   switch (action)
   {
      case PX_ACTION_FILE:
         start_track(index, fade_frames(), 0);
         break;
      case PX_ACTION_SILENCE:
         px_mixer_stop(&mixer, fade_frames());
         set_muted(true);
         break;
      case PX_ACTION_ORIGINAL:
         px_mixer_stop(&mixer, fade_frames());
         set_muted(false);
         break;
      case PX_ACTION_KEEP:
         break;
   }
}

static bool read_song_value(uint32_t *out)
{
   const uint8_t *data = (const uint8_t*)inner.api.get_memory_data(profile.memory_id);
   size_t size         = inner.api.get_memory_size(profile.memory_id);
   uint32_t v          = 0;

   if (!data || (uint64_t)profile.address + profile.size > size)
   {
      if (!st.warned_memory)
      {
         st.warned_memory = true;
         px_log(RETRO_LOG_WARN, "song address 0x%X is outside the core's memory (size 0x%X)",
               (unsigned)profile.address, (unsigned)size);
      }
      return false;
   }

   for (unsigned i = 0; i < profile.size; i++)
      v |= (uint32_t)data[profile.address + i] << (8 * i);
   *out = v & profile.mask;
   return true;
}

static void detect_song(void)
{
   uint32_t v;
   if (!read_song_value(&v))
      return;

   if (!st.have_candidate || v != st.candidate)
   {
      st.have_candidate = true;
      st.candidate      = v;
      st.stable_frames  = 0;
   }
   if (st.stable_frames < 0xFFFF)
      st.stable_frames++;

   if (st.stable_frames >= profile.debounce && (!st.have_applied || v != st.applied))
      apply_song(v);
}

static bool find_profile(const char *content_path, char *out, size_t n)
{
   char path[PX_PATH_MAX];
   char dir[PX_PATH_MAX];
   char stem[256];
   const char *system_dir = NULL;
   const char *hash;

   if (!content_path || !*content_path)
      return false;

   /* "archive.zip#game.sfc": look beside the archive, named after the inner file. */
   snprintf(path, sizeof(path), "%s", content_path);
   hash = strchr(path, '#');
   if (hash)
   {
      px_path_stem(hash + 1, stem, sizeof(stem));
      path[hash - path] = '\0';
   }
   else
      px_path_stem(path, stem, sizeof(stem));
   px_path_dir(path, dir, sizeof(dir));

   snprintf(out, n, "%s/%s.proteus.ini", dir, stem);
   if (px_file_exists(out))
      return true;

   if (fe_env && fe_env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
   {
      snprintf(out, n, "%s/proteus/%s.ini", system_dir, stem);
      if (px_file_exists(out))
         return true;
   }
   return false;
}

static void load_profile_for(const char *content_path)
{
   char path[PX_PATH_MAX * 2];
   char err[PX_PATH_MAX + 128];

   int16_t *scratch = st.scratch;
   size_t scratch_frames = st.scratch_frames;

   px_mixer_free(&mixer);
   px_mixer_init(&mixer);
   memset(&st, 0, sizeof(st));
   st.scratch        = scratch;
   st.scratch_frames = scratch_frames;
   st.audio_enabled  = true;
   profile.loaded = false;

   if (!find_profile(content_path, path, sizeof(path)))
   {
      px_log(RETRO_LOG_INFO, "no profile for this game, passing audio through");
      return;
   }
   if (!px_profile_load(&profile, path, err, sizeof(err)))
   {
      px_log(RETRO_LOG_ERROR, "%s: %s", path, err);
      px_notify("Proteus: profile error, see log");
      return;
   }
   px_log(RETRO_LOG_INFO, "loaded profile %s (%u tracks)", path, profile.track_count);
}

/* ---------------------------------------------------------------------------
 * libretro API
 * ------------------------------------------------------------------------- */

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;
   fe_env = cb;
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      fe_log = log.log;
   if (ensure_inner())
      inner.api.set_environment(env_wrap);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
   fe_video = cb;
   if (ensure_inner())
      inner.api.set_video_refresh(cb);
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
   fe_audio_sample = cb;
   if (ensure_inner())
      inner.api.set_audio_sample(audio_sample_wrap);
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   fe_audio_batch = cb;
   if (ensure_inner())
      inner.api.set_audio_sample_batch(audio_batch_wrap);
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
   fe_input_poll = cb;
   if (ensure_inner())
      inner.api.set_input_poll(cb);
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
   fe_input_state = cb;
   if (ensure_inner())
      inner.api.set_input_state(cb);
}

RETRO_API void retro_init(void)
{
   if (ensure_inner())
      inner.api.init();
}

RETRO_API void retro_deinit(void)
{
   if (inner.ok)
      inner.api.deinit();
   px_mixer_free(&mixer);
   free(st.scratch);
   memset(&st, 0, sizeof(st));
   profile.loaded = false;
   /* Unload so the next session starts the inner core with fresh static state. */
   unload_inner();
}

RETRO_API unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   if (!ensure_inner())
   {
      memset(info, 0, sizeof(*info));
      info->library_name     = PX_NAME " (no inner core)";
      info->library_version  = "0.1.0";
      info->valid_extensions = "";
      return;
   }
   inner.api.get_system_info(info);
   snprintf(inner.library_name, sizeof(inner.library_name), PX_NAME " (%s)",
         info->library_name ? info->library_name : "unknown");
   info->library_name = inner.library_name;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   if (!inner.ok)
   {
      memset(info, 0, sizeof(*info));
      return;
   }
   inner.api.get_system_av_info(info);
   update_sample_rate(info->timing.sample_rate);
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   if (inner.ok)
      inner.api.set_controller_port_device(port, device);
}

RETRO_API void retro_reset(void)
{
   if (!inner.ok)
      return;
   inner.api.reset();
   st.have_candidate = false;
   st.have_applied   = false;
}

RETRO_API void retro_run(void)
{
   if (!inner.ok)
      return;

   if (profile.loaded)
   {
      int av = 0;
      /* Bit 1 is cleared while run-ahead renders frames nobody will hear;
       * the music must not advance during those. */
      st.audio_enabled = !fe_env(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av) || (av & 2);
      /* Detect before running so a mute takes effect in this frame's audio. */
      detect_song();
   }

   inner.api.run();
}

static void put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

RETRO_API size_t retro_serialize_size(void)
{
   if (!inner.ok)
      return 0;
   return inner.api.serialize_size() + (profile.loaded ? PX_STATE_SIZE : 0);
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
   uint8_t *block;
   uint64_t pos;
   uint32_t flags = 0;

   if (!inner.ok)
      return false;
   if (!profile.loaded)
      return inner.api.serialize(data, size);
   if (size < PX_STATE_SIZE || !inner.api.serialize(data, size - PX_STATE_SIZE))
      return false;

   if (st.have_applied)   flags |= 1;
   if (st.muted)          flags |= 2;
   if (st.have_candidate) flags |= 4;

   pos   = px_mixer_position(&mixer);
   block = (uint8_t*)data + size - PX_STATE_SIZE;
   memset(block, 0, PX_STATE_SIZE);
   put_u32(block + 0,  PX_STATE_MAGIC);
   put_u32(block + 4,  PX_STATE_VER);
   put_u32(block + 8,  flags);
   put_u32(block + 12, st.applied);
   put_u32(block + 16, (uint32_t)px_mixer_current_id(&mixer));
   put_u32(block + 20, (uint32_t)pos);
   put_u32(block + 24, (uint32_t)(pos >> 32));
   put_u32(block + 28, st.candidate);
   put_u32(block + 32, st.stable_frames);
   return true;
}

static void restore_music(int index, uint64_t pos)
{
   int current = px_mixer_current_id(&mixer);

   if (index < 0 || index >= (int)profile.track_count
         || profile.tracks[index].action != PX_ACTION_FILE)
   {
      px_mixer_stop(&mixer, 0);
      return;
   }

   if (current >= 0 && !strcmp(profile.tracks[current].path, profile.tracks[index].path))
   {
      /* Rewind and run-ahead load states every frame; only seek on a real jump so
       * the music doesn't stutter. */
      uint64_t cur  = px_mixer_position(&mixer);
      uint64_t diff = cur > pos ? cur - pos : pos - cur;
      mixer.current.id = index;
      if (diff > px_mixer_source_rate(&mixer) / 2)
         px_mixer_seek(&mixer, pos);
      return;
   }

   start_track(index, 0, pos);
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   const uint8_t *block;
   uint32_t flags;

   if (!inner.ok)
      return false;

   block = size >= PX_STATE_SIZE ? (const uint8_t*)data + size - PX_STATE_SIZE : NULL;
   if (!profile.loaded || !block || get_u32(block) != PX_STATE_MAGIC)
      return inner.api.unserialize(data, size);

   if (get_u32(block + 4) != PX_STATE_VER
         || !inner.api.unserialize(data, size - PX_STATE_SIZE))
      return false;

   flags             = get_u32(block + 8);
   st.have_applied   = flags & 1;
   st.have_candidate = flags & 4;
   st.applied        = get_u32(block + 12);
   st.candidate      = get_u32(block + 28);
   st.stable_frames  = get_u32(block + 32);
   restore_music((int)get_u32(block + 16),
         (uint64_t)get_u32(block + 20) | (uint64_t)get_u32(block + 24) << 32);
   set_muted(flags & 2);
   return true;
}

RETRO_API void retro_cheat_reset(void)
{
   if (inner.ok)
      inner.api.cheat_reset();
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   if (inner.ok)
      inner.api.cheat_set(index, enabled, code);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   bool ok;
   if (!ensure_inner())
      return false;

   load_profile_for(game ? game->path : NULL);
   ok = inner.api.load_game(game);
   if (ok)
   {
      struct retro_system_av_info av;
      inner.api.get_system_av_info(&av);
      update_sample_rate(av.timing.sample_rate);
   }
   else
      profile.loaded = false;
   return ok;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   bool ok;
   if (!ensure_inner())
      return false;

   load_profile_for(num > 0 && info ? info[0].path : NULL);
   ok = inner.api.load_game_special(type, info, num);
   if (ok)
   {
      struct retro_system_av_info av;
      inner.api.get_system_av_info(&av);
      update_sample_rate(av.timing.sample_rate);
   }
   else
      profile.loaded = false;
   return ok;
}

RETRO_API void retro_unload_game(void)
{
   if (inner.ok)
      inner.api.unload_game();
   px_mixer_free(&mixer);
   profile.loaded = false;
   st.muted       = false;
}

RETRO_API unsigned retro_get_region(void)
{
   return inner.ok ? inner.api.get_region() : RETRO_REGION_NTSC;
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
   return inner.ok ? inner.api.get_memory_data(id) : NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
   return inner.ok ? inner.api.get_memory_size(id) : 0;
}
