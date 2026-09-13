// SPDX-License-Identifier: LGPL-2.1-or-later
// Turns a snes9x save state into an .spc file: the SNES sound CPU's RAM, registers
// and DSP at that moment. Played back by libgme, it continues the song that was
// playing, with its own loops, independent of the game.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SpcTags
{
   std::string song;
   std::string game;
   std::string dumper = "Proteus Studio";
   std::string comment;
};

struct SpcState
{
   bool mid_opcode;   // the sound CPU was stopped inside an instruction
};

// Finds where this snes9x version keeps the game's writes to the sound CPU. The
// layout differs between versions; `round_trip` loads a state and saves it again.
bool spc_calibrate_snes9x(const std::vector<uint8_t> &state,
      const std::function<std::vector<uint8_t>(const std::vector<uint8_t> &)> &round_trip);

// The four bytes the game last wrote to the sound CPU ($2140-$2143).
bool spc_snes9x_ports(const std::vector<uint8_t> &state, uint8_t ports[4]);
// Builds an .spc file from a snes9x libretro save state (retro_serialize data).
bool spc_from_snes9x_state(const std::vector<uint8_t> &state, const SpcTags &tags,
      std::vector<uint8_t> &spc, SpcState *info, std::string &error);
