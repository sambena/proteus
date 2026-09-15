/* SPDX-License-Identifier: LGPL-2.1-or-later */
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
#include <time.h>

#include "libretro.h"
#include "engine.h"
#include "options.h"
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

static px_engine engine;
static bool engine_ready;

static struct
{
   bool options_dirty;   /* the inner core has not seen the latest mute state */
   bool option_update;   /* a frontend option change Proteus consumed, owed to the inner core */
   bool audio_enabled;   /* false while the frontend runs hidden frames (run-ahead) */
   int16_t *scratch;
   size_t scratch_frames;
} st;

/* <system>/proteus/proteus.log: RetroArch's own log is often off, and this is where
 * profiles live. */
static char log_path[PX_PATH_MAX];

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
   if (log_path[0])
   {
      static const char *names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
      FILE *f = px_fopen(log_path, "ab");
      if (f)
      {
         char stamp[32];
         time_t now = time(NULL);
         strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
         fprintf(f, "%s [%s] %s\n", stamp, names[level <= RETRO_LOG_ERROR ? level : RETRO_LOG_ERROR], msg);
         fclose(f);
      }
   }
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
 * Engine host
 * ------------------------------------------------------------------------- */

static const uint8_t *host_memory(void *userdata, unsigned id, size_t *size)
{
   (void)userdata;
   *size = inner.ok ? inner.api.get_memory_size(id) : 0;
   return inner.ok ? (const uint8_t*)inner.api.get_memory_data(id) : NULL;
}

static const char *host_option(void *userdata, const char *key)
{
   (void)userdata;
   return px_options_get(key);
}

static void host_log(void *userdata, enum retro_log_level level, const char *msg)
{
   (void)userdata;
   px_log(level, "%s", msg);
}

static void host_notify(void *userdata, const char *msg)
{
   (void)userdata;
   px_notify("%s", msg);
}

static void host_mute(void *userdata, bool muted)
{
   (void)userdata;
   (void)muted;
   /* The inner core re-reads its options, and env_wrap forces the mute values. */
   st.options_dirty = true;
}

/* Cheats: the frontend's own, kept so the inner core's list can be rebuilt, and the profile's
 * ([tap], and [patch] while the original music is muted). Cores turn a cheat off only by a reset. */
#define PX_MAX_CHEATS 256
#define PX_PATCH_INDEX 0x7FFF

static struct
{
   struct { unsigned index; bool enabled; char *code; } list[PX_MAX_CHEATS];
   unsigned count;
   char patch[2 * PX_PATCH_CODE_MAX + 1];
   bool patched;
} cheats;

static void rebuild_cheats(void)
{
   if (!inner.ok)
      return;
   inner.api.cheat_reset();
   for (unsigned i = 0; i < cheats.count; i++)
      if (cheats.list[i].enabled)
         inner.api.cheat_set(cheats.list[i].index, true, cheats.list[i].code);
   if (cheats.patched)
      inner.api.cheat_set(PX_PATCH_INDEX, true, cheats.patch);
}

static void host_patch(void *userdata, const char *code)
{
   (void)userdata;
   if (!code == !cheats.patched && (!code || !strcmp(code, cheats.patch)))
      return;
   cheats.patched = code != NULL;
   snprintf(cheats.patch, sizeof(cheats.patch), "%s", code ? code : "");
   rebuild_cheats();
}

static const char *host_core_file(void *userdata)
{
   const char *base = inner.path + strlen(inner.path);
   (void)userdata;
   while (base > inner.path && base[-1] != '/' && base[-1] != '\\')
      base--;
   return base;
}

static const px_host engine_host = {
   NULL, host_memory, host_option, host_log, host_notify, host_mute, host_patch, host_core_file
};

/* ---------------------------------------------------------------------------
 * Callbacks handed to the inner core
 * ------------------------------------------------------------------------- */

static bool RETRO_CALLCONV env_wrap(unsigned cmd, void *data)
{
   bool result;

   if (px_options_intercept(cmd, data, &result))
      return result;

   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable*)data;
         bool ret = fe_env(cmd, data);
         const char *forced = var ? px_engine_mute_override(&engine, var->key) : NULL;
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
         bool ours    = st.options_dirty || st.option_update;
         st.options_dirty = false;
         st.option_update = false;
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
            px_engine_set_rate(&engine, ((const struct retro_system_av_info*)data)->timing.sample_rate);
         return ret;
      }

      case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
         if (px_engine_loaded(&engine))
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
   return px_engine_mixing(&engine) && st.audio_enabled;
}

static void RETRO_CALLCONV audio_sample_wrap(int16_t left, int16_t right)
{
   int16_t frame[2] = { left, right };
   if (mixing_active())
      px_engine_mix_s16(&engine, frame, 1);
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
   px_engine_mix_s16(&engine, buf, frames);
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

/* Loads the game's profile and publishes its song pickers before the inner core
 * loads, so both see the same options. */
static void load_profile_for(const char *content_path)
{
   char path[PX_PATH_MAX * 2];
   const char *system_dir = NULL;

   st.options_dirty = st.option_update = false;
   st.audio_enabled = true;
   px_engine_unload(&engine);

   if (fe_env)
      fe_env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
   /* Written only when the folder exists: that is where profiles are. */
   if (system_dir && *system_dir)
      snprintf(log_path, sizeof(log_path), "%s/proteus/proteus.log", system_dir);
   if (!px_engine_find_profile(content_path, system_dir, path, sizeof(path)))
      px_log(RETRO_LOG_INFO, "no profile for %s (looked for <ROM name>.proteus.ini next to it and %s/proteus/<ROM name>.ini); passing audio through",
            content_path ? content_path : "this game", system_dir ? system_dir : "<system>");
   else
      px_engine_load(&engine, path);

   px_options_set_profile(px_engine_loaded(&engine) ? &engine.profile : NULL);
   px_options_publish();
   px_engine_read_config(&engine);
}

/* ---------------------------------------------------------------------------
 * libretro API
 * ------------------------------------------------------------------------- */

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;
   fe_env = cb;
   px_options_set_frontend(cb);
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      fe_log = log.log;
   if (!engine_ready)
   {
      px_engine_init(&engine, &engine_host);
      engine_ready = true;
   }
   if (ensure_inner())
      inner.api.set_environment(env_wrap);
   /* Cores that declare options do so above, merged with ours; the rest still
    * need Proteus's own options shown. */
   if (!px_options_inner_declared())
      px_options_publish();
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
   px_engine_unload(&engine);
   if (inner.ok)
      inner.api.deinit();
   /* The frontend's cheats belong to this session; the inner core is gone, so nothing is reset there. */
   for (unsigned i = 0; i < cheats.count; i++)
      free(cheats.list[i].code);
   memset(&cheats, 0, sizeof(cheats));
   free(st.scratch);
   memset(&st, 0, sizeof(st));
   px_options_free();
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
   px_engine_set_rate(&engine, info->timing.sample_rate);
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
   px_engine_reset(&engine);
}

RETRO_API void retro_run(void)
{
   if (!inner.ok)
      return;

   if (px_engine_loaded(&engine))
   {
      int av = 0;
      bool updated = false;

      /* Taking the update flag hides it from the inner core; env_wrap hands it on. */
      if (fe_env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      {
         st.option_update = true;
         px_engine_options_changed(&engine);
      }

      /* Bit 1 is cleared while run-ahead renders frames nobody will hear;
       * the music must not advance during those. */
      st.audio_enabled = !fe_env(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av) || (av & 2);
      /* Detect before running so a mute takes effect in this frame's audio. */
      px_engine_frame(&engine);
   }

   inner.api.run();
}

RETRO_API size_t retro_serialize_size(void)
{
   if (!inner.ok)
      return 0;
   return inner.api.serialize_size() + (px_engine_loaded(&engine) ? PX_ENGINE_STATE_SIZE : 0);
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
   if (!inner.ok)
      return false;
   if (!px_engine_loaded(&engine))
      return inner.api.serialize(data, size);
   if (size < PX_ENGINE_STATE_SIZE || !inner.api.serialize(data, size - PX_ENGINE_STATE_SIZE))
      return false;
   px_engine_save_state(&engine, (uint8_t*)data + size - PX_ENGINE_STATE_SIZE);
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   const uint8_t *block;

   if (!inner.ok)
      return false;

   block = size >= PX_ENGINE_STATE_SIZE ? (const uint8_t*)data + size - PX_ENGINE_STATE_SIZE : NULL;
   if (!px_engine_loaded(&engine) || !block || !px_engine_is_state(block))
      return inner.api.unserialize(data, size);

   return inner.api.unserialize(data, size - PX_ENGINE_STATE_SIZE)
         && px_engine_load_state(&engine, block);
}

RETRO_API void retro_cheat_reset(void)
{
   for (unsigned i = 0; i < cheats.count; i++)
      free(cheats.list[i].code);
   cheats.count = 0;
   rebuild_cheats();
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   unsigned i;
   if (!inner.ok || !code)
      return;
   for (i = 0; i < cheats.count && cheats.list[i].index != index; i++)
      ;
   if (i == cheats.count)
   {
      if (cheats.count >= PX_MAX_CHEATS)
         return;
      cheats.count++;
      cheats.list[i].code = NULL;
   }
   free(cheats.list[i].code);
   cheats.list[i].index   = index;
   cheats.list[i].enabled = enabled;
   cheats.list[i].code    = strdup(code);
   inner.api.cheat_set(index, enabled, code);
}

static void after_load(bool ok)
{
   if (ok)
   {
      struct retro_system_av_info av;
      inner.api.get_system_av_info(&av);
      px_engine_set_rate(&engine, av.timing.sample_rate);
      /* The profile's cheats were set before the game loaded; cores keep cheats per game. */
      rebuild_cheats();
   }
   else
      px_engine_unload(&engine);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   bool ok;
   if (!ensure_inner())
      return false;
   load_profile_for(game ? game->path : NULL);
   ok = inner.api.load_game(game);
   after_load(ok);
   return ok;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   bool ok;
   if (!ensure_inner())
      return false;
   load_profile_for(num > 0 && info ? info[0].path : NULL);
   ok = inner.api.load_game_special(type, info, num);
   after_load(ok);
   return ok;
}

RETRO_API void retro_unload_game(void)
{
   if (inner.ok)
      inner.api.unload_game();
   px_engine_unload(&engine);
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
