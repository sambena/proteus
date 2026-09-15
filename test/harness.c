/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Headless libretro frontend that drives Proteus around the test core and checks
 * which tones come out of the mixed audio.
 *
 *   harness gen <dir>                           write the test music files
 *   harness run <core> <test dir> <out.wav>     run the scenarios
 *   harness dsp <plugin> <core> <test dir>      run the DSP plugin with an unwrapped core
 *   harness probe <core> <rom> <system dir> <frames> <out.wav> [ram offsets...]
 *                                               run a real game, list options, watch RAM
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "libretro_dspfilter.h"

#ifdef _WIN32
#include <windows.h>
#define LOAD(p)    (void*)LoadLibraryA(p)
#define SYM(h, n)  (void*)GetProcAddress((HMODULE)h, n)
#else
#include <dlfcn.h>
#define LOAD(p)    dlopen(p, RTLD_NOW)
#define SYM(h, n)  dlsym(h, n)
#endif

#define RATE      32000.0
#define FPS       60.0
#define MAX_OPTS  256
#define MAX_FRAME 4096

static struct
{
   void (*set_environment)(retro_environment_t);
   void (*set_video_refresh)(retro_video_refresh_t);
   void (*set_audio_sample)(retro_audio_sample_t);
   void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*set_input_poll)(retro_input_poll_t);
   void (*set_input_state)(retro_input_state_t);
   void (*init)(void);
   void (*deinit)(void);
   void (*get_system_info)(struct retro_system_info*);
   void (*run)(void);
   size_t (*serialize_size)(void);
   bool (*serialize)(void*, size_t);
   bool (*unserialize)(const void*, size_t);
   bool (*load_game)(const struct retro_game_info*);
   void (*unload_game)(void);
   void *(*get_memory_data)(unsigned);
   size_t (*get_memory_size)(unsigned);
} core;

/* Core options as the frontend sees them. */
static struct
{
   char key[64];
   char value[512];
   char values[8192]; /* "|"-joined, for checking the picker contents */
   char legacy[8192]; /* the raw "Desc; a|b" string in legacy mode */
} opts[MAX_OPTS];
static unsigned opt_count;
static unsigned options_version = 2;
static bool options_updated;

static const char *system_dir;
static int16_t *audio;
static size_t audio_frames, audio_cap;
static size_t frame_offsets[MAX_FRAME];
static unsigned failures;

static void RETRO_CALLCONV log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   (void)level;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
}

static int find_opt(const char *key)
{
   for (unsigned i = 0; i < opt_count; i++)
      if (!strcmp(opts[i].key, key))
         return (int)i;
   return -1;
}

/* Keeps the user's current value when options are re-declared, as RetroArch does. */
static int declare_opt(const char *key, const char *def)
{
   int i = find_opt(key);
   if (i < 0 && opt_count < MAX_OPTS)
   {
      i = (int)opt_count++;
      snprintf(opts[i].key, sizeof(opts[i].key), "%s", key);
      snprintf(opts[i].value, sizeof(opts[i].value), "%s", def ? def : "");
   }
   if (i >= 0)
      opts[i].values[0] = opts[i].legacy[0] = '\0';
   return i;
}

static void append_value(int i, const char *value)
{
   size_t len = strlen(opts[i].values);
   snprintf(opts[i].values + len, sizeof(opts[i].values) - len, "|%s", value);
}

static void set_option(const char *key, const char *value)
{
   int i = find_opt(key);
   printf("  [menu] %s = %s\n", key, value);
   if (i < 0)
   {
      printf("  FAIL: option %s was never declared\n", key);
      failures++;
      return;
   }
   snprintf(opts[i].value, sizeof(opts[i].value), "%s", value);
   options_updated = true;
}

static bool RETRO_CALLCONV env_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char**)data = system_dir;
         return true;
      case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
         *(unsigned*)data = options_version;
         return true;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
      {
         const struct retro_core_options_v2 *o = data;
         if (options_version < 2)
            return false;
         for (const struct retro_core_option_v2_definition *d = o->definitions; d->key; d++)
         {
            int i = declare_opt(d->key, d->default_value);
            for (unsigned v = 0; i >= 0 && d->values[v].value; v++)
               append_value(i, d->values[v].value);
         }
         return true;
      }
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         for (const struct retro_variable *v = data; v->key; v++)
         {
            const char *list = strstr(v->value, "; ");
            const char *bar;
            char def[512];
            int i;
            list = list ? list + 2 : v->value;
            bar  = strchr(list, '|');
            snprintf(def, sizeof(def), "%.*s", (int)(bar ? (size_t)(bar - list) : strlen(list)), list);
            if ((i = declare_opt(v->key, def)) >= 0)
               snprintf(opts[i].legacy, sizeof(opts[i].legacy), "%s", v->value);
         }
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *v = data;
         int i = find_opt(v->key);
         v->value = i >= 0 ? opts[i].value : NULL;
         return i >= 0;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = options_updated;
         options_updated = false;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_SET_MESSAGE:
         printf("  [osd] %s\n", ((const struct retro_message*)data)->msg);
         return true;
      default:
         return false;
   }
}

static void RETRO_CALLCONV video_cb(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static void RETRO_CALLCONV audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static void RETRO_CALLCONV poll_cb(void) {}
static int16_t RETRO_CALLCONV input_cb(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; return 0; }

/* A DSP plugin to run the core's audio through, as RetroArch would. */
static const struct dspfilter_implementation *dsp_impl;
static void *dsp_data;
static unsigned dsp_split = 1;   /* batches each frame's audio reaches the plugin in */

static size_t RETRO_CALLCONV batch_cb(const int16_t *data, size_t frames)
{
   if (audio_frames + frames > audio_cap)
   {
      audio_cap = (audio_frames + frames) * 2;
      audio     = realloc(audio, audio_cap * 2 * sizeof(int16_t));
   }

   if (dsp_data)
   {
      static float buf[8192 * 2];
      size_t done = 0;
      if (frames > 8192)
         frames = 8192;
      for (size_t i = 0; i < frames * 2; i++)
         buf[i] = data[i] / 32768.0f;
      for (unsigned part = 0; part < dsp_split; part++)
      {
         struct dspfilter_input in;
         struct dspfilter_output out;
         size_t n = part + 1 == dsp_split ? frames - done : frames / dsp_split;
         in.samples = buf + done * 2;
         in.frames  = (unsigned)n;
         dsp_impl->process(dsp_data, &out, &in);
         for (size_t i = 0; i < out.frames * 2; i++)
         {
            float v = out.samples[i] * 32768.0f;
            audio[audio_frames * 2 + i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
         }
         audio_frames += out.frames;
         done += n;
      }
      return frames;
   }

   memcpy(audio + audio_frames * 2, data, frames * 2 * sizeof(int16_t));
   audio_frames += frames;
   return frames;
}

static bool open_core(const char *path)
{
   void *h = LOAD(path);
   if (!h)
   {
      printf("cannot load %s\n", path);
      return false;
   }
#define GET(f, n) core.f = SYM(h, n); if (!core.f) { printf("missing %s\n", n); return false; }
   GET(set_environment, "retro_set_environment");
   GET(set_video_refresh, "retro_set_video_refresh");
   GET(set_audio_sample, "retro_set_audio_sample");
   GET(set_audio_sample_batch, "retro_set_audio_sample_batch");
   GET(set_input_poll, "retro_set_input_poll");
   GET(set_input_state, "retro_set_input_state");
   GET(init, "retro_init");
   GET(deinit, "retro_deinit");
   GET(get_system_info, "retro_get_system_info");
   GET(run, "retro_run");
   GET(serialize_size, "retro_serialize_size");
   GET(serialize, "retro_serialize");
   GET(unserialize, "retro_unserialize");
   GET(load_game, "retro_load_game");
   GET(unload_game, "retro_unload_game");
   GET(get_memory_data, "retro_get_memory_data");
   GET(get_memory_size, "retro_get_memory_size");
#undef GET
   return true;
}

static void *content_data;

static bool start_session(const char *content, unsigned version)
{
   struct retro_game_info game = { content, NULL, 0, NULL };
   struct retro_system_info info;
   opt_count       = 0;
   options_version = version;
   options_updated = false;
   core.set_environment(env_cb);
   core.set_video_refresh(video_cb);
   core.set_audio_sample(audio_cb);
   core.set_audio_sample_batch(batch_cb);
   core.set_input_poll(poll_cb);
   core.set_input_state(input_cb);
   core.init();
   audio_frames = 0;

   /* Cores that don't need a path get the ROM in memory, as RetroArch does. */
   core.get_system_info(&info);
   if (!info.need_fullpath)
   {
      FILE *f = fopen(content, "rb");
      long size = 0;
      free(content_data);
      content_data = NULL;
      if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0
            && (content_data = malloc((size_t)size)) && fread(content_data, 1, (size_t)size, f) == (size_t)size)
      {
         game.data = content_data;
         game.size = (size_t)size;
      }
      if (f)
         fclose(f);
   }

   if (!core.load_game(&game))
   {
      printf("load_game failed for %s\n", content);
      return false;
   }
   return true;
}

static void end_session(void)
{
   core.unload_game();
   core.deinit();
}

static void run_frames(unsigned first, unsigned count)
{
   for (unsigned i = 0; i < count; i++)
   {
      frame_offsets[first + i] = audio_frames;
      core.run();
   }
   frame_offsets[first + count] = audio_frames;
}

/* Amplitude of `freq` in the left channel between two frame numbers (Goertzel). */
static double tone_level(unsigned from_frame, unsigned to_frame, double freq)
{
   size_t a = frame_offsets[from_frame], b = frame_offsets[to_frame];
   double coeff = 2.0 * cos(2.0 * M_PI * freq / RATE);
   double s1 = 0.0, s2 = 0.0;
   for (size_t i = a; i < b; i++)
   {
      double s0 = audio[i * 2] + coeff * s1 - s2;
      s2 = s1;
      s1 = s0;
   }
   return 2.0 * sqrt(fabs(s1 * s1 + s2 * s2 - coeff * s1 * s2)) / (double)(b - a);
}

static void check(const char *label, bool ok, const char *detail)
{
   printf("  %-42s %s  %s\n", label, detail, ok ? "ok" : "FAIL");
   if (!ok)
      failures++;
}

static double expect(const char *label, unsigned from, unsigned to, double freq, bool present)
{
   char detail[64];
   double level = tone_level(from, to, freq);
   snprintf(detail, sizeof(detail), "%6.1f Hz %-7s level %6.0f", freq,
         present ? "present" : "absent", level);
   check(label, present ? level > 1500.0 : level < 300.0, detail);
   return level;
}

static void expect_option(const char *key, const char *needle)
{
   char label[128];
   int i = find_opt(key);
   const char *hay = i < 0 ? "" : (opts[i].legacy[0] ? opts[i].legacy : opts[i].values);
   snprintf(label, sizeof(label), "option %s offers %s", key, needle ? needle : "(declared)");
   check(label, i >= 0 && (!needle || strstr(hay, needle)), "");
}

/* ---------------------------------------------------------------------------
 * Test music files
 * ------------------------------------------------------------------------- */

static void write_wav(const char *path, const int16_t *data, size_t frames, unsigned rate)
{
   FILE *f = fopen(path, "wb");
   uint32_t bytes = (uint32_t)(frames * 4);
   uint32_t u32;
   uint16_t u16;
   if (!f)
      return;
   fwrite("RIFF", 1, 4, f); u32 = 36 + bytes; fwrite(&u32, 4, 1, f);
   fwrite("WAVEfmt ", 1, 8, f); u32 = 16; fwrite(&u32, 4, 1, f);
   u16 = 1; fwrite(&u16, 2, 1, f); u16 = 2; fwrite(&u16, 2, 1, f);
   u32 = rate; fwrite(&u32, 4, 1, f); u32 = rate * 4; fwrite(&u32, 4, 1, f);
   u16 = 4; fwrite(&u16, 2, 1, f); u16 = 16; fwrite(&u16, 2, 1, f);
   fwrite("data", 1, 4, f); u32 = bytes; fwrite(&u32, 4, 1, f);
   fwrite(data, 4, frames, f);
   fclose(f);
}

static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* An SPC snapshot whose DSP is already keyed on: voice 0 loops a 16-sample BRR
 * square wave at pitch 0x600 (12 kHz), i.e. 750 Hz, while the SPC700 idles. */
static void write_spc(const char *path)
{
   static uint8_t spc[0x10200];
   uint8_t *ram = spc + 0x100, *dsp = spc + 0x10100;
   static const uint8_t brr[] = { 0xB3, 0x77, 0x77, 0x77, 0x77, 0x99, 0x99, 0x99, 0x99 };
   FILE *f;

   memset(spc, 0, sizeof(spc));
   memcpy(spc, "SNES-SPC700 Sound File Data v0.30", 33);
   spc[0x21] = 26; spc[0x22] = 26; spc[0x23] = 27; spc[0x24] = 30;
   spc[0x25] = 0x00; spc[0x26] = 0x04;  /* PC = $0400 */
   spc[0x2B] = 0xEF;                    /* SP */

   ram[0x0400] = 0x2F; ram[0x0401] = 0xFE;         /* BRA -2 */
   ram[0x0200] = 0x00; ram[0x0201] = 0x03;         /* sample 0 start $0300 */
   ram[0x0202] = 0x00; ram[0x0203] = 0x03;         /* loop $0300 */
   memcpy(ram + 0x0300, brr, sizeof(brr));

   dsp[0x00] = 0x7F; dsp[0x01] = 0x7F;             /* voice 0 volume */
   dsp[0x02] = 0x00; dsp[0x03] = 0x06;             /* pitch */
   dsp[0x04] = 0x00;                               /* source 0 */
   dsp[0x05] = 0x00; dsp[0x07] = 0x7F;             /* direct gain */
   dsp[0x0C] = 0x7F; dsp[0x1C] = 0x7F;             /* main volume */
   dsp[0x4C] = 0x01;                               /* key on voice 0 */
   dsp[0x6C] = 0x20;                               /* echo off, unmuted */
   dsp[0x5D] = 0x02;                               /* sample directory $0200 */

   if ((f = fopen(path, "wb")))
   {
      fwrite(spc, 1, sizeof(spc), f);
      fclose(f);
   }
}

/* A VGM that plays an SN76489 square at 3579545 / (32 * 224) = 499.4 Hz forever. */
static void write_vgm(const char *path)
{
   static const uint8_t cmds[] = {
      0x50, 0x80, 0x50, 0x0E,              /* tone 0 divider 224 */
      0x50, 0x90,                          /* tone 0 loudest */
      0x50, 0xBF, 0x50, 0xDF, 0x50, 0xFF,  /* other channels off */
      0x61, 0x44, 0xAC,                    /* wait 44100 samples (loop start) */
      0x66,
   };
   uint8_t vgm[0x40 + sizeof(cmds)];
   FILE *f;

   memset(vgm, 0, sizeof(vgm));
   memcpy(vgm, "Vgm ", 4);
   put32(vgm + 0x04, sizeof(vgm) - 0x04);
   put32(vgm + 0x08, 0x150);
   put32(vgm + 0x0C, 3579545);
   put32(vgm + 0x18, 44100);
   put32(vgm + 0x1C, 0x40 + 12 - 0x1C);
   put32(vgm + 0x20, 44100);
   put32(vgm + 0x24, 60);
   vgm[0x28] = 0x09; vgm[0x2A] = 16;
   put32(vgm + 0x34, 0x40 - 0x34);
   memcpy(vgm + 0x40, cmds, sizeof(cmds));

   if ((f = fopen(path, "wb")))
   {
      fwrite(vgm, 1, sizeof(vgm), f);
      fclose(f);
   }
}

static int gen(const char *dir)
{
   static const unsigned freqs[] = { 220, 330, 550 };
   const unsigned rate = 44100;
   char path[512];
   int16_t *buf = malloc(rate * 2 * sizeof(int16_t));

   for (unsigned k = 0; k < 3; k++)
   {
      /* One second of a whole number of cycles loops seamlessly. */
      for (unsigned i = 0; i < rate; i++)
         buf[i * 2] = buf[i * 2 + 1] = (int16_t)lrint(8000.0 * sin(2.0 * M_PI * freqs[k] * i / rate));
      snprintf(path, sizeof(path), "%s/tone_%u.wav", dir, freqs[k]);
      write_wav(path, buf, rate, rate);
   }
   free(buf);

   snprintf(path, sizeof(path), "%s/tone_750.spc", dir);
   write_spc(path);
   snprintf(path, sizeof(path), "%s/tone_500.vgm", dir);
   write_vgm(path);
   return 0;
}

/* ---------------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------------- */

#define VGM_HZ (3579545.0 / (32.0 * 224.0))

/* The .dsp settings the plugin sees: its folders point into the test directory. */
static char dsp_system[512], dsp_history[512], dsp_log[512];

static int RETRO_CALLCONV cfg_float(void *u, const char *k, float *v, float d) { (void)u; (void)k; *v = d; return 0; }
static int RETRO_CALLCONV cfg_int(void *u, const char *k, int *v, int d) { (void)u; (void)k; *v = d; return 0; }
static int RETRO_CALLCONV cfg_float_array(void *u, const char *k, float **v, unsigned *n, const float *d, unsigned dn)
{ (void)u; (void)k; (void)d; (void)dn; *v = NULL; *n = 0; return 0; }
static int RETRO_CALLCONV cfg_int_array(void *u, const char *k, int **v, unsigned *n, const int *d, unsigned dn)
{ (void)u; (void)k; (void)d; (void)dn; *v = NULL; *n = 0; return 0; }

static int RETRO_CALLCONV cfg_string(void *u, const char *key, char **out, const char *def)
{
   const char *v = !strcmp(key, "system_dir") ? dsp_system
         : !strcmp(key, "history") ? dsp_history : !strcmp(key, "log") ? dsp_log : def;
   (void)u;
   *out = strdup(v ? v : "");
   return v != def;
}

static int dsp_scenario(const char *plugin, const char *core_path, const char *dir)
{
   static const struct dspfilter_config config = {
      cfg_float, cfg_int, cfg_float_array, cfg_int_array, cfg_string, free
   };
   const struct dspfilter_implementation *(*get_impl)(dspfilter_simd_mask_t);
   struct dspfilter_info info = { (float)RATE };
   char content[512], wrapper[512], zip[512];
   void *lib = LOAD(plugin);
   FILE *f;

   printf("\nscenario: DSP plugin with an unwrapped core\n");
   if (!lib || !(get_impl = SYM(lib, "dspfilter_get_implementation")))
   {
      printf("  FAIL: cannot load %s\n", plugin);
      return 1;
   }
   dsp_impl = get_impl(0);
   check("plugin API version 1 (RetroArch 1.22)", dsp_impl->api_version == 1, "");
   check("plugin short ident", !strcmp(dsp_impl->short_ident, "proteus"), dsp_impl->short_ident);

   /* RetroArch records the running game in its history when content starts. A zipped game is
    * recorded as the archive alone ("games.zip"), so the plugin finds game.proteus.ini through
    * the name of the game inside it. */
   snprintf(content, sizeof(content), "%s/game.tst", dir);
   snprintf(zip, sizeof(zip), "%s/games.zip", dir);
   snprintf(dsp_system, sizeof(dsp_system), "%s", dir);
   snprintf(dsp_history, sizeof(dsp_history), "%s/history.lpl", dir);
   snprintf(dsp_log, sizeof(dsp_log), "%s/proteus.log", dir);
   if ((f = fopen(zip, "wb")))
   {
      /* One empty stored file, "game.tst": local header, central directory, end record. */
      static const unsigned char local[30] = { 'P', 'K', 3, 4, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0 };
      static const unsigned char central[46] = { 'P', 'K', 1, 2, 10, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
      static const unsigned char end[22] = { 'P', 'K', 5, 6, 0, 0, 0, 0, 1, 0, 1, 0, 54, 0, 0, 0, 38, 0, 0, 0, 0, 0 };
      fwrite(local, 1, 30, f);
      fwrite("game.tst", 1, 8, f);
      fwrite(central, 1, 46, f);
      fwrite("game.tst", 1, 8, f);
      fwrite(end, 1, 22, f);
      fclose(f);
   }
   if ((f = fopen(dsp_history, "wb")))
   {
      fprintf(f, "{\n  \"version\": \"1.5\",\n  \"default_core_path\": \"\",\n  \"items\": [\n"
            "    {\n      \"path\": \"%s\",\n      \"label\": \"game\",\n      \"core_path\": \"%s\"\n    }\n  ]\n}\n",
            zip, core_path);
      fclose(f);
   }

   system_dir = dir;

   /* First a game whose profile stops its music with a code patch: the plugin sets the
    * running core's cheat while it replaces, so nothing is muted for the whole game. */
   {
      char patched[512];
      snprintf(patched, sizeof(patched), "%s/patch.tst", dir);
      if ((f = fopen(dsp_history, "wb")))
      {
         fprintf(f, "{\n  \"version\": \"1.5\",\n  \"items\": [\n    {\n      \"path\": \"%s\",\n      \"core_path\": \"%s\"\n    }\n  ]\n}\n",
               patched, core_path);
         fclose(f);
      }
      if (!open_core(core_path) || !start_session(patched, 2))
         return 1;
      dsp_data = dsp_impl->init(&info, &config, NULL);
      check("plugin init", dsp_data != NULL, "");
      if (!dsp_data)
         return 1;
      run_frames(0, 600);
      expect("patch: song 1 original", 6, 60, 440, true);
      expect("patch: song 2 wav replacement", 66, 180, 220, true);
      expect("patch: song 2 game music patched out", 70, 180, 440, false);
      expect("patch: song 2 sound effects kept", 66, 180, 1000, true);
      expect("patch: unmapped song lifts the patch", 490, 540, 440, true);
      dsp_impl->free(dsp_data);
      dsp_data = NULL;
      end_session();
   }

   /* A core sending its audio in several batches a frame: the plugin still counts game frames
    * from time, so a 40-frame debounce holds song 2 (from frame 60) back until frame 100. */
   {
      char split[512], split_profile[512];
      snprintf(split, sizeof(split), "%s/split.tst", dir);
      snprintf(split_profile, sizeof(split_profile), "%s/split.proteus.ini", dir);
      /* The track's file name holds " #", which is not a comment after a value. */
      {
         char from[512], to[512], buf[4096];
         FILE *in, *out;
         size_t n;
         snprintf(from, sizeof(from), "%s/tone_220.wav", dir);
         snprintf(to, sizeof(to), "%s/Stage #2.wav", dir);
         if ((in = fopen(from, "rb")))
         {
            if ((out = fopen(to, "wb")))
            {
               while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                  fwrite(buf, 1, n, out);
               fclose(out);
            }
            fclose(in);
         }
      }
      if ((f = fopen(split_profile, "wb")))
      {
         fprintf(f, "# split batches\n[song]\naddress = 0x42\ndebounce = 40 ; frames\n[mix]\ncrossfade_ms = 0\n"
               "[tracks]\n2 = Stage #2.wav ; song 2\n");
         fclose(f);
      }
      if ((f = fopen(dsp_history, "wb")))
      {
         fprintf(f, "{\n  \"version\": \"1.5\",\n  \"items\": [\n    {\n      \"path\": \"%s\",\n      \"core_path\": \"%s\"\n    }\n  ]\n}\n",
               split, core_path);
         fclose(f);
      }
      if (!start_session(split, 2))
         return 1;
      dsp_data = dsp_impl->init(&info, &config, NULL);
      if (!dsp_data)
         return 1;
      dsp_split = 8;
      run_frames(0, 180);
      dsp_split = 1;
      expect("split batches: debounce still waits 40 frames", 64, 94, 220, false);
      expect("split batches: song 2 replaced after the debounce", 106, 178, 220, true);
      dsp_impl->free(dsp_data);
      dsp_data = NULL;
      end_session();
   }

   if ((f = fopen(dsp_history, "wb")))
   {
      fprintf(f, "{\n  \"version\": \"1.5\",\n  \"default_core_path\": \"\",\n  \"items\": [\n"
            "    {\n      \"path\": \"%s\",\n      \"label\": \"game\",\n      \"core_path\": \"%s\"\n    }\n  ]\n}\n",
            zip, core_path);
      fclose(f);
   }

   if (!start_session(content, 2))
      return 1;
   dsp_data = dsp_impl->init(&info, &config, NULL);
   if (!dsp_data)
      return 1;
   /* The plugin can't mute; per-game core options do it. */
   set_option("testcore_music", "disabled");

   run_frames(0, 420);
   expect("song 1: nothing replaced", 6, 60, 220, false);
   expect("song 1: sound effects", 6, 60, 1000, true);
   expect("song 2: wav replacement", 66, 180, 220, true);
   expect("song 2: sound effects kept", 66, 180, 1000, true);
   expect("song 3: ogg replacement", 186, 300, 330, true);
   expect("song 3: previous track gone", 186, 300, 220, false);
   expect("song 4: silence", 306, 360, 330, false);
   expect("song 5: mp3 replacement", 366, 420, 550, true);
   expect("game options mute the core's music", 6, 420, 440, false);

   /* The wrapper core does the work when it is running; the plugin steps aside. */
   snprintf(wrapper, sizeof(wrapper), "%s/proteus_testcore_libretro.dll", dir);
   check("load wrapper module", LOAD(wrapper) != NULL, "");
   run_frames(420, 60);
   expect("wrapper loaded: plugin stands by", 456, 480, 550, false);
   expect("wrapper loaded: sound effects", 456, 480, 1000, true);

   dsp_impl->free(dsp_data);
   dsp_data = NULL;
   end_session();
   printf("\n%s (%u failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
   return failures ? 1 : 0;
}

static int probe(int argc, char **argv)
{
   unsigned frames = (unsigned)strtoul(argv[5], NULL, 0);
   unsigned watch_count = (unsigned)(argc - 7);
   uint32_t watch[16];
   int last[16];
   unsigned proteus_opts = 0;
   double rms = 0.0;
   struct retro_system_info info;

   system_dir = argv[4];
   if (watch_count > 16)
      watch_count = 16;
   for (unsigned i = 0; i < watch_count; i++)
   {
      watch[i] = (uint32_t)strtoul(argv[7 + i], NULL, 16);
      last[i]  = -1;
   }
   if (!open_core(argv[2]))
      return 1;
   core.get_system_info(&info);
   printf("core: %s %s\n", info.library_name, info.library_version);
   if (!start_session(argv[3], 2))
      return 1;

   for (unsigned i = 0; i < opt_count; i++)
   {
      if (strncmp(opts[i].key, "proteus_", 8))
         continue;
      proteus_opts++;
      printf("  option %-28s = %-10s values: %.120s\n", opts[i].key, opts[i].value, opts[i].values);
   }
   printf("options: %u total, %u from Proteus\n", opt_count, proteus_opts);

   for (unsigned f = 0; f < frames; f++)
   {
      const uint8_t *ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
      size_t ram_size    = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
      size_t before      = audio_frames;
      if (f < MAX_FRAME)
         frame_offsets[f] = before;
      core.run();
      for (unsigned i = 0; ram && i < watch_count; i++)
      {
         if (watch[i] < ram_size && ram[watch[i]] != last[i])
         {
            printf("  frame %5u  ram[0x%04X] = 0x%02X\n", f, (unsigned)watch[i], ram[watch[i]]);
            last[i] = ram[watch[i]];
         }
      }
      for (size_t s = before; s < audio_frames; s++)
         rms += (double)audio[s * 2] * audio[s * 2];
      if ((f + 1) % 60 == 0)
      {
         printf("  second %3u rms %6.0f\n", (f + 1) / 60, sqrt(rms / 32040.0));
         rms = 0.0;
      }
   }
   if (frames < MAX_FRAME)
      frame_offsets[frames] = audio_frames;
   /* Tone levels over half-second windows, for checking generated test tracks. */
   for (unsigned f = 0; f + 30 <= frames && f + 30 < MAX_FRAME; f += 300)
      printf("  frames %4u-%4u  220 Hz %6.0f  750 Hz %6.0f\n", f, f + 30,
            tone_level(f, f + 30, 220.0), tone_level(f, f + 30, 750.0));
   write_wav(argv[6], audio, audio_frames, 32040);
   end_session();
   return 0;
}

int main(int argc, char **argv)
{
   char content[512];
   struct retro_system_info info;
   size_t state_size;
   void *state;

   setvbuf(stdout, NULL, _IONBF, 0);
   if (argc == 3 && !strcmp(argv[1], "gen"))
      return gen(argv[2]);
   if (argc >= 7 && !strcmp(argv[1], "probe"))
      return probe(argc, argv);
   if (argc == 5 && !strcmp(argv[1], "dsp"))
      return dsp_scenario(argv[2], argv[3], argv[4]);
   if (argc != 5 || strcmp(argv[1], "run"))
   {
      fprintf(stderr, "usage: harness gen <dir> | harness run <core> <test dir> <out.wav>\n");
      return 2;
   }
   system_dir = argv[3];
   if (!open_core(argv[2]))
      return 1;

   core.get_system_info(&info);
   printf("core: %s %s\n", info.library_name, info.library_version);
   check("library name", strstr(info.library_name, "Proteus Retune (Test Core)") != NULL, "");
   snprintf(content, sizeof(content), "%s/game.tst", argv[3]);

   /* 1. Song changes swap music while sound effects keep playing. */
   printf("\nscenario: song changes\n");
   if (!start_session(content, 2))
      return 1;
   expect_option("testcore_music", "disabled");
   expect_option("proteus_enabled", "disabled");
   expect_option("proteus_music_volume", "200");
   expect_option("proteus_song_2", "tone_750.spc");
   expect_option("proteus_song_6", "tone_500.vgz");
   expect_option("proteus_song_6", "silence");
   run_frames(0, 900);
   write_wav(argv[4], audio, audio_frames, (unsigned)RATE);

   expect("song 1: original music", 6, 60, 440, true);
   expect("song 1: sound effects", 6, 60, 1000, true);
   expect("song 1: no replacement", 6, 60, 220, false);
   expect("song 2: original muted", 66, 180, 440, false);
   expect("song 2: wav replacement", 66, 180, 220, true);
   expect("song 2: sound effects kept", 66, 180, 1000, true);
   expect("song 3: ogg replacement", 186, 300, 330, true);
   expect("song 3: original muted", 186, 300, 440, false);
   expect("song 3: previous track gone", 186, 300, 220, false);
   expect("song 4: silence, original muted", 306, 360, 440, false);
   expect("song 4: silence, no replacement", 306, 360, 330, false);
   expect("song 4: sound effects kept", 306, 360, 1000, true);
   /* Encoder padding shifts the phase at each 1 s loop, so measure inside one loop. */
   expect("song 5: mp3 replacement", 366, 420, 550, true);
   expect("song 5: original muted", 366, 480, 440, false);
   expect("song 9: unmapped -> original", 486, 540, 440, true);
   expect("song 9: replacement stopped", 486, 540, 550, false);
   expect("song 6: spc replacement", 606, 720, 750, true);
   expect("song 6: original muted", 606, 720, 440, false);
   expect("song 6: sound effects kept", 606, 720, 1000, true);
   expect("song 7: vgz replacement", 726, 840, VGM_HZ, true);
   expect("song 7: spc stopped", 726, 840, 750, false);
   expect("song 1 again: original music", 846, 900, 440, true);
   end_session();

   /* 2. Loading a state restores the replacement track that was playing. */
   printf("\nscenario: save state\n");
   if (!start_session(content, 2))
      return 1;
   run_frames(0, 150);
   state_size = core.serialize_size();
   state      = malloc(state_size);
   check("serialize", core.serialize(state, state_size), "");
   run_frames(150, 60); /* now in song 3 (ogg) */
   check("unserialize", core.unserialize(state, state_size), "");
   run_frames(210, 30);
   expect("after load: wav replacement back", 212, 240, 220, true);
   expect("after load: ogg track stopped", 212, 240, 330, false);
   expect("after load: original still muted", 212, 240, 440, false);
   free(state);
   end_session();

   /* 3. Changing core options while the game runs. */
   printf("\nscenario: core options in game\n");
   if (!start_session(content, 2))
      return 1;
   run_frames(0, 20);
   set_option("testcore_music", "disabled");  /* the wrapped core's own option */
   run_frames(20, 30);
   expect("core's own option still applies", 26, 50, 440, false);
   set_option("testcore_music", "enabled");
   run_frames(50, 50);
   expect("song 2 from profile", 66, 100, 220, true);
   set_option("proteus_song_2", "tone_750.spc");
   run_frames(100, 40);
   {
      double full, half;
      expect("picker: song 2 now spc", 106, 140, 220, false);
      full = expect("picker: song 2 now spc", 106, 140, 750, true);
      set_option("proteus_music_volume", "50");
      run_frames(140, 40);
      half = tone_level(146, 180, 750);
      {
         char detail[64];
         snprintf(detail, sizeof(detail), "ratio %.2f", half / full);
         check("music volume 50%", half / full > 0.45 && half / full < 0.55, detail);
      }
   }
   set_option("proteus_enabled", "disabled");
   run_frames(180, 60);
   expect("disabled: original music back", 186, 240, 440, true);
   expect("disabled: no replacement", 186, 240, 330, false);
   set_option("proteus_enabled", "enabled");
   run_frames(240, 60);
   expect("re-enabled: song 3 replacement", 246, 300, 330, true);
   expect("re-enabled: original muted", 246, 300, 440, false);
   end_session();

   /* 4. Frontends that only know the legacy option API. */
   printf("\nscenario: legacy options\n");
   if (!start_session(content, 0))
      return 1;
   expect_option("testcore_music", "Music; enabled|disabled");
   expect_option("proteus_enabled", "Proteus: Music replacement; enabled|disabled");
   expect_option("proteus_song_2", "Proteus: Song 0x2; profile|original|silence|");
   run_frames(0, 180);
   expect("legacy: wav replacement", 66, 180, 220, true);
   expect("legacy: original muted", 66, 180, 440, false);
   end_session();

   /* 5. Games without a profile pass through untouched. */
   printf("\nscenario: no profile\n");
   snprintf(content, sizeof(content), "%s/other.tst", argv[3]);
   if (!start_session(content, 2))
      return 1;
   expect_option("proteus_enabled", NULL);
   check("no song pickers", find_opt("proteus_song_2") < 0, "");
   run_frames(0, 180);
   expect("no profile: original music", 66, 180, 440, true);
   expect("no profile: no replacement", 66, 180, 220, false);
   end_session();

   /* 6. Command register latching: ignores 0 between pulses and retains song. */
   printf("\nscenario: command register latching\n");
   snprintf(content, sizeof(content), "%s/latch.tst", argv[3]);
   if (!start_session(content, 2))
      return 1;
   run_frames(0, 300);
   expect("latch: song 1 original", 6, 60, 440, true);
   expect("latch: song 2 wav replacement", 66, 180, 220, true);
   expect("latch: song 2 original muted", 66, 180, 440, false);
   expect("latch: song 3 ogg replacement", 186, 300, 330, true);
   expect("latch: song 3 original muted", 186, 300, 440, false);
   expect("latch: song 2 stopped", 186, 300, 220, false);
   end_session();

   /* 7. Command block pattern matching: matches 10 <song> FF <any> at 0x50, ignores sound effects. */
   printf("\nscenario: command block pattern matching\n");
   snprintf(content, sizeof(content), "%s/pattern.tst", argv[3]);
   if (!start_session(content, 2))
      return 1;
   run_frames(0, 300);
   expect("pattern: song 1 original", 6, 60, 440, true);
   expect("pattern: song 2 wav replacement", 66, 180, 220, true);
   expect("pattern: song 2 original muted", 66, 180, 440, false);
   expect("pattern: song 3 ogg replacement", 186, 300, 330, true);
   expect("pattern: song 3 original muted", 186, 300, 440, false);
   expect("pattern: song 2 stopped", 186, 300, 220, false);
   end_session();

   /* 8. Stopping the game's music through its RAM: no channel is muted, so sound effects stay. */
   printf("\nscenario: stopping the game's music with a RAM request\n");
   snprintf(content, sizeof(content), "%s/stop.tst", argv[3]);
   if (!start_session(content, 2))
      return 1;
   run_frames(0, 540);
   expect("stop: song 1 original", 6, 60, 440, true);
   expect("stop: song 2 wav replacement", 66, 180, 220, true);
   expect("stop: song 2 game music stopped", 66, 180, 440, false);
   expect("stop: song 2 sound effects kept", 66, 180, 1000, true);
   expect("stop: song 3 replaces song 2", 186, 300, 330, true);
   expect("stop: song 3 game music stopped", 186, 300, 440, false);
   expect("stop: song 4 silence", 306, 360, 440, false);
   expect("stop: song 4 sound effects kept", 306, 360, 1000, true);
   expect("stop: song 5 mp3 replacement", 366, 480, 550, true);
   expect("stop: unmapped song plays the game's music", 486, 540, 440, true);
   expect("stop: unmapped song stops the replacement", 486, 540, 550, false);
   end_session();

   /* 9. Following requests: a command register reads 0 between songs, and a jingle request
    * (song 0x101, unmapped) stops the replacement even though the song number never changes. */
   printf("\nscenario: following song and jingle requests\n");
   snprintf(content, sizeof(content), "%s/requests.tst", argv[3]);
   if (!start_session(content, 2))
      return 1;
   run_frames(0, 400);
   expect("requests: song 2 wav replacement", 66, 180, 220, true);
   expect("requests: song 2 game music stopped", 66, 180, 440, false);
   expect("requests: song 3 after a stretch of zeros", 186, 246, 330, true);
   expect("requests: jingle stops the replacement", 256, 300, 330, false);
   expect("requests: sound effects kept", 256, 300, 1000, true);
   expect("requests: song 4 silence", 306, 360, 330, false);
   end_session();

   /* 10. Stopping the game's music with a code patch: the core's cheat, on while replacing. */
   printf("\nscenario: stopping the game's music with a code patch\n");
   snprintf(content, sizeof(content), "%s/patch.tst", argv[3]);
   if (!start_session(content, 2))
      return 1;
   run_frames(0, 600);
   expect("patch: song 1 original", 6, 60, 440, true);
   expect("patch: song 2 wav replacement", 66, 180, 220, true);
   expect("patch: song 2 game music patched out", 66, 180, 440, false);
   expect("patch: song 2 sound effects kept", 66, 180, 1000, true);
   expect("patch: song 4 silence", 306, 360, 440, false);
   expect("patch: unmapped song lifts the patch", 486, 540, 440, true);
   expect("patch: song 1 again plays the game's music", 546, 600, 440, true);
   end_session();

   printf("\n%s (%u failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
   return failures ? 1 : 0;
}
