// SPDX-License-Identifier: LGPL-2.1-or-later
#include "reference.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#include "http.h"
#include "platform.h"
#include "zip_read.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static const size_t kSpcRam = 0x100;
static const size_t kSpcMin = 0x10100;

const uint8_t *spc_ram(const std::vector<uint8_t> &spc)
{
   static const char magic[] = "SNES-SPC700 Sound File Data";
   if (spc.size() < kSpcMin || memcmp(spc.data(), magic, sizeof(magic) - 1) != 0)
      return nullptr;
   return spc.data() + kSpcRam;
}

std::string spc_song_title(const std::vector<uint8_t> &spc)
{
   if (!spc_ram(spc) || spc[0x23] != 26)
      return "";
   std::string t((const char*)&spc[0x2E], 32);
   size_t nul = t.find('\0');
   if (nul != std::string::npos)
      t.resize(nul);
   while (!t.empty() && (t.back() == ' ' || (unsigned char)t.back() < 0x20))
      t.pop_back();
   for (char &c : t)
      if ((unsigned char)c < 0x20)
         c = ' ';
   return t;
}

static uint32_t fnv(const uint8_t *p, size_t n)
{
   uint32_t h = 2166136261u;
   for (size_t i = 0; i < n; i++)
      h = (h ^ p[i]) * 16777619u;
   return h;
}

static bool flat_block(const uint8_t *p, size_t n)
{
   for (size_t i = 1; i < n; i++)
      if (p[i] != p[0])
         return false;
   return true;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool is_reference_file(const std::string &name)
{
   std::string e = lower_ext(name);
   return e == "spc" || e == "nsf" || e == "nsfe" || e == "m3u";
}

// The songs of an .nsf or .nsfe: how many, and the titles an .nsfe carries.
static bool nsf_songs(const std::vector<uint8_t> &d, int &count, std::vector<std::string> &titles)
{
   count = 0;
   titles.clear();
   if (d.size() >= 0x80 && !memcmp(d.data(), "NESM\x1A", 5))
   {
      count = d[6];
      return count > 0;
   }
   if (d.size() < 4 || memcmp(d.data(), "NSFE", 4))
      return false;
   // Chunks: size (4, little endian), id (4), data.
   for (size_t pos = 4; pos + 8 <= d.size();)
   {
      uint32_t size = d[pos] | d[pos + 1] << 8 | d[pos + 2] << 16 | (uint32_t)d[pos + 3] << 24;
      const char *id = (const char*)&d[pos + 4];
      size_t body = pos + 8;
      if (body + size > d.size() || !memcmp(id, "NEND", 4))
         break;
      if (!memcmp(id, "INFO", 4) && size >= 9)
         count = d[body + 8];
      else if (!memcmp(id, "tlbl", 4))
         for (size_t at = body; at < body + size;)
         {
            size_t end = at;
            while (end < body + size && d[end])
               end++;
            titles.push_back(std::string((const char*)&d[at], end - at));
            at = end + 1;
         }
      pos = body + size;
   }
   return count > 0;
}

// An extended .m3u line: "file::NSF,track,title,length,loop,fade". Decimal track numbers count
// from 1, "$hex" ones from 0; "\," is a comma inside a field.
static bool m3u_entry(const std::string &line, std::string &file, int &track, std::string &title)
{
   size_t sep = line.find("::");
   if (line.empty() || line[0] == '#' || sep == std::string::npos)
      return false;
   file = line.substr(0, sep);
   size_t comma = line.find(',', sep);
   if (comma == std::string::npos)
      return false;
   std::vector<std::string> fields(1);
   for (size_t j = comma + 1; j < line.size(); j++)
   {
      char c = line[j];
      if (c == '\\' && j + 1 < line.size())
         fields.back() += line[++j];
      else if (c == ',')
         fields.push_back("");
      else if (c != '\r')
         fields.back() += c;
   }
   if (fields[0].empty())
      return false;
   std::string t = fields[0];
   while (!t.empty() && t[0] == ' ')
      t.erase(0, 1);
   track = t[0] == '$' ? (int)strtol(t.c_str() + 1, nullptr, 16) : atoi(t.c_str()) - 1;
   title = fields.size() > 1 ? fields[1] : "";
   while (!title.empty() && (title.back() == ' ' || title.back() == '\t'))
      title.pop_back();
   return track >= 0;
}

bool ReferenceSet::load(const std::string &dir, std::string &error)
{
   clear();
   dir_ = dir;
   if (!dir_exists(dir))
      return true;
   std::vector<std::string> files = list_files(dir);
   std::sort(files.begin(), files.end());
   std::vector<ReferenceSong> songs;
   for (const auto &f : files)
   {
      std::string ext = lower_ext(f);
      if (ext == "spc")
      {
         ReferenceSong s;
         s.path = dir + "\\" + f;
         if (!read_file_bytes(s.path, s.data) || !spc_ram(s.data))
         {
            error = f + " is not an .spc file";
            continue;
         }
         s.title = spc_song_title(s.data);
         if (s.title.empty())
            s.title = stem_of(f);
         songs.push_back(std::move(s));
         continue;
      }
      if (ext != "nsf" && ext != "nsfe")
         continue;
      std::vector<uint8_t> data;
      int count = 0;
      std::vector<std::string> titles;
      if (!read_file_bytes(dir + "\\" + f, data) || !nsf_songs(data, count, titles))
      {
         error = f + " is not an .nsf file";
         continue;
      }
      // A playlist beside it lists the real songs, in order, by name; without one, every song
      // the file holds is listed.
      std::vector<std::pair<int, std::string>> list;
      for (const auto &m : files)
      {
         if (lower_ext(m) != "m3u")
            continue;
         std::string text = read_text(dir + "\\" + m), line, file, title;
         bool mine = stem_of(m) == stem_of(f);
         for (size_t pos = 0; pos < text.size();)
         {
            size_t end = text.find('\n', pos);
            line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            int track;
            if (m3u_entry(line, file, track, title) && (mine || file == f) && track < count)
               list.push_back({ track, title });
         }
         if (!list.empty())
            break;
      }
      if (list.empty())
         for (int t = 0; t < count; t++)
            list.push_back({ t, t < (int)titles.size() ? titles[t] : "" });
      for (auto &entry : list)
      {
         ReferenceSong s;
         s.path = dir + "\\" + f;
         s.data = data;
         s.track = entry.first;
         s.title = !entry.second.empty() ? entry.second : stem_of(f) + " #" + std::to_string(entry.first + 1);
         songs.push_back(std::move(s));
      }
   }
   std::string keep = dir_;
   assign(std::move(songs));
   dir_ = keep;
   return true;
}

void ReferenceSet::assign(std::vector<ReferenceSong> songs)
{
   clear();
   songs_ = std::move(songs);
   index();
}

void ReferenceSet::clear()
{
   dir_.clear();
   songs_.clear();
   hashes_.clear();
   share_.clear();
   content_.clear();
}

void ReferenceSet::index()
{
   size_t n = songs_.size();
   hashes_.assign(n, std::vector<uint32_t>(kBlocks));
   share_.assign(n, std::vector<uint16_t>(kBlocks));
   // Songs that are not .spc files have no sound CPU RAM; they match by their notes alone.
   for (size_t i = 0; i < n; i++)
   {
      const uint8_t *ram = spc_ram(songs_[i].data);
      for (size_t b = 0; ram && b < kBlocks; b++)
         hashes_[i][b] = fnv(ram + b * kBlock, kBlock);
   }
   for (size_t b = 0; b < kBlocks; b++)
   {
      std::unordered_map<uint32_t, uint16_t> count;
      for (size_t i = 0; i < n; i++)
         count[hashes_[i][b]]++;
      for (size_t i = 0; i < n; i++)
         share_[i][b] = count[hashes_[i][b]];
   }
   content_.clear();
   content_.reserve(n * 0x10000);
   for (size_t i = 0; i < n; i++)
   {
      const uint8_t *ram = spc_ram(songs_[i].data);
      for (size_t o = 0; ram && o + kBlock <= 0x10000; o++)
         if (!flat_block(ram + o, kBlock))
            content_.push_back((uint64_t)fnv(ram + o, kBlock) << 16 | i);
   }
   std::sort(content_.begin(), content_.end());
   content_.erase(std::unique(content_.begin(), content_.end()), content_.end());
}

int ReferenceSet::content_share(uint32_t hash) const
{
   auto lo = std::lower_bound(content_.begin(), content_.end(), (uint64_t)hash << 16);
   auto hi = std::lower_bound(lo, content_.end(), ((uint64_t)hash + 1) << 16);
   return std::max<int>(1, (int)(hi - lo));
}

// ---------------------------------------------------------------------------
// Matching rips
// ---------------------------------------------------------------------------

ReferenceSet::Match ReferenceSet::match(const uint8_t *ram, const uint8_t *before) const
{
   Match m;
   if (!ram || songs_.empty())
      return m;
   // Data many songs hold somewhere (instrument samples, which load wherever there is room)
   // says little; song data only its own song holds (and songs dumped while it lingered) says
   // much. Weighting by the square of how many songs hold it keeps the rare data in charge.
   std::vector<double> score(songs_.size(), 0.0);
   for (size_t b = 0; b < kBlocks; b++)
   {
      const uint8_t *p = ram + b * kBlock;
      if (flat_block(p, kBlock))
         continue;
      if (before && memcmp(p, before + b * kBlock, kBlock) == 0)
         continue;
      uint32_t h = fnv(p, kBlock);
      int holders = -1;
      for (size_t i = 0; i < songs_.size(); i++)
         if (hashes_[i][b] == h)
         {
            if (holders < 0)
               holders = content_share(h);
            score[i] += 1.0 / ((double)holders * holders);
         }
   }
   const double kEps = 1e-9;
   for (size_t i = 0; i < songs_.size(); i++)
   {
      if (m.index < 0 || score[i] > m.score + kEps)
      {
         m.index = (int)i;
         m.score = score[i];
      }
      // Versions of one song score the same; the plainest title names it.
      else if (score[i] > m.score - kEps && songs_[i].title.size() < songs_[m.index].title.size())
         m.index = (int)i;
   }
   for (size_t i = 0; i < songs_.size(); i++)
      if (score[i] < m.score - kEps)
         m.second = std::max(m.second, score[i]);
   // A few blocks of song data that only this song has.
   if (m.score < 2.0)
   {
      m.index = -1;
      return m;
   }
   for (size_t i = 0; i < songs_.size(); i++)
      if (score[i] >= m.score * 0.9)
         m.close.push_back((int)i);
   return m;
}

ReferenceSet::Match ReferenceSet::match_spc(const std::vector<uint8_t> &spc, const std::vector<uint8_t> *before) const
{
   return match(spc_ram(spc), before ? spc_ram(*before) : nullptr);
}

// ---------------------------------------------------------------------------
// Song tables in the ROM
// ---------------------------------------------------------------------------

SongTable ReferenceSet::find_song_table(const SnesRom &rom) const
{
   SongTable table;
   const std::vector<uint8_t> &d = rom.data;
   const size_t n = songs_.size();
   if (n == 0 || d.size() < 0x10000)
      return table;
   for (const auto &s : songs_)
      if (!spc_ram(s.data))
         return table;

   // Index every 12-byte window of the ROM.
   const size_t K = 12;
   std::vector<std::pair<uint32_t, uint32_t>> windows;
   windows.reserve(d.size());
   for (size_t i = 0; i + K <= d.size(); i++)
   {
      if (d[i] == d[i + 1] && d[i] == d[i + 4] && d[i] == d[i + 9])
         continue;
      windows.push_back({ fnv(&d[i], K), (uint32_t)i });
   }
   std::sort(windows.begin(), windows.end());

   // Where each reference's own song data (RAM blocks at most two references share)
   // starts in the ROM, and every CPU address a pointer to it, or a few bytes before it
   // (a length or header), could hold.
   struct Want
   {
      uint16_t ref;
      uint8_t back;
      uint32_t target;   // ROM offset
   };
   std::unordered_map<uint32_t, std::vector<Want>> want3, want2;
   for (size_t r = 0; r < n; r++)
   {
      const uint8_t *ram = spc_ram(songs_[r].data);
      std::set<size_t> starts;
      for (size_t a = 0; a + K <= 0x10000;)
      {
         size_t b = a / kBlock;
         if (share_[r][b] > 2 || flat_block(ram + b * kBlock, kBlock))
         {
            a = (b + 1) * kBlock;
            continue;
         }
         uint32_t h = fnv(ram + a, K);
         auto range = std::equal_range(windows.begin(), windows.end(), std::make_pair(h, (uint32_t)0),
               [](const std::pair<uint32_t, uint32_t> &x, const std::pair<uint32_t, uint32_t> &y) { return x.first < y.first; });
         size_t best_len = 0, best_at = 0, tried = 0;
         for (auto it = range.first; it != range.second && tried < 8; ++it, tried++)
         {
            size_t o = it->second, len = 0;
            while (a + len < 0x10000 && o + len < d.size() && ram[a + len] == d[o + len])
               len++;
            if (len > best_len)
            {
               best_len = len;
               best_at = o;
            }
         }
         if (best_len >= 32)
         {
            starts.insert(best_at);
            a += best_len;
         }
         else
            a++;
      }
      for (size_t s : starts)
         for (uint8_t back = 0; back <= 4 && back <= s; back++)
            for (uint32_t cpu : rom.cpu_addresses(s - back))
            {
               Want w{ (uint16_t)r, back, (uint32_t)(s - back) };
               want3[cpu & 0xFFFFFF].push_back(w);
               auto &list2 = want2[cpu & 0xFFFF];
               bool dup = false;
               for (const auto &x : list2)
                  dup = dup || (x.ref == w.ref && x.target == w.target);
               if (!dup)
                  list2.push_back(w);
            }
   }

   struct Hit
   {
      uint32_t at;
      Want want;
      uint8_t bank;
   };
   std::vector<Hit> hits[2];   // [0]: 2-byte pointers, [1]: 3-byte
   for (size_t i = 0; i + 3 <= d.size(); i++)
   {
      uint32_t v2 = d[i] | d[i + 1] << 8;
      auto w2 = want2.find(v2);
      if (w2 != want2.end())
         for (const auto &w : w2->second)
            hits[0].push_back({ (uint32_t)i, w, 0 });
      auto w3 = want3.find(v2 | (uint32_t)d[i + 2] << 16);
      if (w3 != want3.end())
         for (const auto &w : w3->second)
            hits[1].push_back({ (uint32_t)i, w, d[i + 2] });
   }

   // A table: aligned pointers close together, many of them to exactly one song.
   struct Cluster
   {
      int width = 0;
      double score = 0;
      std::vector<Hit> hits;
   } best;
   for (int wi = 1; wi >= 0; wi--)
   {
      int width = wi + 2;
      for (int mod = 0; mod < width; mod++)
      {
         std::vector<Hit> aligned;
         for (const auto &h : hits[wi])
            if ((int)(h.at % width) == mod)
               aligned.push_back(h);
         for (size_t i = 0; i < aligned.size();)
         {
            size_t j = i;
            while (j + 1 < aligned.size() && aligned[j + 1].at - aligned[j].at <= (uint32_t)(8 * width))
               j++;
            std::map<uint32_t, std::set<int>> at_refs;
            for (size_t k = i; k <= j; k++)
               at_refs[aligned[k].at].insert(aligned[k].want.ref);
            std::set<int> unique;
            for (const auto &e : at_refs)
               if (e.second.size() == 1)
                  unique.insert(*e.second.begin());
            double score = unique.size() * (width == 3 ? 1.0 : 0.9);
            if (score > best.score)
            {
               best.width = width;
               best.score = score;
               best.hits.assign(aligned.begin() + i, aligned.begin() + j + 1);
            }
            i = j + 1;
         }
      }
   }
   if (best.score < std::max(4.0, 0.4 * n))
      return table;

   // The header bytes before song data are the same for every song: keep the pointers
   // that agree with most of them.
   int back_votes[5] = { 0 };
   std::map<int, int> bank_votes;
   for (const auto &h : best.hits)
   {
      back_votes[h.want.back]++;
      if (best.width == 3)
         bank_votes[h.bank]++;
   }
   int back = (int)(std::max_element(back_votes, back_votes + 5) - back_votes);
   std::vector<Hit> kept;
   size_t lo_target = d.size(), hi_target = 0;
   for (const auto &h : best.hits)
      if (h.want.back == back)
      {
         kept.push_back(h);
         lo_target = std::min<size_t>(lo_target, h.want.target);
         hi_target = std::max<size_t>(hi_target, h.want.target);
      }
   if (kept.empty())
      return table;
   const int width = best.width;
   size_t first = kept.front().at, last = kept.back().at;
   uint32_t bank2 = 0;
   if (width == 2)
   {
      // Two-byte pointers stay in the bank of the song data.
      std::vector<uint32_t> cpus = rom.cpu_addresses(kept.front().want.target);
      bank2 = cpus.empty() ? 0 : cpus.front() & 0xFF0000;
   }

   // An entry is a pointer into the region the songs occupy.
   auto entry_target = [&](size_t at, size_t &target) {
      if (at + width > d.size())
         return false;
      uint32_t cpu = width == 3 ? (uint32_t)(d[at] | d[at + 1] << 8 | d[at + 2] << 16) : bank2 | d[at] | d[at + 1] << 8;
      const uint8_t *p = rom.at(cpu);
      if (!p)
         return false;
      target = (size_t)(p - d.data());
      const size_t margin = 0x20000;
      return target + margin >= lo_target && target <= hi_target + margin;
   };

   // Entry 0: where the game's code reads the table (LDA/CMP long, or long indexed), or
   // failing that, the first of the valid pointers before the first song found.
   // CPU address -> (entry, byte within it)
   std::map<uint32_t, std::pair<size_t, int>> code_targets;
   for (int k = 0; k <= 16 && (size_t)(k * width) <= first; k++)
   {
      size_t t = first - k * width;
      for (int j = 0; j < width; j++)
         for (uint32_t cpu : rom.cpu_addresses(t + j))
            code_targets[cpu] = { t, j };
   }
   size_t start = first;
   uint32_t start_cpu = 0;
   for (size_t i = 0; i + 4 <= d.size(); i++)
   {
      uint8_t op = d[i];
      if (op != 0xAF && op != 0xBF && op != 0xCF && op != 0xDF)
         continue;
      uint32_t operand = d[i + 1] | d[i + 2] << 8 | (uint32_t)d[i + 3] << 16;
      auto ref = code_targets.find(operand);
      if (ref == code_targets.end())
         continue;
      size_t t = ref->second.first;
      bool valid = true;
      size_t target;
      for (size_t e = t; e < first && valid; e += width)
         valid = entry_target(e, target);
      if (valid && (t < start || !start_cpu))
      {
         start = t;
         start_cpu = operand - (uint32_t)ref->second.second;
      }
   }
   if (!start_cpu)
   {
      size_t target;
      for (int k = 0; k < 16 && start >= (size_t)width && entry_target(start - width, target); k++)
         start -= width;
   }
   size_t end = last + width;
   {
      size_t target;
      while (end + width <= d.size() && (end - start) / width < 512 && entry_target(end, target))
         end += width;
   }

   table.found = true;
   table.rom_offset = start;
   table.cpu_address = start_cpu;
   table.width = width;
   table.entries.assign((end - start) / width, -1);
   table.versions.assign(table.entries.size(), std::vector<int>());
   std::set<int> matched;
   for (const auto &h : kept)
   {
      if (h.at < start || h.at >= end)
         continue;
      std::vector<int> &v = table.versions[(h.at - start) / width];
      if (std::find(v.begin(), v.end(), (int)h.want.ref) == v.end())
         v.push_back(h.want.ref);
      int &e = table.entries[(h.at - start) / width];
      if (e < 0 || songs_[h.want.ref].title.size() < songs_[e].title.size())
         e = h.want.ref;
   }
   for (int e : table.entries)
      if (e >= 0)
         matched.insert(e);
   table.matched = (int)matched.size();
   return table;
}

// ---------------------------------------------------------------------------
// Importing and downloading
// ---------------------------------------------------------------------------

static bool write_bytes(const std::string &path, const std::vector<uint8_t> &data)
{
   std::string text((const char*)data.data(), data.size());
   return write_text(path, text);
}

static int import_zip(const std::vector<uint8_t> &zip, const std::string &dir, std::string &error)
{
   int count = 0;
   make_dirs(dir);
   bool ok = zip_read(zip, [](const std::string &name) { return is_reference_file(name); },
         [&](const std::string &name, std::vector<uint8_t> &data) {
            int songs = 0;
            std::vector<std::string> titles;
            bool playlist = lower_ext(name) == "m3u";
            if ((playlist || spc_ram(data) || nsf_songs(data, songs, titles)) &&
                  write_bytes(dir + "\\" + sanitize_filename(file_name(name)), data) && !playlist)
               count++;
            return true;
         }, error);
   return ok ? count : -1;
}

// ---------------------------------------------------------------------------
// RAR and 7z archives, through 7-Zip
// ---------------------------------------------------------------------------

std::string find_7zip()
{
#ifdef _WIN32
   char found[MAX_PATH];
   if (SearchPathA(nullptr, "7z.exe", nullptr, sizeof(found), found, nullptr))
      return found;
   for (const char *var : { "ProgramW6432", "ProgramFiles", "ProgramFiles(x86)" })
   {
      const char *base = getenv(var);
      if (base && file_exists(std::string(base) + "\\7-Zip\\7z.exe"))
         return std::string(base) + "\\7-Zip\\7z.exe";
   }
#endif
   return "";
}

#ifdef _WIN32
static std::wstring widen_utf8(const std::string &s)
{
   int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
   std::wstring w(n > 0 ? n - 1 : 0, L'\0');
   if (n > 1)
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
   return w;
}

static std::string temp_folder()
{
   wchar_t base[MAX_PATH];
   GetTempPathW(MAX_PATH, base);
   char name[64];
   snprintf(name, sizeof(name), "proteus_refs_%lu_%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
   int n = WideCharToMultiByte(CP_UTF8, 0, base, -1, nullptr, 0, nullptr, nullptr);
   std::string dir(n > 0 ? n - 1 : 0, '\0');
   if (n > 1)
      WideCharToMultiByte(CP_UTF8, 0, base, -1, &dir[0], n, nullptr, nullptr);
   return dir + name;
}

static void remove_folder(const std::string &dir)
{
   for (const auto &f : list_files(dir))
      DeleteFileW(widen_utf8(dir + "\\" + f).c_str());
   RemoveDirectoryW(widen_utf8(dir).c_str());
}
#endif

// Extracts the reference files of any archive 7-Zip opens (SNESmusic.org's .rsn sets are RAR) into `dir`.
static int import_with_7zip(const std::string &archive, const std::string &dir, std::string &error)
{
#ifdef _WIN32
   std::string seven = find_7zip();
   if (seven.empty())
   {
      error = file_name(archive) + " needs 7-Zip to open. Install 7-Zip (7-zip.org), or extract it yourself and import the folder.";
      return -1;
   }
   std::string tmp = temp_folder();
   make_dirs(tmp);
   // e: flat, -y: no prompts, -r: files in subfolders too
   std::wstring cmd = L"\"" + widen_utf8(seven) + L"\" e -y -r \"-o" + widen_utf8(tmp) + L"\" \"" +
         widen_utf8(archive) + L"\" *.spc *.nsf *.nsfe *.m3u";
   STARTUPINFOW si{};
   si.cb = sizeof(si);
   PROCESS_INFORMATION pi{};
   std::vector<wchar_t> line(cmd.begin(), cmd.end());
   line.push_back(0);
   if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
   {
      error = "could not run 7-Zip (error " + std::to_string(GetLastError()) + ")";
      remove_folder(tmp);
      return -1;
   }
   WaitForSingleObject(pi.hProcess, 120000);
   DWORD code = 1;
   GetExitCodeProcess(pi.hProcess, &code);
   CloseHandle(pi.hProcess);
   CloseHandle(pi.hThread);
   int count = 0;
   if (code != 0)
      error = "7-Zip could not extract " + file_name(archive) + " (exit code " + std::to_string(code) + ")";
   else
   {
      make_dirs(dir);
      for (const auto &f : list_files(tmp))
         if (is_reference_file(f) && copy_file_data(tmp + "\\" + f, dir + "\\" + f) && lower_ext(f) != "m3u")
            count++;
   }
   remove_folder(tmp);
   return code == 0 ? count : -1;
#else
   (void)dir;
   error = file_name(archive) + " needs 7-Zip to open; extract it yourself and import the folder.";
   return -1;
#endif
}

int import_reference_songs(const std::string &source, const std::string &dir, std::string &error)
{
   int count = 0;
   std::string ext = lower_ext(source);
   auto is_archive = [](const std::string &e) { return e == "rsn" || e == "rar" || e == "7z"; };
   if (dir_exists(source))
   {
      make_dirs(dir);
      for (const auto &f : list_files(source))
      {
         std::string from = source + "\\" + f, e = lower_ext(f);
         if (e == "zip")
         {
            std::vector<uint8_t> zip;
            int n = read_file_bytes(from, zip) ? import_zip(zip, dir, error) : -1;
            count += std::max(0, n);
         }
         else if (is_archive(e))
            count += std::max(0, import_with_7zip(from, dir, error));
         else if (is_reference_file(f) && copy_file_data(from, dir + "\\" + f) && e != "m3u")
            count++;
      }
   }
   else if (ext == "zip")
   {
      std::vector<uint8_t> zip;
      if (!read_file_bytes(source, zip))
      {
         error = "cannot read " + source;
         return -1;
      }
      count = import_zip(zip, dir, error);
      if (count < 0)
         return -1;
   }
   else if (ext == "spc" || ext == "nsf" || ext == "nsfe")
   {
      make_dirs(dir);
      if (copy_file_data(source, dir + "\\" + file_name(source)))
         count = 1;
      // The playlist that names an .nsf's songs sits beside it.
      std::string m3u = dir_of(source) + "\\" + stem_of(source) + ".m3u";
      if (ext != "spc" && file_exists(m3u))
         copy_file_data(m3u, dir + "\\" + file_name(m3u));
   }
   else if (is_archive(ext))
   {
      count = import_with_7zip(source, dir, error);
      if (count < 0)
         return -1;
   }
   else
   {
      error = "choose a folder, a .zip, .rsn, .rar or .7z archive, or .spc or .nsf files";
      return -1;
   }
   if (count == 0 && error.empty())
      error = "no .spc or .nsf files in " + file_name(source);
   return count;
}

// ---------------------------------------------------------------------------
// Finding a game's set by name
// ---------------------------------------------------------------------------

// "Addams Family, The (USA) [!]" -> "Addams Family, The"
static std::string plain_name(const std::string &name)
{
   std::string out;
   int depth = 0;
   for (char c : name)
   {
      if (c == '(' || c == '[')
         depth++;
      else if ((c == ')' || c == ']') && depth > 0)
         depth--;
      else if (depth == 0)
         out += c;
   }
   while (!out.empty() && out.back() == ' ')
      out.pop_back();
   return out;
}

std::string zophar_slug(const std::string &game_name)
{
   std::string slug;
   for (char c : plain_name(game_name))
   {
      unsigned char u = (unsigned char)c;
      if (c == '\'')
         continue;
      if (isalnum(u))
         slug += (char)tolower(u);
      else if (!slug.empty() && slug.back() != '-')
         slug += '-';
   }
   while (!slug.empty() && slug.back() == '-')
      slug.pop_back();
   return slug;
}

// The words that identify a game: lowercase, without punctuation or articles.
static std::vector<std::string> name_words(const std::string &name)
{
   static const char *kSkip[] = { "the", "a", "an", "of", "and", "to", "in", "no" };
   std::vector<std::string> words;
   std::string w;
   std::string text = plain_name(name) + " ";
   for (char c : text)
   {
      unsigned char u = (unsigned char)c;
      if (c == '\'')
         continue;
      if (isalnum(u))
         w += (char)tolower(u);
      else if (!w.empty())
      {
         // "Mega Man II" is "Mega Man 2".
         static const char *kRoman[] = { "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x" };
         for (int r = 0; r < 9; r++)
            if (w == kRoman[r])
               w = std::to_string(r + 2);
         bool skip = false;
         for (const char *s : kSkip)
            skip = skip || w == s;
         if (!skip && std::find(words.begin(), words.end(), w) == words.end())
            words.push_back(w);
         w.clear();
      }
   }
   return words;
}

// How alike two names are, 0..1: shared words over all words.
double game_name_similarity(const std::string &a, const std::string &b)
{
   std::vector<std::string> x = name_words(a), y = name_words(b);
   if (x.empty() || y.empty())
      return 0;
   size_t common = 0;
   for (const auto &w : x)
      common += std::find(y.begin(), y.end(), w) != y.end();
   return (double)common / (double)(x.size() + y.size() - common);
}

// Spellings archives use for the same name: "Legend of Zelda - A Link to the Past, The" is
// "Legend of Zelda, The - A Link to the Past" and "The Legend of Zelda - A Link to the Past".
static std::vector<std::string> name_variants(const std::vector<std::string> &names)
{
   std::vector<std::string> out;
   auto add = [&](const std::string &n) {
      if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end())
         out.push_back(n);
   };
   for (const auto &raw : names)
   {
      std::string n = plain_name(raw);
      add(n);
      const std::string suffix = ", The";
      std::string base = n;
      if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
      {
         base = n.substr(0, n.size() - suffix.size());
         size_t dash = base.find(" - ");
         if (dash == std::string::npos)
            dash = base.find(": ");
         if (dash != std::string::npos)
            add(base.substr(0, dash) + ", The" + base.substr(dash));
         add("The " + base);
         add(base);
      }
      else if (n.compare(0, 4, "The ") == 0)
      {
         base = n.substr(4);
         size_t dash = base.find(" - ");
         if (dash == std::string::npos)
            dash = base.find(": ");
         add(dash != std::string::npos ? base.substr(0, dash) + ", The" + base.substr(dash) : base + ", The");
         add(base);
      }
      // "Name: Subtitle" and "Name - Subtitle" are written both ways.
      for (const std::string &sep : { std::string(": "), std::string(" - ") })
      {
         size_t at = base.find(sep);
         if (at != std::string::npos)
            add(base.substr(0, at) + (sep == ": " ? " - " : ": ") + base.substr(at + sep.size()));
      }
   }
   return out;
}

static std::string html_text(std::string s)
{
   const std::pair<const char *, const char *> entities[] = { { "&amp;", "&" }, { "&#039;", "'" }, { "&#39;", "'" }, { "&quot;", "\"" } };
   for (const auto &e : entities)
      for (size_t at; (at = s.find(e.first)) != std::string::npos;)
         s.replace(at, strlen(e.first), e.second);
   return s;
}

static std::string percent_decode(const std::string &s)
{
   std::string out;
   for (size_t i = 0; i < s.size(); i++)
   {
      if (s[i] == '%' && i + 2 < s.size())
      {
         out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
         i += 2;
      }
      else
         out += s[i];
   }
   return out;
}

static const double kNameMatch = 0.75;

// ---- Zophar's Domain: zips at /music/nintendo-snes-spc/<slug> and /music/nintendo-nes-nsf/<slug>

// The link to the page's emulated-format (.spc, .nsf) archive.
static std::string find_emu_zip(const std::string &html)
{
   size_t pos = 0;
   while ((pos = html.find(".zophar.zip", pos)) != std::string::npos)
   {
      size_t href = html.rfind("href=\"", pos);
      size_t endq = html.find('"', pos);
      if (href != std::string::npos && endq != std::string::npos)
      {
         std::string link = html.substr(href + 6, endq - href - 6);
         if (link.find("%28EMU%29") != std::string::npos || link.find("(EMU)") != std::string::npos)
            return link;
      }
      pos++;
   }
   return "";
}

static int download_from_zophar(const std::vector<std::string> &variants, const std::string &dir,
      const std::function<void(const std::string &)> &progress, std::string &error, bool nes)
{
   const std::string site = "https://www.zophar.net", section = nes ? "/music/nintendo-nes-nsf/" : "/music/nintendo-snes-spc/";
   const std::string system = nes ? "NES" : "SNES", format = nes ? ".nsf" : ".spc";
   std::string html, err, link, page_url;
   progress("Looking for " + variants.front() + " on Zophar's Domain...");
   for (const auto &v : variants)
   {
      page_url = site + section + zophar_slug(v);
      if (http_fetch(page_url, "", html, err) && !(link = find_emu_zip(html)).empty())
         break;
   }
   if (link.empty())
   {
      // Search with the identifying words, and with each part of a "Name - Subtitle" name,
      // then take the result whose name is most like the game's.
      std::vector<std::string> queries;
      for (const auto &v : variants)
      {
         std::string q;
         for (const auto &w : name_words(v))
            q += (q.empty() ? "" : " ") + w;
         if (std::find(queries.begin(), queries.end(), q) == queries.end())
            queries.push_back(q);
      }
      double best = 0;
      std::string best_url;
      for (size_t qi = 0; qi < queries.size() && qi < 4 && best < 0.99; qi++)
      {
         std::string results;
         if (!http_fetch(site + "/search?search=" + url_encode(queries[qi]), "", results, err))
            continue;
         for (size_t at = 0; (at = results.find(section, at)) != std::string::npos; at += section.size())
         {
            size_t href = results.rfind("href=\"", at), endq = results.find('"', at);
            if (href == std::string::npos || endq == std::string::npos || href + 6 > at)
               continue;
            std::string url = results.substr(href + 6, endq - href - 6);
            std::string slug = url.substr(url.find(section) + section.size());
            if (slug.size() > 5 && slug.compare(slug.size() - 5, 5, ".html") == 0)
               slug.resize(slug.size() - 5);
            std::string spaced = slug;
            std::replace(spaced.begin(), spaced.end(), '-', ' ');
            double score = 0;
            for (const auto &v : variants)
               score = std::max(score, game_name_similarity(v, spaced));
            if (score > best)
            {
               best = score;
               best_url = url[0] == '/' ? site + url : url;
            }
         }
      }
      if (best < kNameMatch)
      {
         error = "Zophar's Domain has no " + system + " soundtrack named like \"" + variants.front() + "\"";
         return -1;
      }
      page_url = best_url;
      progress("Opening " + page_url + "...");
      if (!http_fetch(page_url, "", html, err) || (link = find_emu_zip(html)).empty())
      {
         error = "no " + format + " archive on " + page_url + (err.empty() ? "" : ": " + err);
         return -1;
      }
   }
   if (link.compare(0, 2, "//") == 0)
      link = "https:" + link;
   else if (link[0] == '/')
      link = site + link;
   progress("Downloading " + percent_decode(link.substr(link.rfind('/') + 1)) + " from Zophar's Domain...");
   std::string body;
   if (!http_fetch(link, "", body, err))
   {
      error = "download from Zophar's Domain failed: " + err;
      return -1;
   }
   std::vector<uint8_t> zip(body.begin(), body.end());
   int count = import_zip(zip, dir, error);
   if (count == 0 && error.empty())
      error = "the archive from " + page_url + " has no " + format + " files";
   return count;
}

// ---- SNESmusic.org: sets listed by first letter, .rsn (RAR) archives

static int download_from_snesmusic(const std::vector<std::string> &variants, const std::string &dir,
      const std::function<void(const std::string &)> &progress, std::string &error)
{
   const std::string site = "https://www.snesmusic.org/v2/";
   if (find_7zip().empty())
   {
      error = "SNESmusic.org sets need 7-Zip to open; install 7-Zip (7-zip.org)";
      return -1;
   }
   progress("Looking for " + variants.front() + " on SNESmusic.org...");
   std::vector<std::string> letters;
   for (const auto &v : variants)
   {
      std::string n = plain_name(v);
      if (n.compare(0, 4, "The ") == 0)
         n = n.substr(4);
      if (n.empty())
         continue;
      unsigned char c = (unsigned char)n[0];
      std::string letter = isdigit(c) ? "n1-9" : std::string(1, (char)toupper(c));
      if (isalnum(c) && std::find(letters.begin(), letters.end(), letter) == letters.end())
         letters.push_back(letter);
   }
   double best = 0;
   std::string best_id, best_name, err;
   // Each letter's list comes 30 sets a page (limit=0, 30, 60...).
   for (size_t li = 0; li < letters.size() && best < 0.99; li++)
   for (int page = 0; page < 80 && best < 0.99; page++)
   {
      const std::string &letter = letters[li];
      std::string list;
      if (!http_fetch(site + "select.php?view=sets&char=" + letter + "&limit=" + std::to_string(page * 30), "", list, err))
         break;
      // <a href='profile.php?profile=set&amp;selected=1494'>Legend of Zelda: A Link to the Past</a>
      const std::string key = "profile=set&amp;selected=";
      if (list.find(key) == std::string::npos)
         break;
      // A page past the end repeats nothing new: stop when the next page link is missing.
      bool more = list.find("limit=" + std::to_string((page + 1) * 30)) != std::string::npos;
      for (size_t at = 0; (at = list.find(key, at)) != std::string::npos; at += key.size())
      {
         size_t id_end = list.find('\'', at);
         size_t name_end = id_end == std::string::npos ? id_end : list.find("</a>", id_end);
         if (name_end == std::string::npos)
            continue;
         std::string id = list.substr(at + key.size(), id_end - at - key.size());
         std::string name = html_text(list.substr(id_end + 2, name_end - id_end - 2));
         double score = 0;
         for (const auto &v : variants)
            score = std::max(score, game_name_similarity(v, name));
         if (score > best)
         {
            best = score;
            best_id = id;
            best_name = name;
         }
      }
      if (!more)
         break;
   }
   if (best < kNameMatch)
   {
      error = err.empty() ? "SNESmusic.org has no set named like \"" + variants.front() + "\"" : "SNESmusic.org: " + err;
      return -1;
   }
   std::string profile;
   if (!http_fetch(site + "profile.php?profile=set&selected=" + best_id, "", profile, err))
   {
      error = "SNESmusic.org: " + err;
      return -1;
   }
   const std::string dl = "download.php?spcNow=";
   size_t at = profile.find(dl);
   if (at == std::string::npos)
   {
      error = "SNESmusic.org has no download for " + best_name;
      return -1;
   }
   size_t end = at + dl.size();
   while (end < profile.size() && (isalnum((unsigned char)profile[end]) || profile[end] == '_' || profile[end] == '-'))
      end++;
   std::string url = site + profile.substr(at, end - at);
   progress("Downloading " + best_name + " from SNESmusic.org...");
   std::string body;
   if (!http_fetch(url, "", body, err))
   {
      error = "download from SNESmusic.org failed: " + err;
      return -1;
   }
#ifdef _WIN32
   std::string tmp = temp_folder();
   make_dirs(tmp);
   std::string archive = tmp + "\\set.rsn";
   write_text(archive, body);
   int count = import_with_7zip(archive, dir, error);
   DeleteFileW(widen_utf8(archive).c_str());
   RemoveDirectoryW(widen_utf8(tmp).c_str());
   if (count == 0 && error.empty())
      error = "the SNESmusic.org set for " + best_name + " has no .spc files";
   return count;
#else
   error = "SNESmusic.org downloads are supported on Windows";
   return -1;
#endif
}

int download_reference_songs(const std::vector<std::string> &names, const std::string &dir,
      const std::function<void(const std::string &)> &progress, std::string &error, int sources)
{
   std::vector<std::string> variants = name_variants(names);
   if (variants.empty())
   {
      error = "the game has no name to look for";
      return -1;
   }
   std::string errors;
   for (int nes = 0; nes < 2; nes++)
      if (sources & (nes ? REFERENCES_ZOPHAR_NES : REFERENCES_ZOPHAR))
      {
         std::string e;
         int count = download_from_zophar(variants, dir, progress, e, nes != 0);
         if (count > 0)
            return count;
         errors += (errors.empty() ? "" : "; ") + e;
      }
   if (sources & REFERENCES_SNESMUSIC)
   {
      std::string e;
      int count = download_from_snesmusic(variants, dir, progress, e);
      if (count > 0)
         return count;
      errors += (errors.empty() ? "" : "; ") + e;
   }
   error = errors + ". Download a set yourself and import it.";
   return -1;
}
