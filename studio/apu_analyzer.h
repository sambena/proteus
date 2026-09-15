// SPDX-License-Identifier: GPL-3.0-or-later
// Static 65816 analysis of SNES ROMs to find APU I/O port communication ($2140-$2143),
// song RAM addresses, command latching, and playback routine entry points.
#pragma once

#include "snes_rom.h"
#include "game_db.h"
#include <cstdint>
#include <string>
#include <vector>

struct ApuCandidate
{
   SongAddress address;
   SongStart start;
   int score = 0;
   uint32_t snes_pc = 0;        // 24-bit SNES bus address of the STA $214x instruction
   uint32_t rom_offset = 0;     // offset in the ROM binary
   int port = 0;                // 0..3 ($2140..$2143)
   bool latched = false;
   bool is_command_block = false;
   std::string source_desc;     // e.g. "LDA $012C (latched by STZ $012C)"
   std::string routine_desc;    // e.g. "Routine JSL $C70004 (subroutine at $C70140)"
};

struct ApuAnalysisResult
{
   bool found = false;
   std::vector<ApuCandidate> candidates;
   ApuCandidate best;
};

// Statically analyzes the unheadered SNES ROM data for 65816 APU communication ($2140-$2143).
// Traces backward from APU stores to determine the RAM addresses holding song IDs,
// whether command register latching is used, and the entry routine.
ApuAnalysisResult analyze_snes_apu(const SnesRom &rom);
