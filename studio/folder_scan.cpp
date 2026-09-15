// SPDX-License-Identifier: LGPL-2.1-or-later
#include "folder_scan.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <sstream>

#include "platform.h"
#include "rom_session.h"
#include "tas_runner.h"
#include "zip_read.h"

static const char *kReportHeader = "# Proteus Studio folder scan\n# rom\tgame\treferences\ttable\tsongs\tnamed\taddress\thow\tverdict\tnote\n";

FolderScan::~FolderScan()
{
   stop();
   if (thread_.joinable())
      thread_.join();
}

void FolderScan::start(const Options &options)
{
   if (running_)
      return;
   if (thread_.joinable())
      thread_.join();
   cancel_ = false;
   running_ = true;
   done_ = 0;
   total_ = 0;
   thread_ = std::thread([this, options]() { run(options); });
}

void FolderScan::stop()
{
   cancel_ = true;
}

std::string FolderScan::message()
{
   std::lock_guard<std::mutex> lock(mutex_);
   return message_;
}

void FolderScan::set_message(const std::string &m)
{
   std::lock_guard<std::mutex> lock(mutex_);
   message_ = m;
}

std::vector<FolderScanRow> FolderScan::rows()
{
   std::lock_guard<std::mutex> lock(mutex_);
   return rows_;
}

std::string FolderScan::report_path() const
{
   return report_;
}

static std::string clean(std::string s)
{
   std::replace(s.begin(), s.end(), '\t', ' ');
   std::replace(s.begin(), s.end(), '\n', ' ');
   std::replace(s.begin(), s.end(), '\r', ' ');
   return s;
}

void FolderScan::save_report()
{
   std::ostringstream out;
   out << kReportHeader;
   {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto &r : rows_)
         out << clean(r.rom) << '\t' << clean(r.game) << '\t' << r.references << '\t' << (r.table ? 1 : 0) << '\t' << r.songs << '\t'
             << r.named << '\t' << clean(r.address) << '\t' << clean(r.how) << '\t' << r.verdict << '\t' << clean(r.note) << '\n';
   }
   write_text(report_, out.str());
}

static std::vector<std::string> split_tabs(const std::string &line)
{
   std::vector<std::string> out;
   size_t p = 0;
   while (true)
   {
      size_t t = line.find('\t', p);
      out.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
      if (t == std::string::npos)
         break;
      p = t + 1;
   }
   return out;
}

bool is_nes_rom_file(const std::string &path)
{
   std::string ext = lower_ext(path);
   if (ext == "nes")
      return true;
   std::vector<uint8_t> data;
   if (ext != "zip" || !read_file_bytes(path, data))
      return false;
   bool nes = false;
   std::string err;
   zip_read(data, [&](const std::string &name) { nes = nes || lower_ext(name) == "nes"; return false; },
         [](const std::string &, std::vector<uint8_t> &) { return false; }, err);
   return nes;
}

void FolderScan::run(Options o)
{
   report_ = o.app_dir + "\\folder-scans\\" + sanitize_filename(file_name(normalize_path(o.folder))) + ".tsv";
   make_dirs(dir_of(report_));

   std::vector<std::string> roms;
   for (const auto &f : list_files(o.folder))
   {
      std::string e = lower_ext(f);
      if (e == "sfc" || e == "smc" || e == "swc" || e == "fig" || e == "nes" || e == "zip")
         roms.push_back(f);
   }
   std::sort(roms.begin(), roms.end(), [](const std::string &a, const std::string &b) { return to_lower(a) < to_lower(b); });
   total_ = (int)roms.size();

   // A report from an earlier run: those games are done unless asked to scan them again.
   {
      std::lock_guard<std::mutex> lock(mutex_);
      rows_.clear();
      if (!o.rescan)
      {
         std::istringstream in(read_text(report_));
         std::string line;
         while (std::getline(in, line))
         {
            if (!line.empty() && line.back() == '\r')
               line.pop_back();
            if (line.empty() || line[0] == '#')
               continue;
            std::vector<std::string> c = split_tabs(line);
            if (c.size() < 10)
               continue;
            FolderScanRow r;
            r.rom = c[0]; r.game = c[1]; r.references = atoi(c[2].c_str()); r.table = c[3] == "1";
            r.songs = atoi(c[4].c_str()); r.named = atoi(c[5].c_str()); r.address = c[6]; r.how = c[7];
            r.verdict = c[8]; r.note = c[9];
            rows_.push_back(r);
         }
      }
   }

   bool movies = o.use_movies && bizhawk_installed(o.app_dir);
   int index = 0;
   for (const auto &file : roms)
   {
      if (cancel_)
         break;
      index++;
      std::string path = o.folder + "\\" + file;
      bool known = false, skip = false;
      {
         std::lock_guard<std::mutex> lock(mutex_);
         for (const auto &r : rows_)
            known = known || r.rom == file;
      }
      for (const auto &s : o.skip)
         skip = skip || to_lower(normalize_path(s)) == to_lower(normalize_path(path));
      if (known || skip)
      {
         done_++;
         continue;
      }
      std::string progress = " (" + std::to_string(index) + " of " + std::to_string(roms.size()) + ")";
      set_message("Opening " + file + progress);

      FolderScanRow row;
      row.rom = file;
      row.game = stem_of(file);
      {
         RomSession s;
         s.set_app_dir(o.app_dir);
         std::string err;
         auto wait = [&](const std::function<bool()> &busy, const std::function<std::string()> &what) {
            while (busy())
            {
               if (cancel_)
                  s.stop_scan();
               std::this_thread::sleep_for(std::chrono::milliseconds(200));
               set_message(row.game + progress + ": " + what());
               s.take_log();
            }
         };
         bool nes = is_nes_rom_file(path);
         if (nes && o.nes_core_path.empty())
         {
            row.verdict = "skip";
            row.note = "no NES core chosen";
         }
         else if (!s.open(path, nes ? o.nes_core_path : o.core_path, o.system_dir, err))
         {
            row.verdict = "skip";
            row.note = "could not open: " + err;
         }
         else
         {
            row.game = s.display_name();
            auto refs_loaded = [&]() {
               wait([&] { return s.loading_references(); }, [&] { return s.reference_message(); });
               s.apply_reference_results();
            };
            refs_loaded();
            if (s.references.empty() && o.download_references && !cancel_)
            {
               s.download_references();
               refs_loaded();
            }
            row.references = (int)s.references.size();
            row.table = s.song_table.found;

            if (!cancel_)
            {
               s.start_scan(1, 255);
               wait([&] { return s.scanning(); }, [&] { return s.scan_message(); });
               s.apply_scan_results();
            }
            auto named = [&]() {
               int n = 0;
               std::lock_guard<std::mutex> lock(s.songs_mutex);
               for (const auto &song : s.songs)
                  n += song.has_value && !song.reference.empty();
               return n;
            };
            std::string note = s.scan_message();
            // A TAS movie numbers songs the scan could not.
            if (movies && !s.is_nes() && !cancel_ && !s.references.empty() && (!s.address.known || named() < 3))
            {
               s.find_movies();
               wait([&] { return s.tool_busy(); }, [&] { return s.tool_message(); });
               std::vector<TasPublication> list = s.movie_list();
               auto pick = std::find_if(list.begin(), list.end(), [](const TasPublication &p) { return p.playable() && p.current; });
               if (pick == list.end())
                  pick = std::find_if(list.begin(), list.end(), [](const TasPublication &p) { return p.playable(); });
               if (pick != list.end() && !cancel_)
               {
                  std::vector<std::string> have = s.downloaded_movies();
                  if (have.empty())
                  {
                     s.download_movie(*pick);
                     wait([&] { return s.tool_busy(); }, [&] { return s.tool_message(); });
                     have = s.downloaded_movies();
                  }
                  if (!have.empty() && !cancel_)
                  {
                     s.start_movie(have[0], o.movie_speed, false);
                     wait([&] { return s.scanning(); }, [&] { return s.scan_message(); });
                     s.apply_scan_results();
                     note = s.scan_message();
                  }
               }
            }
            {
               std::lock_guard<std::mutex> lock(s.songs_mutex);
               row.songs = (int)s.songs.size();
            }
            row.named = named();
            if (s.address.known)
            {
               row.address = describe_song_address(s.address);
               row.how = s.address_source;
            }
            row.note = note;
            if (row.named >= 3 && s.address.known)
               row.verdict = "easy";
            else if (row.songs > 0 || row.references > 0)
               row.verdict = "partly";
            else
               row.verdict = "skip";
            s.take_log();
         }
      }
      if (cancel_)
         break;
      {
         std::lock_guard<std::mutex> lock(mutex_);
         rows_.push_back(row);
      }
      save_report();
      done_++;
   }
   set_message(cancel_ ? "Stopped; start again to carry on." : "Done: " + std::to_string(rows().size()) + " games in the report.");
   running_ = false;
}
