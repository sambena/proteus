/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#include "fx.h"
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
   /* The capture interface (proteus_capture.h); NULL in cores without it. */
   void (*capture_enable)(unsigned);
   const struct pxc_frame *(*capture)(unsigned);
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

/* The Atari 2600's picture and sound (fx.h). */
static struct
{
   bool is_2600;            /* the inner core is Stella */
   px_fx_config cfg;
   px_fx_video *video;
   px_fx_audio *audio;

   /* The frame the inner core handed over in this retro_run. */
   bool got_frame;
   const void *frame;
   unsigned frame_w, frame_h;
   size_t frame_pitch;

   bool video_enabled;      /* false while the frontend runs frames nobody will see */
   struct retro_system_av_info av;   /* as the inner core last gave it */
   bool have_av;
   unsigned out_w, out_h;   /* the size the frontend was told */

   uint64_t us_sum;
   unsigned us_frames, us_max;

   /* What Proteus knows of the game in particular, if it does. */
   const px_game *game;
   void *game_state;
   char md5[33];
} fx;

/* The button and the keys for the options, and the options on the picture. */
#define CTL_LOCAL 48
static struct
{
   bool button_on, keys_on;
   bool held;               /* the button is down */
   bool chorded;            /* and something was pressed with it */
   unsigned pad, pad_before;
   unsigned repeat;         /* frames a direction has been down */
   uint32_t keys;           /* KEY_* pressed since the frame before */
   retro_keyboard_event_t inner_keyboard;

   bool panel;              /* the list is on the picture, and the game stands still */
   unsigned selected;
   char toast[96];
   unsigned toast_frames;
   px_panel view;
   char title[64];
   char values[PX_PANEL_LINES][48];

   /* Options changed here that the frontend would not take. */
   struct { char key[64]; char value[128]; } local[CTL_LOCAL];
   unsigned local_count;
} ctl;

enum
{
   KEY_PANEL = 1u << 0, KEY_BEFORE = 1u << 1, KEY_AFTER = 1u << 2, KEY_LESS = 1u << 3,
   KEY_MORE = 1u << 4, KEY_DIGIT = 1u << 5   /* and the nine after it: 1 to 9, then 0 */
};

enum
{
   PAD_BUTTON = 1u << 0, PAD_UP = 1u << 1, PAD_DOWN = 1u << 2, PAD_LEFT = 1u << 3,
   PAD_RIGHT = 1u << 4, PAD_FIRE = 1u << 5, PAD_TRIGGER = 1u << 6
};

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

#ifdef _WIN32
   inner.api.capture_enable = (void (*)(unsigned))(void*)GetProcAddress(inner.lib, "retro_proteus_capture_enable");
   inner.api.capture = (const struct pxc_frame *(*)(unsigned))(void*)GetProcAddress(inner.lib, "retro_proteus_capture");
#else
   inner.api.capture_enable = (void (*)(unsigned))dlsym(inner.lib, "retro_proteus_capture_enable");
   inner.api.capture = (const struct pxc_frame *(*)(unsigned))dlsym(inner.lib, "retro_proteus_capture");
#endif
   if (!inner.api.capture_enable || !inner.api.capture)
      inner.api.capture_enable = NULL, inner.api.capture = NULL;

   /* stella_libretro, stellapx_libretro, stella2014_libretro */
   fx.is_2600 = strncasecmp(base + strlen(PX_PREFIX), "stella", 6) == 0;
   px_options_set_fx(fx.is_2600);

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

/* Forgets the frontend's cheats (the profile's patch stays). */
static void free_cheats(void)
{
   for (unsigned i = 0; i < cheats.count; i++)
      free(cheats.list[i].code);
   cheats.count = 0;
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
 * The Atari 2600's picture and sound
 * ------------------------------------------------------------------------- */

static int16_t *scratch_buffer(size_t frames);

static bool fx_video_on(void)
{
   return fx.is_2600 && fx.video && fx.cfg.video && inner.api.capture;
}

static bool fx_audio_on(void)
{
   return fx.is_2600 && fx.audio && fx.cfg.audio;
}

/* The value an option of the inner core is held at, or NULL: Proteus needs the voices apart
 * and the picture as the TIA made it. */
static const char *fx_override(const char *key)
{
   if (!fx.is_2600 || !key)
      return NULL;
   if (fx_audio_on() && !strcmp(key, "stella_stereo"))
      return "on";
   if (fx_video_on())
   {
      if (!strcmp(key, "stella_filter"))         return "disabled";
      if (!strcmp(key, "stella_phosphor"))       return "off";
      if (!strcmp(key, "stella_crop_hoverscan")) return "disabled";
      if (!strcmp(key, "stella_crop_voverscan")) return "0";
   }
   return NULL;
}

/* Makes the inner core's geometry that of the picture Proteus draws. */
static void fx_geometry(struct retro_system_av_info *av)
{
   unsigned w, h;
   fx.av      = *av;
   fx.have_av = true;
   if (!fx_video_on())
      return;
   px_fx_video_size(&fx.cfg, av->geometry.base_height, &w, &h);
   /* At the core's own size the core's geometry stands: it names the size to show the
    * frame at, which is twice as wide as the frame. */
   if (fx.cfg.sx > 1)
   {
      av->geometry.base_width  = w;
      av->geometry.base_height = h;
   }
   if (av->geometry.max_width < PX_FX_MAX_WIDTH)
      av->geometry.max_width = PX_FX_MAX_WIDTH;
   if (av->geometry.max_height < PX_FX_MAX_HEIGHT)
      av->geometry.max_height = PX_FX_MAX_HEIGHT;
   fx.out_w = w;
   fx.out_h = h;
}

static void fx_tell_geometry(unsigned w, unsigned h)
{
   struct retro_system_av_info av;
   if (!fx.have_av || !fe_env)
      return;
   av = fx.av;
   if (fx.cfg.sx > 1)
   {
      av.geometry.base_width  = w;
      av.geometry.base_height = h;
   }
   else
      av.geometry.base_height = h;
   if (av.geometry.max_width < PX_FX_MAX_WIDTH)
      av.geometry.max_width = PX_FX_MAX_WIDTH;
   if (av.geometry.max_height < PX_FX_MAX_HEIGHT)
      av.geometry.max_height = PX_FX_MAX_HEIGHT;
   fe_env(RETRO_ENVIRONMENT_SET_GEOMETRY, &av);
   fx.out_w = w;
   fx.out_h = h;
}

/* An option's value: as changed here, or else the frontend's. */
static const char *fx_option_get(const char *key)
{
   for (unsigned i = 0; i < ctl.local_count; i++)
      if (!strcmp(ctl.local[i].key, key))
         return ctl.local[i].value;
   return px_options_get(key);
}

/* An [fx] entry of the game's profile, or else what Proteus knows of the game. */
static const char *fx_profile_get(const char *key)
{
   const char *v = px_profile_fx(&engine.profile, key);
   if (!v && fx.game)
   {
      /* Not with the game's own turned off, which is to show the game as any other. */
      const char *on = fx_option_get(PX_OPT_FX_GAME);
      if (!on || !strcmp(on, PX_OPT_FX_PROFILE))
         on = px_profile_fx(&engine.profile, "game");
      if (!on || strcmp(on, "disabled"))
         v = px_game_fx(fx.game, key);
   }
   return v;
}

static void fx_read_config(void)
{
   bool video_was = fx_video_on(), audio_was = fx_audio_on();
   const char *v;
   if (!fx.is_2600)
      return;
   px_fx_config_read(&fx.cfg, fx_option_get, fx_profile_get);
   if (fx.game && fx.game->configure && fx.game_state)
      fx.game->configure(fx.game_state, fx_option_get);
   v = fx_option_get(PX_OPT_FX_BUTTON);
   ctl.button_on = !v || strcmp(v, "off");
   v = fx_option_get(PX_OPT_FX_KEYS);
   ctl.keys_on = !v || strcmp(v, "disabled");
   if (inner.api.capture_enable)
      inner.api.capture_enable(fx.cfg.video
            ? PXC_ENABLE_VIDEO | PXC_ENABLE_WRITES | PXC_ENABLE_AUDIO : 0);
   /* The inner core's options that depend on these are to be read again. */
   if (video_was != fx_video_on() || audio_was != fx_audio_on())
      st.options_dirty = true;
}

/* Finds what Proteus knows of the game, before its options are declared. */
static void fx_identify(const struct retro_game_info *game)
{
   fx.game   = NULL;
   fx.md5[0] = '\0';
   if (!fx.is_2600 || !game)
      return;
   if (game->data && game->size)
      px_md5(game->data, game->size, fx.md5);
   else if (game->path)
   {
      FILE *f = px_fopen(game->path, "rb");
      static uint8_t rom[512 * 1024];
      size_t n = f ? fread(rom, 1, sizeof(rom), f) : 0;
      if (f)
         fclose(f);
      if (n)
         px_md5(rom, n, fx.md5);
   }
   fx.game = px_game_find(fx.md5);
   px_options_set_game(fx.game);
}

static void fx_start(void)
{
   if (!fx.is_2600)
      return;
   if (!fx.video)
      fx.video = px_fx_video_new();
   if (!fx.audio)
      fx.audio = px_fx_audio_new();
   if (fx.game && fx.game->create && !fx.game_state)
      fx.game_state = fx.game->create();
   fx.us_sum = 0;
   fx.us_frames = fx.us_max = 0;
   memset(&ctl, 0, sizeof(ctl));
   fx_read_config();
   if (fx.game)
      px_log(RETRO_LOG_INFO, "%s (%s): Proteus knows this game", fx.game->name, fx.md5);
   else if (fx.md5[0])
      px_log(RETRO_LOG_INFO, "ROM %s: drawn as any game", fx.md5);
   if (!inner.api.capture)
      px_log(RETRO_LOG_WARN, "%s has no capture interface: the picture is passed through. "
            "Install stellapx_libretro and name Proteus " PX_PREFIX "stellapx_libretro for the enhanced picture.",
            inner.path);
}

static void fx_stop(void)
{
   if (fx.game && fx.game->destroy && fx.game_state)
      fx.game->destroy(fx.game_state);
   fx.game_state = NULL;
   fx.game       = NULL;
   px_fx_video_free(fx.video);
   px_fx_audio_free(fx.audio);
   fx.video   = NULL;
   fx.audio   = NULL;
   fx.have_av = false;
   fx.out_w = fx.out_h = 0;
}

/* ---------------------------------------------------------------------------
 * The options by button and by key
 * ------------------------------------------------------------------------- */

static void ctl_say(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(ctl.toast, sizeof(ctl.toast), fmt, ap);
   va_end(ap);
   ctl.toast_frames = 120;
   /* Where Proteus does not draw the picture, the frontend says it. */
   if (!fx_video_on())
      px_notify("%s", ctl.toast);
}

static int ctl_find(const char *key)
{
   unsigned n = px_options_fx_count();
   for (unsigned i = 0; i < n; i++)
      if (!strcmp(px_options_fx_key(i), key))
         return (int)i;
   return -1;
}

/* Which of an option's values it has. */
static unsigned ctl_value(unsigned i)
{
   const char *v = fx_option_get(px_options_fx_key(i));
   unsigned n = px_options_fx_values(i);
   if (!v)
      v = px_options_fx_default(i);
   for (unsigned k = 0; v && k < n; k++)
      if (!strcmp(px_options_fx_value(i, k), v))
         return k;
   return 0;
}

static void ctl_set(unsigned i, unsigned value)
{
   struct retro_variable var;
   var.key   = px_options_fx_key(i);
   var.value = px_options_fx_value(i, value);
   if (!var.key || !var.value)
      return;

   if (!fe_env(RETRO_ENVIRONMENT_SET_VARIABLE, &var))
   {
      unsigned k;
      for (k = 0; k < ctl.local_count && strcmp(ctl.local[k].key, var.key); k++)
         ;
      if (k == ctl.local_count && ctl.local_count < CTL_LOCAL)
         ctl.local_count++;
      if (k < ctl.local_count)
      {
         snprintf(ctl.local[k].key, sizeof(ctl.local[k].key), "%s", var.key);
         snprintf(ctl.local[k].value, sizeof(ctl.local[k].value), "%s", var.value);
      }
   }
   fx_read_config();
   ctl_say("%s: %s", px_options_fx_name(i), px_options_fx_label(i, value));
}

/* To the value after (or before), around the end. */
static void ctl_step(unsigned i, int by)
{
   unsigned n = px_options_fx_values(i);
   if (n)
      ctl_set(i, (ctl_value(i) + n + (by < 0 ? n - 1 : 1)) % n);
}

/* On if off and off if on, whatever says which it is now. */
static void ctl_toggle(const char *key, bool now)
{
   int i = ctl_find(key);
   unsigned n;
   if (i < 0)
      return;
   n = px_options_fx_values((unsigned)i);
   for (unsigned k = 0; k < n; k++)
      if (!strcmp(px_options_fx_value((unsigned)i, k), now ? "disabled" : "enabled"))
      {
         ctl_set((unsigned)i, k);
         return;
      }
}

/* To the value after the one in effect (`now`), the first one aside, which leaves the choice
 * to the profile. */
static void ctl_cycle(const char *key, const char *now)
{
   int i = ctl_find(key);
   unsigned n, at = 0;
   if (i < 0)
      return;
   n = px_options_fx_values((unsigned)i);
   if (n < 2)
      return;
   for (unsigned k = 1; k < n; k++)
      if (!strcmp(px_options_fx_value((unsigned)i, k), now))
         at = k;
   at = at + 1 < n ? at + 1 : 1;
   ctl_set((unsigned)i, at);
}

static void ctl_select(int by)
{
   unsigned n = px_options_fx_count();
   if (!n)
      return;
   ctl.selected = (ctl.selected + n + (by < 0 ? n - 1 : 1)) % n;
   if (!ctl.panel)
      ctl_say("%s: %s", px_options_fx_name(ctl.selected),
            px_options_fx_label(ctl.selected, ctl_value(ctl.selected)));
}

static void ctl_digit(unsigned digit)
{
   static const char *const glow[] = { "off", "low", "medium", "high" };
   static const char *const view[] = { "normal", "layers", "instances",
      "bk", "pf", "bl", "p0", "m0", "p1", "m1" };
   switch (digit)
   {
      case 0: ctl_toggle(PX_OPT_FX_VIDEO, fx.cfg.video); break;
      case 1:
         if (fx.game)
            ctl_toggle(PX_OPT_FX_GAME, fx.cfg.game);
         else
            ctl_say("Proteus knows nothing of this game in particular");
         break;
      case 2: ctl_cycle(PX_OPT_FX_GLOW, glow[fx.cfg.glow & 3]); break;
      case 3: ctl_toggle(PX_OPT_FX_SHADOW, fx.cfg.shadow); break;
      case 4: ctl_toggle(PX_OPT_FX_SMOOTH, fx.cfg.smooth); break;
      case 5: ctl_toggle(PX_OPT_FX_FLICKER, fx.cfg.flicker); break;
      case 6: ctl_toggle(PX_OPT_FX_TRAILS, fx.cfg.trails); break;
      case 7: ctl_toggle(PX_OPT_FX_BACKGROUND, fx.cfg.background); break;
      case 8: ctl_toggle(PX_OPT_FX_SCANLINES, fx.cfg.scanlines); break;
      case 9: ctl_cycle(PX_OPT_FX_VIEW, view[fx.cfg.view < 10 ? fx.cfg.view : 0]); break;
   }
}

static void RETRO_CALLCONV keyboard_wrap(bool down, unsigned keycode, uint32_t character,
      uint16_t mods)
{
   uint32_t key = 0;
   if (fx.is_2600 && ctl.keys_on)
      switch (keycode)
      {
         case RETROK_BACKSLASH:    key = KEY_PANEL; break;
         case RETROK_LEFTBRACKET:  key = KEY_BEFORE; break;
         case RETROK_RIGHTBRACKET: key = KEY_AFTER; break;
         case RETROK_MINUS:        key = KEY_LESS; break;
         case RETROK_EQUALS:       key = KEY_MORE; break;
         case RETROK_0:            key = KEY_DIGIT << 9; break;
         default:
            if (keycode >= RETROK_1 && keycode <= RETROK_9)
               key = KEY_DIGIT << (keycode - RETROK_1);
            break;
      }
   if (key)
   {
      if (down)
         ctl.keys |= key;
      return;
   }
   if (ctl.inner_keyboard)
      ctl.inner_keyboard(down, keycode, character, mods);
}

/* Once a frame, when the frontend has the state of its buttons. */
static void ctl_update(void)
{
   static const struct { unsigned id, bit; } buttons[] = {
      { RETRO_DEVICE_ID_JOYPAD_X, PAD_BUTTON }, { RETRO_DEVICE_ID_JOYPAD_UP, PAD_UP },
      { RETRO_DEVICE_ID_JOYPAD_DOWN, PAD_DOWN }, { RETRO_DEVICE_ID_JOYPAD_LEFT, PAD_LEFT },
      { RETRO_DEVICE_ID_JOYPAD_RIGHT, PAD_RIGHT }, { RETRO_DEVICE_ID_JOYPAD_B, PAD_FIRE },
      { RETRO_DEVICE_ID_JOYPAD_A, PAD_TRIGGER }
   };
   unsigned pad = 0, pressed, turn;
   uint32_t keys = ctl.keys;

   ctl.keys = 0;
   if (!fx.is_2600)
      return;
   if (ctl.toast_frames)
      ctl.toast_frames--;

   if (ctl.button_on && fe_input_state)
      for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
         if (fe_input_state(0, RETRO_DEVICE_JOYPAD, 0, buttons[i].id))
            pad |= buttons[i].bit;
   pressed        = pad & ~ctl.pad;
   ctl.pad_before = ctl.pad;
   ctl.pad        = pad;

   /* A direction held goes on after a while. */
   turn = pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
   if (pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))
   {
      if (++ctl.repeat > 24 && ctl.repeat % 6 == 0)
         turn = pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
   }
   else
      ctl.repeat = 0;

   if (pressed & PAD_BUTTON)
   {
      ctl.held    = true;
      ctl.chorded = false;
   }

   if (ctl.panel)
   {
      if (turn & PAD_UP)    ctl_select(-1);
      if (turn & PAD_DOWN)  ctl_select(1);
      if (turn & PAD_LEFT)  ctl_step(ctl.selected, -1);
      if (turn & PAD_RIGHT) ctl_step(ctl.selected, 1);
      if (pressed & (PAD_FIRE | PAD_TRIGGER))
      {
         ctl.panel   = false;
         ctl.chorded = true;
      }
   }
   else if (ctl.held)
   {
      if (turn & PAD_LEFT)  { ctl_select(-1); ctl.chorded = true; }
      if (turn & PAD_RIGHT) { ctl_select(1); ctl.chorded = true; }
      if (turn & PAD_UP)    { ctl_step(ctl.selected, 1); ctl.chorded = true; }
      if (turn & PAD_DOWN)  { ctl_step(ctl.selected, -1); ctl.chorded = true; }
      if (pressed & PAD_FIRE)
      {
         ctl_toggle(PX_OPT_FX_VIDEO, fx.cfg.video);
         ctl.chorded = true;
      }
      if (pressed & PAD_TRIGGER)
      {
         ctl_digit(1);
         ctl.chorded = true;
      }
   }

   /* The button let go with nothing pressed meanwhile: the list, or no more of it. */
   if (ctl.held && !(pad & PAD_BUTTON))
   {
      ctl.held = false;
      if (!ctl.chorded)
         ctl.panel = !ctl.panel;
   }

   if (keys & KEY_PANEL)  ctl.panel = !ctl.panel;
   if (keys & KEY_BEFORE) ctl_select(-1);
   if (keys & KEY_AFTER)  ctl_select(1);
   if (keys & KEY_LESS)   ctl_step(ctl.selected, -1);
   if (keys & KEY_MORE)   ctl_step(ctl.selected, 1);
   for (unsigned d = 0; d < 10; d++)
      if (keys & (KEY_DIGIT << d))
         ctl_digit(d);

   /* The list needs the picture to be on. */
   if (ctl.panel && !fx_video_on())
   {
      ctl.panel = false;
      ctl_say("The list of options needs the enhanced picture");
   }
}

/* What is on the picture of the options, or NULL. */
static const px_panel *ctl_view(void)
{
   unsigned n = px_options_fx_count();
   if (!ctl.panel && !ctl.toast_frames)
      return NULL;

   memset(&ctl.view, 0, sizeof(ctl.view));
   ctl.view.open  = ctl.panel;
   ctl.view.toast = ctl.toast_frames ? ctl.toast : NULL;
   if (!ctl.panel)
      return &ctl.view;

   if (n > PX_PANEL_LINES)
      n = PX_PANEL_LINES;
   snprintf(ctl.title, sizeof(ctl.title), "Proteus 2600%s%s", fx.game ? " - " : "",
         fx.game ? fx.game->name : "");
   ctl.view.title    = ctl.title;
   ctl.view.hint     = "Up/Down choose  Left/Right change  Fire back";
   ctl.view.count    = n;
   ctl.view.selected = ctl.selected < n ? ctl.selected : 0;
   for (unsigned i = 0; i < n; i++)
   {
      const char *label = px_options_fx_label(i, ctl_value(i));
      snprintf(ctl.values[i], sizeof(ctl.values[i]), "%s", label ? label : "");
      ctl.view.names[i]  = px_options_fx_name(i);
      ctl.view.values[i] = ctl.values[i];
   }
   return &ctl.view;
}

static void RETRO_CALLCONV input_poll_wrap(void)
{
   if (fe_input_poll)
      fe_input_poll();
   ctl_update();
}

/* The inner core does not see the button, nor anything pressed with it or with the list on
 * the picture. */
static int16_t RETRO_CALLCONV input_state_wrap(unsigned port, unsigned device, unsigned index,
      unsigned id)
{
   int16_t v = fe_input_state ? fe_input_state(port, device, index, id) : 0;
   if (!fx.is_2600 || !ctl.button_on || port != 0
         || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
      return v;
   if (ctl.held || ctl.panel)
      return 0;
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      return (int16_t)(v & ~(1 << RETRO_DEVICE_ID_JOYPAD_X));
   return id == RETRO_DEVICE_ID_JOYPAD_X ? 0 : v;
}

static void fx_extra(px_fx_extra *extra, bool advance)
{
   memset(extra, 0, sizeof(*extra));
   extra->game       = fx.game;
   extra->game_state = fx.game_state;
   extra->ram        = (const uint8_t*)inner.api.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
   extra->ram_size   = inner.api.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
   extra->advance    = advance;
   extra->panel      = ctl_view();
}

static void fx_timing(unsigned w, unsigned h)
{
   unsigned us = px_fx_video_last_us(fx.video);
   fx.us_sum += us;
   if (us > fx.us_max)
      fx.us_max = us;
   if (++fx.us_frames >= 1800)
   {
      px_log(RETRO_LOG_INFO, "picture %ux%u: %u us a frame on average, %u us at most",
            w, h, (unsigned)(fx.us_sum / fx.us_frames), fx.us_max);
      fx.us_sum = 0;
      fx.us_frames = fx.us_max = 0;
   }
}

/* Shows the frame of this retro_run: drawn from its capture, or else as the core made it. */
static void fx_present(void)
{
   const struct pxc_frame *c = NULL;
   const uint32_t *px = NULL;
   unsigned w = 0, h = 0;

   if (!fe_video)
      return;
   if (!fx.frame)
   {
      /* The frame before, again. */
      fe_video(NULL, fx.out_w ? fx.out_w : fx.frame_w, fx.out_h ? fx.out_h : fx.frame_h,
            (fx.out_w ? fx.out_w : fx.frame_w) * sizeof(uint32_t));
      return;
   }

   if (fx.video_enabled && fx_video_on())
      c = inner.api.capture(PXC_ABI_VERSION);
   if (c && c->struct_size >= sizeof(*c) && c->width == fx.frame_w && c->height == fx.frame_h)
   {
      px_fx_extra extra;
      fx_extra(&extra, true);
      px = px_fx_video_render(fx.video, c, &fx.cfg, &extra, &w, &h);
   }

   if (!px)
   {
      fe_video(fx.frame, fx.frame_w, fx.frame_h, fx.frame_pitch);
      return;
   }

   if (w != fx.out_w || h != fx.out_h)
      fx_tell_geometry(w, h);
   fe_video(px, w, h, (size_t)w * sizeof(uint32_t));
   fx_timing(w, h);
}

/* With the list of options on the picture the game stands still: its last frame is drawn
 * again as the options are now, and nothing is to be heard. */
static void fx_stand_still(void)
{
   const struct pxc_frame *c = inner.api.capture ? inner.api.capture(PXC_ABI_VERSION) : NULL;
   const uint32_t *px = NULL;
   unsigned w = 0, h = 0;

   if (c && c->struct_size >= sizeof(*c) && fe_video)
   {
      px_fx_extra extra;
      fx_extra(&extra, false);
      px = px_fx_video_render(fx.video, c, &fx.cfg, &extra, &w, &h);
   }
   if (px)
   {
      if (w != fx.out_w || h != fx.out_h)
         fx_tell_geometry(w, h);
      fe_video(px, w, h, (size_t)w * sizeof(uint32_t));
   }
   else if (fe_video)
      fe_video(NULL, fx.out_w, fx.out_h, (size_t)fx.out_w * sizeof(uint32_t));

   if (fe_audio_batch && fx.have_av && fx.av.timing.fps > 1.0 && st.audio_enabled)
   {
      size_t frames = (size_t)(fx.av.timing.sample_rate / fx.av.timing.fps + 0.5);
      int16_t *buf = scratch_buffer(frames);
      if (buf)
      {
         memset(buf, 0, frames * 2 * sizeof(int16_t));
         fe_audio_batch(buf, frames);
      }
   }
}

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
         if (!forced && var)
            forced = fx_override(var->key);
         if (forced)
         {
            var->value = forced;
            return true;
         }
         return ret;
      }

      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      {
         struct retro_system_av_info av;
         if (!data || !fx.is_2600)
            return fe_env(cmd, data);
         /* Only the geometry is the core's to give here; the timing is as before. */
         av = fx.have_av ? fx.av : *(const struct retro_system_av_info*)data;
         av.geometry = ((const struct retro_system_av_info*)data)->geometry;
         fx_geometry(&av);
         return fe_env(cmd, &av);
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
         struct retro_system_av_info av;
         bool ret;
         if (!data)
            return fe_env(cmd, data);
         av = *(const struct retro_system_av_info*)data;
         if (fx.is_2600)
            fx_geometry(&av);
         ret = fe_env(cmd, &av);
         if (ret)
         {
            px_engine_set_rate(&engine, av.timing.sample_rate);
            px_fx_audio_set_rate(fx.audio, av.timing.sample_rate);
         }
         return ret;
      }

      case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
      {
         /* The keys for the options are taken out on their way to the inner core. */
         struct retro_keyboard_callback wrapped;
         if (!data || !fx.is_2600)
            return fe_env(cmd, data);
         ctl.inner_keyboard = ((const struct retro_keyboard_callback*)data)->callback;
         wrapped.callback   = keyboard_wrap;
         return fe_env(cmd, &wrapped);
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
   if (fx_audio_on() && st.audio_enabled)
      px_fx_audio_process(fx.audio, &fx.cfg, frame, 1);
   if (mixing_active())
      px_engine_mix_s16(&engine, frame, 1);
   if (fe_audio_sample)
      fe_audio_sample(frame[0], frame[1]);
}

static size_t RETRO_CALLCONV audio_batch_wrap(const int16_t *data, size_t frames)
{
   bool voices = fx_audio_on() && st.audio_enabled;
   int16_t *buf;
   if (!fe_audio_batch)
      return frames;
   if ((!voices && !mixing_active()) || !(buf = scratch_buffer(frames)))
      return fe_audio_batch(data, frames);
   memcpy(buf, data, frames * 2 * sizeof(int16_t));
   if (voices)
      px_fx_audio_process(fx.audio, &fx.cfg, buf, frames);
   if (mixing_active())
      px_engine_mix_s16(&engine, buf, frames);
   return fe_audio_batch(buf, frames);
}

/* The inner core's frame is kept until retro_run knows what to show for it. */
static void RETRO_CALLCONV video_wrap(const void *data, unsigned width, unsigned height, size_t pitch)
{
   if (!fx_video_on())
   {
      if (fe_video)
         fe_video(data, width, height, pitch);
      return;
   }
   fx.got_frame   = true;
   fx.frame       = data;
   fx.frame_w     = width;
   fx.frame_h     = height;
   fx.frame_pitch = pitch;
}

static void apply_callbacks(void)
{
   if (!inner.ok)
      return;
   if (fe_env)
      inner.api.set_environment(env_wrap);
   if (fe_video)
      inner.api.set_video_refresh(video_wrap);
   if (fe_audio_sample)
      inner.api.set_audio_sample(audio_sample_wrap);
   if (fe_audio_batch)
      inner.api.set_audio_sample_batch(audio_batch_wrap);
   if (fe_input_poll)
      inner.api.set_input_poll(input_poll_wrap);
   if (fe_input_state)
      inner.api.set_input_state(input_state_wrap);
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
      inner.api.set_video_refresh(video_wrap);
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
      inner.api.set_input_poll(input_poll_wrap);
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
   fe_input_state = cb;
   if (ensure_inner())
      inner.api.set_input_state(input_state_wrap);
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
   fx_stop();
   memset(&fx, 0, sizeof(fx));
   /* The frontend's cheats belong to this session; the inner core is gone, so nothing is reset there. */
   free_cheats();
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
   px_fx_audio_set_rate(fx.audio, info->timing.sample_rate);
   if (fx.is_2600)
      fx_geometry(info);
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
   px_fx_video_reset(fx.video);
   px_fx_audio_reset(fx.audio);
   if (fx.game && fx.game->reset && fx.game_state)
      fx.game->reset(fx.game_state);
}

RETRO_API void retro_run(void)
{
   bool engine_on;

   if (!inner.ok)
      return;
   engine_on = px_engine_loaded(&engine);

   if (engine_on || fx.is_2600)
   {
      int av = 0;
      bool updated = false;
      bool have_av;

      /* Taking the update flag hides it from the inner core; env_wrap hands it on. */
      if (fe_env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      {
         st.option_update = true;
         if (engine_on)
            px_engine_options_changed(&engine);
         fx_read_config();
      }

      /* Bit 1 is cleared while run-ahead renders frames nobody will hear;
       * the music must not advance during those. Bit 0 is the same for the picture. */
      have_av = fe_env(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av);
      st.audio_enabled = !have_av || (av & 2);
      fx.video_enabled = !have_av || (av & 1);
      /* Detect before running so a mute takes effect in this frame's audio. */
      if (engine_on)
         px_engine_frame(&engine);
   }

   /* With the list of options on the picture, the game waits. */
   if (fx.is_2600 && ctl.panel)
   {
      input_poll_wrap();
      if (ctl.panel || fx_video_on())
      {
         fx_stand_still();
         return;
      }
   }

   fx.got_frame = false;
   inner.api.run();
   if (fx.got_frame)
      fx_present();
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
   free_cheats();
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
      px_fx_audio_set_rate(fx.audio, av.timing.sample_rate);
      fx.av      = av;
      fx.have_av = true;
      /* The profile's cheats were set before the game loaded; cores keep cheats per game. */
      rebuild_cheats();
   }
   else
   {
      px_engine_unload(&engine);
      fx_stop();
   }
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   bool ok;
   if (!ensure_inner())
      return false;
   /* Before the options are declared, of which the game may have its own. */
   fx_identify(game);
   load_profile_for(game ? game->path : NULL);
   /* Before the inner core loads the game, which is when it reads its options. */
   fx_start();
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
   fx_stop();
   /* The frontend's cheats are for the game unloaded; it sets the next game's after loading it. */
   free_cheats();
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
