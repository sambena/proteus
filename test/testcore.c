/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A fake game core for testing Proteus.
 *
 * Audio: a 440 Hz "music" tone that the core option testcore_music can mute,
 * plus a 1000 Hz "sound effect" tone that is always on.
 * RAM:   system RAM byte 0x42 holds the current song id, following a schedule.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "libretro.h"

#define RATE     32000.0
#define FPS      60.0
#define RAM_SIZE 0x100
#define SONG_ADDR 0x42

static retro_environment_t        env_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_cb;
static retro_input_poll_t         poll_cb;

static struct
{
   uint32_t frame;
   double music_phase;
   double sfx_phase;
   double sample_debt;
   uint8_t ram[RAM_SIZE];
} s;

static bool music_on = true;
static uint16_t pixels[16 * 16];

/* Song schedule; harness.c checks audio against the same frame ranges. */
static uint8_t testcore_song_for_frame(uint32_t frame)
{
   if (frame < 60)  return 1; /* original music */
   if (frame < 180) return 2; /* wav replacement */
   if (frame < 300) return 3; /* ogg replacement */
   if (frame < 360) return 4; /* silence */
   if (frame < 480) return 5; /* mp3 replacement */
   if (frame < 540) return 9; /* unmapped -> original */
   return 1;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "testcore_music", "Music; enabled|disabled" },
      { NULL, NULL },
   };
   env_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
RETRO_API void retro_init(void) { memset(&s, 0, sizeof(s)); }
RETRO_API void retro_deinit(void) {}
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Test Core";
   info->library_version  = "1.0";
   info->valid_extensions = "tst";
   info->need_fullpath    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width  = 16;
   info->geometry.base_height = 16;
   info->geometry.max_width   = 16;
   info->geometry.max_height  = 16;
   info->timing.fps           = FPS;
   info->timing.sample_rate   = RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
RETRO_API void retro_reset(void) { s.frame = 0; }

static void check_variables(void)
{
   struct retro_variable var = { "testcore_music", NULL };
   if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      music_on = strcmp(var.value, "disabled") != 0;
}

RETRO_API void retro_run(void)
{
   bool updated = false;
   int16_t buf[2048 * 2];
   size_t frames;

   if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();

   poll_cb();
   s.ram[SONG_ADDR] = testcore_song_for_frame(s.frame);

   s.sample_debt += RATE / FPS;
   frames = (size_t)s.sample_debt;
   s.sample_debt -= (double)frames;

   for (size_t i = 0; i < frames; i++)
   {
      double v = 4000.0 * sin(s.sfx_phase);
      if (music_on)
         v += 8000.0 * sin(s.music_phase);
      buf[i * 2] = buf[i * 2 + 1] = (int16_t)lrint(v);
      s.music_phase = fmod(s.music_phase + 2.0 * M_PI * 440.0 / RATE, 2.0 * M_PI);
      s.sfx_phase   = fmod(s.sfx_phase + 2.0 * M_PI * 1000.0 / RATE, 2.0 * M_PI);
   }
   audio_cb(buf, frames);
   video_cb(pixels, 16, 16, 32);
   s.frame++;
}

RETRO_API size_t retro_serialize_size(void) { return sizeof(s); }

RETRO_API bool retro_serialize(void *data, size_t size)
{
   if (size < sizeof(s))
      return false;
   memcpy(data, &s, sizeof(s));
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   if (size < sizeof(s))
      return false;
   memcpy(&s, data, sizeof(s));
   return true;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   (void)game;
   check_variables();
   return env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type; (void)info; (void)num;
   return false;
}

RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void *retro_get_memory_data(unsigned id)
{
   return id == RETRO_MEMORY_SYSTEM_RAM ? s.ram : NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
   return id == RETRO_MEMORY_SYSTEM_RAM ? RAM_SIZE : 0;
}
