// SPDX-License-Identifier: GPL-3.0-or-later
#include "n64_scan.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include <memory>

#include "core_host.h"
#include "platform.h"
#include "zip_read.h"

extern "C" {
#include "profile.h"
}

// ---------------------------------------------------------------------------
// ROM header and song names
// ---------------------------------------------------------------------------

bool n64_rom_header(const std::vector<uint8_t> &rom, N64Rom &out)
{
   if (rom.size() < 0x40)
      return false;
   uint8_t h[0x40];
   uint32_t magic = (uint32_t)rom[0] << 24 | rom[1] << 16 | rom[2] << 8 | rom[3];
   for (size_t i = 0; i < 0x40; i++)
   {
      if (magic == 0x80371240u)
         h[i] = rom[i];
      else if (magic == 0x37804012u) // .v64: bytes swapped in pairs
         h[i] = rom[i ^ 1];
      else if (magic == 0x40123780u) // .n64: little-endian words
         h[i] = rom[(i & ~3u) + 3 - (i & 3)];
      else
         return false;
   }
   out.code.assign((const char*)h + 0x3B, 4);
   out.version = h[0x3F];
   out.title.assign((const char*)h + 0x20, 20);
   while (!out.title.empty() && (out.title.back() == ' ' || out.title.back() == '\0'))
      out.title.pop_back();
   return true;
}

static bool n64_extension(const std::string &name)
{
   std::string ext = lower_ext(name);
   return ext == "z64" || ext == "n64" || ext == "v64";
}

bool is_n64_rom_file(const std::string &path)
{
   if (n64_extension(path))
      return true;
   std::vector<uint8_t> data;
   if (lower_ext(path) != "zip" || !read_file_bytes(path, data))
      return false;
   bool n64 = false;
   std::string err;
   zip_read(data, [&](const std::string &name) { n64 = n64 || n64_extension(name); return false; },
         [](const std::string &, std::vector<uint8_t> &) { return false; }, err);
   return n64;
}

// Sequence numbers from the decompilations (n64decomp/sm64 seq_ids.h, zeldaret/oot sequence_table.h),
// with the titles of Zophar's Domain's USF sets where they name a song differently.
static const std::vector<N64Song> kSm64Songs = {
   { 0x01, "Star Collect", "Star Catch Fanfare" }, { 0x02, "Title Theme", nullptr },
   { 0x03, "Bob-omb Battlefield", "Main Theme" }, { 0x04, "Inside the Castle Walls", nullptr },
   { 0x05, "Dire, Dire Docks", nullptr }, { 0x06, "Lethal Lava Land", nullptr }, { 0x07, "Koopa's Theme", nullptr },
   { 0x08, "Snow Mountain", nullptr }, { 0x09, "Slider", nullptr }, { 0x0A, "Haunted House", nullptr },
   { 0x0B, "Piranha Plant's Lullaby", nullptr }, { 0x0C, "Cave Dungeon", nullptr }, { 0x0D, "Star Select", "Game Start" },
   { 0x0E, "Powerful Mario", nullptr }, { 0x0F, "Metallic Mario", nullptr }, { 0x10, "Koopa's Message", nullptr },
   { 0x11, "Koopa's Road", nullptr }, { 0x12, "High Score", nullptr }, { 0x13, "Merry-Go-Round", nullptr },
   { 0x14, "Race Fanfare", nullptr }, { 0x15, "Star Appears", "Power Star" }, { 0x16, "Stage Boss", nullptr },
   { 0x17, "Key Collect", "Koopa Clear" }, { 0x18, "Endless Stairs", "Looping Steps" }, { 0x19, "Ultimate Koopa", nullptr },
   { 0x1A, "Staff Roll", nullptr }, { 0x1B, "Puzzle Solved", "Correct Solution" }, { 0x1C, "Toad's Message", nullptr },
   { 0x1D, "Peach's Message", nullptr }, { 0x1E, "Opening", nullptr }, { 0x1F, "Ultimate Koopa Clear", nullptr },
   { 0x20, "Ending Demo", nullptr }, { 0x21, "File Select", nullptr }, { 0x22, "Lakitu", "Lakitu's Message" },
};

// Ocarina of Time's fanfares (SEQ_FLAG_FANFARE: item gets, ocarina songs, the owl, Game Over) play on
// the fanfare player over the BGM, so they are not listed: they stay the game's own.
static const std::vector<N64Song> kOotSongs = {
   { 0x01, "Nature Ambience", nullptr }, { 0x02, "Hyrule Field", "Hyrule Field Main Theme" },
   { 0x18, "Dodongo's Cavern", nullptr }, { 0x19, "Kakariko Village (Adult)", "Kakariko Village Orchestral Version" },
   { 0x1A, "Enemy Battle", "Battle" }, { 0x1B, "Boss Battle", nullptr }, { 0x1C, "Inside the Deku Tree", nullptr },
   { 0x1D, "Market", nullptr }, { 0x1E, "Title Theme", nullptr }, { 0x1F, "House", nullptr },
   { 0x21, "Boss Clear", nullptr }, { 0x26, "Inside Jabu-Jabu's Belly", nullptr },
   { 0x27, "Kakariko Village (Child)", "Kakariko Village" }, { 0x28, "Great Fairy's Fountain", nullptr },
   { 0x29, "Zelda's Theme", nullptr }, { 0x2A, "Fire Temple", nullptr }, { 0x2C, "Forest Temple", nullptr },
   { 0x2D, "Hyrule Castle Courtyard", nullptr }, { 0x2E, "Ganon's Castle", "Inside Ganon's Castle" },
   { 0x2F, "Lon Lon Ranch", nullptr }, { 0x30, "Goron City", nullptr },
   { 0x31, "Hyrule Field Morning", "Hyrule Field Morning Theme" }, { 0x38, "Mini-Boss Battle", "Middle Boss Battle" },
   { 0x3A, "Temple of Time", nullptr }, { 0x3C, "Kokiri Forest", nullptr }, { 0x3E, "Lost Woods", nullptr },
   { 0x3F, "Spirit Temple", nullptr }, { 0x40, "Horse Race", nullptr }, { 0x41, "Horse Race Goal", nullptr },
   { 0x42, "Ingo's Theme", nullptr }, { 0x4A, "Fairy Flying", nullptr }, { 0x4B, "Deku Tree", nullptr },
   { 0x4C, "Windmill Hut", nullptr }, { 0x4D, "Legend of Hyrule", nullptr }, { 0x4E, "Shooting Gallery", nullptr },
   { 0x4F, "Sheik's Theme", nullptr }, { 0x50, "Zora's Domain", nullptr }, { 0x52, "Adult Link", nullptr },
   { 0x53, "Master Sword", nullptr }, { 0x55, "Shop", nullptr }, { 0x56, "Chamber of the Sages", "Chamber of Sages" },
   { 0x57, "File Select", nullptr }, { 0x58, "Ice Cavern", nullptr }, { 0x5B, "Shadow Temple", nullptr },
   { 0x5C, "Water Temple", nullptr }, { 0x5E, "Seal of Six Sages", nullptr }, { 0x5F, "Gerudo Valley", nullptr },
   { 0x60, "Potion Shop", nullptr }, { 0x61, "Kotake & Koume's Theme", nullptr },
   { 0x62, "Escape from Ganon's Castle", nullptr }, { 0x63, "Ganon's Castle Underground", nullptr },
   { 0x64, "Ganondorf Battle", nullptr }, { 0x65, "Ganon Battle", "Last Battle" },
   { 0x66, "Ocarina of Time", "Ocarina of Time ~Zelda's Ocarina~" }, { 0x67, "Staff Roll 1", "End Credits A" },
   { 0x68, "Staff Roll 2", "End Credits B" }, { 0x69, "Staff Roll 3", "End Credits C" },
   { 0x6A, "Staff Roll 4", "End Credits D" }, { 0x6B, "Fire Boss Battle", "Dinosaur Boss Battle" },
   { 0x6C, "Timed Mini-Game", "Mini Game" },
};

const std::vector<N64Song> *n64_song_names(const std::string &code)
{
   std::string game = code.substr(0, 3);
   if (game == "NSM")
      return &kSm64Songs;
   if (game == "CZL")
      return &kOotSongs;
   return nullptr;
}

// Letters and digits only, lower case: "Kotake & Koume's Theme" -> "kotakekoumestheme".
static std::string title_key(const std::string &s)
{
   std::string k;
   for (unsigned char c : s)
      if (isalnum(c))
         k += (char)tolower(c);
   return k;
}

int n64_song_of_reference(const std::string &code, const std::string &file_name)
{
   const std::vector<N64Song> *songs = n64_song_names(code);
   if (!songs)
      return -1;
   std::string title = stem_of(file_name);
   // "LOZ57": a set's files named after the sequence number.
   if (title.size() >= 3 && isxdigit((unsigned char)title[title.size() - 1]) && isxdigit((unsigned char)title[title.size() - 2]))
   {
      bool letters = true;
      for (size_t i = 0; i + 2 < title.size(); i++)
         letters = letters && isalpha((unsigned char)title[i]);
      int value = (int)strtol(title.substr(title.size() - 2).c_str(), nullptr, 16);
      if (letters && title.size() > 2)
         for (const auto &s : *songs)
            if (s.value == value)
               return value;
   }
   // "19a Hyrule Field Main Theme": the track number goes.
   size_t at = 0;
   while (at < title.size() && isdigit((unsigned char)title[at]))
      at++;
   if (at > 0)
   {
      if (at < title.size() && isalpha((unsigned char)title[at]) && at + 1 < title.size() && (title[at + 1] == ' ' || title[at + 1] == '_'))
         at++;
      while (at < title.size() && (title[at] == ' ' || title[at] == '_'))
         at++;
      title = title.substr(at);
   }
   std::string key = title_key(title);
   for (const auto &s : *songs)
      if (key == title_key(s.zophar ? s.zophar : s.name))
         return s.value;
   return -1;
}

// ---------------------------------------------------------------------------
// Sequence player layouts
// ---------------------------------------------------------------------------

namespace {

struct FloatField
{
   uint32_t offset;
   float min, max;
};

struct HoldField
{
   uint32_t offset;
   const char *text; // the [hold] value
   const char *what;
};

// A sequence player struct of one engine revision, from the decompilations.
struct Layout
{
   const char *engine;
   uint32_t size;
   int players;
   uint32_t seq_id;      // u8 sequence number
   uint32_t seq_data;    // u8* sequence data
   uint32_t channels;    // SequenceChannel* channels[16]
   uint32_t channel_player; // SequenceChannel's SequencePlayer* back to its player
   std::vector<FloatField> floats;
   std::vector<HoldField> hold;
   const char *player_names;
};

const Layout kLayouts[] = {
   // Super Mario 64 (US/JP): gSequencePlayers[3]; the volume is the fade volume, applied every tick.
   { "sm64", 0x140, 3, 0x05, 0x14, 0x2C, 0x40,
      { { 0x18, 0.0f, 1.01f }, { 0x1C, -1.01f, 1.01f }, { 0x20, 0.0f, 1.01f }, { 0x24, 0.0f, 1.01f } },
      { { 0x18, "00000000, 3F800000", "fade volume" } },
      "level music, jingles, sound effects" },
   // Ocarina of Time: gAudioCtx.seqPlayers[4]; the volume scale applies when the recalculate flag is set.
   { "oot", 0x160, 4, 0x04, 0x18, 0x38, 0x4C,
      { { 0x1C, 0.0f, 1.01f }, { 0x20, -1.01f, 1.01f }, { 0x24, 0.0f, 2.01f }, { 0x28, 0.0f, 1.01f },
        { 0x2C, 0.0f, 2.01f }, { 0x30, 0.0f, 2.01f }, { 0x34, 0.001f, 4.01f } },
      { { 0x2C, "00000000, 3F800000", "volume scale" }, { 0x00, "|04", "recalculate the volume" } },
      "BGM, fanfares, sound effects, BGM sub" },
};

// RDRAM as Mupen64Plus-Next keeps it: 32-bit words in host (little endian) order.
struct Rdram
{
   const uint8_t *data;
   size_t size;
   uint8_t byte(uint32_t a) const { return data[(a & ~3u) + 3 - (a & 3)]; }
   uint32_t word(uint32_t a) const { return (uint32_t)byte(a) << 24 | byte(a + 1) << 16 | byte(a + 2) << 8 | byte(a + 3); }
   float f32(uint32_t a) const { uint32_t w = word(a); float f; memcpy(&f, &w, 4); return f; }
};

bool ram_pointer(const Rdram &r, uint32_t w)
{
   return (w & 0xFF000000u) == 0x80000000u && (w & 0x00FFFFFFu) < r.size;
}

bool player_matches(const Rdram &r, const Layout &l, uint32_t p)
{
   uint32_t seq_data = r.word(p + l.seq_data);
   if (seq_data && !ram_pointer(r, seq_data))
      return false;
   for (int c = 0; c < 16; c++)
      if (!ram_pointer(r, r.word(p + l.channels + 4 * c)))
         return false;
   for (const auto &f : l.floats)
   {
      float v = r.f32(p + f.offset);
      if (!std::isfinite(v) || v < f.min || v > f.max)
         return false;
   }
   return true;
}

// Every channel of every player is either one of that player's own (its seqPlayer points back) or
// the one shared "no channel" placeholder, and some player has a channel of its own.
bool array_matches(const Rdram &r, const Layout &l, uint32_t p)
{
   uint32_t none = 0;
   bool owned = false;
   if ((uint64_t)p + (uint64_t)l.size * l.players > r.size)
      return false;
   for (int k = 0; k < l.players; k++)
   {
      uint32_t player = p + k * l.size;
      if (!player_matches(r, l, player))
         return false;
      for (int c = 0; c < 16; c++)
      {
         uint32_t channel = r.word(player + l.channels + 4 * c);
         if ((channel & 0x00FFFFFFu) + l.channel_player + 4 <= r.size
               && r.word((channel & 0x00FFFFFFu) + l.channel_player) == (0x80000000u | player))
            owned = true;
         else if (!none)
            none = channel;
         else if (channel != none)
            return false;
      }
   }
   return owned;
}

std::string hex(uint32_t v, int digits)
{
   char b[16];
   snprintf(b, sizeof(b), "%0*X", digits, v);
   return b;
}

} // namespace

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

bool n64_scan(const std::string &core_path, const std::string &rom_path, const std::string &save_dir,
      double seconds, N64ScanResult &result, std::string &error,
      const std::function<void(const std::string &)> &log)
{
   CoreHost core;
   // Software rendering and high-level audio: no GPU needed, and fast.
   const std::map<std::string, std::string> options = {
      { "mupen64plus-rdp-plugin", "angrylion" },
      { "mupen64plus-rsp-plugin", "hle" },
      { "mupen64plus-cpucore", "dynamic_recompiler" },
   };
   result = N64ScanResult();
   if (!core.load(core_path, rom_path, save_dir, save_dir, error, options))
      return false;
   core.take_log();
   if (!n64_rom_header(core.content_data(), result.rom))
   {
      error = rom_path + " is not an N64 ROM";
      return false;
   }
   core.set_skip_video(true);

   // Candidate arrays: layout index and address -> scans that matched.
   std::map<std::pair<int, uint32_t>, int> hits;
   const int frames = (int)(seconds * 60);
   int last_seq = -1, confirmed_at = -1;
   const Layout *layout = nullptr;
   uint32_t base = 0;

   for (int f = 0; f < frames; f++)
   {
      // Start every 5 seconds from 10 seconds in: past title screens, never deep into a menu.
      bool start = f >= 600 && f % 300 < 6;
      core.run_frame(start ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0);
      core.audio().clear();
      size_t size = 0;
      const uint8_t *data = core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
      if (!data || size < 0x400000)
         continue;
      Rdram r{ data, size };
      if (f == 60 && !(data[0x318] == 0 && (data[0x31A] == 0x40 || data[0x31A] == 0x80)))
      {
         error = "the core's RDRAM is not in Mupen64Plus-Next's word order";
         return false;
      }

      if (!layout && f >= 120 && f % 60 == 0)
      {
         for (int li = 0; li < (int)(sizeof(kLayouts) / sizeof(kLayouts[0])); li++)
            for (uint32_t p = 0; p + 0x400 <= size; p += 4)
               if (ram_pointer(r, r.word(p + kLayouts[li].channels)) && array_matches(r, kLayouts[li], p))
                  hits[{ li, p }]++;
         // An array found in three scans whose BGM player is playing is the one.
         for (const auto &h : hits)
         {
            const Layout &l = kLayouts[h.first.first];
            uint32_t p = h.first.second;
            if (h.second >= 3 && array_matches(r, l, p) && (r.byte(p) & 0x80) && r.byte(p + l.seq_id))
            {
               layout = &l;
               base = p;
               log(std::string("found ") + l.engine + " sequence players at 0x" + hex(0x80000000u | p, 8) +
                     " after " + std::to_string(f / 60) + " s");
               break;
            }
         }
      }
      if (layout)
      {
         int seq = (r.byte(base) & 0x80) ? r.byte(base + layout->seq_id) : -1;
         if (seq != last_seq && seq >= 0)
         {
            result.songs_heard.push_back(seq);
            log("  BGM player song 0x" + hex((uint32_t)seq, 2));
            if (confirmed_at < 0)
               confirmed_at = f;
         }
         last_seq = seq;
         // A few more seconds to hear the song change once more; then done.
         if (confirmed_at >= 0 && (result.songs_heard.size() >= 2 || f - confirmed_at >= 1200))
            break;
      }
   }
   if (!layout)
   {
      error = "no Nintendo EAD sequence players found in " + std::to_string((int)seconds) + " s";
      return false;
   }
   result.found = true;
   result.engine = layout->engine;
   result.players = 0x80000000u | base;
   result.player_count = layout->players;
   return true;
}

static const Layout *layout_named(const std::string &engine)
{
   for (const auto &l : kLayouts)
      if (engine == l.engine)
         return &l;
   return nullptr;
}

bool n64_scan_from_profile(const std::string &profile_path, const N64Rom &rom, N64ScanResult &result)
{
   auto p = std::make_unique<px_profile>();
   char err[1200];
   if (!px_profile_load(p.get(), profile_path.c_str(), err, sizeof(err)) || !p->n64 || !p->active)
      return false;
   uint32_t base = p->active_address;
   for (const auto &l : kLayouts)
   {
      if (p->address != base + l.seq_id || p->hold_count != l.hold.size())
         continue;
      bool same = true;
      for (size_t i = 0; i < l.hold.size(); i++)
         same = same && p->hold[i].address == base + l.hold[i].offset;
      if (!same)
         continue;
      result = N64ScanResult();
      result.found = true;
      result.engine = l.engine;
      result.players = base;
      result.player_count = l.players;
      result.rom = rom;
      return true;
   }
   return false;
}

uint32_t n64_song_address(const N64ScanResult &scan)
{
   const Layout *l = layout_named(scan.engine);
   return l ? scan.players + l->seq_id : 0;
}

std::string n64_profile(const N64ScanResult &scan, const std::string &rom_name,
      const std::map<int, std::pair<std::string, std::string>> &tracks, const std::string &music_dir)
{
   const Layout *layout = layout_named(scan.engine);
   if (!layout)
      return "";
   uint32_t p = scan.players;
   std::string s;
   s += "; " + rom_name + " (" + scan.rom.code + " version " + std::to_string(scan.rom.version) + ")\n";
   s += "; Nintendo EAD sound engine (" + std::string(layout->engine) + "): " + std::to_string(layout->players) +
         " sequence players at 0x" + hex(p, 8) + " (" + layout->player_names + ").\n";
   s += "; Proteus follows the first player's song and holds its volume at zero while replacing; sound\n";
   s += "; effects play on the other players.\n";
   s += "[song]\n";
   s += "byte_order = n64\n";
   s += "address    = 0x" + hex(p + layout->seq_id, 8) + "\n";
   s += "active     = 0x" + hex(p, 8) + " & 0x80\n";
   s += "stopped    = original\n";
   s += "unmapped   = original\n";
   s += "debounce   = 2\n\n";
   s += "[hold]\n";
   for (const auto &h : layout->hold)
   {
      if (h.text[0] == '|')
         s += "0x" + hex(p + h.offset, 8) + " |= " + (h.text + 1) + "   ; " + h.what + "\n";
      else
         s += "0x" + hex(p + h.offset, 8) + " = " + h.text + "   ; " + h.what + "\n";
   }
   s += "\n[mix]\ncrossfade_ms = 400\n\n";
   // One music folder for every revision: the ROM name without its "(USA) (Rev 1)" tags.
   std::string dir = music_dir;
   if (dir.empty())
   {
      std::string game = rom_name;
      size_t tag = game.find(" (");
      if (tag != std::string::npos)
         game.resize(tag);
      dir = "music/" + sanitize_filename(game);
   }
   s += "[library]\ndir = " + dir + "\n\n";
   s += "[debug]\nlog_songs = 1\n\n";
   s += "[tracks]\n";
   s += "; Replace a song with a file (or pick one in RetroArch's Quick Menu > Core Options > Proteus):\n";
   s += ";   0x.. = " + dir + "/my song.ogg | name=...\n";
   std::map<int, std::pair<std::string, std::string>> left = tracks;
   auto line = [&](int value, const std::string &name) {
      auto t = left.find(value);
      std::string spec = t == left.end() ? "original" : t->second.first;
      std::string comment = t == left.end() ? "" : t->second.second;
      if (t != left.end())
         left.erase(t);
      s += "0x" + hex((uint32_t)value, 2) + " = " + spec + (name.empty() ? "" : " | name=" + name) +
            (comment.empty() ? "" : " ; " + comment) + "\n";
   };
   if (const auto *songs = n64_song_names(scan.rom.code))
      for (const auto &song : *songs)
         line(song.value, song.name);
   // Songs mapped that the name list lacks.
   while (!left.empty())
      line(left.begin()->first, "");
   return s;
}
