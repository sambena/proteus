// SPDX-License-Identifier: LGPL-2.1-or-later
// RetroAchievements code notes client: queries the public RA API by ROM MD5 hash
// to discover documented BGM and music RAM addresses.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RaCodeNote
{
   uint32_t address = 0;        // e.g. 0x0523 in system_ram
   int memory = 0;              // 0 = system_ram
   int size = 1;                // 1 or 2 bytes
   std::string address_hex;     // e.g. "$0523"
   std::string note;            // e.g. "Music track playing"
   std::string author;          // e.g. "SporyTike"
   int score = 0;               // relevance ranking score
};

enum class RaLookupStatus
{
   IDLE,
   SEARCHING,
   SUCCESS,
   NO_GAME,                     // ROM hash not found in RetroAchievements
   NO_NOTES,                    // Game found, but no music notes documented
   ERROR_NET                    // Network or HTTP error
};

struct RaLookupResult
{
   RaLookupStatus status = RaLookupStatus::IDLE;
   std::string message;
   int game_id = 0;
   std::vector<RaCodeNote> notes;
};

// Synchronously query RetroAchievements (should be called on a worker thread).
RaLookupResult ra_lookup_music_notes(const std::string &md5_hex);

// JSON parsers exposed for unit testing.
int ra_parse_game_id_json(const std::string &json_str);
std::vector<RaCodeNote> ra_parse_music_notes_json(const std::string &json_str);
