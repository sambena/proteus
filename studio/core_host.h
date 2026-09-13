// SPDX-License-Identifier: LGPL-2.1-or-later
// A minimal libretro frontend: runs one software-rendered core for Proteus Studio.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "libretro.h"

struct CoreOptionValue
{
   std::string value;
   std::string label;
};

struct CoreOption
{
   std::string key;
   std::string desc;
   std::string info;
   std::string category;
   std::string default_value;
   std::string value;
   std::vector<CoreOptionValue> values;
};

class CoreHost
{
public:
   CoreHost();
   ~CoreHost();

   // Loads `core_path` and the ROM at `rom_path` (zip archives are opened in memory).
   bool load(const std::string &core_path, const std::string &rom_path,
         const std::string &system_dir, const std::string &save_dir, std::string &error);
   void unload();
   bool loaded() const { return game_loaded_; }

   // Runs one frame with the given joypad button mask (bit n = RETRO_DEVICE_ID_JOYPAD_n).
   void run_frame(uint16_t buttons);
   void reset();

   // Audio produced since the last call, interleaved stereo at sample_rate().
   std::vector<int16_t> &audio() { return audio_; }
   // Called with each audio batch before it is stored; may modify it (music preview).
   std::function<void(int16_t *frames, size_t count)> audio_filter;

   // Latest video frame as 0xAARRGGBB pixels.
   const std::vector<uint32_t> &frame() const { return frame_; }
   unsigned frame_width() const { return width_; }
   unsigned frame_height() const { return height_; }
   double aspect() const { return aspect_; }
   void set_skip_video(bool skip) { skip_video_ = skip; }

   double fps() const { return av_.timing.fps; }
   double sample_rate() const { return av_.timing.sample_rate; }
   const std::string &library_name() const { return library_name_; }
   // Content path in RetroArch's form: "dir/archive.zip#game.sfc" for archives.
   const std::string &content_path() const { return content_path_; }

   const uint8_t *memory(unsigned id, size_t *size) const;
   uint8_t *memory_mut(unsigned id, size_t *size);

   std::vector<uint8_t> save_state();
   bool load_state(const std::vector<uint8_t> &state);

   std::vector<CoreOption> &options() { return options_; }
   CoreOption *find_option(const std::string &key);
   void set_option(const std::string &key, const std::string &value);
   // Values forced over the user's choice while set (Proteus mutes); empty to clear.
   void set_override(const std::string &key, const std::string &value);
   void clear_overrides();

   std::vector<std::string> &log() { return log_; }

   // libretro callbacks (static trampolines use the single active host).
   bool environment(unsigned cmd, void *data);
   void video(const void *data, unsigned width, unsigned height, size_t pitch);
   size_t audio_batch(const int16_t *data, size_t frames);
   int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id);

private:
   struct Api;
   void declare_v2(const retro_core_options_v2 *opts);
   void declare_v1(const retro_core_option_definition *defs);
   void declare_v0(const retro_variable *vars);
   void add_log(const std::string &line);

   void *lib_ = nullptr;
   Api *api_ = nullptr;
   bool game_loaded_ = false;
   retro_system_av_info av_{};
   std::string library_name_;
   std::string content_path_;
   std::string system_dir_, save_dir_;
   std::vector<uint8_t> content_data_;

   retro_pixel_format pixel_format_ = RETRO_PIXEL_FORMAT_0RGB1555;
   std::vector<uint32_t> frame_;
   unsigned width_ = 0, height_ = 0;
   double aspect_ = 4.0 / 3.0;
   bool skip_video_ = false;

   std::vector<int16_t> audio_;
   uint16_t buttons_ = 0;

   std::vector<CoreOption> options_;
   std::map<std::string, std::string> overrides_;
   bool options_updated_ = false;
   std::vector<std::string> log_;
};
