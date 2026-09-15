// SPDX-License-Identifier: GPL-3.0-or-later
#include "tas_runner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "http.h"
#include "platform.h"
#include "reference.h"
#include "zip_read.h"

#ifdef _WIN32
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// TASVideos
// ---------------------------------------------------------------------------

// The value of "key" in one flat JSON object: a string (unescaped) or a bare token.
static bool json_field(const std::string &obj, const char *key, std::string &value)
{
   std::string k = std::string("\"") + key + "\":";
   size_t p = obj.find(k);
   if (p == std::string::npos)
      return false;
   p += k.size();
   value.clear();
   if (p < obj.size() && obj[p] == '"')
   {
      for (p++; p < obj.size() && obj[p] != '"'; p++)
      {
         char c = obj[p];
         if (c != '\\' || p + 1 >= obj.size())
         {
            value += c;
            continue;
         }
         c = obj[++p];
         if (c == 'n') value += '\n';
         else if (c == 't') value += '\t';
         else if (c == 'u' && p + 4 < obj.size())
         {
            unsigned cp = (unsigned)strtoul(obj.substr(p + 1, 4).c_str(), nullptr, 16);
            p += 4;
            if (cp < 0x80) value += (char)cp;
            else if (cp < 0x800) { value += (char)(0xC0 | cp >> 6); value += (char)(0x80 | (cp & 0x3F)); }
            else { value += (char)(0xE0 | cp >> 12); value += (char)(0x80 | ((cp >> 6) & 0x3F)); value += (char)(0x80 | (cp & 0x3F)); }
         }
         else value += c;
      }
      return true;
   }
   size_t e = obj.find_first_of(",}]", p);
   value = obj.substr(p, e == std::string::npos ? std::string::npos : e - p);
   return true;
}

bool TasPublication::playable() const
{
   std::string e = lower_ext(file);
   return e == "bk2" || e == "bkm";
}

std::string TasPublication::duration() const
{
   int secs = frames / 60;
   char buf[32];
   if (secs >= 3600)
      snprintf(buf, sizeof(buf), "%d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60);
   else
      snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
   return buf;
}

bool tasvideos_snes_publications(std::vector<TasPublication> &out, std::string &error)
{
   out.clear();
   for (int page = 1; page < 50; page++)
   {
      std::string body;
      if (!http_fetch("https://tasvideos.org/api/v1/publications?systems=snes&pageSize=100&currentPage=" + std::to_string(page), "", body, error))
         return !out.empty();
      size_t count = 0;
      for (size_t p = body.find("{\"id\":"); p != std::string::npos; count++)
      {
         size_t next = body.find("{\"id\":", p + 1);
         std::string obj = body.substr(p, next == std::string::npos ? std::string::npos : next - p);
         p = next;
         TasPublication pub;
         std::string v;
         if (json_field(obj, "id", v)) pub.id = atoi(v.c_str());
         json_field(obj, "title", pub.title);
         json_field(obj, "goal", pub.goal);
         json_field(obj, "emulatorVersion", pub.emulator);
         json_field(obj, "movieFileName", pub.file);
         if (json_field(obj, "frames", v)) pub.frames = atoi(v.c_str());
         pub.current = !json_field(obj, "obsoletedById", v) || v == "null";
         // "SNES <game> \"<goal>\" by <authors> in <time>"
         std::string t = pub.title;
         if (t.compare(0, 5, "SNES ") == 0)
            t = t.substr(5);
         size_t by = t.rfind(" by ");
         if (by != std::string::npos)
            t = t.substr(0, by);
         if (!t.empty() && t.back() == '"')
         {
            size_t q = t.rfind(" \"", t.size() - 2);
            if (q != std::string::npos)
               t = t.substr(0, q);
         }
         pub.game = t;
         if (pub.id)
            out.push_back(pub);
      }
      if (count < 100)
         break;
   }
   return true;
}

std::vector<TasPublication> tasvideos_for_game(const std::vector<TasPublication> &all, const std::string &game)
{
   std::vector<TasPublication> out;
   for (TasPublication p : all)
   {
      p.similarity = game_name_similarity(game, p.game);
      if (p.similarity >= 0.6)
         out.push_back(p);
   }
   std::stable_sort(out.begin(), out.end(), [](const TasPublication &a, const TasPublication &b) {
      if (a.playable() != b.playable())
         return a.playable();
      if (std::abs(a.similarity - b.similarity) > 1e-6)
         return a.similarity > b.similarity;
      return a.frames > b.frames;
   });
   return out;
}

bool tasvideos_download(const TasPublication &pub, const std::string &dir, std::string &path, std::string &error)
{
   std::string body;
   if (!http_fetch("https://tasvideos.org/" + std::to_string(pub.id) + "M?handler=Download", "", body, error))
      return false;
   std::vector<uint8_t> data(body.begin(), body.end());
   std::string name = pub.file.empty() ? std::to_string(pub.id) + ".bk2" : pub.file;
   std::vector<uint8_t> movie;
   if (is_zip(data))
   {
      // TASVideos zips the movie; a .bk2 is a zip itself, told apart by its input log.
      bool bk2 = false;
      zip_read(data, [&](const std::string &n) { bk2 = bk2 || n == "Input Log.txt"; return false; },
            [](const std::string &, std::vector<uint8_t> &) { return true; }, error);
      if (bk2)
         movie = data;
      else
         zip_read(data, [](const std::string &n) { std::string e = lower_ext(n); return e == "bk2" || e == "bkm" || e == "smv" || e == "lsmv"; },
               [&](const std::string &n, std::vector<uint8_t> &d) { movie.swap(d); name = file_name(n); return false; }, error);
   }
   else
      movie = data;
   if (movie.empty())
   {
      error = "the download from TASVideos holds no movie";
      return false;
   }
   error.clear();
   make_dirs(dir);
   path = dir + "\\" + sanitize_filename(name);
   if (!write_text(path, std::string(movie.begin(), movie.end())))
   {
      error = "cannot write " + path;
      return false;
   }
   return true;
}

// ---------------------------------------------------------------------------
// BizHawk
// ---------------------------------------------------------------------------

static std::string bizhawk_dir(const std::string &app_dir)
{
   return app_dir + "\\tools\\BizHawk";
}

std::string bizhawk_exe(const std::string &app_dir)
{
   return bizhawk_dir(app_dir) + "\\EmuHawk.exe";
}

bool bizhawk_installed(const std::string &app_dir)
{
   return file_exists(bizhawk_exe(app_dir)) && file_exists(bizhawk_dir(app_dir) + "\\proteus-installed.txt");
}

bool bizhawk_install(const std::string &app_dir, const std::function<void(const std::string &, float)> &progress,
      const std::atomic<bool> *cancel, std::string &error)
{
   progress("Looking up the latest BizHawk release on GitHub...", 0);
   std::string body;
   if (!http_fetch("https://api.github.com/repos/TASEmulators/BizHawk/releases/latest", "", body, error))
      return false;
   std::string url, tag;
   json_field(body, "tag_name", tag);
   for (size_t p = body.find("\"browser_download_url\":"); p != std::string::npos; p = body.find("\"browser_download_url\":", p + 1))
   {
      std::string v;
      json_field(body.substr(p), "browser_download_url", v);
      if (v.size() > 12 && v.compare(v.size() - 12, 12, "-win-x64.zip") == 0)
         url = v;
   }
   if (url.empty())
   {
      error = "the latest BizHawk release has no Windows download";
      return false;
   }
   std::string zip;
   if (!http_download(url, zip, error, [&](size_t done, size_t total) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Downloading BizHawk %s: %.0f of %.0f MB", tag.c_str(), done / 1048576.0, total / 1048576.0);
            progress(msg, total ? 0.9f * (float)done / (float)total : 0);
         }, cancel))
      return false;
   progress("Unpacking BizHawk...", 0.9f);
   std::string dir = bizhawk_dir(app_dir);
   std::vector<uint8_t> data(zip.begin(), zip.end());
   zip.clear();
   zip.shrink_to_fit();
   bool wrote_all = true;
   if (!zip_read(data, [](const std::string &n) { return !n.empty() && n.back() != '/'; },
         [&](const std::string &n, std::vector<uint8_t> &d) {
            std::string path = dir + "\\" + n;
            std::replace(path.begin(), path.end(), '/', '\\');
            make_dirs(dir_of(path));
            wrote_all = write_text(path, std::string(d.begin(), d.end())) && wrote_all;
            return !(cancel && *cancel);
         }, error) || !wrote_all || !file_exists(bizhawk_exe(app_dir)))
   {
      if (error.empty())
         error = "could not unpack BizHawk into " + dir;
      return false;
   }
   write_text(dir + "\\proteus-installed.txt", tag + "\n" + url + "\n");
   progress("BizHawk " + tag + " is installed.", 1);
   return true;
}

static const char *kDumpLua = R"LUA(-- Proteus Studio: plays a TAS movie and saves the sound CPU and game RAM every 2 seconds.
local out = [[%OUT%]]
local every = 120
client.SetSoundOn(false)
client.speedmode(%SPEED%)
local function read(addr, len, domain)
   if memory.read_bytes_as_binary_string then
      return memory.read_bytes_as_binary_string(addr, len, domain)
   end
   local t = memory.read_bytes_as_array(addr, len, domain)
   local parts = {}
   for i = 1, #t, 4096 do parts[#parts + 1] = string.char(table.unpack(t, i, math.min(i + 4095, #t))) end
   return table.concat(parts)
end
local function exists(name)
   local h = io.open(out .. "/" .. name, "r")
   if h then h:close() return true end
   return false
end
local length = movie.length()
while true do
   emu.frameadvance()
   local f = emu.framecount()
   if f % every == 0 then
      local name = string.format("%s/%08d", out, f)
      local h = io.open(name .. ".tmp", "wb")
      h:write(read(0, 0x10000, "APURAM"))
      h:write(read(0x7E0000, 0x20000, "System Bus"))
      h:close()
      os.rename(name .. ".tmp", name .. ".bin")
   end
   if f % 60 == 0 then
      local h = io.open(out .. "/progress.txt", "w")
      h:write(f, " ", length, "\n")
      h:close()
      local s = io.open(out .. "/speed.txt", "r")
      if s then
         local v = tonumber(s:read("*l"))
         s:close()
         os.remove(out .. "/speed.txt")
         if v then client.speedmode(v) end
      end
      if exists("stop.txt") then break end
   end
   if not movie.isloaded() or movie.mode() ~= "PLAY" or f >= length then break end
end
local h = io.open(out .. "/done.txt", "w")
h:write(emu.framecount(), "\n")
h:close()
client.exit()
)LUA";

#ifdef _WIN32
static std::wstring widen(const std::string &s)
{
   int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
   std::wstring w(n > 0 ? n - 1 : 0, L'\0');
   if (n > 1)
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
   return w;
}
#endif

static void remove_file(const std::string &path)
{
#ifdef _WIN32
   DeleteFileW(widen(path).c_str());
#else
   ::remove(path.c_str());
#endif
}

BizHawkRun::~BizHawkRun()
{
   stop();
}

bool BizHawkRun::start(const std::string &app_dir, const std::string &rom, const std::string &movie, int speed, bool show,
      std::string &error)
{
   stop();
   if (!bizhawk_installed(app_dir))
   {
      error = "BizHawk is not installed";
      return false;
   }
   work_ = app_dir + "\\tools\\movie-run";
   make_dirs(work_);
   for (const auto &f : list_files(work_))
      remove_file(work_ + "\\" + f);
   frame_ = length_ = 0;

   std::string out = work_;
   std::replace(out.begin(), out.end(), '\\', '/');
   std::string lua = kDumpLua;
   lua.replace(lua.find("%OUT%"), 5, out);
   lua.replace(lua.find("%SPEED%"), 7, std::to_string(std::max(1, speed)));
   std::string script = app_dir + "\\tools\\proteus-movie.lua";
   if (!write_text(script, lua))
   {
      error = "cannot write " + script;
      return false;
   }
#ifdef _WIN32
   std::string exe = bizhawk_exe(app_dir);
   std::wstring cmd = L"\"" + widen(exe) + L"\" \"--movie=" + widen(movie) + L"\" \"--lua=" + widen(script) + L"\" \"" + widen(rom) + L"\"";
   STARTUPINFOW si;
   memset(&si, 0, sizeof(si));
   si.cb = sizeof(si);
   si.dwFlags = STARTF_USESHOWWINDOW;
   si.wShowWindow = show ? SW_SHOWNOACTIVATE : SW_SHOWMINNOACTIVE;
   PROCESS_INFORMATION pi;
   memset(&pi, 0, sizeof(pi));
   std::wstring dir = widen(bizhawk_dir(app_dir));
   if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS, nullptr, dir.c_str(), &si, &pi))
   {
      error = "could not start BizHawk (error " + std::to_string(GetLastError()) + ")";
      return false;
   }
   CloseHandle(pi.hThread);
   process_ = pi.hProcess;
   return true;
#else
   (void)rom; (void)movie; (void)show;
   error = "BizHawk runs on Windows";
   return false;
#endif
}

void BizHawkRun::set_speed(int speed)
{
   if (process_)
      write_text(work_ + "\\speed.txt", std::to_string(std::max(1, speed)) + "\n");
}

bool BizHawkRun::next_dump(uint32_t &frame, std::vector<uint8_t> &data)
{
   if (work_.empty())
      return false;
   std::vector<std::string> files = list_files(work_);
   std::string first;
   for (const auto &f : files)
      if (lower_ext(f) == "bin" && (first.empty() || f < first))
         first = f;
   if (first.empty())
      return false;
   frame = (uint32_t)strtoul(first.c_str(), nullptr, 10);
   bool ok = read_file_bytes(work_ + "\\" + first, data);
   remove_file(work_ + "\\" + first);
   return ok && data.size() > 0x10000;
}

void BizHawkRun::read_progress()
{
   std::string p = read_text(work_ + "\\progress.txt");
   unsigned f = 0, l = 0;
   if (sscanf(p.c_str(), "%u %u", &f, &l) == 2)
   {
      frame_ = f;
      length_ = l;
   }
}

bool BizHawkRun::finished()
{
   if (work_.empty())
      return true;
   if (file_exists(work_ + "\\done.txt"))
      return true;
#ifdef _WIN32
   return !process_ || WaitForSingleObject((HANDLE)process_, 0) == WAIT_OBJECT_0;
#else
   return true;
#endif
}

uint32_t BizHawkRun::frame()
{
   read_progress();
   return frame_;
}

uint32_t BizHawkRun::length()
{
   read_progress();
   return length_;
}

void BizHawkRun::stop()
{
#ifdef _WIN32
   if (!process_)
      return;
   HANDLE h = (HANDLE)process_;
   if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
   {
      write_text(work_ + "\\stop.txt", "stop\n");
      if (WaitForSingleObject(h, 5000) != WAIT_OBJECT_0)
         TerminateProcess(h, 0);
   }
   CloseHandle(h);
   process_ = nullptr;
#endif
}
