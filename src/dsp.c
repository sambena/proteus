/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Proteus as a RetroArch audio DSP plugin: works with any core, unchanged.
 *
 * RetroArch hands DSP plugins nothing but audio, so the plugin finds the rest
 * itself from inside RetroArch's process:
 *   - the running core is the loaded module exporting the libretro API; its
 *     retro_get_memory_data gives the song address,
 *   - the running game is the newest entry in RetroArch's content history.
 * It cannot change the core's options while the game runs, so it saves the profile's
 * [mute] options as RetroArch game options, which mute the original music from the next
 * time the game is loaded (the wrapper core mutes song by song instead).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#include "libretro_dspfilter.h"
#include "engine.h"
#include "game_options.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#define DSP_EXPORT __declspec(dllexport)
#else
#define DSP_EXPORT __attribute__((visibility("default")))
#endif

typedef void *(*core_memory_data_t)(unsigned);
typedef size_t (*core_memory_size_t)(unsigned);

typedef struct
{
   px_engine engine;
   float rate;
   unsigned frames_since_poll;
   bool polled;

   char system_dir[PX_PATH_MAX];
   char history_path[PX_PATH_MAX];
   char log_path[PX_PATH_MAX];
   char config_dir[PX_PATH_MAX];
   bool game_options; /* RetroArch loads per-game core options */

#ifdef _WIN32
   HMODULE core;
#endif
   char core_path[PX_PATH_MAX];
   core_memory_data_t memory_data;
   core_memory_size_t memory_size;
   bool standby; /* the Proteus wrapper core is running and does the work */

   struct stat history_stat;
   char history_core[PX_PATH_MAX];
   char content[PX_PATH_MAX];        /* newest history entry */
   char loaded_content[PX_PATH_MAX]; /* content whose profile is loaded */
} proteus_dsp;

/* ---------------------------------------------------------------------------
 * Engine host
 * ------------------------------------------------------------------------- */

static void host_log(void *userdata, enum retro_log_level level, const char *msg)
{
   static const char *names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
   proteus_dsp *d = (proteus_dsp*)userdata;
   FILE *f = px_fopen(d->log_path, "ab");
   time_t now = time(NULL);
   char stamp[32];

   if (!f)
      return;
   strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
   fprintf(f, "%s [%s] %s\n", stamp, names[level <= RETRO_LOG_ERROR ? level : RETRO_LOG_ERROR], msg);
   fclose(f);
}

static void dlog(proteus_dsp *d, enum retro_log_level level, const char *fmt, ...)
{
   char msg[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   host_log(d, level, msg);
}

static const uint8_t *host_memory(void *userdata, unsigned id, size_t *size)
{
   proteus_dsp *d = (proteus_dsp*)userdata;
   *size = d->memory_size ? d->memory_size(id) : 0;
   return d->memory_data ? (const uint8_t*)d->memory_data(id) : NULL;
}

static const char *base_name(const char *path);

/* A [patch] goes straight to the running core's cheat list. RetroArch runs DSP plugins with the
 * audio a core hands over after emulating its frame, so the patch takes effect from the next
 * frame. Cores can only turn a cheat off by resetting all of them: cheats set in RetroArch's own
 * menu are lost until they are applied again. */
#define DSP_PATCH_INDEX 0x7FFF
static void host_patch(void *userdata, const char *code)
{
   proteus_dsp *d = (proteus_dsp*)userdata;
#ifdef _WIN32
   typedef void (*cheat_reset_t)(void);
   typedef void (*cheat_set_t)(unsigned, bool, const char*);
   cheat_reset_t reset = d->core ? (cheat_reset_t)(void*)GetProcAddress(d->core, "retro_cheat_reset") : NULL;
   cheat_set_t set = d->core ? (cheat_set_t)(void*)GetProcAddress(d->core, "retro_cheat_set") : NULL;
   if (!reset || !set)
      return;
   reset();
   if (code)
      set(DSP_PATCH_INDEX, true, code);
#else
   (void)d;
   (void)code;
#endif
}

static const char *host_core_file(void *userdata)
{
   proteus_dsp *d = (proteus_dsp*)userdata;
   return base_name(d->core_path);
}

/* ---------------------------------------------------------------------------
 * Finding RetroArch's folders
 * ------------------------------------------------------------------------- */

static bool exe_dir(char *out, size_t n)
{
#ifdef _WIN32
   wchar_t w[PX_PATH_MAX];
   char path[PX_PATH_MAX];
   if (!GetModuleFileNameW(NULL, w, PX_PATH_MAX)
         || WideCharToMultiByte(CP_UTF8, 0, w, -1, path, sizeof(path), NULL, NULL) <= 0)
      return false;
   px_path_dir(path, out, n);
   return true;
#else
   (void)out; (void)n;
   return false;
#endif
}

/* Reads `key = "value"` from retroarch.cfg; ":" at the start means RetroArch's folder. */
static bool cfg_value(const char *cfg, const char *base, const char *key, char *out, size_t n)
{
   char line[2048];
   size_t key_len = strlen(key);
   FILE *f = px_fopen(cfg, "rb");
   bool found = false;

   if (!f)
      return false;
   while (!found && fgets(line, sizeof(line), f))
   {
      char *v, *end;
      if (strncmp(line, key, key_len) || (line[key_len] != ' ' && line[key_len] != '='))
         continue;
      if (!(v = strchr(line + key_len, '"')) || !(end = strchr(v + 1, '"')))
         continue;
      *end = '\0';
      v++;
      if (!*v)
         break;
      if (v[0] == ':')
         snprintf(out, n, "%s%s", base, v + 1);
      else
         snprintf(out, n, "%s", v);
      found = true;
   }
   fclose(f);
   return found;
}

static void config_string(const struct dspfilter_config *config, void *userdata,
      const char *key, char *out, size_t n)
{
   char *value = NULL;
   if (config->get_string(userdata, key, &value, "") && value && *value)
      snprintf(out, n, "%s", value);
   if (value)
      config->free(value);
}

static void find_folders(proteus_dsp *d, const struct dspfilter_config *config, void *userdata)
{
   char base[PX_PATH_MAX - 64] = "", cfg[PX_PATH_MAX], playlists[PX_PATH_MAX - 64], logs[PX_PATH_MAX - 64];
   char flag[16];

   d->game_options = true;
   if (exe_dir(base, sizeof(base)))
   {
      snprintf(cfg, sizeof(cfg), "%s/retroarch.cfg", base);
      if (!cfg_value(cfg, base, "rgui_config_directory", d->config_dir, sizeof(d->config_dir)))
         snprintf(d->config_dir, sizeof(d->config_dir), "%s/config", base);
      if (cfg_value(cfg, base, "game_specific_options", flag, sizeof(flag)))
         d->game_options = strcmp(flag, "false") != 0;
      if (!cfg_value(cfg, base, "system_directory", d->system_dir, sizeof(d->system_dir)))
         snprintf(d->system_dir, sizeof(d->system_dir), "%s/system", base);

      if (!cfg_value(cfg, base, "content_history_path", d->history_path, sizeof(d->history_path)))
      {
         if (!cfg_value(cfg, base, "playlist_directory", playlists, sizeof(playlists)))
            snprintf(playlists, sizeof(playlists), "%s/playlists", base);
         snprintf(d->history_path, sizeof(d->history_path), "%s/builtin/content_history.lpl", playlists);
         if (!px_file_exists(d->history_path))
            snprintf(d->history_path, sizeof(d->history_path), "%s/content_history.lpl", playlists);
      }

      if (!cfg_value(cfg, base, "log_dir", logs, sizeof(logs)))
         snprintf(logs, sizeof(logs), "%s/logs", base);
      snprintf(d->log_path, sizeof(d->log_path), "%s/proteus.log", logs);
   }

   /* proteus_system_dir, proteus_history and proteus_log in the .dsp file win. */
   config_string(config, userdata, "system_dir", d->system_dir, sizeof(d->system_dir));
   config_string(config, userdata, "history", d->history_path, sizeof(d->history_path));
   config_string(config, userdata, "log", d->log_path, sizeof(d->log_path));
}

/* ---------------------------------------------------------------------------
 * Finding the running game and core
 * ------------------------------------------------------------------------- */

/* Copies the JSON string following `"key":` at or after `from` into out. */
static const char *json_string(const char *from, const char *key, char *out, size_t n)
{
   char needle[64];
   const char *p;
   size_t len = 0;

   snprintf(needle, sizeof(needle), "\"%s\"", key);
   if (!(p = strstr(from, needle)))
      return NULL;
   p += strlen(needle);
   while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n')
      p++;
   if (*p++ != '"')
      return NULL;
   for (; *p && *p != '"' && len + 1 < n; p++)
   {
      if (*p == '\\' && p[1])
         p++;
      out[len++] = *p;
   }
   out[len] = '\0';
   return p;
}

/* Reads the newest history entry: RetroArch writes it when content starts. */
static void read_history(proteus_dsp *d)
{
   struct stat s;
   FILE *f;
   char *buf;
   size_t len;
   const char *items;

   if (stat(d->history_path, &s) != 0)
      return;
   if (s.st_mtime == d->history_stat.st_mtime && s.st_size == d->history_stat.st_size)
      return;
   d->history_stat = s;

   if (!(f = px_fopen(d->history_path, "rb")))
      return;
   buf = (char*)malloc(65536);
   len = buf ? fread(buf, 1, 65535, f) : 0;
   fclose(f);
   if (!buf)
      return;
   buf[len] = '\0';

   d->content[0] = d->history_core[0] = '\0';
   if ((items = strstr(buf, "\"items\"")))
   {
      const char *after = json_string(items, "path", d->content, sizeof(d->content));
      if (after)
         json_string(items, "core_path", d->history_core, sizeof(d->history_core));
   }
   free(buf);
}

static const char *base_name(const char *path)
{
   const char *name = path;
   for (const char *p = path; *p; p++)
      if (*p == '/' || *p == '\\')
         name = p + 1;
   return name;
}

#ifdef _WIN32
/* Looks through RetroArch's loaded modules for libretro cores. */
static void find_core(proteus_dsp *d)
{
   HMODULE modules[1024];
   HMODULE self = NULL;
   DWORD needed;
   char temp[PX_PATH_MAX] = "";
   wchar_t wtemp[PX_PATH_MAX];
   HMODULE best = NULL;
   char best_path[PX_PATH_MAX] = "";

   d->standby = false;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
         (LPCWSTR)(void*)&find_core, &self);
   if (GetTempPathW(PX_PATH_MAX, wtemp))
      WideCharToMultiByte(CP_UTF8, 0, wtemp, -1, temp, sizeof(temp), NULL, NULL);
   if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
      return;

   for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++)
   {
      wchar_t w[PX_PATH_MAX];
      char path[PX_PATH_MAX];
      const char *name;

      if (modules[i] == self || !GetProcAddress(modules[i], "retro_run")
            || !GetProcAddress(modules[i], "retro_get_memory_data"))
         continue;
      if (!GetModuleFileNameW(modules[i], w, PX_PATH_MAX)
            || WideCharToMultiByte(CP_UTF8, 0, w, -1, path, sizeof(path), NULL, NULL) <= 0)
         continue;
      name = base_name(path);
      if (!strncasecmp(name, "proteus_", 8))
      {
         d->standby = true;
         return;
      }
      /* Run-ahead's second instance runs from a copy in the temp folder. */
      if (temp[0] && !strncasecmp(path, temp, strlen(temp)))
         continue;
      if (!best || (d->history_core[0] && !strcasecmp(name, base_name(d->history_core))))
      {
         best = modules[i];
         snprintf(best_path, sizeof(best_path), "%s", path);
      }
   }

   if (best != d->core)
   {
      d->loaded_content[0] = '\0';
      d->core        = best;
      d->memory_data = best ? (core_memory_data_t)(void*)GetProcAddress(best, "retro_get_memory_data") : NULL;
      d->memory_size = best ? (core_memory_size_t)(void*)GetProcAddress(best, "retro_get_memory_size") : NULL;
      snprintf(d->core_path, sizeof(d->core_path), "%s", best_path);
   }
}

/* RetroArch unloads cores on the thread that runs the DSP, so this check cannot race. */
static bool core_still_loaded(proteus_dsp *d)
{
   HMODULE h;
   if (!d->core || !d->memory_data)
      return false;
   return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               (LPCWSTR)(void*)d->memory_data, &h)
         && h == d->core
         && (core_memory_data_t)(void*)GetProcAddress(h, "retro_get_memory_data") == d->memory_data;
}
#else
static void find_core(proteus_dsp *d) { d->standby = false; }
static bool core_still_loaded(proteus_dsp *d) { (void)d; return false; }
#endif

static void drop_core(proteus_dsp *d)
{
#ifdef _WIN32
   d->core = NULL;
#endif
   d->memory_data = NULL;
   d->memory_size = NULL;
   d->core_path[0] = '\0';
   if (px_engine_loaded(&d->engine))
      px_engine_unload(&d->engine);
   d->loaded_content[0] = '\0';
}

/* Saves the profile's [mute] options as the game's core options. */
static void save_mutes(proteus_dsp *d)
{
#ifdef _WIN32
   typedef void (*system_info_t)(struct retro_system_info *);
   system_info_t get_info = d->core ? (system_info_t)(void*)GetProcAddress(d->core, "retro_get_system_info") : NULL;
#else
   void *get_info = NULL;
#endif
   struct retro_system_info info;
   char path[PX_PATH_MAX * 2], global[PX_PATH_MAX * 2];
   const char *keys[PX_MAX_MUTE], *values[PX_MAX_MUTE];
   const px_profile *p = &d->engine.profile;
   int changed;

   if (!p->mute_count || !d->config_dir[0])
      return;
   memset(&info, 0, sizeof(info));
#ifdef _WIN32
   if (get_info)
      get_info(&info);
#endif
   if (!info.library_name || !*info.library_name)
   {
      dlog(d, RETRO_LOG_WARN, "the profile's [mute] options need the core's name; set them in Quick Menu > Core Options "
            "and choose Save Game Options, or use the Proteus wrapper core");
      return;
   }
   for (unsigned i = 0; i < p->mute_count; i++)
   {
      keys[i]   = p->mute[i].key;
      values[i] = p->mute[i].value;
   }
   px_game_options_path(d->config_dir, info.library_name, d->content, path, sizeof(path));
   snprintf(global, sizeof(global), "%s/%s/%s.opt", d->config_dir, info.library_name, info.library_name);
   changed = px_game_options_set(path, global, keys, values, p->mute_count);
   if (changed < 0)
      dlog(d, RETRO_LOG_WARN, "could not save the profile's [mute] options to %s", path);
   else if (changed > 0)
      dlog(d, RETRO_LOG_INFO, "saved the profile's [mute] options as this game's core options (%s); "
            "close the game and load it again to mute the original music", path);
   if (!d->game_options)
      dlog(d, RETRO_LOG_WARN, "RetroArch's game-specific core options are turned off (game_specific_options), "
            "so the original music is not muted; turn them on or use the Proteus wrapper core");
}

static void refresh(proteus_dsp *d)
{
   char profile[PX_PATH_MAX * 2];

   d->frames_since_poll = 0;
   d->polled            = true;

   read_history(d);
   find_core(d);
   if (d->standby || !d->core_path[0])
   {
      drop_core(d);
      return;
   }

   /* Only trust the history entry when it names the core that is running. */
   if (!d->content[0] || strcasecmp(base_name(d->history_core), base_name(d->core_path)))
   {
      if (px_engine_loaded(&d->engine))
         px_engine_unload(&d->engine);
      d->loaded_content[0] = '\0';
      return;
   }
   if (!strcmp(d->content, d->loaded_content))
      return;

   snprintf(d->loaded_content, sizeof(d->loaded_content), "%s", d->content);
   px_engine_unload(&d->engine);
   if (!px_engine_find_profile(d->content, d->system_dir, profile, sizeof(profile)))
      return;
   if (px_engine_load(&d->engine, profile))
      save_mutes(d);
}

/* ---------------------------------------------------------------------------
 * DSP plugin API
 * ------------------------------------------------------------------------- */

static void *dsp_init(const struct dspfilter_info *info, const struct dspfilter_config *config,
      void *userdata)
{
   proteus_dsp *d = (proteus_dsp*)calloc(1, sizeof(*d));
   px_host host;

   if (!d)
      return NULL;
   memset(&host, 0, sizeof(host));
   host.userdata  = d;
   host.memory    = host_memory;
   host.log       = host_log;
   host.patch     = host_patch;
   host.core_file = host_core_file;

   px_engine_init(&d->engine, &host);
   d->rate = info->input_rate;
   px_engine_set_rate(&d->engine, info->input_rate);
   find_folders(d, config, userdata);
   return d;
}

static void dsp_process(void *data, struct dspfilter_output *output, const struct dspfilter_input *input)
{
   proteus_dsp *d = (proteus_dsp*)data;

   output->samples = input->samples;
   output->frames  = input->frames;

   d->frames_since_poll += input->frames;
   if (!d->polled || d->frames_since_poll >= (unsigned)(d->rate / 2.0f))
      refresh(d);
   if (!px_engine_loaded(&d->engine))
      return;
   if (!core_still_loaded(d))
   {
      drop_core(d);
      return;
   }

   /* RetroArch runs the DSP once per batch of core audio, so this is per frame. */
   px_engine_frame(&d->engine);
   if (px_engine_mixing(&d->engine) && input->samples && input->frames)
      px_engine_mix_float(&d->engine, input->samples, input->frames);
}

static void dsp_free(void *data)
{
   proteus_dsp *d = (proteus_dsp*)data;
   if (!d)
      return;
   px_engine_unload(&d->engine);
   free(d);
}

static const struct dspfilter_implementation proteus_plug = {
   dsp_init,
   dsp_process,
   dsp_free,
   DSPFILTER_API_VERSION,
   "Proteus Retune",
   "proteus",
};

DSP_EXPORT const struct dspfilter_implementation *dspfilter_get_implementation(dspfilter_simd_mask_t mask)
{
   (void)mask;
   return &proteus_plug;
}
