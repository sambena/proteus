// SPDX-License-Identifier: LGPL-2.1-or-later
#include "n64_scan.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "core_host.h"
#include "platform.h"

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

// Sequence numbers from the decompilations (n64decomp/sm64 seq_ids.h, zeldaret/oot sequence_table.h),
// background music and fanfares only.
static const std::vector<N64Song> kSm64Songs = {
   { 0x01, "Star Collect" }, { 0x02, "Title Theme" }, { 0x03, "Bob-omb Battlefield" },
   { 0x04, "Inside the Castle Walls" }, { 0x05, "Dire, Dire Docks" }, { 0x06, "Lethal Lava Land" },
   { 0x07, "Koopa's Theme" }, { 0x08, "Snow Mountain" }, { 0x09, "Slider" }, { 0x0A, "Haunted House" },
   { 0x0B, "Piranha Plant's Lullaby" }, { 0x0C, "Cave Dungeon" }, { 0x0D, "Star Select" },
   { 0x0E, "Powerful Mario" }, { 0x0F, "Metallic Mario" }, { 0x10, "Koopa's Message" },
   { 0x11, "Koopa's Road" }, { 0x12, "High Score" }, { 0x13, "Merry-Go-Round" }, { 0x14, "Race Fanfare" },
   { 0x15, "Star Appears" }, { 0x16, "Stage Boss" }, { 0x17, "Key Collect" }, { 0x18, "Endless Stairs" },
   { 0x19, "Ultimate Koopa" }, { 0x1A, "Staff Roll" }, { 0x1B, "Puzzle Solved" }, { 0x1C, "Toad's Message" },
   { 0x1D, "Peach's Message" }, { 0x1E, "Opening" }, { 0x1F, "Ultimate Koopa Clear" }, { 0x20, "Ending Demo" },
   { 0x21, "File Select" }, { 0x22, "Lakitu" },
};

static const std::vector<N64Song> kOotSongs = {
   { 0x01, "Nature Ambience" }, { 0x02, "Hyrule Field" }, { 0x18, "Dodongo's Cavern" },
   { 0x19, "Kakariko Village (Adult)" }, { 0x1A, "Enemy Battle" }, { 0x1B, "Boss Battle" },
   { 0x1C, "Inside the Deku Tree" }, { 0x1D, "Market" }, { 0x1E, "Title Theme" }, { 0x1F, "House" },
   { 0x20, "Game Over" }, { 0x21, "Boss Clear" }, { 0x22, "Item Get" }, { 0x23, "Opening Ganondorf" },
   { 0x24, "Heart Container Get" }, { 0x25, "Prelude of Light" }, { 0x26, "Inside Jabu-Jabu's Belly" },
   { 0x27, "Kakariko Village (Child)" }, { 0x28, "Great Fairy's Fountain" }, { 0x29, "Zelda's Theme" },
   { 0x2A, "Fire Temple" }, { 0x2B, "Open Treasure Box" }, { 0x2C, "Forest Temple" },
   { 0x2D, "Hyrule Castle Courtyard" }, { 0x2E, "Ganon's Castle" }, { 0x2F, "Lon Lon Ranch" },
   { 0x30, "Goron City" }, { 0x31, "Hyrule Field Morning" }, { 0x32, "Spiritual Stone Get" },
   { 0x33, "Bolero of Fire" }, { 0x34, "Minuet of Forest" }, { 0x35, "Serenade of Water" },
   { 0x36, "Requiem of Spirit" }, { 0x37, "Nocturne of Shadow" }, { 0x38, "Mini-Boss Battle" },
   { 0x39, "Small Item Get" }, { 0x3A, "Temple of Time" }, { 0x3B, "Event Clear" }, { 0x3C, "Kokiri Forest" },
   { 0x3D, "Fairy Ocarina Get" }, { 0x3E, "Lost Woods" }, { 0x3F, "Spirit Temple" }, { 0x40, "Horse Race" },
   { 0x41, "Horse Race Goal" }, { 0x42, "Ingo's Theme" }, { 0x43, "Medallion Get" },
   { 0x44, "Saria's Song (Ocarina)" }, { 0x45, "Epona's Song (Ocarina)" }, { 0x46, "Zelda's Lullaby (Ocarina)" },
   { 0x47, "Sun's Song (Ocarina)" }, { 0x48, "Song of Time (Ocarina)" }, { 0x49, "Song of Storms (Ocarina)" },
   { 0x4A, "Fairy Flying" }, { 0x4B, "Deku Tree" }, { 0x4C, "Windmill Hut" }, { 0x4D, "Legend of Hyrule" },
   { 0x4E, "Shooting Gallery" }, { 0x4F, "Sheik's Theme" }, { 0x50, "Zora's Domain" }, { 0x51, "Enter Zelda" },
   { 0x52, "Goodbye to Zelda" }, { 0x53, "Master Sword" }, { 0x54, "Ganondorf's Theme" }, { 0x55, "Shop" },
   { 0x56, "Chamber of the Sages" }, { 0x57, "File Select" }, { 0x58, "Ice Cavern" },
   { 0x59, "Open Door of Temple of Time" }, { 0x5A, "Kaepora Gaebora's Theme" }, { 0x5B, "Shadow Temple" },
   { 0x5C, "Water Temple" }, { 0x5D, "Ganon's Castle Bridge" }, { 0x5E, "Seal of Six Sages" },
   { 0x5F, "Gerudo Valley" }, { 0x60, "Potion Shop" }, { 0x61, "Kotake & Koume's Theme" },
   { 0x62, "Escape from Ganon's Castle" }, { 0x63, "Ganon's Castle Under Ground" }, { 0x64, "Ganondorf Battle" },
   { 0x65, "Ganon Battle" }, { 0x66, "Ocarina of Time" }, { 0x67, "Staff Roll 1" }, { 0x68, "Staff Roll 2" },
   { 0x69, "Staff Roll 3" }, { 0x6A, "Staff Roll 4" }, { 0x6B, "Volvagia Battle" }, { 0x6C, "Timed Mini-Game" },
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

std::string n64_profile(const N64ScanResult &scan, const std::string &rom_name)
{
   const Layout *layout = nullptr;
   for (const auto &l : kLayouts)
      if (scan.engine == l.engine)
         layout = &l;
   if (!layout)
      return "";
   uint32_t p = scan.players;
   std::string s;
   s += "; " + rom_name + " (" + scan.rom.code + " version " + std::to_string(scan.rom.version) + ")\n";
   s += "; Nintendo EAD sound engine (" + std::string(layout->engine) + "): " + std::to_string(layout->players) +
         " sequence players at 0x" + hex(p, 8) + " (" + layout->player_names + "), found by proteus-cli n64.\n";
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
   std::string game = rom_name;
   size_t tag = game.find(" (");
   if (tag != std::string::npos)
      game.resize(tag);
   s += "[library]\ndir = music/" + sanitize_filename(game) + "\n\n";
   s += "[debug]\nlog_songs = 1\n\n";
   s += "[tracks]\n";
   s += "; Replace a song with a file (or pick one in RetroArch's Quick Menu > Core Options > Proteus):\n";
   s += ";   0x.. = music/" + sanitize_filename(game) + "/my song.ogg | name=...\n";
   if (const auto *songs = n64_song_names(scan.rom.code))
      for (const auto &song : *songs)
         s += "0x" + hex((uint32_t)song.value, 2) + " = original | name=" + song.name + "\n";
   return s;
}
