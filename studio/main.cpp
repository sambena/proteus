// SPDX-License-Identifier: LGPL-2.1-or-later
// Proteus Studio: open the game to change and a game to take music from, find the
// songs in both, listen to any of them, and generate the Proteus profile.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <SDL.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include "audio_out.h"
#include "platform.h"
#include "profile_export.h"
#include "rom_session.h"
#include "song_finder.h"
#include "zip_read.h"

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Settings
{
   std::string retroarch_dir;
   std::string core_file;
   std::string rom[2];
   float ui_scale = 1.0f;
   int volume = 80;
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
      else if (k == "rom_a") s.rom[0] = v;
      else if (k == "rom_b") s.rom[1] = v;
      else if (k == "ui_scale") s.ui_scale = std::clamp((float)atof(v.c_str()), 0.8f, 2.5f);
      else if (k == "volume") s.volume = std::clamp(atoi(v.c_str()), 0, 100);
   }
   if (s.retroarch_dir.empty())
      for (const char *guess : { "D:\\RetroArch", "C:\\RetroArch-Win64", "C:\\RetroArch" })
         if (file_exists(std::string(guess) + "\\retroarch.exe")) { s.retroarch_dir = guess; break; }
   return s;
}

static void save_settings(const Settings &s)
{
   char scale[16];
   snprintf(scale, sizeof(scale), "%.2f", s.ui_scale);
   write_text(settings_path(), "retroarch_dir=" + s.retroarch_dir + "\ncore=" + s.core_file +
         "\nrom_a=" + s.rom[0] + "\nrom_b=" + s.rom[1] + "\nui_scale=" + scale +
         "\nvolume=" + std::to_string(s.volume) + "\n");
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
// App state
// ---------------------------------------------------------------------------

enum { TARGET = 0, SOURCE = 1 };
enum AdvancedTab { TAB_PLAY, TAB_FINDER, TAB_ADDRESS, TAB_CHANNELS, TAB_PROFILE, TAB_LOG };

struct Palette
{
   ImU32 bg, panel, panel_hi, border, text, dim, accent, accent_hi, side[2], danger, ok;
};

static Palette P;

struct App
{
   Settings settings;
   SDL_Window *window = nullptr;
   SDL_Renderer *renderer = nullptr;
   ImFont *font = nullptr, *font_big = nullptr;
   AudioOut audio;
   RomSession sessions[2];
   std::vector<FoundSong> snap[2];   // songs as of this UI frame

   Assignments assignments;
   ProfileOptions options;
   uint32_t selected_target = 0;
   bool have_selected = false;

   std::vector<std::pair<std::string, std::string>> cores;   // file, display name
   std::string status;
   bool status_error = false;
   std::vector<std::string> log;

   char filter[2][64] = { "", "" };
   int scan_first[2] = { 1, 1 }, scan_last[2] = { 255, 255 };
   int renaming_side = -1;
   size_t renaming_index = 0;
   char rename_buf[128] = "";

   bool show_settings = false;
   bool show_advanced = false;
   int advanced_tab = TAB_PLAY;
   int select_tab = -1;        // tab to bring forward on the next frame
   int live = TARGET;          // which game the Advanced view plays
   bool live_running = false;
   SDL_Texture *game_tex = nullptr;
   int tex_w = 0, tex_h = 0;
   SDL_GameController *controller = nullptr;
   SongFinder finder;
   std::vector<uint8_t> quick_state;

   std::string last_export;
};

static void set_status(App &a, const std::string &msg, bool error = false)
{
   a.status = msg;
   a.status_error = error;
   a.log.push_back(msg);
}

static std::string hex(uint32_t v, int digits = 2)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "%0*X", digits, (unsigned)v);
   return buf;
}

static std::string system_dir(const App &a)
{
   return retroarch_cfg(a.settings.retroarch_dir, "system_directory", a.settings.retroarch_dir + "\\system");
}

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
   // Song ripping reads snes9x save states.
   if (a.settings.core_file.empty())
      for (auto &c : a.cores)
         if (c.first == "snes9x_libretro.dll")
            a.settings.core_file = c.first;
}

// Default channel mutes: the first six voices, which carry the music in most SNES games.
static void default_mutes(App &a)
{
   a.options.mute.clear();
   CoreHost &core = a.sessions[TARGET].core;
   for (int ch = 1; ch <= 6; ch++)
   {
      std::string key = "snes9x_sndchan_volume_" + std::to_string(ch);
      if (core.find_option(key))
         a.options.mute[key] = "0";
   }
}

static void open_rom(App &a, int side, const std::string &path)
{
   if (a.settings.core_file.empty())
   {
      set_status(a, "Choose your RetroArch folder in Settings first; Proteus Studio uses its snes9x core.", true);
      a.show_settings = true;
      return;
   }
   a.audio.stop();
   if (a.live == side)
      a.live_running = false;
   std::string err;
   RomSession &s = a.sessions[side];
   if (!s.open(path, a.settings.retroarch_dir + "\\cores\\" + a.settings.core_file, system_dir(a), err))
   {
      set_status(a, "Could not open " + file_name(path) + ": " + err, true);
      return;
   }
   a.settings.rom[side] = path;
   save_settings(a.settings);

   if (side == TARGET)
   {
      a.assignments.clear();
      a.options = ProfileOptions();
      a.have_selected = false;
      if (!s.profile_path().empty())
         load_profile_mapping(s.profile_path(), a.assignments, a.options);
      else
         default_mutes(a);
      if (a.live == TARGET)
      {
         size_t size = 0;
         s.core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
         a.finder.reset(size);
      }
   }
   std::string msg = "Opened " + s.display_name();
   if (!s.address.known)
      msg += ". No song address is known for this game yet; find it in Advanced.";
   else if (s.songs.empty())
      msg += ". Click Scan songs to find its music.";
   set_status(a, msg);
}

static void play_song(App &a, const std::string &id, const std::string &path, unsigned track = 1)
{
   if (a.audio.playing_id() == id)
   {
      a.audio.stop();
      return;
   }
   std::string err;
   if (!a.audio.play(path, track ? track - 1 : 0, id, err))
      set_status(a, "Cannot play " + file_name(path) + ": " + err, true);
}

// Chooses which game the Advanced view plays; the other one pauses.
static void select_live(App &a, int side)
{
   a.live = side;
   a.live_running = false;
   a.audio.clear_game();
   size_t size = 0;
   a.sessions[side].core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
   a.finder.reset(size);
}

// Opens Advanced on Play & rip with this game running.
static void play_live(App &a, int side)
{
   if (a.live != side)
      select_live(a, side);
   a.show_advanced = true;
   a.select_tab = TAB_PLAY;
   a.audio.stop();
   a.live_running = true;
}

static void assign(App &a, uint32_t value, const FoundSong &song, const std::string &game)
{
   Assignment as;
   as.kind = Assignment::FILE;
   as.path = song.spc_path;
   as.label = game + ": " + song.title;
   a.assignments[value] = as;
}

// Imports reference songs (a folder, zip or .spc) for the game on one side.
static void import_references(App &a, int side, const std::string &source)
{
   RomSession &s = a.sessions[side];
   if (!s.is_open())
   {
      set_status(a, "Open the game first, then add its reference songs.", true);
      return;
   }
   std::string err;
   int n = s.import_references(source, err);
   if (n <= 0)
      set_status(a, "No reference songs imported: " + err, true);
   else
      set_status(a, "Imported " + std::to_string(n) + " reference songs for " + s.display_name() + ".");
}

// Reference songs the list has found (distinct titles).
static int references_found(const App &a, int side)
{
   std::vector<std::string> seen;
   for (const auto &song : a.snap[side])
      if (!song.reference.empty() && std::find(seen.begin(), seen.end(), song.reference) == seen.end())
         seen.push_back(song.reference);
   return (int)seen.size();
}

// A zip archive of .spc files, rather than a zipped ROM.
static bool is_spc_archive(const std::string &path)
{
   std::vector<uint8_t> data;
   if (lower_ext(path) != "zip" || !read_file_bytes(path, data))
      return false;
   bool spc = false;
   std::string err;
   zip_read(data, [&](const std::string &name) { spc = spc || lower_ext(name) == "spc"; return false; },
         [](const std::string &, std::vector<uint8_t> &) { return false; }, err);
   return spc;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

static ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

enum Icon { ICON_PLAY, ICON_STOP };

static bool icon_button(const char *id, Icon icon, float size, ImU32 color, bool filled = false)
{
   ImVec2 pos = ImGui::GetCursorScreenPos();
   bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
   bool hovered = ImGui::IsItemHovered();
   ImDrawList *dl = ImGui::GetWindowDrawList();
   ImVec2 c(pos.x + size / 2, pos.y + size / 2);
   if (filled || hovered)
      dl->AddCircleFilled(c, size / 2, filled ? color : IM_COL32(255, 255, 255, 28));
   ImU32 fg = filled ? P.bg : color;
   float r = size * 0.22f;
   if (icon == ICON_PLAY)
      dl->AddTriangleFilled(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x - r * 0.7f, c.y + r), ImVec2(c.x + r, c.y), fg);
   else
      dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), fg, 1.5f);
   return pressed;
}

static void chip(const char *text, ImU32 color)
{
   ImVec2 ts = ImGui::CalcTextSize(text);
   ImVec2 pos = ImGui::GetCursorScreenPos();
   float pad = 6;
   ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + ts.x + pad * 2, pos.y + ts.y + 2),
         (color & 0x00FFFFFF) | 0x33000000, 4);
   ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + pad, pos.y + 1), color, text);
   ImGui::Dummy(ImVec2(ts.x + pad * 2, ts.y + 2));
}

static bool primary_button(const char *label, ImVec2 size, ImU32 color)
{
   ImGui::PushStyleColor(ImGuiCol_Button, col(color));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(color) * ImVec4(1.15f, 1.15f, 1.15f, 1));
   ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(color) * ImVec4(0.9f, 0.9f, 0.9f, 1));
   ImGui::PushStyleColor(ImGuiCol_Text, col(P.bg));
   bool pressed = ImGui::Button(label, size);
   ImGui::PopStyleColor(4);
   return pressed;
}

static void help_marker(const char *text)
{
   ImGui::SameLine();
   ImGui::TextDisabled("(?)");
   if (ImGui::BeginItemTooltip())
   {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
      ImGui::TextUnformatted(text);
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
   }
}

static std::string open_rom_dialog(App &a, int side)
{
   std::string start = a.settings.rom[side].empty() ? a.settings.rom[1 - side] : a.settings.rom[side];
   return open_file_dialog(side == TARGET ? "Open the game to change" : "Open the game to take music from",
         { { "SNES ROMs", "*.sfc;*.smc;*.swc;*.fig;*.zip" }, { "All files", "*.*" } }, dir_of(start));
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

static void draw_panel_header(App &a, int side)
{
   RomSession &s = a.sessions[side];
   ImGui::PushStyleColor(ImGuiCol_Text, col(P.side[side]));
   ImGui::TextUnformatted(side == TARGET ? "GAME TO CHANGE" : "MUSIC SOURCE");
   ImGui::PopStyleColor();

   if (!s.is_open())
      return;

   ImGui::PushFont(a.font_big, a.font_big->LegacySize);
   ImGui::TextUnformatted(s.display_name().c_str());
   ImGui::PopFont();

   if (s.address.known)
   {
      std::string addr = "Song address " + describe_song_address(s.address) +
            (s.address.latch && s.address.bytes.empty() ? " (command)" : "");
      chip(addr.c_str(), P.ok);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Proteus follows the music through this address. From: %s.\nChange it in Advanced > Game info.",
               s.address_source.c_str());
   }
   else
   {
      chip("No song address", side == TARGET ? P.danger : P.dim);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip(side == TARGET ? "Generate INI needs to know where the game keeps its song number.\n"
                                            "Scans find it for many games; otherwise use Advanced > Find song address."
                                          : "Not needed for the music source.");
   }
   ImGui::SameLine();
   if (s.start.kind != SongStart::NONE)
   {
      chip(s.start.kind == SongStart::ROUTINE ? "Songs start: music routine" : "Songs start: RAM command", P.ok);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("%s\nFrom: %s.", describe_song_start(s.start).c_str(), s.start_source.c_str());
   }
   else
   {
      chip("Songs start: unknown", P.dim);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Scan songs finds how this game starts its songs.");
   }
   ImGui::SameLine();
   std::string count = std::to_string(a.snap[side].size()) + " songs";
   chip(count.c_str(), P.dim);

   std::string refs;
   ImU32 refs_color = P.dim;
   if (s.loading_references())
      refs = "Loading reference songs...";
   else if (!s.references.empty())
   {
      int found = references_found(a, side);
      refs = "Reference songs: " + std::to_string(found) + " of " + std::to_string(s.references.size()) + " listed";
      refs_color = found ? P.ok : P.dim;
   }
   else
      refs = "No reference songs";
   float width = ImGui::CalcTextSize(refs.c_str()).x + 20;
   ImGui::SameLine();
   if (ImGui::GetContentRegionAvail().x < width)
      ImGui::NewLine();
   chip(refs.c_str(), refs_color);
   if (ImGui::IsItemHovered())
   {
      std::string tip;
      if (s.loading_references())
         tip = s.reference_message();
      else if (s.references.empty())
         tip = "The game's soundtrack as .spc files (SNESmusic.org, Zophar's Domain).\n"
               "With them, songs are named after the reference they match, scans keep only real songs,\n"
               "and a song table in the ROM lists every song by number.\nAdd them with Reference songs, or drop a folder or .zip here.";
      else
      {
         tip = "Scans and rips are named after the reference .spc they match.\nFolder: " + s.references.dir();
         if (s.song_table.found)
            tip += "\nThe ROM lists " + std::to_string(s.song_table.entries.size()) + " songs in a table (ROM offset " +
                   hex((uint32_t)s.song_table.rom_offset, 6) + "); Scan songs lists and checks them.";
         else
            tip += "\nNo song table was found in the ROM.";
      }
      ImGui::SetTooltip("%s", tip.c_str());
   }
}

static void draw_empty_panel(App &a, int side)
{
   ImVec2 avail = ImGui::GetContentRegionAvail();
   ImVec2 pos = ImGui::GetCursorScreenPos();
   ImDrawList *dl = ImGui::GetWindowDrawList();
   ImVec2 max(pos.x + avail.x, pos.y + avail.y - 4);
   dl->AddRect(pos, max, P.border, 10.0f, 0, 2.0f);

   const char *title = side == TARGET ? "Open the game you want to change" : "Open the game to take music from";
   const char *hint = "Drop a ROM here, or";
   float cy = pos.y + avail.y * 0.40f;
   ImGui::PushFont(a.font_big, a.font_big->LegacySize);
   ImVec2 ts = ImGui::CalcTextSize(title);
   dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(pos.x + (avail.x - ts.x) / 2, cy), P.text, title);
   ImGui::PopFont();
   ImVec2 hs = ImGui::CalcTextSize(hint);
   dl->AddText(ImVec2(pos.x + (avail.x - hs.x) / 2, cy + ts.y + 10), P.dim, hint);

   float bw = 160;
   ImGui::SetCursorScreenPos(ImVec2(pos.x + (avail.x - bw) / 2, cy + ts.y + hs.y + 24));
   ImGui::PushID(side);
   if (primary_button("Open ROM...", ImVec2(bw, 0), P.side[side]))
   {
      std::string path = open_rom_dialog(a, side);
      if (!path.empty())
         open_rom(a, side, path);
   }
   ImGui::PopID();
   if (!a.settings.rom[side].empty() && file_exists(a.settings.rom[side]))
   {
      std::string reopen = "Reopen " + stem_of(a.settings.rom[side]);
      ImVec2 rs = ImGui::CalcTextSize(reopen.c_str());
      ImGui::SetCursorScreenPos(ImVec2(pos.x + (avail.x - rs.x) / 2 - 8, ImGui::GetCursorScreenPos().y + 6));
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
      ImGui::PushID(side + 10);
      if (ImGui::Button(reopen.c_str()))
         open_rom(a, side, a.settings.rom[side]);
      ImGui::PopID();
      ImGui::PopStyleColor();
   }
   ImGui::SetCursorScreenPos(pos);
   ImGui::Dummy(ImVec2(avail.x, avail.y - 4));
}

static void draw_scan_bar(App &a, int side)
{
   RomSession &s = a.sessions[side];
   ImGui::PushID(side);
   if (s.scanning())
   {
      std::string label = s.scan_message();
      ImGui::ProgressBar(s.scan_progress(), ImVec2(-90, 0), label.c_str());
      ImGui::SameLine();
      if (ImGui::Button("Stop", ImVec2(-1, 0)))
         s.stop_scan();
   }
   else
   {
      ImGui::BeginDisabled(a.live == side && a.live_running);
      if (primary_button("Scan songs", ImVec2(120, 0), P.side[side]))
      {
         a.audio.stop();
         s.start_scan(a.scan_first[side], a.scan_last[side]);
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      {
         if (a.live == side && a.live_running)
            ImGui::SetTooltip("Pause the game in Advanced to scan.");
         else if (s.song_table.found)
            ImGui::SetTooltip("Lists the %d songs of the ROM song table, then plays each song number\n"
                  "to check it against the reference songs.", (int)s.song_table.entries.size());
         else if (s.start.kind == SongStart::NONE)
            ImGui::SetTooltip("Finds how the game starts its music, then plays song numbers %d-%d\n"
                  "and keeps the ones that make music.", a.scan_first[side], a.scan_last[side]);
         else
            ImGui::SetTooltip("Plays song numbers %d-%d with %s\nand keeps the ones that make music.",
                  a.scan_first[side], a.scan_last[side], describe_song_start(s.start).c_str());
      }
      ImGui::SameLine();
      bool live_here = a.show_advanced && a.live == side && a.live_running;
      if (ImGui::Button(live_here ? "Pause game" : "Play & rip"))
      {
         if (live_here)
            a.live_running = false;
         else
            play_live(a, side);
      }
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Play this game in Advanced. Each new song is ripped into this list a few seconds after it starts.");
      ImGui::SameLine();
      if (ImGui::Button("Reference songs"))
         ImGui::OpenPopup("references");
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("The game's soundtrack as .spc files, to name and check the songs found.");
      if (ImGui::BeginPopup("references"))
      {
         bool busy = s.loading_references();
         if (ImGui::MenuItem("Download from Zophar's Domain", nullptr, false, !busy))
         {
            s.download_references();
            set_status(a, "Looking for " + s.display_name() + " on Zophar's Domain...");
         }
         if (ImGui::MenuItem("Import folder...", nullptr, false, !busy))
         {
            std::string dir = pick_folder_dialog("Folder with the game's .spc files");
            if (!dir.empty())
               import_references(a, side, dir);
         }
         if (ImGui::MenuItem("Import .zip or .spc...", nullptr, false, !busy))
         {
            std::string path = open_file_dialog("Reference songs", { { "SPC sets", "*.zip;*.spc;*.rsn" }, { "All files", "*.*" } }, "");
            if (!path.empty())
               import_references(a, side, path);
         }
         ImGui::Separator();
         bool have = !s.references.empty();
         if (ImGui::MenuItem("List reference songs", nullptr, false, have && !busy))
         {
            a.audio.stop();
            s.list_reference_songs();
         }
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds every reference song to this list without playing the game.\n"
                  "Enough for a music source; songs of the game to change need numbers from a scan.");
         if (ImGui::MenuItem("Open reference folder"))
         {
            make_dirs(s.reference_dir());
            open_folder(s.reference_dir());
         }
         if (ImGui::MenuItem("Remove reference songs", nullptr, false, have && !busy))
         {
            s.remove_references();
            set_status(a, "Removed the reference songs for " + s.display_name() + ".");
         }
         ImGui::EndPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Open ROM..."))
      {
         std::string path = open_rom_dialog(a, side);
         if (!path.empty())
            open_rom(a, side, path);
      }
      if (!s.songs.empty())
      {
         ImGui::SameLine();
         if (ImGui::Button("Clear songs"))
         {
            a.audio.stop();
            s.clear_library();
            set_status(a, "Cleared scanned songs for " + s.display_name());
         }
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Delete all scanned songs and cached rips for this game.");
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##filter", "Filter songs", a.filter[side], sizeof(a.filter[side]));
      std::string msg = s.loading_references() ? s.reference_message() : s.scan_message();
      if (!msg.empty())
         ImGui::TextColored(col(P.dim), "%s", msg.c_str());
   }
   ImGui::PopID();
}

static bool matches_filter(const App &a, int side, const FoundSong &s)
{
   if (!a.filter[side][0])
      return true;
   std::string f = to_lower(a.filter[side]);
   return to_lower(s.title).find(f) != std::string::npos || to_lower(hex(s.value)).find(f) != std::string::npos;
}

static void draw_title_cell(App &a, int side, size_t index, const FoundSong &song, bool selected)
{
   if (a.renaming_side == side && a.renaming_index == index)
   {
      ImGui::SetNextItemWidth(-1);
      ImGui::SetKeyboardFocusHere();
      if (ImGui::InputText("##rename", a.rename_buf, sizeof(a.rename_buf),
               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
      {
         a.sessions[side].rename_song(index, a.rename_buf);
         a.renaming_side = -1;
      }
      else if (ImGui::IsItemDeactivated())
         a.renaming_side = -1;
      return;
   }
   ImGui::PushStyleColor(ImGuiCol_Header, col(P.side[side]) * ImVec4(1, 1, 1, 0.18f));
   if (ImGui::Selectable(song.title.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
   {
      if (side == TARGET && song.has_value)
      {
         a.selected_target = song.value;
         a.have_selected = true;
      }
   }
   ImGui::PopStyleColor();
   if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
   {
      a.renaming_side = side;
      a.renaming_index = index;
      snprintf(a.rename_buf, sizeof(a.rename_buf), "%s", song.title.c_str());
   }
   if (ImGui::BeginPopupContextItem())
   {
      if (ImGui::MenuItem("Rename"))
      {
         a.renaming_side = side;
         a.renaming_index = index;
         snprintf(a.rename_buf, sizeof(a.rename_buf), "%s", song.title.c_str());
      }
      if (ImGui::MenuItem("Show file"))
         open_folder(dir_of(song.spc_path));
      if (ImGui::MenuItem("Remove from list"))
      {
         if (side == TARGET)
            a.assignments.erase(song.value);
         a.sessions[side].remove_song(index);
      }
      ImGui::EndPopup();
   }

   // Songs from the source can be dragged onto songs of the game to change.
   if (side == SOURCE && ImGui::BeginDragDropSource())
   {
      uint32_t idx = (uint32_t)index;
      ImGui::SetDragDropPayload("PX_SOURCE_SONG", &idx, sizeof(idx));
      ImGui::Text("Use \"%s\"", song.title.c_str());
      ImGui::EndDragDropSource();
   }
   if (side == TARGET && song.has_value && ImGui::BeginDragDropTarget())
   {
      if (const ImGuiPayload *p = ImGui::AcceptDragDropPayload("PX_SOURCE_SONG"))
      {
         uint32_t idx = *(uint32_t*)p->Data;
         if (idx < a.snap[SOURCE].size())
            assign(a, song.value, a.snap[SOURCE][idx], a.sessions[SOURCE].display_name());
      }
      ImGui::EndDragDropTarget();
   }
}

static void draw_replacement_cell(App &a, const FoundSong &song)
{
   if (!song.has_value)
   {
      ImGui::TextDisabled("needs a song number");
      return;
   }
   auto it = a.assignments.find(song.value);
   Assignment current = it == a.assignments.end() ? Assignment() : it->second;

   std::string rid = "R:" + hex(song.value);
   if (current.kind == Assignment::FILE)
   {
      bool playing = a.audio.playing_id() == rid;
      if (icon_button("##rplay", playing ? ICON_STOP : ICON_PLAY, ImGui::GetFrameHeight(), P.side[SOURCE], playing))
         play_song(a, rid, current.path, current.track);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Listen to the replacement");
   }
   else
      ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
   ImGui::SameLine();

   const char *preview = current.kind == Assignment::ORIGINAL ? "Original music"
         : current.kind == Assignment::SILENCE ? "Silence" : current.label.c_str();
   ImGui::PushStyleColor(ImGuiCol_Text, col(current.kind == Assignment::FILE ? P.side[SOURCE] : P.dim));
   ImGui::SetNextItemWidth(-1);
   bool open = ImGui::BeginCombo("##repl", preview, ImGuiComboFlags_HeightLarge);
   ImGui::PopStyleColor();
   if (open)
   {
      if (ImGui::Selectable("Original music", current.kind == Assignment::ORIGINAL))
         a.assignments.erase(song.value);
      if (ImGui::Selectable("Silence", current.kind == Assignment::SILENCE))
      {
         Assignment as;
         as.kind = Assignment::SILENCE;
         a.assignments[song.value] = as;
      }
      RomSession &src = a.sessions[SOURCE];
      if (src.is_open() && !a.snap[SOURCE].empty())
      {
         ImGui::SeparatorText(src.display_name().c_str());
         for (size_t i = 0; i < a.snap[SOURCE].size(); i++)
         {
            const FoundSong &b = a.snap[SOURCE][i];
            ImGui::PushID((int)i);
            std::string label = (b.has_value ? hex(b.value) + "  " : "") + b.title;
            if (ImGui::Selectable(label.c_str(), current.kind == Assignment::FILE && current.path == b.spc_path))
               assign(a, song.value, b, src.display_name());
            ImGui::PopID();
         }
      }
      ImGui::Separator();
      if (ImGui::Selectable("Music file..."))
      {
         std::string path = open_file_dialog("Choose replacement music",
               { { "Music", "*.spc;*.nsf;*.nsfe;*.vgm;*.vgz;*.gbs;*.ogg;*.mp3;*.wav" }, { "All files", "*.*" } },
               dir_of(current.path));
         if (!path.empty())
         {
            Assignment as;
            as.kind = Assignment::FILE;
            as.path = path;
            as.label = stem_of(path);
            a.assignments[song.value] = as;
         }
      }
      ImGui::EndCombo();
   }
   if (ImGui::BeginDragDropTarget())
   {
      if (const ImGuiPayload *p = ImGui::AcceptDragDropPayload("PX_SOURCE_SONG"))
      {
         uint32_t idx = *(uint32_t*)p->Data;
         if (idx < a.snap[SOURCE].size())
            assign(a, song.value, a.snap[SOURCE][idx], a.sessions[SOURCE].display_name());
      }
      ImGui::EndDragDropTarget();
   }
}

static void draw_song_table(App &a, int side)
{
   RomSession &s = a.sessions[side];
   std::vector<FoundSong> &songs = a.snap[side];

   if (songs.empty())
   {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, col(P.dim));
      if (s.scanning())
         ImGui::TextWrapped("Looking for songs. They appear here as they are found.");
      else if (!s.references.empty())
         ImGui::TextWrapped("No songs yet. Scan songs plays the game's song numbers and names each song after the "
               "reference it matches. For a music source, Reference songs > List reference songs adds them all "
               "without playing the game.");
      else
         ImGui::TextWrapped("No songs yet. Scan songs plays every song number from a moment early in the game "
               "and keeps the ones that make music. You can also play the game in Advanced and rip songs "
               "as you hear them. Reference songs (the game's .spc soundtrack) name the songs and make scans reliable.");
      ImGui::PopStyleColor();
      return;
   }

   int columns = side == TARGET ? 5 : 4;
   ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX |
         ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH;
   if (!ImGui::BeginTable("songs", columns, flags))
      return;
   float fh = ImGui::GetFrameHeight();
   ImGui::TableSetupScrollFreeze(0, 1);
   ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fh);
   ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000").x);
   ImGui::TableSetupColumn("Song", ImGuiTableColumnFlags_WidthStretch, 1.0f);
   ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("no match").x + 12);
   if (side == TARGET)
      ImGui::TableSetupColumn("Replace with", ImGuiTableColumnFlags_WidthStretch, 1.3f);
   ImGui::TableHeadersRow();

   for (size_t i = 0; i < songs.size(); i++)
   {
      const FoundSong &song = songs[i];
      if (!matches_filter(a, side, song))
         continue;
      ImGui::PushID((int)i);
      ImGui::TableNextRow(ImGuiTableRowFlags_None, fh + 6);
      bool selected = side == TARGET && a.have_selected && song.has_value && song.value == a.selected_target;

      ImGui::TableSetColumnIndex(0);
      std::string id = std::string(side == TARGET ? "A:" : "B:") + song.spc_path;
      bool playing = a.audio.playing_id() == id;
      if (icon_button("##play", playing ? ICON_STOP : ICON_PLAY, fh, P.side[side], playing))
         play_song(a, id, song.spc_path);

      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(col(P.dim), "%s", song.has_value ? hex(song.value).c_str() : "--");

      ImGui::TableSetColumnIndex(2);
      ImGui::AlignTextToFramePadding();
      draw_title_cell(a, side, i, song, selected);

      ImGui::TableSetColumnIndex(3);
      ImGui::AlignTextToFramePadding();
      if (!s.references.empty() && song.reference.empty())
      {
         ImGui::TextColored(col(P.dim), "no match");
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Not one of the reference songs: a sound effect, silence, or a song the set lacks.");
      }
      else if (song.kind == SONG_JINGLE)
         ImGui::TextColored(col(P.dim), "jingle");

      if (side == TARGET)
      {
         ImGui::TableSetColumnIndex(4);
         draw_replacement_cell(a, song);
      }
      ImGui::PopID();
   }
   ImGui::EndTable();
}

static void draw_panel(App &a, int side, ImVec2 size)
{
   ImGui::PushStyleColor(ImGuiCol_ChildBg, col(P.panel));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
   ImGui::BeginChild(side == TARGET ? "panel_a" : "panel_b", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
   RomSession &s = a.sessions[side];
   draw_panel_header(a, side);
   ImGui::Spacing();
   if (!s.is_open())
      draw_empty_panel(a, side);
   else
   {
      draw_scan_bar(a, side);
      ImGui::Spacing();
      draw_song_table(a, side);
   }
   ImGui::EndChild();
   ImGui::PopStyleVar();
   ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Footer: now playing and profile generation
// ---------------------------------------------------------------------------

static std::string describe_playing(App &a)
{
   std::string id = a.audio.playing_id();
   if (id.size() < 2)
      return "";
   if (id[0] == 'R')
   {
      uint32_t v = (uint32_t)strtoul(id.c_str() + 2, nullptr, 16);
      auto it = a.assignments.find(v);
      return it == a.assignments.end() ? "" : it->second.label;
   }
   int side = id[0] == 'A' ? TARGET : SOURCE;
   std::string path = id.substr(2);
   for (auto &s : a.snap[side])
      if (s.spc_path == path)
         return a.sessions[side].display_name() + ": " + s.title;
   return file_name(path);
}

static void generate_profile(App &a)
{
   RomSession &t = a.sessions[TARGET];
   std::string path = profile_path_for(t, system_dir(a));
   int copied = 0;
   std::string err;
   if (!export_profile(t, a.assignments, a.options, path, copied, err))
   {
      set_status(a, err, true);
      return;
   }
   a.last_export = path;
   int mapped = 0;
   for (auto &as : a.assignments)
      mapped += as.second.kind != Assignment::ORIGINAL;
   set_status(a, "Wrote " + path + " with " + std::to_string(mapped) + " replaced songs (" +
         std::to_string(copied) + " music files copied)." +
         (file_exists(path + ".bak") ? " Your earlier profile is saved as " + file_name(path) + ".bak." : ""));
}

static void draw_footer(App &a, float height)
{
   ImGui::PushStyleColor(ImGuiCol_ChildBg, col(P.panel));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));
   ImGui::BeginChild("footer", ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

   float fh = ImGui::GetFrameHeight();
   std::string playing = describe_playing(a);
   bool is_playing = !playing.empty();
   if (icon_button("##stopall", is_playing ? ICON_STOP : ICON_PLAY, fh * 1.3f, P.accent, is_playing) && is_playing)
      a.audio.stop();
   ImGui::SameLine();
   ImGui::BeginGroup();
   if (is_playing)
   {
      int secs = (int)a.audio.position_seconds();
      ImGui::TextUnformatted(playing.c_str());
      ImGui::TextColored(col(P.dim), "%d:%02d", secs / 60, secs % 60);
   }
   else
   {
      ImGui::TextColored(col(P.dim), "Nothing playing");
      ImGui::TextColored(col(P.dim), "Click a play button to listen to any song");
   }
   ImGui::EndGroup();
   ImGui::SameLine(0, 24);
   ImGui::SetCursorPosY(ImGui::GetCursorPosY() + fh * 0.2f);
   ImGui::SetNextItemWidth(120);
   if (ImGui::SliderInt("##volume", &a.settings.volume, 0, 100, "Volume %d%%"))
      a.audio.volume = a.settings.volume;

   // Right side: status and the profile button.
   RomSession &t = a.sessions[TARGET];
   float bw = 180;
   ImGui::SameLine(ImGui::GetWindowWidth() - bw - 16);
   ImGui::SetCursorPosY(ImGui::GetCursorPosY() - fh * 0.2f);
   ImGui::BeginDisabled(!t.is_open() || !t.address.known);
   if (primary_button("Generate INI", ImVec2(bw, fh * 1.5f), P.accent))
      generate_profile(a);
   ImGui::EndDisabled();
   if (t.is_open() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", t.address.known ? profile_path_for(t, system_dir(a)).c_str()
            : "The game to change needs a song address first (Advanced).");

   ImGui::EndChild();
   ImGui::PopStyleVar();
   ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Advanced drawer
// ---------------------------------------------------------------------------

static void upload_frame(App &a, CoreHost &core)
{
   const auto &px = core.frame();
   int w = (int)core.frame_width(), h = (int)core.frame_height();
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

static void live_rip(App &a)
{
   RomSession &s = a.sessions[a.live];
   std::string msg;
   std::lock_guard<std::mutex> lock(s.core_mutex);
   s.rip_now(msg);
   set_status(a, msg);
}

static void tab_play(App &a)
{
   ImGui::AlignTextToFramePadding();
   ImGui::TextUnformatted("Play");
   for (int side = 0; side < 2; side++)
   {
      RomSession &g = a.sessions[side];
      ImGui::SameLine();
      ImGui::BeginDisabled(!g.is_open());
      std::string label = (g.is_open() ? g.display_name() : std::string(side == TARGET ? "Game to change" : "Music source")) +
            (side == TARGET ? "  (game to change)" : "  (music source)");
      ImGui::PushStyleColor(ImGuiCol_CheckMark, col(P.side[side]));
      ImGui::PushID(side);
      if (ImGui::RadioButton(label.c_str(), a.live == side) && a.live != side)
         select_live(a, side);
      ImGui::PopID();
      ImGui::PopStyleColor();
      ImGui::EndDisabled();
   }
   RomSession &s = a.sessions[a.live];
   if (!s.is_open())
   {
      ImGui::TextDisabled("Open a ROM first.");
      return;
   }
   ImGui::SameLine(0, 24);
   ImGui::BeginDisabled(s.scanning());
   if (ImGui::Button(a.live_running ? "Pause (P)" : "Play (P)", ImVec2(100, 0)))
   {
      a.live_running = !a.live_running;
      a.audio.clear_game();
   }
   ImGui::SameLine();
   if (ImGui::Button("Reset"))
   {
      std::lock_guard<std::mutex> lock(s.core_mutex);
      s.core.reset();
   }
   ImGui::SameLine();
   if (ImGui::Button("Rip current song (R)"))
      live_rip(a);
   ImGui::SameLine();
   if (ImGui::Button("Scan from this moment"))
   {
      {
         std::lock_guard<std::mutex> lock(s.core_mutex);
         s.use_current_moment_for_scans();
      }
      set_status(a, "Scans of " + s.display_name() + " now start from this moment.");
   }
   help_marker("Scans start a few seconds after the game boots. Some games only load part of their music "
         "at a time (per world or level); play to that part and scan from there to find those songs.");
   ImGui::EndDisabled();

   ImVec2 avail = ImGui::GetContentRegionAvail();
   float img_h = avail.y - ImGui::GetTextLineHeightWithSpacing() * 1.2f;
   if (a.game_tex && img_h > 40)
   {
      float aspect = 4.0f / 3.0f;
      float h = img_h, w = h * aspect;
      if (w > avail.x * 0.6f) { w = avail.x * 0.6f; h = w / aspect; }
      ImGui::Image((ImTextureID)(intptr_t)a.game_tex, ImVec2(w, h));
      ImGui::SameLine();
   }
   ImGui::BeginGroup();
   ImGui::TextColored(col(P.dim), "Controls");
   ImGui::TextUnformatted("Arrows: D-pad    Z/X: B/A    A/S: Y/X\nQ/W: L/R    Enter: Start    RShift: Select\n"
         "F2/F4: save/load state    Tab: fast forward");
   ImGui::Spacing();
   if (s.address.known)
   {
      if (s.heard_song())
         ImGui::Text("Current song number: %s", hex(s.last_song_value()).c_str());
      ImGui::TextWrapped("New song numbers are ripped automatically a few seconds after they start.");
   }
   else
      ImGui::TextWrapped("Without a song address, press Rip current song whenever new music plays.");
   ImGui::EndGroup();
}

static void tab_finder(App &a)
{
   RomSession &s = a.sessions[a.live];
   ImGui::TextWrapped("Play the game (Play tab). Right after the music changes, press Music changed (M). "
         "When the music stays the same across a scene change, press Same music (N). The song address "
         "is among the few bytes that follow the music.");
   if (!s.is_open())
      return;
   if (ImGui::Button("Music changed (M)"))
      a.finder.mark_changed();
   ImGui::SameLine();
   if (ImGui::Button("Same music (N)"))
      a.finder.mark_same();
   ImGui::SameLine();
   if (ImGui::Button("Start over"))
   {
      size_t size = 0;
      s.core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
      a.finder.reset(size);
   }
   ImGui::SameLine();
   ImGui::TextColored(col(P.dim), "%u marks, %zu candidates", a.finder.marks(), a.finder.candidate_count());

   if (a.finder.marks() == 0)
      return;
   if (ImGui::BeginTable("cands", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
   {
      ImGui::TableSetupColumn("Address");
      ImGui::TableSetupColumn("Value now");
      ImGui::TableSetupColumn("Kind");
      ImGui::TableSetupColumn("");
      ImGui::TableHeadersRow();
      for (const SongCandidate &c : a.finder.candidates(40, true))
      {
         ImGui::PushID((int)c.address);
         ImGui::TableNextRow();
         ImGui::TableNextColumn();
         ImGui::Text("$%s", hex(c.address, 4).c_str());
         ImGui::TableNextColumn();
         ImGui::Text("%s", hex(c.value).c_str());
         ImGui::TableNextColumn();
         ImGui::TextUnformatted(c.command ? "command" : "song number");
         ImGui::TableNextColumn();
         if (ImGui::SmallButton("Use as song address"))
         {
            s.address = SongAddress();
            s.address.known = true;
            s.address.address = c.address;
            s.address.latch = c.command;
            s.address.debounce = c.command ? 1 : 2;
            s.address_source = "song finder";
            s.save_to_game_db("song address from the song finder");
            set_status(a, "Song address for " + s.display_name() + " set to $" + hex(c.address, 4) + " and saved to the game database.");
         }
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Proteus follows the music by reading this address.");
         ImGui::PopID();
      }
      ImGui::EndTable();
   }
}

static void tab_address(App &a)
{
   if (ImGui::BeginTable("addr", 2, ImGuiTableFlags_SizingStretchSame))
   {
      for (int side = 0; side < 2; side++)
      {
         ImGui::TableNextColumn();
         RomSession &s = a.sessions[side];
         ImGui::PushID(side);
         ImGui::TextColored(col(P.side[side]), "%s", side == TARGET ? "GAME TO CHANGE" : "MUSIC SOURCE");
         if (!s.is_open())
         {
            ImGui::TextDisabled("No ROM open.");
            ImGui::PopID();
            continue;
         }
         char crc[16];
         snprintf(crc, sizeof(crc), "%08X", s.rom_crc32());
         ImGui::SameLine();
         ImGui::TextColored(col(P.dim), "%s  ROM %s", s.display_name().c_str(), crc);
         ImGui::BeginDisabled(s.scanning());

         ImGui::SeparatorText("Song address");
         SongAddress &ad = s.address;
         bool changed = false;
         ImGui::SetNextItemWidth(140);
         if (ImGui::InputScalar("Address", ImGuiDataType_U32, &ad.address, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal))
         {
            ad.known = true;
            changed = true;
         }
         ImGui::SetNextItemWidth(140);
         changed |= ImGui::Combo("Memory", &ad.memory, "system_ram\0save_ram\0video_ram\0");
         ImGui::SetNextItemWidth(140);
         changed |= ImGui::SliderInt("Size (bytes)", &ad.size, 1, 4);
         static char pattern_text[2][64];
         static std::string pattern_shown[2];
         std::string current_pat = format_song_pattern(ad);
         if (pattern_shown[side] != current_pat)
         {
            snprintf(pattern_text[side], sizeof(pattern_text[side]), "%s", current_pat.c_str());
            pattern_shown[side] = current_pat;
         }
         ImGui::SetNextItemWidth(140);
         if (ImGui::InputTextWithHint("Pattern", "e.g. 10 xx FF 05", pattern_text[side], sizeof(pattern_text[side]),
                  ImGuiInputTextFlags_EnterReturnsTrue))
         {
            parse_song_pattern(pattern_text[side], ad);
            changed = true;
         }
         help_marker("For command blocks like Chrono Trigger's $1E00: 10 xx .. .. \nxx marks the song number; .. matches any value.\nPress Enter to apply.");
         changed |= ImGui::Checkbox("Command register", &ad.latch);
         help_marker("The address only holds a song number for a moment when music starts, then returns to 0.");
         ImGui::SetNextItemWidth(140);
         changed |= ImGui::SliderInt("Debounce frames", &ad.debounce, 1, 30);
         if (changed)
            s.address_source = "manual entry";
         ImGui::TextColored(col(P.dim), "From: %s", ad.known ? s.address_source.c_str() : "not known");

         // RetroAchievements code notes lookup
         ImGui::Spacing();
         bool querying_ra = s.querying_retroachievements();
         ImGui::BeginDisabled(querying_ra);
         if (ImGui::Button("Query RetroAchievements"))
            s.query_retroachievements();
         ImGui::EndDisabled();
         help_marker("Queries the RetroAchievements API using the ROM's MD5 hash to discover documented music/BGM RAM addresses.\nScan songs checks the notes found against the songs it starts and uses one that holds the song number.");

         RaLookupResult ra_res;
         {
            std::lock_guard<std::mutex> lock(s.ra_mutex);
            ra_res = s.ra_result;
         }

         if (querying_ra || ra_res.status == RaLookupStatus::SEARCHING)
         {
            ImGui::TextColored(col(P.accent), "Querying RetroAchievements for MD5 %s...", s.rom_md5().substr(0, 8).c_str());
         }
         else if (ra_res.status == RaLookupStatus::SUCCESS)
         {
            ImGui::TextColored(col(P.ok), "%s (Game ID %d):", ra_res.message.c_str(), ra_res.game_id);
            for (size_t ni = 0; ni < ra_res.notes.size(); ni++)
            {
               const auto &cn = ra_res.notes[ni];
               ImGui::PushID((int)ni);
               if (ImGui::SmallButton("Use"))
               {
                  ad.address = cn.address;
                  ad.known = true;
                  ad.memory = cn.memory;
                  ad.size = cn.size;
                  ad.bytes.clear();
                  ad.any.clear();
                  ad.offset = 0;
                  s.address_source = "RetroAchievements: " + cn.note;
               }
               ImGui::SameLine();
               ImGui::Text("%s", cn.address_hex.c_str());
               ImGui::SameLine();
               ImGui::TextColored(col(P.text), "%s", cn.note.c_str());
               if (!cn.author.empty())
               {
                  ImGui::SameLine();
                  ImGui::TextColored(col(P.dim), "(%s)", cn.author.c_str());
               }
               ImGui::PopID();
            }
         }
         else if (ra_res.status == RaLookupStatus::NO_GAME)
         {
            ImGui::TextColored(col(P.dim), "ROM hash not found on RetroAchievements.");
         }
         else if (ra_res.status == RaLookupStatus::NO_NOTES)
         {
            ImGui::TextColored(col(P.dim), "Game found (ID %d), but no music notes documented.", ra_res.game_id);
         }
         else if (ra_res.status == RaLookupStatus::ERROR_NET)
         {
            ImGui::TextColored(col(P.danger), "RetroAchievements lookup failed: %s", ra_res.message.c_str());
         }

         // Static 65816 APU analysis
         ImGui::Spacing();
         if (ImGui::Button("Run Static 65816 Analysis"))
            s.run_static_analysis();
         help_marker("Statically scans the ROM binary for 65816 writes to APU ports $2140-$2143 for RAM that may hold the song number.\nThese are suggestions: Scan songs checks them against the songs it starts.");

         if (s.apu_analysis.found)
         {
            ImGui::TextColored(col(P.ok), "Static APU Analysis (%d candidate%s):",
                  (int)s.apu_analysis.candidates.size(),
                  s.apu_analysis.candidates.size() == 1 ? "" : "s");
            for (size_t ci = 0; ci < s.apu_analysis.candidates.size(); ci++)
            {
               const auto &cand = s.apu_analysis.candidates[ci];
               ImGui::PushID((int)(2000 + ci));
               if (ImGui::SmallButton("Use"))
               {
                  ad = cand.address;
                  s.address_source = "static analysis: " + cand.source_desc;
                  set_status(a, "Using " + describe_song_address(ad) + " (unchecked). Scan songs checks it.");
               }
               ImGui::SameLine();
               ImGui::Text("$%s", hex(cand.address.address, 4).substr(2).c_str());
               ImGui::SameLine();
               ImGui::TextColored(col(P.text), "%s", cand.source_desc.c_str());
               ImGui::SameLine();
               ImGui::TextColored(col(P.dim), "[Score %d, %s]", cand.score, cand.routine_desc.c_str());
               ImGui::PopID();
            }
         }

         ImGui::SeparatorText("How songs start");
         static char start_text[2][160];
         static std::string shown[2];
         std::string current = format_song_start(s.start);
         if (shown[side] != current)
         {
            snprintf(start_text[side], sizeof(start_text[side]), "%s", current.c_str());
            shown[side] = current;
         }
         ImGui::SetNextItemWidth(-1);
         if (ImGui::InputTextWithHint("##start", "found by Scan songs", start_text[side], sizeof(start_text[side]),
                  ImGuiInputTextFlags_EnterReturnsTrue))
         {
            SongStart parsed;
            if (!start_text[side][0])
            {
               s.start = SongStart();
               s.start_source.clear();
            }
            else if (parse_song_start(start_text[side], parsed))
            {
               s.start = parsed;
               s.start_source = "manual entry";
            }
            else
               set_status(a, "Could not read that. Examples: ram 0x1DFB bytes=xx settle=150  or  "
                     "routine jsl 0xC70004 block=0x1E00 bytes=10 xx FF 05 settle=300", true);
         }
         help_marker("ram <address> bytes=<command>: write a command the game polls (xx is the song number).\n"
               "routine <jsl|jsr> <address> [block=<address> bytes=<command>] [a=song]: call the game's music "
               "routine with the command in RAM or the song number in A.\n"
               "settle=<frames>: how long a song gets to start before it is ripped.\nPress Enter to apply.");
         ImGui::TextColored(col(P.dim), "%s%s%s", s.start.kind == SongStart::NONE ? "Not known" : describe_song_start(s.start).c_str(),
               s.start_source.empty() ? "" : "  -  from: ", s.start_source.c_str());

         ImGui::SeparatorText("Scan");
         ImGui::SetNextItemWidth(140);
         ImGui::SliderInt("First song", &a.scan_first[side], 0, 255, "%d");
         ImGui::SetNextItemWidth(140);
         ImGui::SliderInt("Last song", &a.scan_last[side], a.scan_first[side], 255, "%d");

         if (ImGui::Button("Save to game database"))
         {
            s.save_to_game_db("entered by hand");
            set_status(a, "Saved " + s.display_name() + " to " + GameDb::get().path());
         }
         ImGui::SameLine();
         if (ImGui::Button("Open database file"))
            open_folder(GameDb::get().path());
         ImGui::EndDisabled();
         ImGui::PopID();
      }
      ImGui::EndTable();
   }
}

static void tab_channels(App &a)
{
   RomSession &t = a.sessions[TARGET];
   ImGui::TextWrapped("While a replacement plays, these channels of the game to change are muted. Music usually "
         "uses the first channels and sound effects the last ones; mute fewer to keep more effects.");
   if (!t.is_open())
      return;
   int shown = 0;
   for (auto &o : t.core.options())
   {
      if (o.key.find("sndchan") == std::string::npos && o.key.find("channel") == std::string::npos)
         continue;
      std::string mute_value;
      for (auto &v : o.values)
         if (v.value == "0" || v.value == "disabled" || v.value == "off")
         {
            mute_value = v.value;
            break;
         }
      if (mute_value.empty())
         continue;
      shown++;
      bool muted = a.options.mute.count(o.key) > 0;
      if (ImGui::Checkbox(o.desc.c_str(), &muted))
      {
         if (muted)
            a.options.mute[o.key] = mute_value;
         else
            a.options.mute.erase(o.key);
      }
   }
   if (!shown)
      ImGui::TextDisabled("This core has no channel options; replacements play over the original music.");

   ImGui::SeparatorText("Mix");
   ImGui::SetNextItemWidth(220);
   ImGui::SliderInt("Replacement volume", &a.options.music_volume, 0, 200, "%d%%");
   ImGui::SetNextItemWidth(220);
   ImGui::SliderInt("Game volume", &a.options.game_volume, 0, 200, "%d%%");
   ImGui::SetNextItemWidth(220);
   ImGui::SliderInt("Crossfade", &a.options.crossfade_ms, 0, 3000, "%d ms");
}

static void tab_profile(App &a)
{
   RomSession &t = a.sessions[TARGET];
   if (!t.is_open())
   {
      ImGui::TextDisabled("Open the game to change first.");
      return;
   }
   if (!t.address.known)
   {
      ImGui::TextDisabled("The game to change needs a song address before a profile can be written.");
      return;
   }
   std::string path = profile_path_for(t, system_dir(a));
   ImGui::TextColored(col(P.dim), "%s", path.c_str());
   if (!a.last_export.empty())
   {
      ImGui::SameLine();
      if (ImGui::SmallButton("Open folder"))
         open_folder(dir_of(a.last_export));
   }
   std::vector<std::pair<std::string, std::string>> plan;
   std::string text = profile_text(t, a.assignments, a.options, path, &plan);
   ImGui::InputTextMultiline("##ini", (char*)text.c_str(), text.size() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
}

static void tab_log(App &a)
{
   if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
   {
      for (auto &l : a.log)
         ImGui::TextUnformatted(l.c_str());
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
         ImGui::SetScrollHereY(1.0f);
   }
   ImGui::EndChild();
}

static void draw_advanced(App &a, float height)
{
   ImGui::PushStyleColor(ImGuiCol_ChildBg, col(P.panel));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));
   ImGui::BeginChild("advanced", ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
   if (ImGui::BeginTabBar("adv"))
   {
      struct { const char *name; int tab; void (*draw)(App &); } tabs[] = {
         { "Play & rip", TAB_PLAY, tab_play },
         { "Find song address", TAB_FINDER, tab_finder },
         { "Game info", TAB_ADDRESS, tab_address },
         { "Channels & mix", TAB_CHANNELS, tab_channels },
         { "INI preview", TAB_PROFILE, tab_profile },
         { "Log", TAB_LOG, tab_log },
      };
      for (auto &t : tabs)
         if (ImGui::BeginTabItem(t.name, nullptr, a.select_tab == t.tab ? ImGuiTabItemFlags_SetSelected : 0))
         {
            if (a.select_tab == t.tab)
               a.select_tab = -1;
            a.advanced_tab = t.tab;
            t.draw(a);
            ImGui::EndTabItem();
         }
      ImGui::EndTabBar();
   }
   ImGui::EndChild();
   ImGui::PopStyleVar();
   ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Header and settings
// ---------------------------------------------------------------------------

static void draw_settings(App &a)
{
   if (a.show_settings)
      ImGui::OpenPopup("Settings");
   ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
   if (!ImGui::BeginPopupModal("Settings", &a.show_settings, ImGuiWindowFlags_NoSavedSettings))
      return;
   static char ra[1024];
   static bool init = false;
   if (!init || ImGui::IsWindowAppearing())
   {
      snprintf(ra, sizeof(ra), "%s", a.settings.retroarch_dir.c_str());
      init = true;
   }
   ImGui::TextUnformatted("RetroArch folder");
   ImGui::SetNextItemWidth(-90);
   if (ImGui::InputText("##ra", ra, sizeof(ra), ImGuiInputTextFlags_EnterReturnsTrue))
   {
      a.settings.retroarch_dir = ra;
      refresh_cores(a);
   }
   ImGui::SameLine();
   if (ImGui::Button("Browse...", ImVec2(-1, 0)))
   {
      std::string dir = pick_folder_dialog("RetroArch folder");
      if (!dir.empty())
      {
         snprintf(ra, sizeof(ra), "%s", dir.c_str());
         a.settings.retroarch_dir = dir;
         a.settings.core_file.clear();
         refresh_cores(a);
      }
   }

   ImGui::Spacing();
   ImGui::TextUnformatted("SNES core");
   std::string current = a.settings.core_file.empty() ? "(none found)" : a.settings.core_file;
   for (auto &c : a.cores)
      if (c.first == a.settings.core_file)
         current = c.second;
   ImGui::SetNextItemWidth(-1);
   if (ImGui::BeginCombo("##core", current.c_str()))
   {
      for (auto &c : a.cores)
         if (ImGui::Selectable(c.second.c_str(), c.first == a.settings.core_file))
            a.settings.core_file = c.first;
      ImGui::EndCombo();
   }
   if (a.settings.core_file != "snes9x_libretro.dll")
      ImGui::TextColored(col(P.danger), "Ripping songs needs snes9x (snes9x_libretro.dll).");

   ImGui::Spacing();
   ImGui::TextUnformatted("Interface size");
   ImGui::SetNextItemWidth(-1);
   if (ImGui::SliderFloat("##scale", &a.settings.ui_scale, 0.8f, 2.0f, "%.2fx"))
      ImGui::GetStyle().FontScaleMain = a.settings.ui_scale;

   ImGui::Spacing();
   if (primary_button("Done", ImVec2(120, 0), P.accent))
   {
      a.settings.retroarch_dir = ra;
      save_settings(a.settings);
      a.show_settings = false;
      ImGui::CloseCurrentPopup();
   }
   ImGui::EndPopup();
}

static void draw_header(App &a)
{
   ImGui::PushFont(a.font_big, a.font_big->LegacySize);
   float big = ImGui::GetFontSize();
   ImGui::TextUnformatted("Proteus Studio");
   ImGui::PopFont();
   ImGui::SameLine();
   ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (big - ImGui::GetFontSize()) * 0.5f);
   ImGui::TextColored(col(P.dim), "Swap a game's music with songs from another game");

   float right = ImGui::GetWindowWidth() - 16;
   float w_adv = ImGui::CalcTextSize("Advanced").x + 32, w_set = ImGui::CalcTextSize("Settings").x + 24;
   ImGui::SameLine(right - w_adv - w_set - 8);
   ImGui::PushStyleColor(ImGuiCol_Button, col(a.show_advanced ? P.panel_hi : P.panel));
   if (ImGui::Button(a.show_advanced ? "Advanced  v" : "Advanced  ^", ImVec2(w_adv, 0)))
      a.show_advanced = !a.show_advanced;
   ImGui::SameLine();
   if (ImGui::Button("Settings", ImVec2(w_set, 0)))
      a.show_settings = true;
   ImGui::PopStyleColor();
}

static void draw_ui(App &a)
{
   ImGuiViewport *vp = ImGui::GetMainViewport();
   ImGui::SetNextWindowPos(vp->WorkPos);
   ImGui::SetNextWindowSize(vp->WorkSize);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
   ImGui::Begin("Proteus Studio", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
   ImGui::PopStyleVar();

   draw_header(a);
   ImGui::Spacing();

   float fh = ImGui::GetFrameHeight();
   float footer_h = fh * 1.5f + 22;
   float status_h = ImGui::GetTextLineHeightWithSpacing();
   float avail = ImGui::GetContentRegionAvail().y - footer_h - status_h - ImGui::GetStyle().ItemSpacing.y * 2;
   float adv_h = a.show_advanced ? std::floor(avail * 0.45f) : 0;
   float panels_h = avail - adv_h - (a.show_advanced ? ImGui::GetStyle().ItemSpacing.y : 0);

   float gap = ImGui::GetStyle().ItemSpacing.x;
   float half = std::floor((ImGui::GetContentRegionAvail().x - gap) / 2);
   draw_panel(a, TARGET, ImVec2(half, panels_h));
   ImGui::SameLine();
   draw_panel(a, SOURCE, ImVec2(0, panels_h));

   if (a.show_advanced)
      draw_advanced(a, adv_h);
   draw_footer(a, footer_h);
   ImGui::TextColored(col(a.status_error ? P.danger : P.dim), "%s", a.status.c_str());

   draw_settings(a);
   ImGui::End();
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

static void apply_theme()
{
   P.bg        = IM_COL32(17, 18, 22, 255);
   P.panel     = IM_COL32(25, 27, 33, 255);
   P.panel_hi  = IM_COL32(38, 41, 50, 255);
   P.border    = IM_COL32(44, 47, 57, 255);
   P.text      = IM_COL32(230, 231, 235, 255);
   P.dim       = IM_COL32(140, 145, 158, 255);
   P.accent    = IM_COL32(150, 132, 255, 255);
   P.accent_hi = IM_COL32(172, 158, 255, 255);
   P.side[0]   = IM_COL32(255, 145, 100, 255);
   P.side[1]   = IM_COL32(80, 205, 180, 255);
   P.danger    = IM_COL32(255, 110, 110, 255);
   P.ok        = IM_COL32(120, 210, 140, 255);

   ImGuiStyle &st = ImGui::GetStyle();
   ImGui::StyleColorsDark(&st);
   st.WindowRounding = 0;
   st.ChildRounding = 10;
   st.FrameRounding = 6;
   st.PopupRounding = 8;
   st.GrabRounding = 6;
   st.TabRounding = 6;
   st.ScrollbarRounding = 8;
   st.FramePadding = ImVec2(10, 6);
   st.ItemSpacing = ImVec2(10, 8);
   st.CellPadding = ImVec2(8, 4);
   st.ScrollbarSize = 12;
   st.WindowBorderSize = 0;
   st.ChildBorderSize = 1;
   st.FrameBorderSize = 0;

   ImVec4 *c = st.Colors;
   c[ImGuiCol_Text]                 = col(P.text);
   c[ImGuiCol_TextDisabled]         = col(P.dim);
   c[ImGuiCol_WindowBg]             = col(P.bg);
   c[ImGuiCol_ChildBg]              = col(P.panel);
   c[ImGuiCol_PopupBg]              = col(IM_COL32(30, 32, 39, 250));
   c[ImGuiCol_Border]               = col(P.border);
   c[ImGuiCol_FrameBg]              = col(IM_COL32(35, 38, 46, 255));
   c[ImGuiCol_FrameBgHovered]       = col(IM_COL32(45, 49, 60, 255));
   c[ImGuiCol_FrameBgActive]        = col(IM_COL32(52, 56, 68, 255));
   c[ImGuiCol_Button]               = col(IM_COL32(40, 43, 52, 255));
   c[ImGuiCol_ButtonHovered]        = col(IM_COL32(52, 56, 68, 255));
   c[ImGuiCol_ButtonActive]         = col(IM_COL32(60, 65, 80, 255));
   c[ImGuiCol_Header]               = col(IM_COL32(45, 49, 60, 255));
   c[ImGuiCol_HeaderHovered]        = col(IM_COL32(45, 49, 60, 200));
   c[ImGuiCol_HeaderActive]         = col(IM_COL32(55, 60, 74, 255));
   c[ImGuiCol_TableHeaderBg]        = col(P.panel);
   c[ImGuiCol_TableRowBg]           = col(IM_COL32(0, 0, 0, 0));
   c[ImGuiCol_TableRowBgAlt]        = col(IM_COL32(255, 255, 255, 6));
   c[ImGuiCol_TableBorderLight]     = col(IM_COL32(255, 255, 255, 10));
   c[ImGuiCol_Tab]                  = col(P.panel);
   c[ImGuiCol_TabHovered]           = col(P.panel_hi);
   c[ImGuiCol_TabSelected]          = col(P.panel_hi);
   c[ImGuiCol_TabSelectedOverline]  = col(P.accent);
   c[ImGuiCol_SliderGrab]           = col(P.accent);
   c[ImGuiCol_SliderGrabActive]     = col(P.accent_hi);
   c[ImGuiCol_CheckMark]            = col(P.accent);
   c[ImGuiCol_PlotHistogram]        = col(P.accent);
   c[ImGuiCol_Separator]            = col(P.border);
   c[ImGuiCol_ScrollbarBg]          = col(IM_COL32(0, 0, 0, 0));
   c[ImGuiCol_ScrollbarGrab]        = col(IM_COL32(60, 64, 76, 255));
   c[ImGuiCol_DragDropTarget]       = col(P.side[1]);
   c[ImGuiCol_ModalWindowDimBg]     = col(IM_COL32(0, 0, 0, 140));
}

static void load_fonts(App &a)
{
   ImGuiIO &io = ImGui::GetIO();
   const char *regular = "C:\\Windows\\Fonts\\segoeui.ttf";
   const char *bold = "C:\\Windows\\Fonts\\seguisb.ttf";
   if (file_exists(regular))
      a.font = io.Fonts->AddFontFromFileTTF(regular, 17.0f);
   if (file_exists(bold))
      a.font_big = io.Fonts->AddFontFromFileTTF(bold, 23.0f);
   if (!a.font)
      a.font = io.Fonts->AddFontDefault();
   if (!a.font_big)
      a.font_big = a.font;
   io.FontDefault = a.font;
   ImGui::GetStyle().FontSizeBase = 17.0f;
}

// ---------------------------------------------------------------------------
// Input and the live game
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

static bool game_has_keyboard(const App &a)
{
   return a.show_advanced && (a.advanced_tab == TAB_PLAY || a.advanced_tab == TAB_FINDER) &&
         a.live_running && !ImGui::GetIO().WantTextInput;
}

static void handle_key(App &a, SDL_Keycode key)
{
   if (!game_has_keyboard(a) && !(a.show_advanced && a.advanced_tab == TAB_PLAY && key == SDLK_p))
      return;
   RomSession &s = a.sessions[a.live];
   switch (key)
   {
      case SDLK_p: a.live_running = !a.live_running; a.audio.clear_game(); break;
      case SDLK_r: live_rip(a); break;
      case SDLK_m: a.finder.mark_changed(); set_status(a, "Marked: music changed"); break;
      case SDLK_n: a.finder.mark_same(); set_status(a, "Marked: same music"); break;
      case SDLK_F2:
      {
         std::lock_guard<std::mutex> lock(s.core_mutex);
         a.quick_state = s.core.save_state();
         set_status(a, a.quick_state.empty() ? "Save state failed" : "State saved");
         break;
      }
      case SDLK_F4:
      {
         std::lock_guard<std::mutex> lock(s.core_mutex);
         if (s.core.load_state(a.quick_state))
            set_status(a, "State loaded");
         a.audio.clear_game();
         break;
      }
      default: break;
   }
}

static void run_live_game(App &a)
{
   RomSession &s = a.sessions[a.live];
   bool active = a.show_advanced && (a.advanced_tab == TAB_PLAY || a.advanced_tab == TAB_FINDER) &&
         a.live_running && s.is_open() && !s.scanning();
   if (!active)
      return;
   std::unique_lock<std::mutex> lock(s.core_mutex, std::try_to_lock);
   if (!lock.owns_lock())
      return;

   ImGuiIO &io = ImGui::GetIO();
   uint16_t pad = (io.WantTextInput ? 0 : keyboard_pad()) | controller_pad(a.controller);
   bool fast = !io.WantTextInput && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB];
   const size_t target = (size_t)(AudioOut::kRate / s.core.fps() * 3);
   for (int i = 0; i < (fast ? 4 : 3) && (fast || a.audio.game_queued() < target); i++)
   {
      s.play_frame(pad);
      size_t size = 0;
      const uint8_t *ram = s.core.memory(RETRO_MEMORY_SYSTEM_RAM, &size);
      if (ram)
         a.finder.update(ram, size);
      if (!fast)
         a.audio.push_game(s.core.audio().data(), s.core.audio().size() / 2, s.core.sample_rate());
      s.core.audio().clear();
   }
   upload_frame(a, s.core);
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
   static App app;
   App &a = app;
   a.settings = load_settings();

   a.window = SDL_CreateWindow("Proteus Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
         (int)(1480 * std::min(a.settings.ui_scale, 1.2f)), (int)(900 * std::min(a.settings.ui_scale, 1.2f)),
         SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
   a.renderer = SDL_CreateRenderer(a.window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
   if (!a.window || !a.renderer)
   {
      fprintf(stderr, "SDL window: %s\n", SDL_GetError());
      return 1;
   }
   refresh_cores(a);
   a.audio.open();
   a.audio.volume = a.settings.volume;

   IMGUI_CHECKVERSION();
   ImGui::CreateContext();
   ImGuiIO &io = ImGui::GetIO();
   io.IniFilename = nullptr;
   io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
   apply_theme();
   load_fonts(a);
   ImGui::GetStyle().ScaleAllSizes(a.settings.ui_scale);
   ImGui::GetStyle().FontScaleMain = a.settings.ui_scale;
   ImGui_ImplSDL2_InitForSDLRenderer(a.window, a.renderer);
   ImGui_ImplSDLRenderer2_Init(a.renderer);

   if (a.settings.retroarch_dir.empty() || a.settings.core_file.empty())
      a.show_settings = true;
   for (int side = 0; side < 2; side++)
      if (!a.settings.rom[side].empty() && file_exists(a.settings.rom[side]) && !a.settings.core_file.empty())
         open_rom(a, side, a.settings.rom[side]);
   if (a.status.empty())
      set_status(a, "Open the game to change on the left and a game to take music from on the right.");

   bool quit = false;
   while (!quit)
   {
      SDL_Event ev;
      while (SDL_PollEvent(&ev))
      {
         ImGui_ImplSDL2_ProcessEvent(&ev);
         if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE))
            quit = true;
         else if (ev.type == SDL_KEYDOWN && !ev.key.repeat)
            handle_key(a, ev.key.keysym.sym);
         else if (ev.type == SDL_CONTROLLERDEVICEADDED && !a.controller)
            a.controller = SDL_GameControllerOpen(ev.cdevice.which);
         else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && a.controller &&
               SDL_GameControllerFromInstanceID(ev.cdevice.which) == a.controller)
         {
            SDL_GameControllerClose(a.controller);
            a.controller = nullptr;
         }
         else if (ev.type == SDL_DROPFILE && ev.drop.file)
         {
            // Dropped on the left half: the game to change; on the right: the source.
            int mx = 0, my = 0, ww = 0, wh = 0;
            SDL_GetGlobalMouseState(&mx, &my);
            int wx = 0, wy = 0;
            SDL_GetWindowPosition(a.window, &wx, &wy);
            SDL_GetWindowSize(a.window, &ww, &wh);
            std::string path = ev.drop.file;
            SDL_free(ev.drop.file);
            int side = (mx - wx) < ww / 2 ? TARGET : SOURCE;
            std::string ext = lower_ext(path);
            if (dir_exists(path) || ext == "spc" || ext == "rsn" || is_spc_archive(path))
               import_references(a, side, path);
            else
               open_rom(a, side, path);
         }
      }

      run_live_game(a);

      for (int side = 0; side < 2; side++)
      {
         a.sessions[side].apply_scan_results();
         a.sessions[side].apply_reference_results();
         for (auto &line : a.sessions[side].take_log())
            a.log.push_back(line);
         std::lock_guard<std::mutex> lock(a.sessions[side].songs_mutex);
         a.snap[side] = a.sessions[side].songs;
      }
      if (a.log.size() > 2000)
         a.log.erase(a.log.begin(), a.log.begin() + 500);

      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      draw_ui(a);
      ImGui::Render();
      SDL_SetRenderDrawColor(a.renderer, 17, 18, 22, 255);
      SDL_RenderClear(a.renderer);
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), a.renderer);
      SDL_RenderPresent(a.renderer);
   }

   a.audio.stop();
   for (auto &s : a.sessions)
      s.close();
   save_settings(a.settings);
   if (a.game_tex)
      SDL_DestroyTexture(a.game_tex);
   ImGui_ImplSDLRenderer2_Shutdown();
   ImGui_ImplSDL2_Shutdown();
   ImGui::DestroyContext();
   SDL_DestroyRenderer(a.renderer);
   SDL_DestroyWindow(a.window);
   SDL_Quit();
   return 0;
}
