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
   }
   if (s.retroarch_dir.empty())
      for (const char *guess : { "D:\\RetroArch", "C:\\RetroArch-Win64", "C:\\RetroArch" })
         if (file_exists(std::string(guess) + "\\retroarch.exe")) { s.retroarch_dir = guess; break; }
   return s;
}

static void save_settings(const Settings &s)
{
   write_text(settings_path(), "retroarch_dir=" + s.retroarch_dir + "\ncore=" + s.core_file + "\nrom=" + s.rom_path + "\n");
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
   a.status = "Loaded " + content_name(a);

   size_t size = 0;
   a.core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
   a.finder.reset(size);

   char found[2048];
   if (px_engine_find_profile(a.core.content_path().c_str(), system_dir(a).c_str(), found, sizeof(found)))
      load_profile_into_ui(a, found);
   else
      a.profile.path = system_dir(a) + "\\proteus\\" + content_name(a) + ".ini";
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

   ImGui::SeparatorText("Controls");
   ImGui::TextWrapped("Arrows: D-pad   X: A   Z: B   S: X   A: Y   Q/W: L/R   Enter: Start   Right Shift: Select\n"
         "P: pause   F1: reset   Tab: fast forward   F2/F4: save/load quick state\n"
         "M: music changed   N: music is the same   (drag & drop ROMs, controller supported)");
}

static void ui_finder(App &a)
{
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
      ImGui::BeginDisabled(s.clip.empty());
      if (ImGui::SmallButton(s.recording ? "recording..." : "Play original"))
         play_clip(a, s.clip, s.clip_rate);
      ImGui::EndDisabled();
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
         ImGui::BeginDisabled(!s.path[0]);
         if (ImGui::Button("Listen"))
            play_file(a, s);
         ImGui::EndDisabled();
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

   float status_h = ImGui::GetFrameHeightWithSpacing();
   float avail_h = ImGui::GetContentRegionAvail().y - status_h;
   float left_w = ImGui::GetContentRegionAvail().x * 0.5f;

   ImGui::BeginChild("game", ImVec2(left_w, avail_h), ImGuiChildFlags_Borders);
   if (a.game_tex)
   {
      float bar_h = ImGui::GetFrameHeightWithSpacing();
      ImVec2 avail = ImGui::GetContentRegionAvail();
      avail.y = std::max(10.0f, avail.y - bar_h - 4.0f);
      float aspect = (float)a.core.aspect();
      float w = avail.x, h = w / aspect;
      if (h > avail.y) { h = avail.y; w = h * aspect; }
      ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - w) / 2, ImGui::GetCursorPosY() + (avail.y - h) / 2));
      ImGui::Image((ImTextureID)(intptr_t)a.game_tex, ImVec2(w, h));

      ImGui::SetCursorPos(ImVec2(8, ImGui::GetWindowHeight() - bar_h - 4));
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
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90);
      ImGui::SliderInt("##vol", &a.audio.volume, 0, 100, "%d%%");
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Master volume");
   }
   else
      ImGui::TextDisabled("Choose a core and a ROM in the Setup tab, then Start game.\nYou can also drag and drop a ROM file here.");
   ImGui::EndChild();

   ImGui::SameLine();
   ImGui::BeginChild("tools", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
   if (ImGui::BeginTabBar("tabs"))
   {
      if (ImGui::BeginTabItem("Setup")) { ui_setup(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Find address")) { ui_finder(a); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Songs")) { ui_songs(a); ImGui::EndTabItem(); }
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
         a.paused = a.was_paused;
      }
      if (a.clip_playing && a.pad)
      {
         a.audio.clear();
         a.clip_playing = false;
         a.paused = a.was_paused;
      }

      if (a.core.loaded())
      {
         if (a.analyze.running)
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
