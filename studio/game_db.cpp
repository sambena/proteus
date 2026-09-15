// SPDX-License-Identifier: LGPL-2.1-or-later
#include "game_db.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "platform.h"

// Games confirmed by Proteus Studio scans, used until the user's file says otherwise.
static const char kBuiltIn[] = R"(
[B19ED489]
name = Super Mario World
song_address = system_ram 0x1DFB size=1 latch=1 debounce=1
start = ram 0x1DFB bytes=xx settle=150
note = built in: scanning $1DFB starts songs; $0DDA does not follow it

[777AAC2F]
name = The Legend of Zelda: A Link to the Past
song_address = system_ram 0x0130 size=1 latch=0 debounce=2
start = ram 0x012C bytes=xx settle=150
note = built in: $012C starts songs; $0130 keeps the song number (RetroAchievements note, confirmed by a scan)

[2D206BF7]
name = Chrono Trigger
song_address = system_ram 0x1E00 bytes=10 xx .. .. latch=1 debounce=1
start = routine jsl 0xC70004 block=0x1E00 bytes=10 xx FF 05 settle=300
note = built in: the music routine takes its command from $1E00
)";

static const char *kMemoryNames[] = { "system_ram", "save_ram", "video_ram" };

static std::string hex(uint32_t v, int digits)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "0x%0*X", digits, (unsigned)v);
   return buf;
}

std::string describe_song_start(const SongStart &s)
{
   auto command = [&](uint32_t at) {
      std::string out = "$" + hex(at, 4).substr(2) + " =";
      if (s.bytes.empty())
         return out + " song";
      for (size_t i = 0; i < s.bytes.size(); i++)
         out += (int)i == s.offset ? " song" : " " + hex(s.bytes[i], 2).substr(2);
      return out;
   };
   switch (s.kind)
   {
      case SongStart::RAM:
         return command(s.address);
      case SongStart::ROUTINE:
      {
         std::string out = std::string(s.jsr ? "JSR $" : "JSL $") + hex(s.address, 6).substr(2);
         if (s.fill_block)
            out += " with " + command(s.block);
         if (s.song_in_a)
            out += " with the song in A";
         return out;
      }
      default:
         return "unknown";
   }
}

static bool is_any(const SongAddress &a, size_t i)
{
   return i < a.any.size() && a.any[i];
}

bool read_song_address(const SongAddress &a, const uint8_t *ram, size_t ram_size, uint32_t &value)
{
   if (!a.known || !ram)
      return false;
   if (a.events && a.events_address < ram_size && ram[a.events_address])
   {
      value = 0x100u | ram[a.events_address];
      return true;
   }
   if (!a.bytes.empty())
   {
      if ((uint64_t)a.address + a.bytes.size() > ram_size)
         return false;
      for (size_t i = 0; i < a.bytes.size(); i++)
         if ((int)i != a.offset && !is_any(a, i) && ram[a.address + i] != a.bytes[i])
            return false;
      value = ram[a.address + a.offset];
      return true;
   }
   if ((uint64_t)a.address + a.size > ram_size)
      return false;
   value = 0;
   for (int i = 0; i < a.size; i++)
      value |= (uint32_t)ram[a.address + i] << (8 * i);
   return true;
}

std::string describe_song_address(const SongAddress &a)
{
   if (!a.known)
      return "not known";
   std::string out = "$" + hex(a.address, 4).substr(2);
   if (!a.bytes.empty())
   {
      out += " =";
      for (size_t i = 0; i < a.bytes.size(); i++)
         out += (int)i == a.offset ? " song" : is_any(a, i) ? " .." : " " + hex(a.bytes[i], 2).substr(2);
   }
   if (a.events)
      out += " + $" + hex(a.events_address, 4).substr(2);
   return out;
}

static std::vector<std::string> split(const std::string &s)
{
   std::vector<std::string> out;
   std::istringstream in(s);
   std::string w;
   while (in >> w)
      out.push_back(w);
   return out;
}

// "10 xx .. 05" -> bytes, the song position and (for song addresses) the bytes that may be anything
static void parse_bytes(const std::vector<std::string> &words, size_t from,
      std::vector<uint8_t> &bytes, int &offset, std::vector<bool> *any = nullptr)
{
   bytes.clear();
   offset = 0;
   if (any)
      any->clear();
   for (size_t i = from; i < words.size() && words[i].find('=') == std::string::npos; i++)
   {
      bool wild = words[i] == ".." || words[i] == "??";
      if (words[i] == "xx" || words[i] == "XX")
      {
         offset = (int)bytes.size();
         bytes.push_back(0);
      }
      else
         bytes.push_back(wild ? 0 : (uint8_t)strtoul(words[i].c_str(), nullptr, 16));
      if (any)
         any->push_back(wild);
   }
   if (bytes.size() == 1)
   {
      bytes.clear();   // the song number alone
      if (any)
         any->clear();
   }
}

std::string format_song_pattern(const SongAddress &a)
{
   if (a.bytes.empty())
      return "";
   std::string bytes;
   for (size_t i = 0; i < a.bytes.size(); i++)
      bytes += ((int)i == a.offset ? std::string("xx") : is_any(a, i) ? std::string("..") : hex(a.bytes[i], 2).substr(2)) +
               (i + 1 < a.bytes.size() ? " " : "");
   return bytes;
}

bool parse_song_pattern(const std::string &text, SongAddress &a)
{
   std::vector<std::string> w = split(text);
   if (w.empty())
   {
      a.bytes.clear();
      a.any.clear();
      a.offset = 0;
      a.size = 1;
      return true;
   }
   parse_bytes(w, 0, a.bytes, a.offset, &a.any);
   a.size = a.bytes.empty() ? 1 : (int)a.bytes.size();
   return true;
}

static void parse(const std::string &text, std::map<uint32_t, GameInfo> &games)
{
   std::istringstream in(text);
   std::string line;
   GameInfo *g = nullptr;
   while (std::getline(in, line))
   {
      if (!line.empty() && line.back() == '\r')
         line.pop_back();
      size_t semi = line.find(';');
      if (semi != std::string::npos && line.compare(0, 4, "note") != 0)
         line.erase(semi);
      size_t a = line.find_first_not_of(" \t");
      if (a == std::string::npos)
         continue;
      line.erase(0, a);
      if (line[0] == '[')
      {
         uint32_t crc = (uint32_t)strtoul(line.c_str() + 1, nullptr, 16);
         g = &games[crc];
         *g = GameInfo();
         g->crc32 = crc;
         continue;
      }
      size_t eq = line.find('=');
      if (!g || eq == std::string::npos)
         continue;
      std::string key = line.substr(0, eq), value = line.substr(eq + 1);
      while (!key.empty() && key.back() == ' ')
         key.pop_back();
      value.erase(0, value.find_first_not_of(' '));
      std::vector<std::string> w = split(value);

      // Options written as name=value inside a line.
      auto opt = [&](const char *name, const char *fallback) {
         std::string prefix = std::string(name) + "=";
         for (auto &x : w)
            if (x.compare(0, prefix.size(), prefix) == 0)
               return x.substr(prefix.size());
         return std::string(fallback);
      };

      if (key == "name")
         g->name = value;
      else if (key == "note")
         g->note = value;
      else if (key == "song_address" && w.size() >= 2)
      {
         g->song.known = true;
         for (int i = 0; i < 3; i++)
            if (w[0] == kMemoryNames[i])
               g->song.memory = i;
         g->song.address = (uint32_t)strtoul(w[1].c_str(), nullptr, 0);
         g->song.size = atoi(opt("size", "1").c_str());
         g->song.latch = opt("latch", "0") == "1";
         g->song.debounce = atoi(opt("debounce", "2").c_str());
         std::string events = opt("events", "");
         g->song.events = !events.empty();
         g->song.events_address = (uint32_t)strtoul(events.c_str(), nullptr, 0);
         for (size_t i = 0; i < w.size(); i++)
            if (w[i].compare(0, 6, "bytes=") == 0)
            {
               w[i] = w[i].substr(6);
               parse_bytes(w, i, g->song.bytes, g->song.offset, &g->song.any);
               g->song.size = g->song.bytes.empty() ? 1 : (int)g->song.bytes.size();
               break;
            }
      }
      else if (key == "start")
          parse_song_start(value, g->start);
      else if (key == "silence" && w.size() >= 2 && w[0] == "ram")
      {
         g->silence.known = true;
         g->silence.address = (uint32_t)strtoul(w[1].c_str(), nullptr, 0);
         g->silence.value = (uint8_t)strtoul(opt("value", "0").c_str(), nullptr, 0);
      }
   }
}

bool parse_song_start(const std::string &text, SongStart &s)
{
   std::vector<std::string> w = split(text);
   auto opt = [&](const char *name, const char *fallback) {
      std::string prefix = std::string(name) + "=";
      for (auto &x : w)
         if (x.compare(0, prefix.size(), prefix) == 0)
            return x.substr(prefix.size());
      return std::string(fallback);
   };
   SongStart out;
   if (w.size() >= 2 && w[0] == "ram")
   {
      out.kind = SongStart::RAM;
      out.address = (uint32_t)strtoul(w[1].c_str(), nullptr, 0);
   }
   else if (w.size() >= 3 && w[0] == "routine" && (w[1] == "jsl" || w[1] == "jsr"))
   {
      out.kind = SongStart::ROUTINE;
      out.jsr = w[1] == "jsr";
      out.address = (uint32_t)strtoul(w[2].c_str(), nullptr, 0);
      std::string block = opt("block", "");
      out.fill_block = !block.empty();
      out.block = (uint32_t)strtoul(block.c_str(), nullptr, 0);
      out.song_in_a = opt("a", "") == "song";
   }
   else
      return false;
   out.settle_frames = atoi(opt("settle", "150").c_str());
   for (size_t i = 0; i < w.size(); i++)
      if (w[i].compare(0, 6, "bytes=") == 0)
      {
         w[i] = w[i].substr(6);
         parse_bytes(w, i, out.bytes, out.offset);
         break;
      }
   s = out;
   return true;
}

std::string format_song_start(const SongStart &s)
{
   if (s.kind == SongStart::NONE)
      return "";
   std::string bytes;
   if (s.bytes.empty())
      bytes = "xx";
   for (size_t i = 0; i < s.bytes.size(); i++)
      bytes += ((int)i == s.offset ? std::string("xx") : hex(s.bytes[i], 2).substr(2)) + (i + 1 < s.bytes.size() ? " " : "");
   std::string t;
   if (s.kind == SongStart::RAM)
      t = "ram " + hex(s.address, 4) + " bytes=" + bytes;
   else
   {
      t = std::string("routine ") + (s.jsr ? "jsr " : "jsl ") + hex(s.address, 6);
      if (s.fill_block)
         t += " block=" + hex(s.block, 4) + " bytes=" + bytes;
      if (s.song_in_a)
         t += " a=song";
   }
   return t + " settle=" + std::to_string(s.settle_frames);
}

GameDb &GameDb::get()
{
   static GameDb db;
   return db;
}

std::string GameDb::path() const
{
   return app_data_dir() + "\\games.ini";
}

void GameDb::load_locked()
{
   if (loaded_)
      return;
   loaded_ = true;
   parse(kBuiltIn, games_);
   parse(read_text(path()), games_);
}

void GameDb::save_locked()
{
   std::string t = "; Proteus Studio game database: how each game keeps and starts its songs.\n"
                   "; Sections are ROM CRC32s (without copier headers). Studio updates this file when\n"
                   "; scans confirm something; edits made here are used the next time a ROM opens.\n"
                   ";\n"
                   "; song_address = <memory> <address> size=<bytes> latch=<0|1> debounce=<frames> [events=<jingle command address>]\n"
                   "; song_address = <memory> <address> bytes=<pattern, xx = song number, .. = any> latch=1 debounce=1\n"
                   "; start = ram <address> bytes=<command, xx = song number> settle=<frames>\n"
                   "; start = routine <jsl|jsr> <address> [block=<address>] [bytes=...] [a=song] settle=<frames>\n"
                   "; silence = ram <address> value=<byte that stops the game's music>\n";
   for (auto &e : games_)
   {
      const GameInfo &g = e.second;
      char crc[16];
      snprintf(crc, sizeof(crc), "%08X", g.crc32);
      t += std::string("\n[") + crc + "]\n";
      if (!g.name.empty())
         t += "name = " + g.name + "\n";
      if (g.song.known)
      {
         t += std::string("song_address = ") + kMemoryNames[g.song.memory] + " " + hex(g.song.address, 4);
         if (!g.song.bytes.empty())
            t += " bytes=" + format_song_pattern(g.song);
         else
            t += " size=" + std::to_string(g.song.size);
         t += " latch=" + std::string(g.song.latch ? "1" : "0") +
              " debounce=" + std::to_string(g.song.debounce);
         if (g.song.events)
            t += " events=" + hex(g.song.events_address, 4);
         t += "\n";
      }
      if (g.start.kind != SongStart::NONE)
         t += "start = " + format_song_start(g.start) + "\n";
      if (g.silence.known)
         t += "silence = ram " + hex(g.silence.address, 4) + " value=" + hex(g.silence.value, 2) + "\n";
      if (!g.note.empty())
         t += "note = " + g.note + "\n";
   }
   write_text(path(), t);
}

bool GameDb::find(uint32_t crc32, GameInfo &out)
{
   std::lock_guard<std::mutex> lock(mutex_);
   load_locked();
   auto it = games_.find(crc32);
   if (it == games_.end())
      return false;
   out = it->second;
   return true;
}

void GameDb::put(const GameInfo &info)
{
   std::lock_guard<std::mutex> lock(mutex_);
   load_locked();
   games_[info.crc32] = info;
   save_locked();
}
