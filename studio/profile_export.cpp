// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_export.h"

#include <cmath>
#include <cstdio>
#include <memory>

#include "platform.h"

extern "C" {
#include "profile.h"
}

static const char *kMemoryNames[] = { "system_ram", "save_ram", "video_ram" };

static std::string hex(uint32_t v, int digits)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "0x%0*X", digits, (unsigned)v);
   return buf;
}

std::string profile_path_for(const RomSession &target, const std::string &system_dir)
{
   if (!target.profile_path().empty())
      return target.profile_path();
   return system_dir + "\\proteus\\" + target.game_name() + ".ini";
}

std::string profile_text(RomSession &target, const Assignments &assignments, const ProfileOptions &options,
      const std::string &profile_path, std::vector<std::pair<std::string, std::string>> *copy_plan)
{
   const SongAddress &a = target.address;
   std::string profile_dir = dir_of(profile_path);
   std::string music_rel = "music/" + sanitize_filename(target.game_name());

   std::string t;
   t += "; Proteus Retune profile for " + target.game_name() + "\n; Made with Proteus Studio\n\n[song]\n";
   t += std::string("memory   = ") + kMemoryNames[a.memory] + "\n";
   t += "address  = " + hex(a.address, 4) + "\n";
   if (!a.bytes.empty())
      t += "bytes    = " + format_song_pattern(a) + "\n";
   else
      t += "size     = " + std::to_string(a.size) + "\n";
   t += "debounce = " + std::to_string(a.debounce) + "\n";
   if (a.latch)
      t += "latch    = 1\n";
   if (a.events)
      t += "events   = " + hex(a.events_address, 4) + "   ; jingles: song 0x100 + command\n";
   // A tap reports sound effects too: values not listed leave the music alone.
   bool tapped = !target.tap.empty();
   t += tapped ? "unmapped = keep       ; requests not listed are sound effects\n" : "unmapped = original\n";

   if (target.silence.known)
      t += "\n[silence]\n; stops the game's own music, keeping its sound effects\naddress  = " +
           hex(target.silence.address, 4) + "\nvalue    = " + hex(target.silence.value, 2) + "\n";
   // Cheat codes by core; a core's codes are long, so eight go on each line (Proteus joins them).
   auto codes = [&](const char *section, const char *comment, const CoreCodes &by_core) {
      if (by_core.empty())
         return;
      t += std::string("\n[") + section + "]\n; " + comment + "\n";
      for (auto &c : by_core)
      {
         std::string line;
         int n = 0;
         for (size_t start = 0; start <= c.second.size();)
         {
            size_t plus = c.second.find('+', start);
            if (plus == std::string::npos)
               plus = c.second.size();
            line += (n ? "+" : "") + c.second.substr(start, plus - start);
            start = plus + 1;
            if (++n == 8 || start > c.second.size())
            {
               t += c.first + " = " + line + "\n";
               line.clear();
               n = 0;
            }
         }
      }
   };
   codes("tap", "makes the game write each sound it requests (+ 1) to the song address", target.tap);
   codes("patch", "stops the game's own music code while a replacement plays, keeping its sound effects", target.patch);
   if (!options.mute.empty())
   {
      t += "\n[mute]\n";
      for (auto &m : options.mute)
         t += m.first + " = " + m.second + "\n";
   }
   t += "\n[mix]\nmusic_volume = " + std::to_string(options.music_volume) +
        "\ngame_volume  = " + std::to_string(options.game_volume) +
        "\ncrossfade_ms = " + std::to_string(options.crossfade_ms) + "\n";

   std::map<uint32_t, std::string> titles;
   {
      std::lock_guard<std::mutex> lock(target.songs_mutex);
      for (auto &s : target.songs)
         if (s.has_value)
            titles[s.value] = s.title;
   }

   t += "\n[tracks]\n";
   for (auto &entry : assignments)
   {
      const Assignment &as = entry.second;
      if (as.kind == Assignment::ORIGINAL)
         continue;
      std::string spec;
      if (as.kind == Assignment::SILENCE)
         spec = "silence";
      else
      {
         std::string file = as.path;
         if (copy_plan)
         {
            std::string rel = music_rel + "/" + hex(entry.first, 2).substr(2) + "_" +
                  sanitize_filename(as.label.empty() ? stem_of(as.path) : as.label) + "." + lower_ext(as.path);
            std::string dest = profile_dir + "\\" + rel;
            if (normalize_path(dest) != normalize_path(as.path))
               copy_plan->push_back({ as.path, dest });
            file = rel;
         }
         // Quotes keep ';' (a comment) and '|' (options) in a file name.
         bool quote = file.find_first_of(";|") != std::string::npos ||
               (!file.empty() && (file.front() == ' ' || file.back() == ' '));
         spec = quote ? "\"" + file + "\"" : file;
         if (as.track > 1)
            spec += " | track=" + std::to_string(as.track);
      }
      std::string comment;
      auto title = titles.find(entry.first);
      if (title != titles.end())
         comment = title->second;
      if (!as.label.empty())
         comment += " <- " + as.label;
      t += hex(entry.first, 2) + " = " + spec + (comment.empty() ? "" : " ; " + comment) + "\n";
   }
   // Unmapped requests keep the music with a tap, so the game's own songs are listed as such.
   if (tapped)
      for (auto &title : titles)
      {
         auto as = assignments.find(title.first);
         if (as == assignments.end() || as->second.kind == Assignment::ORIGINAL)
            t += hex(title.first, 2) + " = original ; " + title.second + "\n";
      }
   return t;
}

bool export_profile(RomSession &target, const Assignments &assignments, const ProfileOptions &options,
      const std::string &profile_path, int &files_copied, std::string &error)
{
   files_copied = 0;
   if (!target.address.known)
   {
      error = "The game has no song address yet, so Proteus cannot tell which song is playing.";
      return false;
   }
   std::vector<std::pair<std::string, std::string>> plan;
   std::string text = profile_text(target, assignments, options, profile_path, &plan);
   for (auto &copy : plan)
   {
      if (!copy_file_data(copy.first, copy.second))
      {
         error = "Could not copy " + copy.first + " to " + copy.second;
         return false;
      }
      files_copied++;
   }
   make_dirs(dir_of(profile_path));
   // A profile written by hand may hold settings Studio does not edit; keep a copy.
   if (file_exists(profile_path) && read_text(profile_path).find("Made with Proteus Studio") == std::string::npos
         && !file_exists(profile_path + ".bak") && !copy_file_data(profile_path, profile_path + ".bak"))
   {
      error = "Could not back up the existing profile to " + profile_path + ".bak";
      return false;
   }
   if (!write_text(profile_path, text))
   {
      error = "Could not write " + profile_path;
      return false;
   }
   return true;
}

void load_profile_mapping(const std::string &profile_path, Assignments &assignments, ProfileOptions &options)
{
   auto profile = std::make_unique<px_profile>();   // large; folder scans load profiles at the same time
   px_profile &p = *profile;
   char err[1200];
   if (!px_profile_load(&p, profile_path.c_str(), err, sizeof(err)))
      return;

   options.music_volume = (int)std::lround(p.music_volume * 100);
   options.game_volume = (int)std::lround(p.game_volume * 100);
   options.crossfade_ms = (int)p.crossfade_ms;
   options.mute.clear();
   for (unsigned i = 0; i < p.mute_count; i++)
      options.mute[p.mute[i].key] = p.mute[i].value;

   // Labels live in the comment after "<- ".
   std::map<uint32_t, std::string> labels;
   std::string text = read_text(profile_path);
   size_t pos = 0;
   while (pos < text.size())
   {
      size_t end = text.find('\n', pos);
      std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      pos = end == std::string::npos ? text.size() : end + 1;
      size_t eq = line.find('='), arrow = line.find(" <- ");
      if (eq == std::string::npos || arrow == std::string::npos || arrow < eq)
         continue;
      std::string label = line.substr(arrow + 4);
      while (!label.empty() && (label.back() == '\r' || label.back() == ' '))
         label.pop_back();
      labels[(uint32_t)strtoul(line.substr(0, eq).c_str(), nullptr, 0)] = label;
   }

   assignments.clear();
   for (unsigned i = 0; i < p.track_count; i++)
   {
      const px_track &t = p.tracks[i];
      Assignment as;
      if (t.action == PX_ACTION_SILENCE)
         as.kind = Assignment::SILENCE;
      else if (t.action == PX_ACTION_FILE)
      {
         as.kind = Assignment::FILE;
         as.path = t.path;
         as.track = t.subtrack + 1;
         as.label = labels.count(t.value) ? labels[t.value] : stem_of(t.path);
      }
      else
         continue;
      assignments[t.value] = as;
   }
}
