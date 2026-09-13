// SPDX-License-Identifier: LGPL-2.1-or-later
// An SNES ROM image: its header, checksum, and where CPU addresses land in it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SnesRom
{
   std::vector<uint8_t> data;   // without a copier header
   enum Map { LOROM, HIROM, EXHIROM } map = LOROM;
   std::string title;           // from the internal header
   uint32_t crc32 = 0;          // of `data`: identifies the exact ROM
   std::string md5;             // 32-char lowercase hex of `data` (unheadered), used by RetroAchievements

   // Takes the ROM file contents (as loaded by the core). False if it is not an SNES ROM.
   bool load(const std::vector<uint8_t> &content);
   // The ROM bytes at a CPU address, or nullptr where the address is not ROM.
   const uint8_t *at(uint32_t address, size_t length = 1) const;
};
