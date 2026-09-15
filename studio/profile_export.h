// SPDX-License-Identifier: GPL-3.0-or-later
// Writes the Proteus profile for the game being changed: its song address, the
// songs mapped to replacements, and the music files copied next to the profile.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "rom_session.h"

struct Assignment
{
   enum Kind { ORIGINAL, SILENCE, FILE } kind = ORIGINAL;
   std::string path;    // FILE: the song file (a rip from the source ROM, or any music file)
   unsigned track = 1;  // FILE: song inside multi-song files
   std::string label;   // "Chrono Trigger: Battle Theme"
};

struct ProfileOptions
{
   int music_volume = 100;
   int game_volume = 100;
   int crossfade_ms = 400;
   std::map<std::string, std::string> mute;   // core option -> value while replacing
};

using Assignments = std::map<uint32_t, Assignment>;

// Where the profile for `target` goes: its existing profile, or system/proteus/<game>.ini.
std::string profile_path_for(const RomSession &target, const std::string &system_dir);

// The profile text. With `copy_plan`, song files are renamed into music/<game>/ and
// each (source, destination) is added to the plan; without it, paths stay absolute.
std::string profile_text(RomSession &target, const Assignments &assignments, const ProfileOptions &options,
      const std::string &profile_path, std::vector<std::pair<std::string, std::string>> *copy_plan);

// Copies the music and writes the profile. Returns false with `error` set on failure.
bool export_profile(RomSession &target, const Assignments &assignments, const ProfileOptions &options,
      const std::string &profile_path, int &files_copied, std::string &error);

// Reads the mapping and options back from an existing profile.
void load_profile_mapping(const std::string &profile_path, Assignments &assignments, ProfileOptions &options);
