// SPDX-License-Identifier: LGPL-2.1-or-later
// A tap on an NES game's sound routine: cheat codes that make the game write each sound it
// requests to a byte of RAM, for games whose music code keeps no song number there.
//
// An .nsf rip names the game's "play this sound" routine (its init calls it with the song in
// A; see nsf_init_calls) and holds its code as the ROM does. The tap moves the routine's first
// instructions into a stub in blank ROM space, which stores A + 1 (so request 0 still reads as
// a change) and carries on into the routine. Cheats patch what the CPU reads, each checked
// against the byte it replaces, so the tap does nothing where the ROM differs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct NesRomPatch
{
   uint16_t address = 0;   // CPU address
   uint8_t compare = 0;    // the ROM byte there
   uint8_t value = 0;      // what the CPU reads instead
};

struct NesTap
{
   uint16_t routine = 0;              // the game's sound routine
   uint16_t stub = 0;                 // where the stub sits
   uint16_t ram = 0;                  // the byte the stub writes
   std::vector<NesRomPatch> patches;

   // FCEUmm's form: "8106?C9:4C+8107?F0:E0+...".
   std::string fceumm_cheat() const;
};

// Designs a tap on `routine`, whose first bytes the .nsf holds as `routine_code`, in the iNES
// ROM `ines`, writing to `ram` ($0000-$07FF). False with a reason when the routine's code is
// not in the ROM, its first instructions cannot move, or there is no blank space for the stub.
bool nes_tap_design(const std::vector<uint8_t> &ines, uint16_t routine, const std::vector<uint8_t> &routine_code,
      uint16_t ram, NesTap &tap, std::string &error);

// Whether the iNES ROM holds `code` where a bank would put it at CPU `address` ($8000-$FFFF):
// code found in an .nsf is the game's own only where it is.
bool nes_rom_holds(const std::vector<uint8_t> &ines, uint16_t address, const std::vector<uint8_t> &code);

// FCEUmm cheat codes making `address` onwards read `bytes` where the ROM holds `original`.
std::string fceumm_cheat(uint16_t address, const std::vector<uint8_t> &original, const std::vector<uint8_t> &bytes);

// RAM bytes ($0200-$07FF) no instruction in the ROM reads or writes by address, from the end
// of RAM down: places a tap can write that the game's code does not name. Indexed access can
// still reach them, so a tap's byte is also checked while the game plays.
std::vector<uint16_t> nes_unnamed_ram(const std::vector<uint8_t> &ines);
