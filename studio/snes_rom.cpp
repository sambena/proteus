// SPDX-License-Identifier: LGPL-2.1-or-later
#include "snes_rom.h"

#include <algorithm>

#include <zlib.h>

// How much a header at `base` looks like the real one: a matching checksum pair, a map
// mode byte for this mapping, and a reset vector into ROM.
static int header_score(const std::vector<uint8_t> &d, size_t base, uint8_t map_low_nibble)
{
   if (d.size() < base + 0x40)
      return -1;
   int score = 0;
   uint16_t comp = d[base + 0x1C] | d[base + 0x1D] << 8;
   uint16_t sum = d[base + 0x1E] | d[base + 0x1F] << 8;
   if ((uint16_t)(sum + comp) == 0xFFFF)
      score += 4;
   if ((d[base + 0x15] & 0x0F) == map_low_nibble)
      score += 2;
   uint16_t reset = d[base + 0x3C] | d[base + 0x3D] << 8;
   if (reset >= 0x8000)
      score += 1;
   for (int i = 0; i < 21; i++)
      if (d[base + i] < 0x20 || d[base + i] > 0x7E)
         return score - 1;
   return score + 1;
}

bool SnesRom::load(const std::vector<uint8_t> &content)
{
   data = content;
   if (data.size() % 0x400 == 0x200)
      data.erase(data.begin(), data.begin() + 0x200);
   if (data.size() < 0x8000)
      return false;

   int lo = header_score(data, 0x7FC0, 0x0);
   int hi = header_score(data, 0xFFC0, 0x1);
   int exhi = header_score(data, 0x40FFC0, 0x5);
   size_t base = 0x7FC0;
   map = LOROM;
   if (hi > lo && hi >= exhi)
   {
      map = HIROM;
      base = 0xFFC0;
   }
   else if (exhi > lo && exhi > hi)
   {
      map = EXHIROM;
      base = 0x40FFC0;
   }
   if (std::max({ lo, hi, exhi }) < 4)
      return false;

   title.assign((const char*)&data[base], 21);
   while (!title.empty() && (title.back() == ' ' || title.back() == 0))
      title.pop_back();
   crc32 = (uint32_t)::crc32(0, data.data(), (uInt)data.size());
   return true;
}

const uint8_t *SnesRom::at(uint32_t address, size_t length) const
{
   uint8_t bank = (address >> 16) & 0xFF;
   uint16_t addr = address & 0xFFFF;
   if (bank == 0x7E || bank == 0x7F)
      return nullptr;
   size_t off;
   switch (map)
   {
      case LOROM:
         if (addr < 0x8000)
            return nullptr;
         off = ((size_t)(bank & 0x7F) << 15) | (addr & 0x7FFF);
         break;
      case HIROM:
         if ((bank & 0x40) == 0 && addr < 0x8000)
            return nullptr;
         off = address & 0x3FFFFF;
         break;
      default:   // EXHIROM: banks C0-FF are the first 4 MB, 40-7D the rest
         if ((bank & 0x40) == 0 && addr < 0x8000)
            return nullptr;
         off = (address & 0x3FFFFF) | ((bank & 0x80) ? 0 : 0x400000);
         break;
   }
   return off + length <= data.size() ? &data[off] : nullptr;
}
