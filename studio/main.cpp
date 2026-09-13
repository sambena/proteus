// SPDX-License-Identifier: LGPL-2.1-or-later
// Proteus Studio: play a game, find its music, and map songs to replacements.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include "core_host.h"
#include "platform.h"
#include "presets.h"
#include "song_finder.h"

extern "C" {
#include "engine.h"
#include "music.h"
#include "profile.h"
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Settings
{
   std::string retroarch_dir;
   std::string core_file;
   std::string rom_path;
   float ui_scale = 1.25f;
};

static std::string settings_path() { return app_data_dir() + "/studio.cfg"; }

static Settings load_settings()
{
   Settings s;
   std::string text = read_text(settings_path());
   size_t pos = 0;
   while (pos < text.size())
   {
      size_t end = text.find('\n', pos);
      std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      pos = end == std::string::npos ? text.size() : end + 1;
      if (!line.empty() && line.back() == '\r')
         line.pop_back();
      size_t eq = line.find('=');
      if (eq == std::string::npos)
         continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      if (k == "retroarch_dir") s.retroarch_dir = v;
      else if (k == "core") s.core_file = v;
      else if (k == "rom") s.rom_path = v;
      else if (k == "ui_scale")
      {
         try { s.ui_scale = std::stof(v); } catch (...) {}
         if (s.ui_scale < 0.8f) s.ui_scale = 0.8f;
         if (s.ui_scale > 2.5f) s.ui_scale = 2.5f;
      }
   }
   if (s.retroarch_dir.empty())
      for (const char *guess : { "D:\\RetroArch", "C:\\RetroArch-Win64", "C:\\RetroArch" })
         if (file_exists(std::string(guess) + "\\retroarch.exe")) { s.retroarch_dir = guess; break; }
   if (s.retroarch_dir.empty())
      s.retroarch_dir = "D:\\RetroArch";
   return s;
}

static void save_settings(const Settings &s)
{
   write_text(settings_path(), "retroarch_dir=" + s.retroarch_dir + "\ncore=" + s.core_file + "\nrom=" + s.rom_path + "\nui_scale=" + std::to_string(s.ui_scale) + "\n");
}

// Reads `key = "value"` from retroarch.cfg, expanding RetroArch's ":" prefix.
static std::string retroarch_cfg(const std::string &ra, const std::string &key, const std::string &fallback)
{
   std::string text = read_text(ra + "\\retroarch.cfg");
   size_t pos = 0;
   while ((pos = text.find(key, pos)) != std::string::npos)
   {
      bool line_start = pos == 0 || text[pos - 1] == '\n';
      size_t after = pos + key.size();
      if (line_start && after < text.size() && (text[after] == ' ' || text[after] == '='))
      {
         size_t q1 = text.find('"', after), nl = text.find('\n', after);
         size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
         if (q1 != std::string::npos && q2 != std::string::npos && (nl == std::string::npos || q2 < nl))
         {
            std::string v = text.substr(q1 + 1, q2 - q1 - 1);
            if (v.empty())
               return fallback;
            return v[0] == ':' ? ra + v.substr(1) : v;
         }
      }
      pos = after;
   }
   return fallback;
}

// ---------------------------------------------------------------------------
// Audio output
// ---------------------------------------------------------------------------

struct AudioOut
{
   SDL_AudioDeviceID dev = 0;
   SDL_AudioStream *stream = nullptr;
   int stream_rate = 0;
   int volume = 100;
   bool muted = false;
   static constexpr int kRate = 48000;

   ~AudioOut()
   {
      if (dev) { SDL_CloseAudioDevice(dev); dev = 0; }
      if (stream) { SDL_FreeAudioStream(stream); stream = nullptr; }
   }

   void open()
   {
      SDL_AudioSpec want{}, have{};
      want.freq = kRate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
      dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
      if (dev)
         SDL_PauseAudioDevice(dev, 0);
   }

   void push(const int16_t *frames, size_t count, double rate)
   {
      if (!dev || !count || muted || volume <= 0)
         return;
      int r = (int)std::lround(rate);
      if (!stream || r != stream_rate)
      {
         if (stream)
            SDL_FreeAudioStream(stream);
         stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, r, AUDIO_S16SYS, 2, kRate);
         stream_rate = r;
      }
      if (!stream)
         return;
      if (volume < 100)
      {
         static std::vector<int16_t> scaled;
         scaled.resize(count * 2);
         float v = volume / 100.0f;
         for (size_t i = 0; i < count * 2; i++)
            scaled[i] = (int16_t)std::clamp((int)std::lround(frames[i] * v), -32768, 32767);
         SDL_AudioStreamPut(stream, scaled.data(), (int)(count * 4));
      }
      else
         SDL_AudioStreamPut(stream, frames, (int)(count * 4));

      static std::vector<uint8_t> buf;
      int avail = SDL_AudioStreamAvailable(stream);
      if (avail <= 0)
         return;
      buf.resize(avail);
      int got = SDL_AudioStreamGet(stream, buf.data(), avail);
      if (got > 0)
         SDL_QueueAudio(dev, buf.data(), got);
   }

   Uint32 queued() const { return dev ? SDL_GetQueuedAudioSize(dev) : 0; }
   void clear() { if (dev) SDL_ClearQueuedAudio(dev); if (stream) SDL_AudioStreamClear(stream); }
};

static bool write_wav(const std::string &path, const std::vector<int16_t> &samples, int rate)
{
   FILE *f = px_fopen(path.c_str(), "wb");
   if (!f)
      return false;
   uint32_t data_bytes = (uint32_t)(samples.size() * sizeof(int16_t));
   uint32_t file_bytes = data_bytes + 36;
   uint16_t channels = 2, bits_per_sample = 16, block_align = 4;
   uint32_t byte_rate = (uint32_t)(rate * block_align);

   fwrite("RIFF", 1, 4, f);
   fwrite(&file_bytes, 4, 1, f);
   fwrite("WAVEfmt ", 1, 8, f);
   uint32_t fmt_chunk_size = 16;
   uint16_t format_tag = 1;
   fwrite(&fmt_chunk_size, 4, 1, f);
   fwrite(&format_tag, 2, 1, f);
   fwrite(&channels, 2, 1, f);
   fwrite(&rate, 4, 1, f);
   fwrite(&byte_rate, 4, 1, f);
   fwrite(&block_align, 2, 1, f);
   fwrite(&bits_per_sample, 2, 1, f);
   fwrite("data", 1, 4, f);
   fwrite(&data_bytes, 4, 1, f);
   fwrite(samples.data(), 2, samples.size(), f);
   return fclose(f) == 0;
}

// ---------------------------------------------------------------------------
// Songs and the profile being edited
// ---------------------------------------------------------------------------

enum MapAction { MAP_ORIGINAL, MAP_SILENCE, MAP_FILE };

struct Song
{
   uint32_t value = 0;
   char name[96] = "";
   std::vector<int16_t> clip;
   double clip_rate = 0;
   bool recording = false;
   SDL_Texture *thumb = nullptr;
   int thumb_w = 0, thumb_h = 0;
   int action = MAP_ORIGINAL;
   char path[1024] = "";
   int volume = 100;
   bool loop = true;
   int loop_start = 0;
   int track = 1;
};

struct ProfileEdit
{
   std::string path;          // where it was loaded from / will be saved
   int memory = 0;            // index into kMemories
   uint32_t address = 0;
   bool have_address = false;
   int size = 1;
   bool latch = false;
   int debounce = 2;
   int unmapped = 0;          // original, silence, keep
   int crossfade = 400;
   int music_volume = 100;
   int game_volume = 100;
   std::map<std::string, std::string> mute;
   std::vector<std::string> library;
};

static const struct { const char *name; unsigned id; } kMemories[] = {
   { "system_ram", RETRO_MEMORY_SYSTEM_RAM },
   { "save_ram", RETRO_MEMORY_SAVE_RAM },
   { "video_ram", RETRO_MEMORY_VIDEO_RAM },
};
static const char *kUnmapped[] = { "original", "silence", "keep" };

struct App
{
   Settings settings;
   SDL_Renderer *renderer = nullptr;
   AudioOut audio;
   CoreHost core;
   SDL_Texture *game_tex = nullptr;
   int tex_w = 0, tex_h = 0;

   std::vector<std::pair<std::string, std::string>> cores; // file, display name
   bool paused = false;
   bool fast_forward = false;
   uint16_t pad = 0;
   SDL_GameController *controller = nullptr;
   std::vector<uint8_t> quick_state;
   std::string status;
   std::vector<std::string> log;

   SongFinder finder;
   bool finder_active = true;

   ProfileEdit profile;
   std::vector<Song> songs;
   uint32_t current_song = 0;
   bool have_current = false;
   uint32_t candidate_value = 0;
   unsigned candidate_frames = 0;

   // Preview: the real Proteus engine mixing the mapping into the game.
   px_engine engine{};
   bool preview = false;
   bool preview_dirty = false;

   // Analysis: play every value of a command register from a save state.
   struct
   {
      bool running = false;
      int from = 1, to = 0x7F, next = 0;
      uint32_t command = 0;
      std::vector<uint8_t> base;
      int found = 0, silent = 0;
      bool capturing = false;
   } analyze;

   // Auto-probe: sweep RAM addresses to detect which ones trigger sound changes.
   struct ProbedRegister
   {
      uint32_t address = 0;
      uint8_t original_val = 0;
      uint8_t test_val = 0;
      double score = 0;
      std::vector<int16_t> clip;
      double clip_rate = 0;
   };

   struct
   {
      bool running = false;
      uint32_t start_addr = 0;
      uint32_t end_addr = 0x1FFF;
      uint32_t current_addr = 0;
      int test_val = 2;
      std::vector<uint8_t> base;
      std::vector<int16_t> baseline_audio;
      double baseline_rms = 0;
      std::vector<ProbedRegister> results;
      bool capturing = false;
   } probe;

   // Clip playback pauses the game.
   bool clip_playing = false;
   bool was_paused = false;

   const RomPreset *detected_preset = nullptr;
   std::string detected_header_title;
   std::string detected_platform;

   struct CompareSource
   {
      std::string path;
      std::string name;
      std::string system;
      enum Type { TYPE_NONE, TYPE_ROM, TYPE_FOLDER, TYPE_PRESET } type = TYPE_NONE;
      const RomPreset *preset = nullptr;
      std::vector<Song> songs;
      int selected_song = -1;
      char filter[128] = "";
   } compare;

   int selected_target_song = 0;
   int playing_target_index = -1;
   int playing_compare_index = -1;
   bool is_playing = false;
   bool show_game_screen = false;
   bool mute_emulator = false;
   std::string export_status;

   struct DeepScan
   {
      bool running = false;
      std::vector<uint8_t> base;
      uint32_t address = 0;
      int mem = 0;
      int from = 1;
      int to = 64;
      int current = 1;
      int found = 0;
      int silent = 0;
      bool is_compare = false;
      std::string status_msg;
   } deep_scan;
};

static App *g_app = nullptr;

static void app_log(App &a, const std::string &msg)
{
   a.log.push_back(msg);
   if (a.log.size() > 1000)
      a.log.erase(a.log.begin(), a.log.begin() + 200);
}

static std::string hex(uint32_t v, int digits = 2)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "0x%0*X", digits, (unsigned)v);
   return buf;
}

static std::string content_name(const App &a)
{
   const std::string &c = a.core.content_path();
   size_t hash = c.find('#');
   return stem_of(hash == std::string::npos ? c : c.substr(hash + 1));
}

static std::string system_dir(const App &a)
{
   return retroarch_cfg(a.settings.retroarch_dir, "system_directory", a.settings.retroarch_dir + "\\system");
}

static Song *find_song(App &a, uint32_t value)
{
   for (auto &s : a.songs)
      if (s.value == value)
         return &s;
   return nullptr;
}

static void make_thumb(App &a, Song &s)
{
   const auto &px = a.core.frame();
   if (px.empty())
      return;
   if (s.thumb)
      SDL_DestroyTexture(s.thumb);
   s.thumb_w = (int)a.core.frame_width();
   s.thumb_h = (int)a.core.frame_height();
   s.thumb = SDL_CreateTexture(a.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, s.thumb_w, s.thumb_h);
   if (s.thumb)
      SDL_UpdateTexture(s.thumb, nullptr, px.data(), s.thumb_w * 4);
}

static Song &add_song(App &a, uint32_t value)
{
   if (Song *s = find_song(a, value))
      return *s;
   Song s;
   s.value = value;
   a.songs.push_back(s);
   std::sort(a.songs.begin(), a.songs.end(), [](const Song &x, const Song &y) { return x.value < y.value; });
   a.preview_dirty = true;
   return *find_song(a, value);
}

static void apply_rom_preset(App &a, const RomPreset &p)
{
   a.profile.address = p.address;
   a.profile.have_address = true;
   a.profile.memory = p.memory;
   a.profile.size = p.size;
   a.profile.latch = p.latch;
   a.profile.debounce = p.debounce;
   a.have_current = false;
   for (const auto &ks : p.songs)
   {
      Song *s = find_song(a, ks.value);
      if (!s)
      {
         Song &new_s = add_song(a, ks.value);
         snprintf(new_s.name, sizeof(new_s.name), "%s", ks.title.c_str());
      }
      else if (s->name[0] == '\0')
      {
         snprintf(s->name, sizeof(s->name), "%s", ks.title.c_str());
      }
   }
   a.status = "Applied preset: " + p.name + " (" + hex(p.address, 4) + (p.latch ? ", latch" : "") + ")";
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

static void load_profile_into_ui(App &a, const std::string &path)
{
   static px_profile p;
   char err[1200];
   if (!px_profile_load(&p, path.c_str(), err, sizeof(err)))
   {
      app_log(a, std::string("profile: ") + err);
      return;
   }
   ProfileEdit e;
   e.path = path;
   for (int i = 0; i < 3; i++)
      if (kMemories[i].id == p.memory_id)
         e.memory = i;
   e.address = p.address;
   e.have_address = true;
   e.size = (int)p.size;
   e.latch = p.latch;
   e.debounce = (int)p.debounce;
   e.unmapped = p.unmapped == PX_ACTION_SILENCE ? 1 : p.unmapped == PX_ACTION_KEEP ? 2 : 0;
   e.crossfade = (int)p.crossfade_ms;
   e.music_volume = (int)std::lround(p.music_volume * 100);
   e.game_volume = (int)std::lround(p.game_volume * 100);
   for (unsigned i = 0; i < p.mute_count; i++)
      e.mute[p.mute[i].key] = p.mute[i].value;
   for (unsigned i = 0; i < p.library_count; i++)
      e.library.push_back(relative_to(dir_of(path), p.library[i]));
   a.profile = e;

   for (unsigned i = 0; i < p.track_count; i++)
   {
      Song &s = add_song(a, p.tracks[i].value);
      const px_track &t = p.tracks[i];
      s.action = t.action == PX_ACTION_FILE ? MAP_FILE : t.action == PX_ACTION_SILENCE ? MAP_SILENCE : MAP_ORIGINAL;
      snprintf(s.path, sizeof(s.path), "%s", t.path);
      s.volume = (int)std::lround(t.volume * 100);
      s.loop = t.loop;
      s.loop_start = (int)t.loop_start;
      s.track = (int)t.subtrack + 1;
   }

   // Song names live in the comment after each track line.
   std::string text = read_text(path);
   size_t pos = 0;
   while (pos < text.size())
   {
      size_t end = text.find('\n', pos);
      std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      pos = end == std::string::npos ? text.size() : end + 1;
      size_t eq = line.find('='), semi = line.find(" ; ");
      if (eq == std::string::npos || semi == std::string::npos || semi < eq)
         continue;
      uint32_t value = (uint32_t)strtoul(line.substr(0, eq).c_str(), nullptr, 0);
      if (Song *s = find_song(a, value))
      {
         std::string name = line.substr(semi + 3);
         while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
            name.pop_back();
         snprintf(s->name, sizeof(s->name), "%s", name.c_str());
      }
   }
   app_log(a, "loaded profile " + path);
}

static std::string profile_text(const App &a, const std::string &profile_dir, bool absolute_paths)
{
   const ProfileEdit &e = a.profile;
   std::string t;
   char line[1400];
   t += "; Proteus Retune profile for " + content_name(a) + "\n; Made with Proteus Studio\n\n[song]\n";
   t += std::string("memory   = ") + kMemories[e.memory].name + "\n";
   t += "address  = " + hex(e.address, 4) + "\n";
   t += "size     = " + std::to_string(e.size) + "\n";
   t += "debounce = " + std::to_string(e.debounce) + "\n";
   t += std::string("unmapped = ") + kUnmapped[e.unmapped] + "\n";
   if (e.latch)
      t += "latch    = 1\n";
   if (!e.mute.empty())
   {
      t += "\n[mute]\n";
      for (auto &m : e.mute)
         t += m.first + " = " + m.second + "\n";
   }
   t += "\n[mix]\nmusic_volume = " + std::to_string(e.music_volume) + "\ngame_volume  = "
         + std::to_string(e.game_volume) + "\ncrossfade_ms = " + std::to_string(e.crossfade) + "\n";
   if (!e.library.empty())
   {
      t += "\n[library]\n";
      for (auto &l : e.library)
         t += "dir = " + l + "\n";
   }
   t += "\n[tracks]\n";
   for (const Song &s : a.songs)
   {
      std::string spec;
      if (s.action == MAP_FILE && s.path[0])
      {
         spec = absolute_paths ? s.path : relative_to(profile_dir, s.path);
         if (s.volume != 100) spec += " | volume=" + std::to_string(s.volume);
         if (!s.loop) spec += " | loop=0";
         if (s.loop_start > 0) spec += " | loop_start=" + std::to_string(s.loop_start);
         if (s.track > 1) spec += " | track=" + std::to_string(s.track);
      }
      else
         spec = s.action == MAP_SILENCE ? "silence" : "original";
      snprintf(line, sizeof(line), "%s = %s%s%s\n", hex(s.value).c_str(), spec.c_str(),
            s.name[0] ? " ; " : "", s.name);
      t += line;
   }
   return t;
}

// Writes the channel mutes as RetroArch per-game core options for the DSP plugin,
// starting from the game's (or the core's) existing options so nothing else changes.
static bool write_game_options(App &a, std::string &written)
{
   std::string dir = retroarch_cfg(a.settings.retroarch_dir, "rgui_config_directory",
         a.settings.retroarch_dir + "\\config") + "\\" + a.core.library_name();
   std::string game = dir + "\\" + content_name(a) + ".opt";
   std::string base = file_exists(game) ? read_text(game) : read_text(dir + "\\" + a.core.library_name() + ".opt");

   std::map<std::string, std::string> values;
   std::vector<std::string> order;
   for (auto &o : a.core.options())
   {
      values[o.key] = o.value;
      order.push_back(o.key);
   }
   size_t pos = 0;
   while (pos < base.size())
   {
      size_t end = base.find('\n', pos);
      std::string line = base.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      pos = end == std::string::npos ? base.size() : end + 1;
      size_t eq = line.find(" = \""), q = line.rfind('"');
      if (eq == std::string::npos || q <= eq + 3)
         continue;
      std::string k = line.substr(0, eq);
      if (!values.count(k))
         order.push_back(k);
      values[k] = line.substr(eq + 4, q - eq - 4);
   }
   for (auto &m : a.profile.mute)
      values[m.first] = m.second;

   std::string out;
   for (auto &k : order)
      out += k + " = \"" + values[k] + "\"\n";
   make_dirs(dir);
   written = game;
   return write_text(game, out);
}

// ---------------------------------------------------------------------------
// Preview through the Proteus engine
// ---------------------------------------------------------------------------

static const uint8_t *host_memory(void *, unsigned id, size_t *size) { return g_app->core.memory(id, size); }
static void host_log(void *, enum retro_log_level, const char *msg) { app_log(*g_app, std::string("preview: ") + msg); }
static void host_notify(void *, const char *msg) { g_app->status = msg; }
static void host_mute(void *, bool muted)
{
   for (auto &m : g_app->profile.mute)
      g_app->core.set_override(m.first, muted ? m.second : "");
}

static void start_preview(App &a)
{
   if (!a.profile.have_address)
   {
      a.status = "Set the song address first.";
      a.preview = false;
      return;
   }
   std::string path = app_data_dir() + "/preview.ini";
   write_text(path, profile_text(a, app_data_dir(), true));
   a.core.clear_overrides();
   if (px_engine_load(&a.engine, path.c_str()))
   {
      px_engine_set_rate(&a.engine, a.core.sample_rate());
      a.preview = true;
   }
   else
   {
      a.preview = false;
      a.status = "Preview failed; see the log.";
   }
   a.preview_dirty = false;
}

static void stop_preview(App &a)
{
   px_engine_unload(&a.engine);
   a.core.clear_overrides();
   a.preview = false;
}

// ---------------------------------------------------------------------------
// Emulation
// ---------------------------------------------------------------------------

static void refresh_cores(App &a)
{
   a.cores.clear();
   std::string dir = a.settings.retroarch_dir + "\\cores";
   std::vector<std::string> files = list_files(dir);
   std::sort(files.begin(), files.end());
   for (auto &f : files)
   {
      if (f.size() < 13 || f.compare(f.size() - 13, 13, "_libretro.dll") || !f.compare(0, 8, "proteus_"))
         continue;
      std::string info = read_text(a.settings.retroarch_dir + "\\info\\" + f.substr(0, f.size() - 4) + ".info");
      std::string name = f;
      size_t p = info.find("display_name = \"");
      if (p != std::string::npos)
         name = info.substr(p + 16, info.find('"', p + 16) - p - 16);
      a.cores.push_back({ f, name });
   }
}

static void load_game(App &a)
{
   std::string err;
   stop_preview(a);
   a.audio.clear();
   for (auto &s : a.songs)
      if (s.thumb)
         SDL_DestroyTexture(s.thumb);
   a.songs.clear();
   a.profile = ProfileEdit();
   a.have_current = false;
   a.analyze.running = false;
   a.probe.running = false;
   a.probe.results.clear();

   std::string save_dir = app_data_dir() + "\\saves";
   make_dirs(save_dir);
   if (!a.core.load(a.settings.retroarch_dir + "\\cores\\" + a.settings.core_file, a.settings.rom_path,
            system_dir(a), save_dir, err))
   {
      a.status = err;
      app_log(a, err);
      return;
   }
   save_settings(a.settings);
   a.paused = !a.show_game_screen;
   a.status = "Loaded " + content_name(a);

   size_t size = 0;
   a.core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
   a.finder.reset(size);

   a.detected_header_title = detect_rom_header_title(a.core.content_data().data(), a.core.content_data().size(), a.detected_platform);
   a.detected_preset = detect_preset(a.core.content_data().data(), a.core.content_data().size(), a.settings.rom_path);

   char found[2048];
   if (px_engine_find_profile(a.core.content_path().c_str(), system_dir(a).c_str(), found, sizeof(found)))
      load_profile_into_ui(a, found);
   else
   {
      a.profile.path = system_dir(a) + "\\proteus\\" + content_name(a) + ".ini";
      if (a.detected_preset)
      {
         apply_rom_preset(a, *a.detected_preset);
         a.status = "Preset loaded: " + a.detected_preset->name + " (" + std::to_string(a.songs.size()) + " songs ready to mix & match)";
      }
   }
}

static uint32_t read_address(App &a, uint32_t address, int size, bool *ok)
{
   size_t mem = 0;
   const uint8_t *ram = a.core.memory(kMemories[a.profile.memory].id, &mem);
   uint32_t v = 0;
   *ok = ram && (uint64_t)address + size <= mem;
   for (int i = 0; *ok && i < size; i++)
      v |= (uint32_t)ram[address + i] << (8 * i);
   return v;
}

// Follows the song address while playing and collects every song heard.
static void track_songs(App &a)
{
   if (!a.profile.have_address)
      return;
   bool ok;
   uint32_t v = read_address(a, a.profile.address, a.profile.size, &ok);
   if (!ok || (a.profile.latch && v == 0))
      return;
   if (v != a.candidate_value)
   {
      a.candidate_value = v;
      a.candidate_frames = 0;
   }
   if (++a.candidate_frames < (unsigned)std::max(1, a.profile.debounce) || (a.have_current && v == a.current_song))
      return;

   a.current_song = v;
   a.have_current = true;
   bool fresh = find_song(a, v) == nullptr;
   Song &s = add_song(a, v);
   if (fresh || s.clip.empty())
   {
      s.clip.clear();
      s.clip_rate = a.core.sample_rate();
      s.recording = true;
      make_thumb(a, s);
   }
   app_log(a, "song " + hex(v) + (fresh ? " (new)" : ""));
}

static void on_core_audio(int16_t *frames, size_t count)
{
   App &a = *g_app;
   if (!a.analyze.capturing && !a.probe.capturing)
   {
      // Record the original music before any preview is mixed in.
      for (auto &s : a.songs)
      {
         if (!s.recording)
            continue;
         s.clip.insert(s.clip.end(), frames, frames + count * 2);
         if (s.clip.size() >= (size_t)(s.clip_rate * 12) * 2)
            s.recording = false;
      }
   }

   if (a.preview && px_engine_mixing(&a.engine) && !a.analyze.capturing && !a.probe.capturing)
      px_engine_mix_s16(&a.engine, frames, count);
}

static void run_one_frame(App &a, bool output_audio)
{
   if (a.preview)
      px_engine_frame(&a.engine);
   a.core.run_frame(a.pad);
   size_t size = 0;
   const uint8_t *ram = a.core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
   if (a.finder_active && ram)
      a.finder.update(ram, size);
   track_songs(a);
   if (output_audio)
      a.audio.push(a.core.audio().data(), a.core.audio().size() / 2, a.core.sample_rate());
   a.core.audio().clear();
}

static void upload_frame(App &a)
{
   const auto &px = a.core.frame();
   int w = (int)a.core.frame_width(), h = (int)a.core.frame_height();
   if (px.empty())
      return;
   if (!a.game_tex || w != a.tex_w || h != a.tex_h)
   {
      if (a.game_tex)
         SDL_DestroyTexture(a.game_tex);
      a.game_tex = SDL_CreateTexture(a.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
      a.tex_w = w;
      a.tex_h = h;
   }
   SDL_UpdateTexture(a.game_tex, nullptr, px.data(), w * 4);
}

// One song of the analysis per UI frame: load the base state, send the command,
// let the sound driver start, and record what comes out.
static void analyze_step(App &a)
{
   auto &an = a.analyze;
   if (!an.running)
      return;
   if (an.next > an.to)
   {
      an.running = false;
      a.core.load_state(an.base);
      a.core.audio().clear();
      a.status = "Analysis finished: " + std::to_string(an.found) + " songs, " + std::to_string(an.silent) + " silent values.";
      return;
   }

   int v = an.next++;
   a.core.load_state(an.base);
   size_t size = 0;
   uint8_t *ram = a.core.memory_mut(kMemories[a.profile.memory].id, &size);
   if (!ram || an.command >= size)
   {
      an.running = false;
      return;
   }
   bool ok;
   uint32_t before = read_address(a, a.profile.address, a.profile.size, &ok);
   ram[an.command] = (uint8_t)v;

   an.capturing = true;
   std::vector<int16_t> clip;
   const int settle = 20, capture = 300;
   for (int f = 0; f < settle + capture; f++)
   {
      a.core.run_frame(0);
      if (f >= settle)
         clip.insert(clip.end(), a.core.audio().begin(), a.core.audio().end());
      a.core.audio().clear();
   }
   an.capturing = false;

   double sum = 0;
   for (int16_t s : clip)
      sum += (double)s * s;
   double rms = clip.empty() ? 0 : std::sqrt(sum / clip.size());
   if (rms < 60)
   {
      an.silent++;
      return;
   }

   // Name the song by what the song address shows, when it follows the command.
   uint32_t after = read_address(a, a.profile.address, a.profile.size, &ok);
   uint32_t key = (a.profile.have_address && a.profile.address != an.command && ok && after != before) ? after : (uint32_t)v;
   bool fresh = find_song(a, key) == nullptr;
   Song &s = add_song(a, key);
   if (fresh || s.clip.empty())
   {
      s.clip = clip;
      s.clip_rate = a.core.sample_rate();
      make_thumb(a, s);
   }
   if (!s.name[0])
      snprintf(s.name, sizeof(s.name), "command %s", hex((uint32_t)v).c_str());
   an.found++;
}

static void poke_ram(App &a, uint32_t address, uint8_t val)
{
   size_t size = 0;
   uint8_t *ram = a.core.memory_mut(kMemories[a.profile.memory].id, &size);
   if (ram && address < size)
   {
      ram[address] = val;
      a.status = "Poked " + hex(address, 4) + " = " + hex(val);
      app_log(a, a.status);
   }
}

static void start_probe(App &a)
{
   if (!a.core.loaded())
      return;
   stop_preview(a);
   auto &pb = a.probe;
   pb.base = a.core.save_state();
   if (pb.base.empty())
   {
      a.status = "This core cannot save states, so it cannot be auto-probed.";
      return;
   }
   pb.results.clear();
   pb.baseline_audio.clear();

   size_t ram_size = 0;
   a.core.memory(kMemories[a.profile.memory].id, &ram_size);
   if (ram_size == 0)
   {
      a.status = "No RAM accessible for this memory region.";
      return;
   }
   if (pb.end_addr >= ram_size)
      pb.end_addr = (uint32_t)(ram_size - 1);
   if (pb.start_addr > pb.end_addr)
      pb.start_addr = 0;

   // 1. Record baseline audio (40 frames with no modifications)
   a.core.set_skip_video(true);
   pb.capturing = true;
   a.core.load_state(pb.base);
   a.core.audio().clear();
   for (int f = 0; f < 40; f++)
   {
      a.core.run_frame(0);
      pb.baseline_audio.insert(pb.baseline_audio.end(), a.core.audio().begin(), a.core.audio().end());
      a.core.audio().clear();
   }
   pb.capturing = false;

   double sum = 0;
   for (int16_t s : pb.baseline_audio)
      sum += (double)s * s;
   pb.baseline_rms = pb.baseline_audio.empty() ? 0 : std::sqrt(sum / pb.baseline_audio.size());

   pb.current_addr = pb.start_addr;
   pb.running = true;
   a.status = "Auto-probing RAM 0x" + hex(pb.start_addr, 4) + " to 0x" + hex(pb.end_addr, 4) + "...";
   app_log(a, a.status);
}

static void probe_step(App &a)
{
   auto &pb = a.probe;
   if (!pb.running)
      return;

   size_t ram_size = 0;
   uint8_t *ram = a.core.memory_mut(kMemories[a.profile.memory].id, &ram_size);
   if (!ram)
   {
      pb.running = false;
      a.core.set_skip_video(false);
      return;
   }

   const int kBatch = 8;
   for (int step = 0; step < kBatch && pb.current_addr <= pb.end_addr; step++, pb.current_addr++)
   {
      uint32_t addr = pb.current_addr;
      if (addr >= ram_size)
         break;

      uint8_t orig_val = ram[addr];
      uint8_t tval = (orig_val == (uint8_t)pb.test_val) ? (uint8_t)(pb.test_val + 1) : (uint8_t)pb.test_val;

      a.core.load_state(pb.base);
      ram[addr] = tval;
      a.core.audio().clear();

      pb.capturing = true;
      std::vector<int16_t> clip;
      for (int f = 0; f < 40; f++)
      {
         a.core.run_frame(0);
         clip.insert(clip.end(), a.core.audio().begin(), a.core.audio().end());
         a.core.audio().clear();
      }
      pb.capturing = false;

      double diff_sum = 0;
      size_t cmp_len = std::min(clip.size(), pb.baseline_audio.size());
      for (size_t i = 0; i < cmp_len; i++)
      {
         double d = (double)clip[i] - (double)pb.baseline_audio[i];
         diff_sum += d * d;
      }
      double diff_rms = cmp_len ? std::sqrt(diff_sum / cmp_len) : 0;

      if (diff_rms > 120.0)
      {
         App::ProbedRegister res;
         res.address = addr;
         res.original_val = orig_val;
         res.test_val = tval;
         res.score = diff_rms;
         res.clip = clip;
         res.clip_rate = a.core.sample_rate();
         pb.results.push_back(res);

         std::sort(pb.results.begin(), pb.results.end(), [](const App::ProbedRegister &x, const App::ProbedRegister &y) {
            return x.score > y.score;
         });
         if (pb.results.size() > 50)
            pb.results.pop_back();
      }
   }

   if (pb.current_addr > pb.end_addr)
   {
      pb.running = false;
      a.core.set_skip_video(false);
      a.core.load_state(pb.base);
      a.core.audio().clear();
      a.status = "Probe finished: " + std::to_string(pb.results.size()) + " sound register candidate(s) found.";
      app_log(a, a.status);
   }
}

static void play_clip(App &a, const std::vector<int16_t> &clip, double rate)
{
   a.audio.clear();
   a.was_paused = a.paused;
   a.paused = true;
   a.clip_playing = true;
   a.audio.push(clip.data(), clip.size() / 2, rate);
}

static void play_file(App &a, const Song &s)
{
   char err[1200];
   px_source *src = px_source_open(s.path, (unsigned)std::max(0, s.track - 1), true, AudioOut::kRate, err, sizeof(err));
   if (!src)
   {
      a.status = err;
      return;
   }
   std::vector<int16_t> buf(px_source_rate(src) * 20 * 2);
   size_t got = px_source_read(src, buf.data(), buf.size() / 2);
   buf.resize(got * 2);
   double rate = px_source_rate(src);
   px_source_close(src);
   play_clip(a, buf, rate);
}

static void stop_audio(App &a)
{
   a.audio.clear();
   a.clip_playing = false;
   a.is_playing = false;
   a.playing_target_index = -1;
   a.playing_compare_index = -1;
   a.paused = true; // Always halt emulator audio output so Stop means silence
}

static void capture_and_play_song(App &a, Song &s)
{
   if (!a.core.loaded() || !a.profile.have_address)
      return;
   std::vector<uint8_t> st = a.core.save_state();
   if (st.empty())
   {
      poke_ram(a, a.profile.address, (uint8_t)s.value);
      a.paused = false;
      return;
   }
   size_t size = 0;
   uint8_t *ram = a.core.memory_mut(kMemories[a.profile.memory].id, &size);
   if (ram && a.profile.address < size)
      ram[a.profile.address] = (uint8_t)s.value;
   a.core.set_skip_video(true);
   a.core.audio().clear();
   s.clip.clear();
   for (int f = 0; f < 180; f++)
   {
      a.core.run_frame(0);
      s.clip.insert(s.clip.end(), a.core.audio().begin(), a.core.audio().end());
      a.core.audio().clear();
   }
   a.core.set_skip_video(false);
   a.core.load_state(st);
   s.clip_rate = a.core.sample_rate();
   if (!s.clip.empty())
      play_clip(a, s.clip, s.clip_rate);
}

static void capture_and_play_compare_song(App &a, Song &s, int index)
{
   if (a.compare.path.empty() || !file_exists(a.compare.path))
   {
      a.status = "Compare ROM file not found: " + a.compare.path;
      return;
   }

   uint32_t addr = a.compare.preset ? a.compare.preset->address : (a.profile.have_address ? a.profile.address : 0x1DFB);
   int mem = a.compare.preset ? a.compare.preset->memory : 0;

   a.status = "Auditioning compare ROM song " + (s.name[0] ? std::string(s.name) : hex(s.value)) + "...";

   std::string target_rom = a.settings.rom_path;
   std::string target_core = a.settings.retroarch_dir + "\\cores\\" + a.settings.core_file;
   std::string err;
   std::vector<uint8_t> target_state;
   if (a.core.loaded())
      target_state = a.core.save_state();

   std::string save_dir = app_data_dir() + "\\saves";
   if (a.core.load(target_core, a.compare.path, system_dir(a), save_dir, err))
   {
      size_t size = 0;
      uint8_t *ram = a.core.memory_mut(kMemories[mem].id, &size);
      if (ram && addr < size)
         ram[addr] = (uint8_t)s.value;

      a.core.set_skip_video(true);
      a.core.audio().clear();
      s.clip.clear();
      for (int f = 0; f < 180; f++)
      {
         a.core.run_frame(0);
         s.clip.insert(s.clip.end(), a.core.audio().begin(), a.core.audio().end());
         a.core.audio().clear();
      }
      a.core.set_skip_video(false);
      s.clip_rate = a.core.sample_rate();

      // Restore target ROM & state
      if (!target_rom.empty())
      {
         a.core.load(target_core, target_rom, system_dir(a), save_dir, err);
         if (!target_state.empty())
            a.core.load_state(target_state);
      }

      if (!s.clip.empty())
      {
         play_clip(a, s.clip, s.clip_rate);
         a.is_playing = true;
         a.playing_compare_index = index;
         a.status = "Playing compare song: " + (s.name[0] ? std::string(s.name) : hex(s.value));
      }
      else
      {
         a.status = "No audio produced by compare ROM for " + hex(s.value);
      }
   }
   else
   {
      a.status = "Could not load compare ROM in current core: " + err;
   }
}

static void start_deep_scan(App &a, bool is_compare, int max_val = 64)
{
   if (!a.core.loaded())
   {
      a.status = "Please load a core and ROM first before scanning.";
      return;
   }
   stop_audio(a);

   uint32_t addr = a.profile.address;
   int mem = a.profile.memory;
   if (!a.profile.have_address)
   {
      if (a.detected_preset)
      {
         apply_rom_preset(a, *a.detected_preset);
         addr = a.profile.address;
         mem = a.profile.memory;
      }
      else if (!a.probe.results.empty())
      {
         addr = a.probe.results[0].address;
         mem = 0;
         a.profile.address = addr;
         a.profile.have_address = true;
      }
      else
      {
         addr = 0x1DFB;
         mem = 0;
         a.profile.address = addr;
         a.profile.have_address = true;
      }
   }

   a.deep_scan.base = a.core.save_state();
   if (a.deep_scan.base.empty())
   {
      a.status = "This core cannot save states; deep scan requires state save support.";
      return;
   }

   a.deep_scan.address = addr;
   a.deep_scan.mem = mem;
   a.deep_scan.from = 1;
   a.deep_scan.to = std::clamp(max_val, 16, 255);
   a.deep_scan.current = 1;
   a.deep_scan.found = 0;
   a.deep_scan.silent = 0;
   a.deep_scan.is_compare = is_compare;
   a.deep_scan.running = true;
   a.core.set_skip_video(true);
   a.status = "Deep scanning " + std::string(is_compare ? "Compare" : "Target") + " ROM from 0x01 to " + hex((uint32_t)a.deep_scan.to) + "...";
   app_log(a, a.status);
}

static void deep_scan_step(App &a)
{
   auto &ds = a.deep_scan;
   if (!ds.running)
      return;

   size_t size = 0;
   uint8_t *ram = a.core.memory_mut(kMemories[ds.mem].id, &size);
   if (!ram || ds.address >= size)
   {
      ds.running = false;
      a.core.set_skip_video(false);
      return;
   }

   for (int step = 0; step < 2 && ds.current <= ds.to; step++, ds.current++)
   {
      uint32_t val = (uint32_t)ds.current;

      a.core.load_state(ds.base);
      ram[ds.address] = (uint8_t)val;
      a.core.audio().clear();

      std::vector<int16_t> clip;
      const int settle = 15, capture = 50;
      for (int f = 0; f < settle + capture; f++)
      {
         a.core.run_frame(0);
         if (f >= settle)
            clip.insert(clip.end(), a.core.audio().begin(), a.core.audio().end());
         a.core.audio().clear();
      }

      double sum = 0;
      for (int16_t s : clip)
         sum += (double)s * s;
      double rms = clip.empty() ? 0 : std::sqrt(sum / clip.size());

      if (rms >= 55.0)
      {
         std::vector<Song> &song_list = ds.is_compare ? a.compare.songs : a.songs;
         Song *existing = nullptr;
         for (auto &s : song_list)
         {
            if (s.value == val)
            {
               existing = &s;
               break;
            }
         }

         if (!existing)
         {
            Song s;
            s.value = val;
            snprintf(s.name, sizeof(s.name), "Sound 0x%02X", (unsigned)val);
            s.clip = clip;
            s.clip_rate = a.core.sample_rate();
            if (!ds.is_compare)
               make_thumb(a, s);
            song_list.push_back(s);
            ds.found++;
         }
         else if (existing->clip.empty())
         {
            existing->clip = clip;
            existing->clip_rate = a.core.sample_rate();
            if (!ds.is_compare && !existing->thumb)
               make_thumb(a, *existing);
            ds.found++;
         }
      }
      else
      {
         ds.silent++;
      }
   }

   if (ds.current > ds.to)
   {
      ds.running = false;
      a.core.set_skip_video(false);
      a.core.load_state(ds.base);
      a.core.audio().clear();
      a.paused = true;
      a.status = "Deep scan complete: " + std::to_string(ds.found) + " songs found (tested " + std::to_string(ds.to) + " sound IDs)!";
      app_log(a, a.status);
   }
}

static void play_song_item(App &a, Song &s, bool is_compare, int index)
{
   if (a.is_playing && ((is_compare && a.playing_compare_index == index) || (!is_compare && a.playing_target_index == index)))
   {
      stop_audio(a);
      return;
   }

   stop_audio(a);

   // 1. If song has an audio file that exists, play it (WAV, MP3, OGG, FLAC, SPC, NSF, VGM, GBS)
   if (s.path[0] && file_exists(s.path))
   {
      play_file(a, s);
      a.is_playing = true;
      if (is_compare) a.playing_compare_index = index;
      else a.playing_target_index = index;
      return;
   }

   // 2. If song has a recorded clip, play it
   if (!s.clip.empty())
   {
      play_clip(a, s.clip, s.clip_rate);
      a.is_playing = true;
      if (is_compare) a.playing_compare_index = index;
      else a.playing_target_index = index;
      return;
   }

   // 3. For compare song from a ROM: capture and play
   if (is_compare && a.compare.type == App::CompareSource::TYPE_ROM)
   {
      capture_and_play_compare_song(a, s, index);
      return;
   }

   // 4. For target song with core loaded: capture / audition live
   if (!is_compare && a.core.loaded() && a.profile.have_address)
   {
      capture_and_play_song(a, s);
      a.is_playing = true;
      a.playing_target_index = index;
      a.status = "Auditioning " + (s.name[0] ? std::string(s.name) : hex(s.value));
      return;
   }

   a.status = "No audio available for " + (s.name[0] ? std::string(s.name) : hex(s.value));
}

static void load_compare_rom(App &a, const std::string &path)
{
   a.compare.path = path;
   a.compare.type = App::CompareSource::TYPE_ROM;
   a.compare.name = stem_of(path);
   a.compare.system = "ROM";
   a.compare.preset = nullptr;
   a.compare.songs.clear();
   a.compare.selected_song = -1;

   std::vector<uint8_t> data;
   if (read_file_bytes(path, data))
   {
      std::string sys;
      detect_rom_header_title(data.data(), data.size(), sys);
      if (!sys.empty()) a.compare.system = sys;
      a.compare.preset = detect_preset(data.data(), data.size(), path);
   }

   if (a.compare.preset)
   {
      a.compare.name = a.compare.preset->name;
      a.compare.system = a.compare.preset->system;
      for (const auto &ks : a.compare.preset->songs)
      {
         Song s;
         s.value = ks.value;
         snprintf(s.name, sizeof(s.name), "%s", ks.title.c_str());
         a.compare.songs.push_back(s);
      }
   }

   char found[2048];
   if (px_engine_find_profile(path.c_str(), system_dir(a).c_str(), found, sizeof(found)))
   {
      px_profile p;
      char err[512];
      if (px_profile_load(&p, found, err, sizeof(err)))
      {
         for (unsigned i = 0; i < p.track_count; i++)
         {
            bool exists = false;
            for (auto &cs : a.compare.songs)
            {
               if (cs.value == p.tracks[i].value)
               {
                  if (p.tracks[i].action == PX_ACTION_FILE)
                     snprintf(cs.path, sizeof(cs.path), "%s", p.tracks[i].path);
                  exists = true;
                  break;
               }
            }
            if (!exists)
            {
               Song s;
               s.value = p.tracks[i].value;
               snprintf(s.name, sizeof(s.name), "song 0x%02X", (unsigned)s.value);
               if (p.tracks[i].action == PX_ACTION_FILE)
                  snprintf(s.path, sizeof(s.path), "%s", p.tracks[i].path);
               a.compare.songs.push_back(s);
            }
         }
      }
   }

   if (a.compare.songs.empty())
   {
      for (uint32_t i = 1; i <= 32; i++)
      {
         Song s;
         s.value = i;
         snprintf(s.name, sizeof(s.name), "Song %s", hex(i).c_str());
         a.compare.songs.push_back(s);
      }
   }

   a.status = "Loaded compare ROM: " + a.compare.name + " (" + std::to_string(a.compare.songs.size()) + " songs)";
   app_log(a, a.status);
}

static void load_compare_file(App &a, const std::string &path)
{
   a.compare.path = path;
   a.compare.type = App::CompareSource::TYPE_FOLDER;
   a.compare.name = stem_of(path);
   a.compare.system = "Music File (" + lower_ext(path) + ")";
   a.compare.preset = nullptr;
   a.compare.songs.clear();
   a.compare.selected_song = -1;

   unsigned count = px_source_song_count(path.c_str());
   if (count > 1)
   {
      for (unsigned i = 1; i <= count; i++)
      {
         Song s;
         s.value = i;
         s.track = (int)i;
         snprintf(s.name, sizeof(s.name), "%s - Track %u", stem_of(path).c_str(), i);
         snprintf(s.path, sizeof(s.path), "%s", path.c_str());
         s.action = MAP_FILE;
         a.compare.songs.push_back(s);
      }
      a.status = "Loaded multi-track music file: " + a.compare.name + " (" + std::to_string(count) + " tracks)";
   }
   else
   {
      Song s;
      s.value = 1;
      s.track = 1;
      snprintf(s.name, sizeof(s.name), "%s", stem_of(path).c_str());
      snprintf(s.path, sizeof(s.path), "%s", path.c_str());
      s.action = MAP_FILE;
      a.compare.songs.push_back(s);
      a.status = "Loaded music file: " + a.compare.name;
   }
   app_log(a, a.status);
}

static void load_compare_folder(App &a, const std::string &folder)
{
   a.compare.path = folder;
   a.compare.type = App::CompareSource::TYPE_FOLDER;
   a.compare.name = file_name(folder);
   a.compare.system = "Music Folder";
   a.compare.preset = nullptr;
   a.compare.songs.clear();
   a.compare.selected_song = -1;

   std::vector<std::string> files = list_files(folder);
   std::sort(files.begin(), files.end());
   for (const auto &f : files)
   {
      std::string ext = lower_ext(f);
      if (ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac" ||
          ext == "spc" || ext == "vgm" || ext == "vgz" || ext == "nsf" ||
          ext == "nsfe" || ext == "gbs" || ext == "hes" || ext == "kss" ||
          ext == "gym" || ext == "ay"  || ext == "sap")
      {
         std::string full_path = folder + "\\" + f;
         unsigned count = px_source_song_count(full_path.c_str());
         if (count > 1)
         {
            for (unsigned t = 1; t <= count; t++)
            {
               Song s;
               s.value = (uint32_t)(a.compare.songs.size() + 1);
               s.track = (int)t;
               snprintf(s.name, sizeof(s.name), "%s - #%u", stem_of(f).c_str(), t);
               snprintf(s.path, sizeof(s.path), "%s", full_path.c_str());
               s.action = MAP_FILE;
               a.compare.songs.push_back(s);
            }
         }
         else
         {
            Song s;
            s.value = (uint32_t)(a.compare.songs.size() + 1);
            s.track = 1;
            snprintf(s.name, sizeof(s.name), "%s", stem_of(f).c_str());
            snprintf(s.path, sizeof(s.path), "%s", full_path.c_str());
            s.action = MAP_FILE;
            a.compare.songs.push_back(s);
         }
      }
   }

   a.status = "Loaded compare music folder: " + a.compare.name + " (" + std::to_string(a.compare.songs.size()) + " tracks)";
   app_log(a, a.status);
}

static void load_compare_preset(App &a, const RomPreset &p)
{
   a.compare.path = p.name;
   a.compare.type = App::CompareSource::TYPE_PRESET;
   a.compare.name = p.name;
   a.compare.system = p.system;
   a.compare.preset = &p;
   a.compare.songs.clear();
   a.compare.selected_song = -1;

   for (const auto &ks : p.songs)
   {
      Song s;
      s.value = ks.value;
      snprintf(s.name, sizeof(s.name), "%s", ks.title.c_str());
      a.compare.songs.push_back(s);
   }

   a.status = "Loaded compare preset: " + a.compare.name + " (" + std::to_string(a.compare.songs.size()) + " songs)";
   app_log(a, a.status);
}

static void match_songs(App &a, Song &target, const Song &source)
{
   target.action = MAP_FILE;
   target.track = source.track;
   if (source.path[0])
   {
      snprintf(target.path, sizeof(target.path), "%s", source.path);
      if (!source.clip.empty())
      {
         target.clip = source.clip;
         target.clip_rate = source.clip_rate;
      }
   }
   else
   {
      std::string game = content_name(a);
      std::string safe = sanitize_filename(source.name[0] ? source.name : ("track_" + hex(source.value)));
      std::string sys = system_dir(a);
      std::string dest = sys + "\\proteus\\music\\" + game + "\\" + hex(target.value) + "_" + safe + ".wav";
      snprintf(target.path, sizeof(target.path), "%s", dest.c_str());
      if (!source.clip.empty())
      {
         target.clip = source.clip;
         target.clip_rate = source.clip_rate;
      }
   }
   a.preview_dirty = true;
   a.status = "Matched: " + (target.name[0] ? std::string(target.name) : hex(target.value)) +
              " -> " + (source.name[0] ? std::string(source.name) : hex(source.value));
   app_log(a, a.status);
}

static void swap_target_and_compare(App &a)
{
   if (a.compare.type != App::CompareSource::TYPE_ROM || a.compare.path.empty())
   {
      a.status = "Compare source must be a ROM to swap emulation.";
      return;
   }

   std::string old_rom = a.settings.rom_path;
   std::string old_name = content_name(a);
   std::string old_sys = a.detected_platform;
   const RomPreset *old_preset = a.detected_preset;
   std::vector<Song> old_songs = a.songs;

   std::string new_rom = a.compare.path;

   a.compare.path = old_rom;
   a.compare.name = old_name;
   a.compare.system = old_sys;
   a.compare.type = App::CompareSource::TYPE_ROM;
   a.compare.preset = old_preset;
   a.compare.songs = old_songs;
   a.compare.selected_song = -1;

   a.settings.rom_path = new_rom;
   load_game(a);
   a.status = "Swapped Target & Compare ROMs. Now editing: " + content_name(a);
   app_log(a, a.status);
}

static void auto_match_songs(App &a)
{
   if (a.songs.empty() || a.compare.songs.empty())
      return;

   int count = 0;
   for (Song &target : a.songs)
   {
      std::string tname = to_lower(target.name[0] ? target.name : "");
      std::string thex = to_lower(hex(target.value));

      int best_idx = -1;
      int best_score = 0;

      for (size_t j = 0; j < a.compare.songs.size(); j++)
      {
         const Song &src = a.compare.songs[j];
         std::string sname = to_lower(src.name[0] ? src.name : "");
         int score = 0;

         if (!tname.empty() && sname == tname)
            score = 100;
         else if (!tname.empty() && (sname.find(tname) != std::string::npos || tname.find(sname) != std::string::npos))
            score = 70;
         else if (sname.find(thex) != std::string::npos || (!thex.empty() && sname.find(thex.substr(2)) != std::string::npos))
            score = 50;

         if (score > best_score)
         {
            best_score = score;
            best_idx = (int)j;
         }
      }

      if (best_idx >= 0 && best_score >= 50)
      {
         match_songs(a, target, a.compare.songs[best_idx]);
         count++;
      }
   }

   a.status = "Auto-matched " + std::to_string(count) + " song(s) by title / number.";
   app_log(a, a.status);
}

static void match_all_in_order(App &a)
{
   if (a.songs.empty() || a.compare.songs.empty())
      return;

   size_t n = std::min(a.songs.size(), a.compare.songs.size());
   for (size_t i = 0; i < n; i++)
      match_songs(a, a.songs[i], a.compare.songs[i]);

   a.status = "Matched " + std::to_string(n) + " song(s) 1:1 in order.";
   app_log(a, a.status);
}

static bool export_to_retroarch(App &a, std::string &out_ini, std::string &out_music_dir, int &exported_tracks)
{
   std::string game = content_name(a);
   if (game.empty())
   {
      a.status = "Error: No target ROM loaded to export.";
      return false;
   }

   std::string sys = system_dir(a);
   if (sys.empty())
   {
      a.status = "Error: RetroArch system folder not configured.";
      return false;
   }

   std::string proteus_dir = sys + "\\proteus";
   std::string music_dir = proteus_dir + "\\music\\" + game;
   make_dirs(proteus_dir);
   make_dirs(music_dir);

   out_ini = proteus_dir + "\\" + game + ".ini";
   out_music_dir = music_dir;
   exported_tracks = 0;

   for (Song &s : a.songs)
   {
      if (s.action != MAP_FILE)
         continue;

      std::string safe_name = sanitize_filename(s.name[0] ? s.name : ("track_" + hex(s.value)));
      std::string dest_wav = music_dir + "\\" + hex(s.value) + "_" + safe_name + ".wav";

      if (s.path[0] && file_exists(s.path))
      {
         std::string cur_path = normalize_path(s.path);
         std::string target_dir_norm = normalize_path(music_dir);
         if (cur_path.find(target_dir_norm) == std::string::npos)
         {
            std::string dest_file = music_dir + "\\" + file_name(s.path);
            if (copy_file_data(s.path, dest_file))
            {
               snprintf(s.path, sizeof(s.path), "%s", dest_file.c_str());
               exported_tracks++;
            }
         }
         else
         {
            exported_tracks++;
         }
      }
      else if (!s.clip.empty())
      {
         if (write_wav(dest_wav, s.clip, (int)s.clip_rate))
         {
            snprintf(s.path, sizeof(s.path), "%s", dest_wav.c_str());
            exported_tracks++;
         }
      }
   }

   std::string ini_content = profile_text(a, proteus_dir, false);
   if (!write_text(out_ini, ini_content))
   {
      a.status = "Failed to write " + out_ini;
      return false;
   }

   std::string opt_written;
   write_game_options(a, opt_written);

   a.export_status = "Exported " + game + ".ini (" + std::to_string(exported_tracks) + " tracks) to RetroArch!";
   a.status = a.export_status;
   app_log(a, a.status);
   return true;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static void help(const char *text)
{
   ImGui::SameLine();
   ImGui::TextDisabled("(?)");
   if (ImGui::BeginItemTooltip())
   {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
      ImGui::TextUnformatted(text);
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
   }
}

static void ui_setup(App &a)
{
   static char ra[1024];
   if (!ra[0])
      snprintf(ra, sizeof(ra), "%s", a.settings.retroarch_dir.c_str());
   ImGui::SeparatorText("RetroArch");
   ImGui::SetNextItemWidth(-90);
   if (ImGui::InputText("##ra", ra, sizeof(ra)))
      a.settings.retroarch_dir = ra;
   ImGui::SameLine();
   if (ImGui::Button("Browse##ra"))
   {
      std::string d = pick_folder_dialog("RetroArch folder");
      if (!d.empty())
      {
         snprintf(ra, sizeof(ra), "%s", d.c_str());
         a.settings.retroarch_dir = d;
         refresh_cores(a);
      }
   }
   if (a.cores.empty() && ImGui::Button("Find cores"))
      refresh_cores(a);

   ImGui::SeparatorText("Core");
   std::string current = a.settings.core_file;
   for (auto &c : a.cores)
      if (c.first == a.settings.core_file)
         current = c.second;
   ImGui::SetNextItemWidth(-1);
   if (ImGui::BeginCombo("##core", current.empty() ? "Choose a core" : current.c_str()))
   {
      for (auto &c : a.cores)
         if (ImGui::Selectable(c.second.c_str(), c.first == a.settings.core_file))
            a.settings.core_file = c.first;
      ImGui::EndCombo();
   }

   ImGui::SeparatorText("Game");
   ImGui::TextWrapped("%s", a.settings.rom_path.empty() ? "No ROM chosen" : a.settings.rom_path.c_str());
   if (ImGui::Button("Choose ROM..."))
   {
      std::string rom = open_file_dialog("Open ROM", { { "ROMs and archives", "*.zip;*.sfc;*.smc;*.nes;*.gb;*.gbc;*.gba;*.md;*.gen;*.bin;*.sms;*.gg;*.pce" }, { "All files", "*.*" } },
            a.settings.rom_path.empty() ? "" : dir_of(a.settings.rom_path));
      if (!rom.empty())
         a.settings.rom_path = rom;
   }
   ImGui::SameLine();
   ImGui::BeginDisabled(a.settings.core_file.empty() || a.settings.rom_path.empty());
   if (ImGui::Button(a.core.loaded() ? "Restart game" : "Start game"))
      load_game(a);
   ImGui::EndDisabled();

   if (a.detected_preset)
   {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.7f, 0.3f, 0.8f));
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
      ImGui::BeginChild("setup_preset_banner", ImVec2(0, 68), true);
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Preset Detected: %s (%s)",
                         a.detected_preset->name.c_str(), a.detected_preset->system.c_str());
      ImGui::TextDisabled("%s", a.detected_preset->description.c_str());
      if (ImGui::Button(("Apply " + a.detected_preset->name + " Preset (1-Click Setup)").c_str()))
         apply_rom_preset(a, *a.detected_preset);
      ImGui::EndChild();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
   }

   ImGui::SeparatorText("Controls");
   ImGui::TextWrapped("Arrows: D-pad   X: A   Z: B   S: X   A: Y   Q/W: L/R   Enter: Start   Right Shift: Select\n"
         "P: pause   F1: reset   Tab: fast forward   F2/F4: save/load quick state\n"
         "M: music changed   N: music is the same   (drag & drop ROMs, controller supported)");
}

static void ui_finder(App &a)
{
   if (ImGui::CollapsingHeader("ROM Presets (Instant 1-Click Setup)", ImGuiTreeNodeFlags_DefaultOpen))
   {
      if (a.detected_preset)
      {
         ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Preset Detected: %s (%s)",
                            a.detected_preset->name.c_str(), a.detected_preset->system.c_str());
         ImGui::TextDisabled("%s", a.detected_preset->description.c_str());
         if (ImGui::Button(("Apply " + a.detected_preset->name + " Preset (1-Click)").c_str(), ImVec2(280, 0)))
            apply_rom_preset(a, *a.detected_preset);
      }
      else
      {
         ImGui::TextWrapped("Select a game from the built-in preset library to automatically configure song address, "
                            "command register latching, and known song names, or run Auto-Probe below.");
      }

      ImGui::SetNextItemWidth(300);
      if (ImGui::BeginCombo("##preset_picker", "Choose from preset database..."))
      {
         for (const auto &p : get_rom_presets())
         {
            std::string label = p.name + " (" + p.system + ") - " + hex(p.address, 4);
            if (ImGui::Selectable(label.c_str()))
               apply_rom_preset(a, p);
         }
         ImGui::EndCombo();
      }
      ImGui::Spacing();
   }

   if (ImGui::CollapsingHeader("Auto-Probe RAM (Fast Register Discovery)", ImGuiTreeNodeFlags_DefaultOpen))
   {
      ImGui::TextWrapped("Pokes RAM addresses from a save state to detect which ones trigger music or sound effects. "
                         "Get to the title screen or gameplay first.");
      ImGui::SetNextItemWidth(80);
      ImGui::InputScalar("Start##pb", ImGuiDataType_U32, &a.probe.start_addr, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80);
      ImGui::InputScalar("End##pb", ImGuiDataType_U32, &a.probe.end_addr, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(60);
      ImGui::InputInt("Test val##pb", &a.probe.test_val, 1, 16);
      a.probe.test_val = std::clamp(a.probe.test_val, 1, 255);

      ImGui::BeginDisabled(!a.core.loaded() || a.probe.running);
      if (ImGui::Button(a.probe.running ? "Probing..." : "Start Auto-Probe", ImVec2(150, 0)))
         start_probe(a);
      ImGui::EndDisabled();

      if (a.probe.running)
      {
         ImGui::SameLine();
         if (ImGui::Button("Stop Probe"))
         {
            a.probe.running = false;
            a.core.set_skip_video(false);
            a.core.load_state(a.probe.base);
            a.status = "Probe stopped.";
         }
         float prog = a.probe.end_addr > a.probe.start_addr
               ? (float)(a.probe.current_addr - a.probe.start_addr) / (float)(a.probe.end_addr - a.probe.start_addr + 1)
               : 0.0f;
         ImGui::ProgressBar(prog, ImVec2(-1, 0), (std::to_string(a.probe.results.size()) + " candidate(s) found").c_str());
      }

      if (!a.probe.results.empty())
      {
         ImGui::Text("Found %zu address(es) that actively trigger audio changes:", a.probe.results.size());
         if (ImGui::BeginTable("probe_res", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
         {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Activity", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Poke", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Listen", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < a.probe.results.size() && i < 30; i++)
            {
               auto &r = a.probe.results[i];
               ImGui::PushID((int)r.address + 0x2000000);
               ImGui::TableNextRow();
               ImGui::TableNextColumn();
               ImGui::Text("%s", hex(r.address, 4).c_str());

               ImGui::TableNextColumn();
               float score_bar = std::clamp((float)(r.score / 6000.0), 0.05f, 1.0f);
               ImGui::ProgressBar(score_bar, ImVec2(-1, 0), (std::to_string((int)r.score)).c_str());

               ImGui::TableNextColumn();
               if (ImGui::SmallButton("Use"))
               {
                  a.profile.address = r.address;
                  a.profile.have_address = true;
                  a.profile.memory = 0;
                  a.profile.size = 1;
                  a.profile.latch = true;
                  a.have_current = false;
                  a.status = "Song address set to " + hex(r.address, 4) + " (command/latch)";
               }
               ImGui::SameLine();
               if (ImGui::SmallButton("Analyze"))
                  a.analyze.command = r.address;

               ImGui::TableNextColumn();
               if (ImGui::SmallButton("Poke"))
                  poke_ram(a, r.address, (uint8_t)a.probe.test_val);

               ImGui::TableNextColumn();
               ImGui::BeginDisabled(r.clip.empty());
               if (ImGui::SmallButton("Play"))
                  play_clip(a, r.clip, r.clip_rate);
               ImGui::EndDisabled();

               ImGui::PopID();
            }
            ImGui::EndTable();
         }
      }
   }

   ImGui::SeparatorText("Manual Change Marking");
   ImGui::TextWrapped("Play the game. Each time the music changes, press M (or the button) right after you hear it. "
         "While walking around with the same music, press N now and then. After a few changes, the song address "
         "is usually one of the few candidates left.");
   ImGui::BeginDisabled(!a.core.loaded());
   if (ImGui::Button("Music changed (M)", ImVec2(180, 0)))
      a.finder.mark_changed();
   ImGui::SameLine();
   if (ImGui::Button("Music is the same (N)", ImVec2(180, 0)))
      a.finder.mark_same();
   ImGui::SameLine();
   if (ImGui::Button("Start over"))
   {
      size_t size = 0;
      a.core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
      a.finder.reset(size);
   }
   ImGui::EndDisabled();

   ImGui::Text("Marks: %u   Song candidates: %zu   Command candidates: %zu", a.finder.marks(),
         a.finder.marks() ? a.finder.candidate_count() : 0, a.finder.marks() ? a.finder.command_count() : 0);

   static int poke_test_val = 2;
   ImGui::SetNextItemWidth(70);
   ImGui::InputInt("Test poke value", &poke_test_val, 1, 16);
   poke_test_val = std::clamp(poke_test_val, 0, 255);
   help("Click Poke on any candidate below to inject this value and hear if the song changes.");

   for (int kind = 0; kind < 2; kind++)
   {
      bool commands = kind == 1;
      ImGui::SeparatorText(commands ? "Command registers" : "Song addresses");
      if (commands)
         help("A command register holds a song number only for a moment when the game asks for new music. "
              "Proteus can detect songs with it (latch), and the Analyze tab uses it to play every song.");
      auto list = a.finder.candidates(40, commands);
      if (list.empty())
      {
         ImGui::TextDisabled(a.finder.marks() ? "No candidates left. Start over and mark more carefully." : "Mark a music change to begin.");
         continue;
      }
      if (ImGui::BeginTable(commands ? "cmd" : "song", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
      {
         ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 70);
         ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 50);
         ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch);
         ImGui::TableHeadersRow();
         for (auto &c : list)
         {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", hex(c.address, 4).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s", hex(c.value).c_str());
            ImGui::TableNextColumn();
            ImGui::PushID((int)c.address + (commands ? 0x1000000 : 0));
            if (ImGui::SmallButton("Use"))
            {
               a.profile.address = c.address;
               a.profile.have_address = true;
               a.profile.memory = 0;
               a.profile.size = 1;
               a.profile.latch = commands;
               a.have_current = false;
               a.status = "Song address set to " + hex(c.address, 4) + (commands ? " (command register)" : "");
            }
            if (commands)
            {
               ImGui::SameLine();
               if (ImGui::SmallButton("Analyze with it"))
                  a.analyze.command = c.address;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Poke"))
               poke_ram(a, c.address, (uint8_t)poke_test_val);
            ImGui::SameLine();
            if (ImGui::SmallButton("+1"))
               poke_ram(a, c.address, (uint8_t)((c.value + 1) & 0xFF));
            ImGui::SameLine();
            if (ImGui::SmallButton("0"))
               poke_ram(a, c.address, 0);
            ImGui::PopID();
         }
         ImGui::EndTable();
      }
   }
}

static void ui_mix_and_match(App &a)
{
   std::string game = content_name(a);
   std::string sys = system_dir(a);
   std::string target_title = a.settings.rom_path.empty() ? "No ROM loaded" : game;
   if (a.detected_preset)
      target_title += " (" + a.detected_preset->name + ")";

   std::string cmp_title = a.compare.name.empty() ? "(None loaded)" : a.compare.name;
   if (!a.compare.system.empty())
      cmp_title += " [" + a.compare.system + "]";

   // Top header: Source & Target cards
   ImGui::BeginChild("mix_header", ImVec2(0, 100), true);

   // Left column of header: Target Game
   ImGui::BeginGroup();
   ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Target Game (Primary ROM): %s", target_title.c_str());

   if (ImGui::SmallButton("Choose Target ROM...##top"))
   {
      std::string rom = open_file_dialog("Open Target ROM",
         { { "ROMs and archives", "*.zip;*.sfc;*.smc;*.nes;*.gb;*.gbc;*.gba;*.md;*.gen;*.bin;*.sms;*.gg;*.pce" }, { "All files", "*.*" } },
         a.settings.rom_path.empty() ? "" : dir_of(a.settings.rom_path));
      if (!rom.empty())
      {
         a.settings.rom_path = rom;
         load_game(a);
      }
   }
   if (a.detected_preset)
   {
      ImGui::SameLine();
      if (ImGui::SmallButton(("Apply " + a.detected_preset->name + " Preset##top").c_str()))
         apply_rom_preset(a, *a.detected_preset);
   }
   ImGui::SameLine();
   ImGui::SetNextItemWidth(140);
   if (ImGui::BeginCombo("##target_presets", "Target Preset..."))
   {
      for (const auto &p : get_rom_presets())
      {
         std::string label = p.name + " (" + p.system + ")";
         if (ImGui::Selectable(label.c_str()))
            apply_rom_preset(a, p);
      }
      ImGui::EndCombo();
   }
   ImGui::SameLine();
   ImGui::BeginDisabled(!a.core.loaded() || a.deep_scan.running);
   if (ImGui::SmallButton("Deep Scan ROM for Songs..."))
      start_deep_scan(a, false, 64);
   ImGui::EndDisabled();

   ImGui::SameLine();
   ImGui::TextDisabled("| %zu song(s)", a.songs.size());

   if (a.deep_scan.running && !a.deep_scan.is_compare)
   {
      ImGui::ProgressBar((float)a.deep_scan.current / (float)a.deep_scan.to, ImVec2(-80, 0),
         ("Scanning Target ROM: " + hex(a.deep_scan.current) + "/" + hex(a.deep_scan.to) + " (" + std::to_string(a.deep_scan.found) + " found)").c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Stop Scan"))
         a.deep_scan.current = a.deep_scan.to + 1;
   }
   ImGui::EndGroup();

   ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.52f);

   // Right column of header: Compare Game / Source
   ImGui::BeginGroup();
   ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Compare / Source Songs: %s", cmp_title.c_str());

   if (ImGui::SmallButton("Load Compare ROM..."))
   {
      std::string rom = open_file_dialog("Open Compare ROM",
         { { "ROMs and archives", "*.zip;*.sfc;*.smc;*.nes;*.gb;*.gbc;*.gba;*.md;*.gen;*.bin;*.sms;*.gg;*.pce" }, { "All files", "*.*" } },
         a.settings.rom_path.empty() ? "" : dir_of(a.settings.rom_path));
      if (!rom.empty())
         load_compare_rom(a, rom);
   }
   ImGui::SameLine();
   if (ImGui::SmallButton("Load Music File (NSF/SPC/VGM)..."))
   {
      std::string f = open_file_dialog("Open Game Music File",
         { { "Chiptunes and Audio", "*.nsf;*.nsfe;*.spc;*.vgm;*.vgz;*.gym;*.gbs;*.hes;*.kss;*.wav;*.mp3;*.ogg;*.flac" }, { "All files", "*.*" } },
         a.settings.rom_path.empty() ? "" : dir_of(a.settings.rom_path));
      if (!f.empty())
         load_compare_file(a, f);
   }
   ImGui::SameLine();
   if (ImGui::SmallButton("Load Music Folder / OST..."))
   {
      std::string folder = pick_folder_dialog("Choose Music Folder or OST Directory");
      if (!folder.empty())
         load_compare_folder(a, folder);
   }
   ImGui::SameLine();
   ImGui::SetNextItemWidth(140);
   if (ImGui::BeginCombo("##cmp_presets", "Preset Library..."))
   {
      for (const auto &p : get_rom_presets())
      {
         std::string label = p.name + " (" + p.system + ")";
         if (ImGui::Selectable(label.c_str()))
            load_compare_preset(a, p);
      }
      ImGui::EndCombo();
   }
   if (a.compare.type == App::CompareSource::TYPE_ROM && !a.compare.path.empty())
   {
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.42f, 0.70f, 1.0f));
      if (ImGui::SmallButton("Swap Target <-> Compare ROM"))
         swap_target_and_compare(a);
      ImGui::PopStyleColor();

      ImGui::SameLine();
      ImGui::BeginDisabled(a.deep_scan.running);
      if (ImGui::SmallButton("Deep Scan Compare ROM..."))
         start_deep_scan(a, true, 64);
      ImGui::EndDisabled();
   }
   ImGui::SameLine();
   ImGui::TextDisabled("| %zu track(s)", a.compare.songs.size());

   if (a.deep_scan.running && a.deep_scan.is_compare)
   {
      ImGui::ProgressBar((float)a.deep_scan.current / (float)a.deep_scan.to, ImVec2(-80, 0),
         ("Scanning Compare ROM: " + hex(a.deep_scan.current) + "/" + hex(a.deep_scan.to) + " (" + std::to_string(a.deep_scan.found) + " found)").c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Stop Scan##cmp"))
         a.deep_scan.current = a.deep_scan.to + 1;
   }
   ImGui::EndGroup();

   ImGui::EndChild();

   // Action bar
   ImGui::BeginChild("mix_actions", ImVec2(0, 36), false);

   ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.58f, 0.28f, 1.0f));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.70f, 0.35f, 1.0f));
   if (ImGui::Button("Export to RetroArch (INI & Music)", ImVec2(230, 0)))
   {
      std::string ini, mdir;
      int n = 0;
      export_to_retroarch(a, ini, mdir, n);
   }
   ImGui::PopStyleColor(2);

   ImGui::SameLine();
   ImGui::BeginDisabled(a.songs.empty() || a.compare.songs.empty());
   if (ImGui::Button("Auto-Match by Name/Track #"))
      auto_match_songs(a);
   ImGui::SameLine();
   if (ImGui::Button("Match All 1:1 in Order"))
      match_all_in_order(a);
   ImGui::EndDisabled();

   ImGui::SameLine();
   ImGui::BeginDisabled(a.songs.empty());
   if (ImGui::Button("Clear All Replacements"))
   {
      for (auto &s : a.songs)
      {
         s.action = MAP_ORIGINAL;
         s.path[0] = '\0';
         s.clip.clear();
      }
      a.preview_dirty = true;
      a.status = "Cleared all replacements. All songs set to original.";
   }
   ImGui::EndDisabled();

   ImGui::SameLine();
   std::string music_dest = sys + "\\proteus\\music" + (game.empty() ? "" : ("\\" + game));
   if (ImGui::Button("Open Music Folder"))
   {
      make_dirs(music_dest);
      open_folder(music_dest);
   }

   ImGui::EndChild();

   ImGui::TextDisabled("Output: %s\\proteus\\%s.ini  |  Music: %s",
         sys.c_str(), game.empty() ? "{game}" : game.c_str(), music_dest.c_str());
   ImGui::Separator();

   // Main dual-panel area
   float avail_w = ImGui::GetContentRegionAvail().x;
   float left_w = (avail_w - 12.0f) * 0.58f;
   float right_w = avail_w - left_w - 12.0f;
   float pane_h = ImGui::GetContentRegionAvail().y;

   // -------------------------------------------------------------
   // LEFT PANEL: Target Game Songs
   // -------------------------------------------------------------
   ImGui::BeginChild("target_panel", ImVec2(left_w, pane_h), true);
   ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Target Songs (%s)", target_title.c_str());
   ImGui::SameLine();
   if (ImGui::SmallButton("+ Add Song Slot"))
   {
      uint32_t next_val = a.songs.empty() ? 1 : (a.songs.back().value + 1);
      Song &s = add_song(a, next_val);
      snprintf(s.name, sizeof(s.name), "Song %s", hex(next_val).c_str());
   }

   if (a.songs.empty())
   {
      ImGui::Spacing();
      ImGui::TextWrapped("No songs configured for this game yet.");
      if (a.detected_preset)
      {
         if (ImGui::Button(("Apply " + a.detected_preset->name + " Preset (Instant)").c_str()))
            apply_rom_preset(a, *a.detected_preset);
      }
      else
      {
         ImGui::TextWrapped("Pick a preset or click '+ Add Song Slot' to start mapping music.");
      }
   }
   else
   {
      if (ImGui::BeginTable("target_table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
      {
         ImGui::TableSetupScrollFreeze(0, 1);
         ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_WidthFixed, 25);
         ImGui::TableSetupColumn("Play", ImGuiTableColumnFlags_WidthFixed, 60);
         ImGui::TableSetupColumn("Target Song", ImGuiTableColumnFlags_WidthFixed, 150);
         ImGui::TableSetupColumn("Replacement Track", ImGuiTableColumnFlags_WidthStretch);
         ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90);
         ImGui::TableHeadersRow();

         for (size_t i = 0; i < a.songs.size(); i++)
         {
            Song &s = a.songs[i];
            ImGui::PushID((int)i + 20000);
            ImGui::TableNextRow();

            // Column 1: Select radio
            ImGui::TableNextColumn();
            bool is_selected = (a.selected_target_song == (int)i);
            if (ImGui::RadioButton("##sel", is_selected))
               a.selected_target_song = (int)i;

            // Column 2: Play Original
            ImGui::TableNextColumn();
            bool is_orig_playing = (a.is_playing && a.playing_target_index == (int)i);
            if (is_orig_playing)
            {
               ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
               if (ImGui::SmallButton("Stop"))
                  stop_audio(a);
               ImGui::PopStyleColor();
            }
            else
            {
               ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.3f, 1.0f));
               if (ImGui::SmallButton("Play"))
                  play_song_item(a, s, false, (int)i);
               ImGui::PopStyleColor();
            }

            // Column 3: Target Song
            ImGui::TableNextColumn();
            ImGui::Text("%s", hex(s.value).c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", s.name, sizeof(s.name)))
               a.preview_dirty = true;

            // Column 4: Replacement
            ImGui::TableNextColumn();
            if (s.action == MAP_FILE)
            {
               std::string fname = s.path[0] ? file_name(s.path) : "no file";
               ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.9f, 1.0f), "[File] %s", fname.c_str());
               ImGui::SameLine();
               bool is_repl_playing = (a.is_playing && a.playing_target_index == (int)i + 10000);
               if (is_repl_playing)
               {
                  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                  if (ImGui::SmallButton("Stop##repl"))
                     stop_audio(a);
                  ImGui::PopStyleColor();
               }
               else
               {
                  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.7f, 1.0f));
                  if (ImGui::SmallButton("Play##repl"))
                  {
                     stop_audio(a);
                     if (s.path[0] && file_exists(s.path))
                     {
                        play_file(a, s);
                        a.is_playing = true;
                        a.playing_target_index = (int)i + 10000;
                     }
                     else if (!s.clip.empty())
                     {
                        play_clip(a, s.clip, s.clip_rate);
                        a.is_playing = true;
                        a.playing_target_index = (int)i + 10000;
                     }
                  }
                  ImGui::PopStyleColor();
               }
               ImGui::SameLine();
               ImGui::SetNextItemWidth(65);
               if (ImGui::SliderInt("##v", &s.volume, 0, 200, "%d%%"))
                  a.preview_dirty = true;
            }
            else if (s.action == MAP_SILENCE)
            {
               ImGui::TextDisabled("[Silence]");
            }
            else
            {
               ImGui::TextDisabled("[Original Music]");
            }

            // Column 5: Action
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(a.compare.songs.empty() || a.compare.selected_song < 0 || a.compare.selected_song >= (int)a.compare.songs.size());
            if (ImGui::SmallButton("<- Match"))
            {
               match_songs(a, s, a.compare.songs[a.compare.selected_song]);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (s.action != MAP_ORIGINAL)
            {
               if (ImGui::SmallButton("x##rst"))
               {
                  s.action = MAP_ORIGINAL;
                  s.path[0] = '\0';
                  s.clip.clear();
                  a.preview_dirty = true;
               }
            }
            else
            {
               if (ImGui::SmallButton("Mute"))
               {
                  s.action = MAP_SILENCE;
                  a.preview_dirty = true;
               }
            }

            ImGui::PopID();
         }
         ImGui::EndTable();
      }
   }
   ImGui::EndChild();

   ImGui::SameLine();

   // -------------------------------------------------------------
   // RIGHT PANEL: Compare / Source Songs
   // -------------------------------------------------------------
   ImGui::BeginChild("compare_panel", ImVec2(right_w, pane_h), true);
   ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Source: %s", cmp_title.c_str());
   ImGui::SetNextItemWidth(-1);
   ImGui::InputTextWithHint("##cmp_flt", "Search songs / tracks...", a.compare.filter, sizeof(a.compare.filter));

   if (a.compare.songs.empty())
   {
      ImGui::Spacing();
      ImGui::TextWrapped("No compare source loaded. Use the buttons above to load a Compare ROM, a music folder (OST), or pick a Preset.");
   }
   else
   {
      if (ImGui::BeginTable("compare_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
      {
         ImGui::TableSetupScrollFreeze(0, 1);
         ImGui::TableSetupColumn("Play", ImGuiTableColumnFlags_WidthFixed, 60);
         ImGui::TableSetupColumn("Source Song", ImGuiTableColumnFlags_WidthStretch);
         ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80);
         ImGui::TableHeadersRow();

         std::string filter_str = to_lower(a.compare.filter);

         for (size_t i = 0; i < a.compare.songs.size(); i++)
         {
            Song &cs = a.compare.songs[i];
            std::string sname = cs.name[0] ? cs.name : hex(cs.value);
            if (!filter_str.empty() && to_lower(sname).find(filter_str) == std::string::npos)
               continue;

            ImGui::PushID((int)i + 50000);
            ImGui::TableNextRow();

            // Column 1: Play
            ImGui::TableNextColumn();
            bool is_cmp_playing = (a.is_playing && a.playing_compare_index == (int)i);
            if (is_cmp_playing)
            {
               ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
               if (ImGui::SmallButton("Stop##cmp"))
                  stop_audio(a);
               ImGui::PopStyleColor();
            }
            else
            {
               ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.3f, 1.0f));
               if (ImGui::SmallButton("Play##cmp"))
                  play_song_item(a, cs, true, (int)i);
               ImGui::PopStyleColor();
            }

            // Column 2: Song Name (selectable)
            ImGui::TableNextColumn();
            bool is_sel = (a.compare.selected_song == (int)i);
            if (ImGui::Selectable(sname.c_str(), is_sel))
               a.compare.selected_song = (int)i;

            // Column 3: Assign to Selected Target
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(a.songs.empty() || a.selected_target_song < 0 || a.selected_target_song >= (int)a.songs.size());
            if (ImGui::SmallButton("<- Use"))
            {
               a.compare.selected_song = (int)i;
               match_songs(a, a.songs[a.selected_target_song], cs);
            }
            ImGui::EndDisabled();

            ImGui::PopID();
         }
         ImGui::EndTable();
      }
   }
   ImGui::EndChild();
}

static void ui_songs(App &a)
{
   ImGui::BeginDisabled(!a.core.loaded());
   bool pv = a.preview;
   if (ImGui::Checkbox("Hear replacements in the game", &pv))
   {
      if (pv)
         start_preview(a);
      else
         stop_preview(a);
   }
   help("Runs the real Proteus engine on the game: song detection, channel muting and your mapping.");
   ImGui::EndDisabled();
   if (a.have_current)
   {
      ImGui::SameLine();
      ImGui::Text("   Playing now: %s", hex(a.current_song).c_str());
   }

   if (!a.profile.have_address)
      ImGui::TextWrapped("Set the song address (Find address tab, or Profile tab) and songs will collect here as you play.");

   if (!ImGui::BeginTable("songs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y)))
      return;
   ImGui::TableSetupScrollFreeze(0, 1);
   ImGui::TableSetupColumn("Song", ImGuiTableColumnFlags_WidthFixed, 200);
   ImGui::TableSetupColumn("Heard at", ImGuiTableColumnFlags_WidthFixed, 110);
   ImGui::TableSetupColumn("Replacement", ImGuiTableColumnFlags_WidthStretch);
   ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30);
   ImGui::TableHeadersRow();

   int remove = -1;
   for (size_t i = 0; i < a.songs.size(); i++)
   {
      Song &s = a.songs[i];
      ImGui::PushID((int)i);
      ImGui::TableNextRow();
      bool now = a.have_current && s.value == a.current_song;

      ImGui::TableNextColumn();
      ImGui::Text("%s%s", hex(s.value).c_str(), now ? "  < now" : "");
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputTextWithHint("##name", "name", s.name, sizeof(s.name)))
         a.preview_dirty = true;
      bool is_orig_playing = (a.is_playing && a.playing_target_index == (int)i);
      if (is_orig_playing)
      {
         ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
         if (ImGui::SmallButton("Stop"))
            stop_audio(a);
         ImGui::PopStyleColor();
      }
      else
      {
         ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.3f, 1.0f));
         if (ImGui::SmallButton(s.recording ? "recording..." : "Play"))
            play_song_item(a, s, false, (int)i);
         ImGui::PopStyleColor();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Re-record"))
      {
         s.clip.clear();
         s.clip_rate = a.core.sample_rate();
         s.recording = true;
         make_thumb(a, s);
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(s.clip.empty());
      if (ImGui::SmallButton("Save WAV"))
      {
         std::string dir = app_data_dir() + "\\clips";
         make_dirs(dir);
         std::string out = dir + "\\" + content_name(a) + "_" + hex(s.value) + ".wav";
         if (write_wav(out, s.clip, (int)s.clip_rate))
            a.status = "Saved " + out;
         else
            a.status = "Failed to write " + out;
         app_log(a, a.status);
      }
      ImGui::EndDisabled();

      ImGui::TableNextColumn();
      if (s.thumb)
         ImGui::Image((ImTextureID)(intptr_t)s.thumb, ImVec2(100, 100.0f * s.thumb_h / std::max(1, s.thumb_w)));

      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(130);
      if (ImGui::Combo("##action", &s.action, "Original music\0Silence\0File\0"))
         a.preview_dirty = true;
      if (s.action == MAP_FILE)
      {
         ImGui::SameLine();
         if (ImGui::Button("Choose..."))
         {
            std::string f = open_file_dialog("Replacement music", {
               { "Music", "*.ogg;*.mp3;*.wav;*.spc;*.nsf;*.nsfe;*.vgm;*.vgz;*.gym;*.gbs;*.hes;*.kss;*.ay;*.sap" },
               { "All files", "*.*" } }, s.path[0] ? dir_of(s.path) : system_dir(a) + "\\proteus");
            if (!f.empty())
            {
               snprintf(s.path, sizeof(s.path), "%s", f.c_str());
               a.preview_dirty = true;
            }
         }
         ImGui::SameLine();
         bool is_repl_playing = (a.is_playing && a.playing_target_index == (int)i + 10000);
         if (is_repl_playing)
         {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Stop##listen"))
               stop_audio(a);
            ImGui::PopStyleColor();
         }
         else
         {
            ImGui::BeginDisabled(!s.path[0] && s.clip.empty());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.7f, 1.0f));
            if (ImGui::Button("Play##listen"))
            {
               stop_audio(a);
               if (s.path[0] && file_exists(s.path))
               {
                  play_file(a, s);
                  a.is_playing = true;
                  a.playing_target_index = (int)i + 10000;
               }
               else if (!s.clip.empty())
               {
                  play_clip(a, s.clip, s.clip_rate);
                  a.is_playing = true;
                  a.playing_target_index = (int)i + 10000;
               }
            }
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
         }
         ImGui::TextWrapped("%s", s.path[0] ? file_name(s.path).c_str() : "no file chosen");
         ImGui::SetNextItemWidth(120);
         if (ImGui::SliderInt("volume %", &s.volume, 0, 200)) a.preview_dirty = true;
         ImGui::SameLine();
         if (ImGui::Checkbox("loop", &s.loop)) a.preview_dirty = true;
         ImGui::SameLine();
         ImGui::SetNextItemWidth(70);
         if (ImGui::InputInt("song #", &s.track, 0)) { s.track = std::max(1, s.track); a.preview_dirty = true; }
         help("For files with several songs (NSF, GBS, KSS...): which one to play.");
         ImGui::SetNextItemWidth(120);
         if (ImGui::InputInt("loop start (samples)", &s.loop_start, 0)) { s.loop_start = std::max(0, s.loop_start); a.preview_dirty = true; }
      }

      ImGui::TableNextColumn();
      if (ImGui::SmallButton("x"))
         remove = (int)i;
      ImGui::PopID();
   }
   ImGui::EndTable();
   if (remove >= 0)
   {
      if (a.songs[remove].thumb)
         SDL_DestroyTexture(a.songs[remove].thumb);
      a.songs.erase(a.songs.begin() + remove);
      a.preview_dirty = true;
   }
}

static void ui_channels(App &a)
{
   ImGui::TextWrapped("Find the channels that carry the music: toggle them while the game plays and listen. "
         "Checked channels are muted whenever replacement music (or silence) plays. Sound effects often "
         "use the last channels, so leave those playing.");
   if (!a.core.loaded())
      return;
   int shown = 0;
   for (auto &o : a.core.options())
   {
      std::string k = o.key;
      bool channel = k.find("chan") != std::string::npos || k.find("voice") != std::string::npos
            || (k.find("volume") != std::string::npos && k.find("_") != std::string::npos && k.find("audio") == std::string::npos);
      if (!channel)
         continue;
      // The value that silences the channel: "0" for volume sliders, else "disabled".
      std::string mute_value;
      for (auto &v : o.values)
         if (v.value == "0" || v.value == "disabled" || v.value == "off")
         { mute_value = v.value; break; }
      if (mute_value.empty())
         continue;
      shown++;
      ImGui::PushID(k.c_str());
      bool listening_muted = o.value == mute_value;
      if (ImGui::Checkbox("off now", &listening_muted))
         a.core.set_option(k, listening_muted ? mute_value : o.default_value);
      ImGui::SameLine();
      bool in_profile = a.profile.mute.count(k) > 0;
      if (ImGui::Checkbox("mute for replacements", &in_profile))
      {
         if (in_profile)
            a.profile.mute[k] = mute_value;
         else
            a.profile.mute.erase(k);
         a.preview_dirty = true;
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(o.desc.c_str());
      ImGui::PopID();
   }
   if (!shown)
      ImGui::TextDisabled("This core has no per-channel options. Replacement music will play over the original.");
}

static void ui_analyze(App &a)
{
   auto &an = a.analyze;
   ImGui::TextWrapped("Plays every song the game has, without playing through it. Needs a command register "
         "(from the Find address tab). Get to a calm spot in the game first: the analysis starts from a "
         "snapshot of this moment for each song.");
   ImGui::SetNextItemWidth(120);
   ImGui::InputScalar("Command register", ImGuiDataType_U32, &an.command, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal);
   ImGui::SetNextItemWidth(120);
   ImGui::InputInt("First value", &an.from, 1, 16);
   ImGui::SetNextItemWidth(120);
   ImGui::InputInt("Last value", &an.to, 1, 16);
   an.from = std::clamp(an.from, 0, 255);
   an.to = std::clamp(an.to, an.from, 255);

   ImGui::BeginDisabled(!a.core.loaded() || an.running || !an.command);
   if (ImGui::Button("Analyze songs"))
   {
      stop_preview(a);
      an.base = a.core.save_state();
      an.next = an.from;
      an.found = an.silent = 0;
      an.running = !an.base.empty();
      if (!an.running)
         a.status = "This core cannot save states, so it cannot be analyzed.";
   }
   ImGui::EndDisabled();
   if (an.running)
   {
      ImGui::SameLine();
      if (ImGui::Button("Stop"))
         an.next = an.to + 1;
      ImGui::ProgressBar((float)(an.next - an.from) / (float)(an.to - an.from + 1), ImVec2(-1, 0),
            (std::to_string(an.found) + " songs found").c_str());
   }
}

static void ui_profile(App &a)
{
   ProfileEdit &e = a.profile;
   ImGui::SeparatorText("Song detection");
   ImGui::SetNextItemWidth(140);
   ImGui::Combo("Memory", &e.memory, "system_ram\0save_ram\0video_ram\0");
   ImGui::SetNextItemWidth(140);
   if (ImGui::InputScalar("Address", ImGuiDataType_U32, &e.address, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal))
      e.have_address = true;
   ImGui::TextUnformatted("Size");
   ImGui::SameLine(); ImGui::RadioButton("1 byte", &e.size, 1);
   ImGui::SameLine(); ImGui::RadioButton("2 bytes", &e.size, 2);
   ImGui::SameLine(); ImGui::RadioButton("4 bytes", &e.size, 4);
   ImGui::Checkbox("Command register (latch)", &e.latch);
   help("The address only holds a song number briefly when music starts; keep playing the last song until the next one.");
   ImGui::SetNextItemWidth(140);
   ImGui::SliderInt("Debounce frames", &e.debounce, 1, 30);
   ImGui::SetNextItemWidth(140);
   ImGui::Combo("Unlisted songs", &e.unmapped, "Original music\0Silence\0Keep playing\0");

   ImGui::SeparatorText("Mix");
   ImGui::SetNextItemWidth(200); ImGui::SliderInt("Music volume %", &e.music_volume, 0, 200);
   ImGui::SetNextItemWidth(200); ImGui::SliderInt("Game volume %", &e.game_volume, 0, 200);
   ImGui::SetNextItemWidth(200); ImGui::SliderInt("Crossfade ms", &e.crossfade, 0, 3000);

   ImGui::SeparatorText("Save");
   static char path[1024];
   static std::string shown;
   if (shown != e.path)
   {
      snprintf(path, sizeof(path), "%s", e.path.c_str());
      shown = e.path;
   }
   ImGui::SetNextItemWidth(-1);
   if (ImGui::InputText("##path", path, sizeof(path)))
      e.path = shown = path;
   if (ImGui::Button("RetroArch system folder"))
      e.path = system_dir(a) + "\\proteus\\" + content_name(a) + ".ini";
   ImGui::SameLine();
   if (ImGui::Button("Next to the ROM"))
      e.path = dir_of(a.settings.rom_path) + "\\" + content_name(a) + ".proteus.ini";

   ImGui::BeginDisabled(!a.core.loaded() || !e.have_address);
   if (ImGui::Button("Save profile", ImVec2(160, 0)))
   {
      make_dirs(dir_of(e.path));
      if (write_text(e.path, profile_text(a, dir_of(e.path), false)))
         a.status = "Saved " + e.path;
      else
         a.status = "Could not write " + e.path;
      app_log(a, a.status);
   }
   ImGui::SameLine();
   ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.58f, 0.28f, 1.0f));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.70f, 0.35f, 1.0f));
   if (ImGui::Button("Export to RetroArch (INI & Music)", ImVec2(240, 0)))
   {
      std::string ini, mdir;
      int n = 0;
      export_to_retroarch(a, ini, mdir, n);
   }
   ImGui::PopStyleColor(2);
   ImGui::SameLine();
   ImGui::BeginDisabled(e.mute.empty());
   if (ImGui::Button("Save channel mutes for the DSP plugin"))
   {
      std::string written;
      a.status = write_game_options(a, written) ? "Wrote game options " + written : "Could not write " + written;
      app_log(a, a.status);
   }
   ImGui::EndDisabled();
   help("The DSP plugin cannot mute channels itself. This saves the mutes as RetroArch game options for this game "
        "and core, keeping your other options. The music stays muted for the whole game.");
   ImGui::EndDisabled();

   if (ImGui::CollapsingHeader("Profile text"))
   {
      std::string text = profile_text(a, dir_of(e.path), false);
      ImGui::InputTextMultiline("##text", (char*)text.c_str(), text.size() + 1, ImVec2(-1, 300), ImGuiInputTextFlags_ReadOnly);
   }
}

static void ui_log(App &a)
{
   if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
   {
      for (auto &l : a.core.log())
         a.log.push_back(l);
      a.core.log().clear();
      for (auto &l : a.log)
         ImGui::TextUnformatted(l.c_str());
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
         ImGui::SetScrollHereY(1.0f);
   }
   ImGui::EndChild();
}

static void draw_ui(App &a)
{
   ImGuiViewport *vp = ImGui::GetMainViewport();
   ImGui::SetNextWindowPos(vp->WorkPos);
   ImGui::SetNextWindowSize(vp->WorkSize);
   ImGui::Begin("Proteus Studio", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
         | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

   float bar_h = ImGui::GetFrameHeightWithSpacing() + 6.0f;
   float status_h = ImGui::GetFrameHeightWithSpacing();
   float avail_h = ImGui::GetContentRegionAvail().y - status_h - bar_h;

   // Top Toolbar
   ImGui::BeginChild("toolbar", ImVec2(0, bar_h), false);
   ImGui::Checkbox("Game Screen", &a.show_game_screen);
   ImGui::SameLine();
   ImGui::Checkbox("Mute Game", &a.mute_emulator);
   if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Mutes emulator background audio so you only hear auditioned songs");
   ImGui::SameLine();

   bool audio_active = a.is_playing || a.clip_playing || (a.core.loaded() && !a.paused && !a.mute_emulator);
   if (audio_active)
   {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.3f, 0.3f, 1.0f));
      if (ImGui::Button("Stop Audio"))
         stop_audio(a);
      ImGui::PopStyleColor(2);
   }
   else
   {
      if (ImGui::Button("Stop Audio"))
         stop_audio(a);
   }
   ImGui::SameLine();
   ImGui::SetNextItemWidth(90);
   ImGui::SliderInt("##vol", &a.audio.volume, 0, 100, "%d%%");
   if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Master volume");

   ImGui::SameLine(0, 15);
   ImGui::SetNextItemWidth(115);
   static const char *scale_names[] = { "Scale: 100%", "Scale: 125%", "Scale: 140%", "Scale: 160%", "Scale: 200%" };
   static const float scale_factors[] = { 1.0f, 1.25f, 1.40f, 1.60f, 2.0f };
   int sel_scale = 1;
   for (int i = 0; i < 5; i++)
      if (std::abs(a.settings.ui_scale - scale_factors[i]) < 0.05f) sel_scale = i;
   if (ImGui::Combo("##uiscale", &sel_scale, scale_names, 5))
   {
      a.settings.ui_scale = scale_factors[sel_scale];
      ImGui::GetIO().FontGlobalScale = a.settings.ui_scale;
      save_settings(a.settings);
   }
   if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Adjust UI and text scaling for readability");

   ImGui::SameLine(0, 15);
   ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.58f, 0.28f, 1.0f));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.70f, 0.35f, 1.0f));
   if (ImGui::Button("Export to RetroArch (Generate INI & Music)"))
   {
      std::string ini, mdir;
      int n = 0;
      export_to_retroarch(a, ini, mdir, n);
   }
   ImGui::PopStyleColor(2);

   if (!a.export_status.empty())
   {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%s", a.export_status.c_str());
   }
   ImGui::EndChild();

   if (a.show_game_screen)
   {
      float left_w = ImGui::GetContentRegionAvail().x * 0.40f;
      ImGui::BeginChild("game", ImVec2(left_w, avail_h), ImGuiChildFlags_Borders);
      if (a.game_tex)
      {
         float gbar_h = ImGui::GetFrameHeightWithSpacing();
         ImVec2 avail = ImGui::GetContentRegionAvail();
         avail.y = std::max(10.0f, avail.y - gbar_h - 4.0f);
         float aspect = (float)a.core.aspect();
         float w = avail.x, h = w / aspect;
         if (h > avail.y) { h = avail.y; w = h * aspect; }
         ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - w) / 2, ImGui::GetCursorPosY() + (avail.y - h) / 2));
         ImGui::Image((ImTextureID)(intptr_t)a.game_tex, ImVec2(w, h));

         ImGui::SetCursorPos(ImVec2(8, ImGui::GetWindowHeight() - gbar_h - 4));
         if (ImGui::Button(a.paused ? "Resume (P)" : "Pause (P)"))
         {
            a.paused = !a.paused;
            a.audio.clear();
         }
         ImGui::SameLine();
         if (ImGui::Button("Reset (F1)"))
         {
            a.core.reset();
            a.status = "Game reset";
            a.audio.clear();
         }
         ImGui::SameLine();
         if (ImGui::Button("Save (F2)"))
         {
            a.quick_state = a.core.save_state();
            a.status = a.quick_state.empty() ? "Save state failed" : "State saved";
         }
         ImGui::SameLine();
         ImGui::BeginDisabled(a.quick_state.empty());
         if (ImGui::Button("Load (F4)"))
         {
            if (a.core.load_state(a.quick_state)) { a.status = "State loaded"; a.have_current = false; a.audio.clear(); }
         }
         ImGui::EndDisabled();
      }
      else
         ImGui::TextDisabled("Choose a core and a ROM in Setup or Mix & Match to start emulation.\nDrag and drop ROM supported.");
      ImGui::EndChild();
      ImGui::SameLine();
   }

   ImGui::BeginChild("tools", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
   if (ImGui::BeginTabBar("tabs"))
   {
      if (ImGui::BeginTabItem("Mix & Match")) { ui_mix_and_match(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Setup")) { ui_setup(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Songs")) { ui_songs(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Find address")) { ui_finder(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Channels")) { ui_channels(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Analyze")) { ui_analyze(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Profile")) { ui_profile(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Log")) { ui_log(a); ImGui::EndTabItem(); }
      ImGui::EndTabBar();
   }
   ImGui::EndChild();

   ImGui::Text("%s%s   %s", a.paused ? "[paused] " : "", a.fast_forward ? "[fast] " : "", a.status.c_str());
   ImGui::End();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static uint16_t keyboard_pad()
{
   const Uint8 *k = SDL_GetKeyboardState(nullptr);
   uint16_t b = 0;
   auto set = [&](SDL_Scancode sc, unsigned id) { if (k[sc]) b |= 1 << id; };
   set(SDL_SCANCODE_UP, RETRO_DEVICE_ID_JOYPAD_UP);
   set(SDL_SCANCODE_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN);
   set(SDL_SCANCODE_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT);
   set(SDL_SCANCODE_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT);
   set(SDL_SCANCODE_X, RETRO_DEVICE_ID_JOYPAD_A);
   set(SDL_SCANCODE_Z, RETRO_DEVICE_ID_JOYPAD_B);
   set(SDL_SCANCODE_S, RETRO_DEVICE_ID_JOYPAD_X);
   set(SDL_SCANCODE_A, RETRO_DEVICE_ID_JOYPAD_Y);
   set(SDL_SCANCODE_Q, RETRO_DEVICE_ID_JOYPAD_L);
   set(SDL_SCANCODE_W, RETRO_DEVICE_ID_JOYPAD_R);
   set(SDL_SCANCODE_RETURN, RETRO_DEVICE_ID_JOYPAD_START);
   set(SDL_SCANCODE_RSHIFT, RETRO_DEVICE_ID_JOYPAD_SELECT);
   return b;
}

static uint16_t controller_pad(SDL_GameController *c)
{
   if (!c)
      return 0;
   uint16_t b = 0;
   auto set = [&](SDL_GameControllerButton btn, unsigned id) { if (SDL_GameControllerGetButton(c, btn)) b |= 1 << id; };
   set(SDL_CONTROLLER_BUTTON_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP);
   set(SDL_CONTROLLER_BUTTON_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN);
   set(SDL_CONTROLLER_BUTTON_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT);
   set(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT);
   // Positional layout: the bottom face button is the SNES B button.
   set(SDL_CONTROLLER_BUTTON_A, RETRO_DEVICE_ID_JOYPAD_B);
   set(SDL_CONTROLLER_BUTTON_B, RETRO_DEVICE_ID_JOYPAD_A);
   set(SDL_CONTROLLER_BUTTON_X, RETRO_DEVICE_ID_JOYPAD_Y);
   set(SDL_CONTROLLER_BUTTON_Y, RETRO_DEVICE_ID_JOYPAD_X);
   set(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, RETRO_DEVICE_ID_JOYPAD_L);
   set(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, RETRO_DEVICE_ID_JOYPAD_R);
   set(SDL_CONTROLLER_BUTTON_START, RETRO_DEVICE_ID_JOYPAD_START);
   set(SDL_CONTROLLER_BUTTON_BACK, RETRO_DEVICE_ID_JOYPAD_SELECT);
   if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) < -16000) b |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
   if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) > 16000) b |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
   if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) < -16000) b |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
   if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) > 16000) b |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
   return b;
}

static void handle_key(App &a, SDL_Keycode key)
{
   switch (key)
   {
      case SDLK_F1:
         if (a.core.loaded()) { a.core.reset(); a.status = "Game reset"; a.audio.clear(); }
         break;
      case SDLK_m: if (a.core.loaded()) { a.finder.mark_changed(); a.status = "Marked: music changed"; } break;
      case SDLK_n: if (a.core.loaded()) { a.finder.mark_same(); a.status = "Marked: music is the same"; } break;
      case SDLK_p: a.paused = !a.paused; a.audio.clear(); break;
      case SDLK_F2: a.quick_state = a.core.save_state(); a.status = a.quick_state.empty() ? "Save state failed" : "State saved"; break;
      case SDLK_F4:
         if (a.core.load_state(a.quick_state)) { a.status = "State loaded"; a.have_current = false; a.audio.clear(); }
         break;
      default: break;
   }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
   (void)argc; (void)argv;
   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
   {
      fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
      return 1;
   }
   SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
   SDL_Window *window = SDL_CreateWindow("Proteus Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
         1440, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
   SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
   if (!window || !renderer)
   {
      fprintf(stderr, "SDL window: %s\n", SDL_GetError());
      return 1;
   }

   static App app;
   App &a = app;
   g_app = &a;
   a.renderer = renderer;
   a.settings = load_settings();
   refresh_cores(a);
   a.audio.open();
   a.core.audio_filter = on_core_audio;
   px_host host{};
   host.memory = host_memory;
   host.log = host_log;
   host.notify = host_notify;
   host.mute = host_mute;
   px_engine_init(&a.engine, &host);

   IMGUI_CHECKVERSION();
   ImGui::CreateContext();
   ImGuiIO &io = ImGui::GetIO();
   static std::string ini = app_data_dir() + "/imgui.ini";
   io.IniFilename = ini.c_str();
   io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
   ImGui::StyleColorsDark();

   ImFontConfig font_cfg;
   font_cfg.SizePixels = 16.0f;
   io.Fonts->AddFontDefault(&font_cfg);
   io.FontGlobalScale = a.settings.ui_scale;

   ImGuiStyle &style = ImGui::GetStyle();
   style.FramePadding = ImVec2(6, 4);
   style.ItemSpacing = ImVec2(8, 5);
   style.WindowPadding = ImVec2(10, 10);
   style.ScrollbarSize = 16.0f;

   ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
   ImGui_ImplSDLRenderer2_Init(renderer);

   bool quit = false;
   while (!quit)
   {
      SDL_Event ev;
      while (SDL_PollEvent(&ev))
      {
         ImGui_ImplSDL2_ProcessEvent(&ev);
         if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE))
            quit = true;
         else if (ev.type == SDL_KEYDOWN && !ev.key.repeat && !io.WantCaptureKeyboard)
            handle_key(a, ev.key.keysym.sym);
         else if (ev.type == SDL_CONTROLLERDEVICEADDED && !a.controller)
            a.controller = SDL_GameControllerOpen(ev.cdevice.which);
         else if (ev.type == SDL_CONTROLLERDEVICEREMOVED)
         {
            if (a.controller && SDL_GameControllerFromInstanceID(ev.cdevice.which) == a.controller)
            {
               SDL_GameControllerClose(a.controller);
               a.controller = nullptr;
            }
         }
         else if (ev.type == SDL_DROPFILE)
         {
            char *dropped = ev.drop.file;
            if (dropped)
            {
               a.settings.rom_path = dropped;
               SDL_free(dropped);
               if (!a.settings.core_file.empty())
                  load_game(a);
               else
                  a.status = "ROM selected; choose a core in Setup and start game.";
            }
         }
      }

      // Games only get input when no text field has the keyboard.
      a.pad = (io.WantCaptureKeyboard ? 0 : keyboard_pad()) | controller_pad(a.controller);
      a.fast_forward = !io.WantCaptureKeyboard && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB];

      if (a.clip_playing && a.audio.queued() == 0)
      {
         a.clip_playing = false;
         a.is_playing = false;
         a.playing_target_index = -1;
         a.playing_compare_index = -1;
         if (!a.show_game_screen)
            a.paused = true;
         else
            a.paused = a.was_paused;
      }
      if (a.clip_playing && a.pad)
      {
         a.audio.clear();
         a.clip_playing = false;
         a.is_playing = false;
         a.playing_target_index = -1;
         a.playing_compare_index = -1;
         if (!a.show_game_screen)
            a.paused = true;
         else
            a.paused = a.was_paused;
      }

      if (a.core.loaded())
      {
         if (a.deep_scan.running)
            deep_scan_step(a);
         else if (a.analyze.running)
            analyze_step(a);
         else if (a.probe.running)
            probe_step(a);
         else if (!a.paused)
         {
            if (a.preview_dirty && a.preview)
               start_preview(a);
            if (a.fast_forward)
               for (int i = 0; i < 4; i++)
                  run_one_frame(a, false);
            else
            {
               // Audio drives the pace: run frames until about two frames are queued.
               const Uint32 target = (Uint32)(AudioOut::kRate / a.core.fps() * 2 * 4);
               for (int i = 0; i < 3 && a.audio.queued() < target; i++)
                  run_one_frame(a, true);
            }
         }
         upload_frame(a);
      }

      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      draw_ui(a);
      ImGui::Render();
      SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
      SDL_RenderClear(renderer);
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
      SDL_RenderPresent(renderer);
   }

   stop_preview(a);
   a.core.unload();
   save_settings(a.settings);
   if (a.game_tex)
      SDL_DestroyTexture(a.game_tex);
   for (auto &s : a.songs)
      if (s.thumb)
         SDL_DestroyTexture(s.thumb);
   if (a.controller)
      SDL_GameControllerClose(a.controller);
   ImGui_ImplSDLRenderer2_Shutdown();
   ImGui_ImplSDL2_Shutdown();
   ImGui::DestroyContext();
   SDL_DestroyRenderer(renderer);
   SDL_DestroyWindow(window);
   SDL_Quit();
   return 0;
}
