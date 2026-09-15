// SPDX-License-Identifier: GPL-3.0-or-later
#include "core_host.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

#include "zip_read.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include "util.h"
}

struct CoreHost::Api
{
   void (*set_environment)(retro_environment_t);
   void (*set_video_refresh)(retro_video_refresh_t);
   void (*set_audio_sample)(retro_audio_sample_t);
   void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*set_input_poll)(retro_input_poll_t);
   void (*set_input_state)(retro_input_state_t);
   void (*init)(void);
   void (*deinit)(void);
   void (*get_system_info)(retro_system_info*);
   void (*get_system_av_info)(retro_system_av_info*);
   void (*reset)(void);
   void (*run)(void);
   size_t (*serialize_size)(void);
   bool (*serialize)(void*, size_t);
   bool (*unserialize)(const void*, size_t);
   bool (*load_game)(const retro_game_info*);
   void (*unload_game)(void);
   void *(*get_memory_data)(unsigned);
   size_t (*get_memory_size)(unsigned);
   void (*cheat_reset)(void);
   void (*cheat_set)(unsigned, bool, const char*);
};

// libretro callbacks carry no context, so every host gets a slot with its own set
// of trampolines. Hosts in different slots may run on different threads.
enum { kSlots = 4 };
static CoreHost *g_hosts[kSlots];
static std::mutex g_slots_mutex;

template <int N> struct Tramp
{
   static bool RETRO_CALLCONV env(unsigned cmd, void *data) { return g_hosts[N] && g_hosts[N]->environment(cmd, data); }
   static void RETRO_CALLCONV video(const void *d, unsigned w, unsigned h, size_t p) { if (g_hosts[N]) g_hosts[N]->video(d, w, h, p); }
   static size_t RETRO_CALLCONV audio_batch(const int16_t *d, size_t f) { return g_hosts[N] ? g_hosts[N]->audio_batch(d, f) : f; }
   static void RETRO_CALLCONV audio_sample(int16_t l, int16_t r) { int16_t s[2] = { l, r }; if (g_hosts[N]) g_hosts[N]->audio_batch(s, 1); }
   static void RETRO_CALLCONV input_poll(void) {}
   static int16_t RETRO_CALLCONV input_state(unsigned p, unsigned d, unsigned i, unsigned id) { return g_hosts[N] ? g_hosts[N]->input_state(p, d, i, id) : 0; }
   static void RETRO_CALLCONV log(enum retro_log_level level, const char *fmt, ...)
   {
      char msg[1024];
      va_list ap;
      (void)level;
      va_start(ap, fmt);
      vsnprintf(msg, sizeof(msg), fmt, ap);
      va_end(ap);
      size_t len = strlen(msg);
      while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
         msg[--len] = '\0';
      if (g_hosts[N])
         g_hosts[N]->add_log(msg);
   }
};

struct TrampSet
{
   retro_environment_t env;
   retro_video_refresh_t video;
   retro_audio_sample_batch_t audio_batch;
   retro_audio_sample_t audio_sample;
   retro_input_poll_t input_poll;
   retro_input_state_t input_state;
   retro_log_printf_t log;
};

#define TRAMPS(N) { Tramp<N>::env, Tramp<N>::video, Tramp<N>::audio_batch, Tramp<N>::audio_sample, \
                    Tramp<N>::input_poll, Tramp<N>::input_state, Tramp<N>::log }
static const TrampSet kTramps[kSlots] = { TRAMPS(0), TRAMPS(1), TRAMPS(2), TRAMPS(3) };
#undef TRAMPS

CoreHost::CoreHost()
{
   std::lock_guard<std::mutex> lock(g_slots_mutex);
   for (int i = 0; i < kSlots; i++)
      if (!g_hosts[i])
      {
         g_hosts[i] = this;
         slot_ = i;
         break;
      }
}

CoreHost::~CoreHost()
{
   unload();
   std::lock_guard<std::mutex> lock(g_slots_mutex);
   if (slot_ >= 0)
      g_hosts[slot_] = nullptr;
}

static bool read_file(const std::string &path, std::vector<uint8_t> &out);

// A DLL loaded twice shares its globals, so every slot but the first loads its
// own copy of the core (proteus_slot1_snes9x_libretro.dll next to the settings).
static std::string core_copy_for_slot(const std::string &core_path, int slot, const std::string &copies_dir)
{
   if (slot <= 0 || copies_dir.empty())
      return core_path;
   size_t sep = core_path.find_last_of("/\\");
   std::string name = sep == std::string::npos ? core_path : core_path.substr(sep + 1);
   std::string copy = copies_dir + "/slot" + std::to_string(slot) + "_" + name;

   std::vector<uint8_t> src, dst;
   if (!read_file(core_path, src))
      return core_path;
   if (!read_file(copy, dst) || dst != src)
   {
      FILE *f = px_fopen(copy.c_str(), "wb");
      if (!f)
         return core_path;
      bool ok = fwrite(src.data(), 1, src.size(), f) == src.size();
      if (fclose(f) != 0 || !ok)
         return core_path;
   }
   return copy;
}

void CoreHost::add_log(const std::string &line)
{
   std::lock_guard<std::mutex> lock(log_mutex_);
   log_.push_back(line);
   if (log_.size() > 2000)
      log_.erase(log_.begin(), log_.begin() + 500);
}

std::vector<std::string> CoreHost::take_log()
{
   std::lock_guard<std::mutex> lock(log_mutex_);
   std::vector<std::string> lines;
   lines.swap(log_);
   return lines;
}

// ---------------------------------------------------------------------------
// Content loading
// ---------------------------------------------------------------------------

static bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
   FILE *f = px_fopen(path.c_str(), "rb");
   if (!f)
      return false;
   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   fseek(f, 0, SEEK_SET);
   out.resize(size > 0 ? (size_t)size : 0);
   bool ok = size <= 0 || fread(out.data(), 1, out.size(), f) == out.size();
   fclose(f);
   return ok;
}

static std::string lower_ext(const std::string &name)
{
   size_t dot = name.find_last_of('.');
   std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
   for (char &c : ext)
      c = (char)tolower((unsigned char)c);
   return ext;
}

static bool ext_allowed(const std::string &ext, const std::string &valid)
{
   size_t start = 0;
   while (start <= valid.size())
   {
      size_t bar = valid.find('|', start);
      std::string v = valid.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
      if (v == ext)
         return true;
      if (bar == std::string::npos)
         break;
      start = bar + 1;
   }
   return false;
}

// Extracts the first entry whose extension the core accepts (stored or deflated zips).
static bool unzip_first(const std::vector<uint8_t> &zip, const std::string &valid_exts,
      std::string &name, std::vector<uint8_t> &out, std::string &error)
{
   bool found = false;
   if (!zip_read(zip, [&](const std::string &entry) { return ext_allowed(lower_ext(entry), valid_exts); },
            [&](const std::string &entry, std::vector<uint8_t> &data) {
               name = entry;
               out.swap(data);
               found = true;
               return false;
            }, error))
      return false;
   if (!found)
      error = "no file in the archive matches the core's extensions (" + valid_exts + ")";
   return found;
}

// ---------------------------------------------------------------------------
// Loading the core
// ---------------------------------------------------------------------------

bool CoreHost::load(const std::string &core_path, const std::string &rom_path,
      const std::string &system_dir, const std::string &save_dir, std::string &error,
      const std::map<std::string, std::string> &overrides)
{
   unload();
   system_dir_ = system_dir;
   save_dir_   = save_dir;
   if (slot_ < 0)
   {
      error = "too many cores open at once";
      return false;
   }
   std::string lib_path = core_copy_for_slot(core_path, slot_, save_dir);

#ifdef _WIN32
   wchar_t *w = px_utf8_to_wide(lib_path.c_str());
   lib_ = w ? (void*)LoadLibraryExW(w, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
   free(w);
   auto sym = [&](const char *n) { return (void*)GetProcAddress((HMODULE)lib_, n); };
#else
   lib_ = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
   auto sym = [&](const char *n) { return dlsym(lib_, n); };
#endif
   if (!lib_)
   {
      error = "cannot load core " + core_path;
      return false;
   }

   api_ = new Api();
   bool ok = true;
#define SYM(field, name) ok = ok && (api_->field = (decltype(api_->field))sym(name)) != nullptr
   SYM(set_environment, "retro_set_environment");
   SYM(set_video_refresh, "retro_set_video_refresh");
   SYM(set_audio_sample, "retro_set_audio_sample");
   SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
   SYM(set_input_poll, "retro_set_input_poll");
   SYM(set_input_state, "retro_set_input_state");
   SYM(init, "retro_init");
   SYM(deinit, "retro_deinit");
   SYM(get_system_info, "retro_get_system_info");
   SYM(get_system_av_info, "retro_get_system_av_info");
   SYM(reset, "retro_reset");
   SYM(run, "retro_run");
   SYM(serialize_size, "retro_serialize_size");
   SYM(serialize, "retro_serialize");
   SYM(unserialize, "retro_unserialize");
   SYM(load_game, "retro_load_game");
   SYM(unload_game, "retro_unload_game");
   SYM(get_memory_data, "retro_get_memory_data");
   SYM(get_memory_size, "retro_get_memory_size");
   SYM(cheat_reset, "retro_cheat_reset");
   SYM(cheat_set, "retro_cheat_set");
#undef SYM
   if (!ok)
   {
      error = core_path + " is not a libretro core";
      unload();
      return false;
   }

   const TrampSet &t = kTramps[slot_];
   options_.clear();
   overrides_ = overrides;
   api_->set_environment(t.env);
   api_->set_video_refresh(t.video);
   api_->set_audio_sample(t.audio_sample);
   api_->set_audio_sample_batch(t.audio_batch);
   api_->set_input_poll(t.input_poll);
   api_->set_input_state(t.input_state);
   api_->init();

   retro_system_info info{};
   api_->get_system_info(&info);
   library_name_ = info.library_name ? info.library_name : "core";
   std::string valid = info.valid_extensions ? info.valid_extensions : "";

   retro_game_info game{};
   std::string ext = lower_ext(rom_path);
   content_path_ = rom_path;
   if (!read_file(rom_path, content_data_))
   {
      error = "cannot read " + rom_path;
      unload();
      return false;
   }

   std::string extracted_path;
   if (ext == "zip" && !info.block_extract && !ext_allowed("zip", valid))
   {
      std::vector<uint8_t> inner;
      std::string name;
      if (!unzip_first(content_data_, valid, name, inner, error))
      {
         unload();
         return false;
      }
      content_data_.swap(inner);
      content_path_ = rom_path + "#" + name;
      if (info.need_fullpath)
      {
         // Cores that read files themselves get a temporary copy.
         const char *base = strrchr(name.c_str(), '/');
         // Hosts in other slots may open a zip holding a file of the same name at the same time.
         extracted_path = save_dir_ + "/extracted_slot" + std::to_string(slot_) + "_" + (base ? base + 1 : name.c_str());
         FILE *f = px_fopen(extracted_path.c_str(), "wb");
         if (f)
         {
            fwrite(content_data_.data(), 1, content_data_.size(), f);
            fclose(f);
         }
      }
   }

   game.path = info.need_fullpath && !extracted_path.empty() ? extracted_path.c_str() : content_path_.c_str();
   if (!info.need_fullpath)
   {
      game.data = content_data_.data();
      game.size = content_data_.size();
   }
   if (!api_->load_game(&game))
   {
      error = "the core could not load " + rom_path;
      for (auto &l : take_log())
         error += "\n  | " + l;
      unload();
      return false;
   }

   game_loaded_ = true;
   api_->get_system_av_info(&av_);
   aspect_ = av_.geometry.aspect_ratio > 0 ? av_.geometry.aspect_ratio
         : (double)av_.geometry.base_width / (av_.geometry.base_height ? av_.geometry.base_height : 1);
   add_log("loaded " + content_path_ + " with " + library_name_);
   return true;
}

void CoreHost::unload()
{
   if (api_)
   {
      if (game_loaded_)
         api_->unload_game();
      if (api_->deinit)
         api_->deinit();
   }
   game_loaded_ = false;
   delete api_;
   api_ = nullptr;
   if (lib_)
   {
#ifdef _WIN32
      FreeLibrary((HMODULE)lib_);
#else
      dlclose(lib_);
#endif
      lib_ = nullptr;
   }
   frame_.clear();
   width_ = height_ = 0;
   audio_.clear();
   content_data_.clear();
   skip_video_ = false;
}

void CoreHost::run_frame(uint16_t buttons)
{
   if (!game_loaded_)
      return;
   buttons_ = buttons;
   api_->run();
}

void CoreHost::reset()
{
   if (game_loaded_)
      api_->reset();
}

const uint8_t *CoreHost::memory(unsigned id, size_t *size) const
{
   *size = game_loaded_ ? api_->get_memory_size(id) : 0;
   return game_loaded_ ? (const uint8_t*)api_->get_memory_data(id) : nullptr;
}

uint8_t *CoreHost::memory_mut(unsigned id, size_t *size)
{
   *size = game_loaded_ ? api_->get_memory_size(id) : 0;
   return game_loaded_ ? (uint8_t*)api_->get_memory_data(id) : nullptr;
}

void CoreHost::cheat_reset()
{
   if (game_loaded_)
      api_->cheat_reset();
}

void CoreHost::cheat_set(unsigned index, bool enabled, const std::string &code)
{
   if (game_loaded_)
      api_->cheat_set(index, enabled, code.c_str());
}

std::vector<uint8_t> CoreHost::save_state()
{
   std::vector<uint8_t> state;
   if (!game_loaded_)
      return state;
   state.resize(api_->serialize_size());
   if (state.empty() || !api_->serialize(state.data(), state.size()))
      state.clear();
   return state;
}

bool CoreHost::load_state(const std::vector<uint8_t> &state)
{
   return game_loaded_ && !state.empty() && api_->unserialize(state.data(), state.size());
}

// ---------------------------------------------------------------------------
// Core options
// ---------------------------------------------------------------------------

CoreOption *CoreHost::find_option(const std::string &key)
{
   for (auto &o : options_)
      if (o.key == key)
         return &o;
   return nullptr;
}

void CoreHost::set_option(const std::string &key, const std::string &value)
{
   if (CoreOption *o = find_option(key))
   {
      o->value         = value;
      options_updated_ = true;
   }
}

void CoreHost::set_override(const std::string &key, const std::string &value)
{
   if (value.empty())
      overrides_.erase(key);
   else
      overrides_[key] = value;
   options_updated_ = true;
}

void CoreHost::clear_overrides()
{
   if (!overrides_.empty())
      options_updated_ = true;
   overrides_.clear();
}

static std::string str(const char *s) { return s ? s : ""; }

void CoreHost::declare_v2(const retro_core_options_v2 *opts)
{
   std::map<std::string, std::string> previous;
   for (auto &o : options_)
      previous[o.key] = o.value;
   options_.clear();
   if (!opts)
      return;
   for (auto *d = opts->definitions; d && d->key; d++)
   {
      CoreOption o;
      o.key           = d->key;
      o.desc          = str(d->desc_categorized && *d->desc_categorized ? d->desc_categorized : d->desc);
      o.info          = str(d->info);
      o.category      = str(d->category_key);
      o.default_value = str(d->default_value);
      for (unsigned v = 0; v < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[v].value; v++)
         o.values.push_back({ d->values[v].value, str(d->values[v].label) });
      if (o.default_value.empty() && !o.values.empty())
         o.default_value = o.values[0].value;
      o.value = previous.count(o.key) ? previous[o.key] : o.default_value;
      options_.push_back(o);
   }
}

void CoreHost::declare_v1(const retro_core_option_definition *defs)
{
   std::map<std::string, std::string> previous;
   for (auto &o : options_)
      previous[o.key] = o.value;
   options_.clear();
   for (auto *d = defs; d && d->key; d++)
   {
      CoreOption o;
      o.key           = d->key;
      o.desc          = str(d->desc);
      o.info          = str(d->info);
      o.default_value = str(d->default_value);
      for (unsigned v = 0; v < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[v].value; v++)
         o.values.push_back({ d->values[v].value, str(d->values[v].label) });
      if (o.default_value.empty() && !o.values.empty())
         o.default_value = o.values[0].value;
      o.value = previous.count(o.key) ? previous[o.key] : o.default_value;
      options_.push_back(o);
   }
}

void CoreHost::declare_v0(const retro_variable *vars)
{
   std::map<std::string, std::string> previous;
   for (auto &o : options_)
      previous[o.key] = o.value;
   options_.clear();
   for (auto *v = vars; v && v->key; v++)
   {
      CoreOption o;
      std::string spec = str(v->value);
      size_t semi = spec.find("; ");
      o.key  = v->key;
      o.desc = semi == std::string::npos ? o.key : spec.substr(0, semi);
      std::string list = semi == std::string::npos ? spec : spec.substr(semi + 2);
      size_t start = 0;
      while (start <= list.size())
      {
         size_t bar = list.find('|', start);
         o.values.push_back({ list.substr(start, bar == std::string::npos ? std::string::npos : bar - start), "" });
         if (bar == std::string::npos)
            break;
         start = bar + 1;
      }
      o.default_value = o.values.empty() ? "" : o.values[0].value;
      o.value = previous.count(o.key) ? previous[o.key] : o.default_value;
      options_.push_back(o);
   }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

bool CoreHost::environment(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((retro_log_callback*)data)->log = kTramps[slot_].log;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char**)data = system_dir_.c_str();
         return true;
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = save_dir_.c_str();
         return true;
      case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
         *(const char**)data = system_dir_.c_str();
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      {
         auto fmt = *(const retro_pixel_format*)data;
         if (fmt != RETRO_PIXEL_FORMAT_0RGB1555 && fmt != RETRO_PIXEL_FORMAT_XRGB8888
               && fmt != RETRO_PIXEL_FORMAT_RGB565)
            return false;
         pixel_format_ = fmt;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      {
         const auto *g = (const retro_game_geometry*)data;
         av_.geometry = *g;
         if (g->aspect_ratio > 0)
            aspect_ = g->aspect_ratio;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
         av_ = *(const retro_system_av_info*)data;
         return true;
      case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
         *(unsigned*)data = 2;
         return true;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
         declare_v2((const retro_core_options_v2*)data);
         return true;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
         declare_v2(data ? ((const retro_core_options_v2_intl*)data)->us : nullptr);
         return true;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
         declare_v1((const retro_core_option_definition*)data);
         return true;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
         declare_v1(data ? ((const retro_core_options_intl*)data)->us : nullptr);
         return true;
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         declare_v0((const retro_variable*)data);
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         auto *var = (retro_variable*)data;
         var->value = nullptr;
         if (!var->key)
            return false;
         auto ov = overrides_.find(var->key);
         if (ov != overrides_.end())
         {
            var->value = ov->second.c_str();
            return true;
         }
         if (CoreOption *o = find_option(var->key))
         {
            var->value = o->value.c_str();
            return true;
         }
         return false;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data     = options_updated_;
         options_updated_ = false;
         return true;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
      case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
      case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
      case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
      case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
      case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
         if (data)
            *(bool*)data = true;
         return true;
      case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
         *(int*)data = 3;
         return true;
      case RETRO_ENVIRONMENT_SET_MESSAGE:
         add_log(str(((const retro_message*)data)->msg));
         return true;
      default:
         return false;
   }
}

void CoreHost::video(const void *data, unsigned width, unsigned height, size_t pitch)
{
   if (skip_video_ || !data || data == RETRO_HW_FRAME_BUFFER_VALID || !width || !height)
      return;
   width_  = width;
   height_ = height;
   frame_.resize((size_t)width * height);

   for (unsigned y = 0; y < height; y++)
   {
      uint32_t *dst = &frame_[(size_t)y * width];
      if (pixel_format_ == RETRO_PIXEL_FORMAT_XRGB8888)
      {
         const uint32_t *src = (const uint32_t*)((const uint8_t*)data + y * pitch);
         for (unsigned x = 0; x < width; x++)
            dst[x] = 0xFF000000u | src[x];
      }
      else
      {
         const uint16_t *src = (const uint16_t*)((const uint8_t*)data + y * pitch);
         for (unsigned x = 0; x < width; x++)
         {
            uint16_t p = src[x];
            uint32_t r, g, b;
            if (pixel_format_ == RETRO_PIXEL_FORMAT_RGB565)
            {
               r = (p >> 11) & 0x1F; g = (p >> 5) & 0x3F; b = p & 0x1F;
               r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
            }
            else
            {
               r = (p >> 10) & 0x1F; g = (p >> 5) & 0x1F; b = p & 0x1F;
               r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
            }
            dst[x] = 0xFF000000u | r << 16 | g << 8 | b;
         }
      }
   }
}

size_t CoreHost::audio_batch(const int16_t *data, size_t frames)
{
   size_t start = audio_.size();
   audio_.insert(audio_.end(), data, data + frames * 2);
   if (audio_filter)
      audio_filter(&audio_[start], frames);
   return frames;
}

int16_t CoreHost::input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   if (port == 0 && (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
      return id == RETRO_DEVICE_ID_ANALOG_X ? analog_x_ : id == RETRO_DEVICE_ID_ANALOG_Y ? analog_y_ : 0;
   if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
      return 0;
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      return (int16_t)buttons_;
   return id < 16 ? (buttons_ >> id) & 1 : 0;
}
