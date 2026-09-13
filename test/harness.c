/* Headless libretro frontend that drives Proteus around the test core and checks
 * which tones come out of the mixed audio.
 *
 *   harness gen <dir>                           write tone_220/330/550.wav
 *   harness run <core> <test dir> <out.wav>     run the scenarios
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#ifdef _WIN32
#include <windows.h>
#define LOAD(p)    (void*)LoadLibraryA(p)
#define SYM(h, n)  (void*)GetProcAddress((HMODULE)h, n)
#define UNLOAD(h)  FreeLibrary((HMODULE)h)
#else
#include <dlfcn.h>
#define LOAD(p)    dlopen(p, RTLD_NOW)
#define SYM(h, n)  dlsym(h, n)
#define UNLOAD(h)  dlclose(h)
#endif

#define RATE 32000.0
#define FPS  60.0

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
} core;

static const char *system_dir;
static char var_keys[16][64];
static char var_values[16][64];
static unsigned var_count;

static int16_t *audio;
static size_t audio_frames, audio_cap;
static size_t frame_offsets[4096];
static unsigned failures;

static void RETRO_CALLCONV log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   (void)level;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
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
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         for (const struct retro_variable *v = data; v->key && var_count < 16; v++)
         {
            const char *opts = strstr(v->value, "; ");
            const char *bar;
            size_t len;
            opts = opts ? opts + 2 : v->value;
            bar  = strchr(opts, '|');
            len  = bar ? (size_t)(bar - opts) : strlen(opts);
            snprintf(var_keys[var_count], 64, "%s", v->key);
            snprintf(var_values[var_count], 64, "%.*s", (int)len, opts);
            var_count++;
         }
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *v = data;
         for (unsigned i = 0; i < var_count; i++)
            if (!strcmp(var_keys[i], v->key))
            {
               v->value = var_values[i];
               return true;
            }
         v->value = NULL;
         return false;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_SET_MESSAGE:
         printf("[osd] %s\n", ((const struct retro_message*)data)->msg);
         return true;
      default:
         return false;
   }
}

static void RETRO_CALLCONV video_cb(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static void RETRO_CALLCONV audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static void RETRO_CALLCONV poll_cb(void) {}
static int16_t RETRO_CALLCONV input_cb(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; return 0; }

static size_t RETRO_CALLCONV batch_cb(const int16_t *data, size_t frames)
{
   if (audio_frames + frames > audio_cap)
   {
      audio_cap = (audio_frames + frames) * 2;
      audio     = realloc(audio, audio_cap * 2 * sizeof(int16_t));
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
#undef GET
   return true;
}

static bool start_session(const char *content)
{
   struct retro_game_info game = { content, NULL, 0, NULL };
   var_count = 0;
   core.set_environment(env_cb);
   core.set_video_refresh(video_cb);
   core.set_audio_sample(audio_cb);
   core.set_audio_sample_batch(batch_cb);
   core.set_input_poll(poll_cb);
   core.set_input_state(input_cb);
   core.init();
   audio_frames = 0;
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

static void expect(const char *label, unsigned from, unsigned to, double freq, bool present)
{
   double level = tone_level(from, to, freq);
   bool ok = present ? level > 1500.0 : level < 300.0;
   printf("  %-34s %4.0f Hz %-7s level %6.0f  %s\n", label, freq,
         present ? "present" : "absent", level, ok ? "ok" : "FAIL");
   if (!ok)
      failures++;
}

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

static int gen(const char *dir)
{
   static const unsigned freqs[] = { 220, 330, 550 };
   const unsigned rate = 44100;
   int16_t *buf = malloc(rate * 2 * sizeof(int16_t));
   for (unsigned k = 0; k < 3; k++)
   {
      char path[512];
      /* One second of a whole number of cycles loops seamlessly. */
      for (unsigned i = 0; i < rate; i++)
         buf[i * 2] = buf[i * 2 + 1] = (int16_t)lrint(8000.0 * sin(2.0 * M_PI * freqs[k] * i / rate));
      snprintf(path, sizeof(path), "%s/tone_%u.wav", dir, freqs[k]);
      write_wav(path, buf, rate, rate);
   }
   free(buf);
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
   if (!strstr(info.library_name, "Proteus Retune (Test Core)"))
   {
      printf("FAIL: unexpected library name\n");
      failures++;
   }

   /* 1. Song changes swap music while sound effects keep playing. */
   printf("\nscenario: song changes\n");
   snprintf(content, sizeof(content), "%s/game.tst", argv[3]);
   if (!start_session(content))
      return 1;
   run_frames(0, 600);
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
   expect("song 1 again: original music", 546, 600, 440, true);
   end_session();

   /* 2. Loading a state restores the replacement track that was playing. */
   printf("\nscenario: save state\n");
   if (!start_session(content))
      return 1;
   run_frames(0, 150);
   state_size = core.serialize_size();
   state      = malloc(state_size);
   if (!core.serialize(state, state_size))
   {
      printf("  FAIL: serialize\n");
      failures++;
   }
   run_frames(150, 60); /* now in song 3 (ogg) */
   if (!core.unserialize(state, state_size))
   {
      printf("  FAIL: unserialize\n");
      failures++;
   }
   run_frames(210, 30);
   expect("after load: wav replacement back", 212, 240, 220, true);
   expect("after load: ogg track stopped", 212, 240, 330, false);
   expect("after load: original still muted", 212, 240, 440, false);
   free(state);
   end_session();

   /* 3. Games without a profile pass through untouched. */
   printf("\nscenario: no profile\n");
   snprintf(content, sizeof(content), "%s/other.tst", argv[3]);
   if (!start_session(content))
      return 1;
   run_frames(0, 180);
   expect("no profile: original music", 66, 180, 440, true);
   expect("no profile: no replacement", 66, 180, 220, false);
   end_session();

   printf("\n%s (%u failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
   return failures ? 1 : 0;
}
