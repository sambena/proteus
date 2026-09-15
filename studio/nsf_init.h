// SPDX-License-Identifier: LGPL-2.1-or-later
// What an .nsf's init routine does to the NES's RAM when it starts a song. NSF rips contain the
// game's own music code, and many start a song the way the game does: by writing a request to
// the RAM its music code polls (The Legend of Zelda: $0600 = 80 for the title song). Running
// init for each song on a small 6502 shows those bytes, which a scan can then write in the game.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The RAM ($0000-$07FF) init leaves written when starting song `song` (from 0): address -> value.
bool nsf_init_ram(const std::vector<uint8_t> &nsf, int song, std::map<uint16_t, uint8_t> &writes, std::string &error);

struct NsfRequest
{
   uint16_t address = 0;                 // RAM the request is written to
   std::map<int, uint8_t> song_values;   // NSF song (from 0) -> value written there
};

// The RAM bytes init writes with a value that depends on the song: for each, the value of every
// song that writes it. Bytes most songs write come first.
std::vector<NsfRequest> nsf_song_requests(const std::vector<uint8_t> &nsf, std::string &error);
