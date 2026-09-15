// SPDX-License-Identifier: GPL-3.0-or-later
#include "apu_analyzer.h"

#include <algorithm>
#include <cstdio>
#include <set>

static std::string hex4(uint32_t v)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "%04X", (unsigned)(v & 0xFFFF));
   return buf;
}

static std::string hex6(uint32_t v)
{
   char buf[16];
   snprintf(buf, sizeof(buf), "%06X", (unsigned)(v & 0xFFFFFF));
   return buf;
}

static uint32_t rom_to_snes(const SnesRom &rom, size_t off)
{
   if (rom.map == SnesRom::HIROM || rom.map == SnesRom::EXHIROM)
   {
      return 0xC00000 + (uint32_t)off;
   }
   else // LOROM
   {
      uint32_t bank = (uint32_t)(off >> 15);
      uint32_t addr = 0x8000 + (uint32_t)(off & 0x7FFF);
      return (bank << 16) | addr;
   }
}

struct RawStore
{
   size_t rom_offset = 0;
   uint32_t snes_addr = 0;
   int port = 0; // 0..3
   int op_len = 3;
   bool indexed = false;
};

// Check if nearby data looks like a 4-byte command pattern table (e.g. 10 00 FF FF, 10 01 FF FF, ...)
static bool find_command_table_pattern(const std::vector<uint8_t> &data, size_t near_offset,
                                      std::vector<uint8_t> &pattern_bytes, int &song_offset)
{
   size_t search_start = near_offset > 2048 ? near_offset - 2048 : 0;
   size_t search_end = std::min(data.size(), near_offset + 2048);

   for (size_t i = search_start; i + 16 <= search_end; i++)
   {
      // Check for 4 entries of 4 bytes each where one byte increments (0, 1, 2, 3)
      // and the other 3 bytes remain constant
      for (int var_pos = 0; var_pos < 4; var_pos++)
      {
         bool match = true;
         uint8_t v0 = data[i + var_pos];
         for (int entry = 0; entry < 4; entry++)
         {
            size_t eoff = i + entry * 4;
            if (data[eoff + var_pos] != (uint8_t)(v0 + entry))
            {
               match = false;
               break;
            }
            for (int b = 0; b < 4; b++)
            {
               if (b != var_pos && data[eoff + b] != data[i + b])
               {
                  match = false;
                  break;
               }
            }
            if (!match)
               break;
         }
         if (match)
         {
            pattern_bytes.clear();
            for (int b = 0; b < 4; b++)
               pattern_bytes.push_back(data[i + b]);
            song_offset = var_pos;
            return true;
         }
      }
   }
   return false;
}

ApuAnalysisResult analyze_snes_apu(const SnesRom &rom)
{
   ApuAnalysisResult result;
   if (rom.data.size() < 0x8000)
      return result;

   const auto &d = rom.data;
   std::vector<RawStore> stores;

   // 1. Scan for APU port store instructions
   for (size_t i = 0; i + 3 <= d.size(); i++)
   {
      // Absolute: STA $2140..$2143 (8D 40 21 .. 8D 43 21)
      if (d[i] == 0x8D && d[i + 2] == 0x21 && d[i + 1] >= 0x40 && d[i + 1] <= 0x43)
      {
         RawStore s;
         s.rom_offset = i;
         s.snes_addr = rom_to_snes(rom, i);
         s.port = d[i + 1] - 0x40;
         s.op_len = 3;
         s.indexed = false;
         stores.push_back(s);
      }
      // Absolute indexed: STA $2140,X .. STA $2143,X (9D 40 21 .. 9D 43 21)
      else if (d[i] == 0x9D && d[i + 2] == 0x21 && d[i + 1] >= 0x40 && d[i + 1] <= 0x43)
      {
         RawStore s;
         s.rom_offset = i;
         s.snes_addr = rom_to_snes(rom, i);
         s.port = d[i + 1] - 0x40;
         s.op_len = 3;
         s.indexed = true;
         stores.push_back(s);
      }
      // Absolute long: STA $002140 .. STA $002143 (8F 40 21 00 .. 8F 43 21 00)
      else if (i + 4 <= d.size() && d[i] == 0x8F && d[i + 2] == 0x21 && d[i + 3] == 0x00 &&
               d[i + 1] >= 0x40 && d[i + 1] <= 0x43)
      {
         RawStore s;
         s.rom_offset = i;
         s.snes_addr = rom_to_snes(rom, i);
         s.port = d[i + 1] - 0x40;
         s.op_len = 4;
         s.indexed = false;
         stores.push_back(s);
      }
   }

   if (stores.empty())
      return result;

   // 2. Identify multi-port blocks: check if 3 or 4 ports are written in close succession
   std::vector<bool> in_command_block(stores.size(), false);
   for (size_t i = 0; i < stores.size(); i++)
   {
      std::set<int> nearby_ports;
      for (size_t j = 0; j < stores.size(); j++)
      {
         size_t diff = stores[j].rom_offset > stores[i].rom_offset
            ? stores[j].rom_offset - stores[i].rom_offset
            : stores[i].rom_offset - stores[j].rom_offset;
         if (diff <= 32)
            nearby_ports.insert(stores[j].port);
      }
      if (nearby_ports.size() >= 3)
         in_command_block[i] = true;
   }

   // 3. Analyze each store site
   for (size_t si = 0; si < stores.size(); si++)
   {
      const auto &st = stores[si];
      size_t off = st.rom_offset;
      // The code's CPU bank: JSR and JMP operands are 16-bit addresses within it. A LoROM
      // bank holds 32KB of ROM at $8000-$FFFF; a HiROM bank holds 64KB.
      bool lorom = rom.map != SnesRom::HIROM && rom.map != SnesRom::EXHIROM;
      size_t bank_mask = lorom ? 0x7FFF : 0xFFFF;
      size_t bank_start = off & ~bank_mask;
      size_t lookback = std::min<size_t>(off, 64);
      size_t start_off = off - lookback;

      // Scan backward for function boundary / entry point. Without decoding the code,
      // an RTS/RTL byte may be an operand ($40 of STA $2140 reads as RTI), so a byte only
      // counts when the bytes before it are not an instruction that takes it as operand.
      auto takes_operand = [&](size_t at) {
         static const uint8_t two[] = { 0xA9, 0xA2, 0xA0, 0x85, 0xA5, 0x86, 0x84, 0x64, 0xC9, 0xE0, 0xC0, 0x29, 0x09, 0x49,
                                        0x69, 0xE9, 0xD0, 0xF0, 0x80, 0x90, 0xB0, 0x10, 0x30, 0x50, 0x70, 0xC2, 0xE2, 0xE6, 0xC6 };
         static const uint8_t three[] = { 0x8D, 0xAD, 0x9C, 0xCD, 0xEE, 0xCE, 0x8E, 0x8C, 0xAE, 0xAC, 0x2C, 0x20, 0x4C, 0x9D,
                                          0xBD, 0x99, 0xB9, 0xEC, 0xCC, 0x0D, 0x2D, 0x4D, 0x6D, 0xED, 0xF4, 0xA2, 0xA0, 0xA9 };
         static const uint8_t four[] = { 0x8F, 0xAF, 0x22, 0x5C, 0x9F, 0xBF, 0xCF };
         auto in = [](uint8_t op, const uint8_t *list, size_t n) { return std::find(list, list + n, op) != list + n; };
         return (at >= 1 && in(d[at - 1], two, sizeof(two))) || (at >= 2 && in(d[at - 2], three, sizeof(three))) ||
                (at >= 3 && in(d[at - 3], four, sizeof(four)));
      };
      size_t func_entry = start_off;
      for (size_t bi = off; bi-- > start_off;)
      {
         // 60 = RTS, 6B = RTL
         if ((d[bi] == 0x60 || d[bi] == 0x6B) && !takes_operand(bi))
         {
            func_entry = bi + 1;
            break;
         }
      }

      // Check if function ends with RTL (called via JSL) or RTS (called via JSR)
      bool is_rtl = false;
      for (size_t fi = off + st.op_len; fi < std::min(d.size(), off + 128); fi++)
      {
         if (d[fi] == 0x6B)
         {
            is_rtl = true;
            break;
         }
         if (d[fi] == 0x60 || d[fi] == 0x40)
            break;
      }

      // Check if the sound driver sets Direct Page (PLD)
      // e.g. LDX #$1E00; PHX; PLD (A2 00 1E DA 2B) or PEA $1E00; PLD (F4 00 1E 2B)
      int dp_base = -1;
      size_t dp_setter_off = 0;
      size_t dp_search_start = (off & ~0xFFFF);
      if (off > 4096 && dp_search_start < off - 4096)
         dp_search_start = off - 4096;
      for (size_t di = dp_search_start; di + 4 <= off; di++)
      {
         if ((d[di] == 0xA2 && d[di + 3] == 0xDA && di + 4 < off && d[di + 4] == 0x2B) ||
             (d[di] == 0xF4 && d[di + 3] == 0x2B))
         {
            dp_base = d[di + 1] | (d[di + 2] << 8);
            dp_setter_off = di;
            // Continue scanning to find the DP setter closest to the APU write
         }
      }

      // Find STZ addresses in the backward window or shortly after the store
      std::set<uint32_t> stz_addrs;
      for (size_t zi = func_entry; zi + 3 <= off; zi++)
      {
         if (d[zi] == 0x9C) // STZ $xxxx
            stz_addrs.insert(d[zi + 1] | (d[zi + 2] << 8));
         else if (d[zi] == 0x64 && dp_base >= 0) // STZ $dd
            stz_addrs.insert((uint32_t)(dp_base + d[zi + 1]));
      }
      for (size_t zi = off + st.op_len; zi + 3 <= std::min(d.size(), off + 24); zi++)
      {
         if (d[zi] == 0x9C)
            stz_addrs.insert(d[zi + 1] | (d[zi + 2] << 8));
         else if (d[zi] == 0x64 && dp_base >= 0)
            stz_addrs.insert((uint32_t)(dp_base + d[zi + 1]));
      }

      // Find LDAs feeding the store
      struct Source
      {
         enum Type { ABS, LONG, DP } type;
         uint32_t addr;
         size_t pos;
      };
      std::vector<Source> sources;

      for (size_t li = func_entry; li < off; li++)
      {
         if (takes_operand(li))
            continue;
         // LDA abs: AD ll hh
         if (d[li] == 0xAD && li + 2 < off)
         {
            uint32_t a = d[li + 1] | (d[li + 2] << 8);
            if (a != 0x2140 && a != 0x2141 && a != 0x2142 && a != 0x2143)
               sources.push_back({ Source::ABS, a, li });
         }
         // LDA long: AF ll hh bb
         else if (d[li] == 0xAF && li + 3 < off)
         {
            uint32_t a = d[li + 1] | (d[li + 2] << 8) | (d[li + 3] << 16);
            sources.push_back({ Source::LONG, a, li });
         }
         // LDA dp: A5 dd
         else if (d[li] == 0xA5 && li + 1 < off)
         {
            uint32_t a = d[li + 1];
            if (dp_base >= 0)
               a += (uint32_t)dp_base;
            sources.push_back({ dp_base >= 0 ? Source::ABS : Source::DP, a, li });
         }
      }

      if (sources.empty())
         continue;

      // The closest LDA before the store is the immediate feeder
      const auto &top_src = sources.back();
      uint32_t ram_addr = top_src.addr;
      int memory_type = 0; // 0 system_ram, 1 save_ram

      if (top_src.type == Source::LONG)
      {
         uint8_t bank = (ram_addr >> 16) & 0xFF;
         if (bank == 0x7E)
            ram_addr &= 0xFFFF;
         else if (bank == 0x7F)
            ram_addr = 0x10000 + (ram_addr & 0xFFFF);
         else if (((bank & 0x7F) < 0x40) && (ram_addr & 0xFFFF) < 0x2000)
            ram_addr &= 0xFFFF;   // low work RAM mirror
         else
            continue;             // ROM, I/O or cartridge RAM: not where a song number is kept
      }

      // $2000-$7FFF in the low banks is I/O and expansion, not work RAM.
      if (top_src.type != Source::LONG && ram_addr >= 0x2000 && ram_addr < 0x8000)
         continue;

      // Check for command register latching
      bool latched = stz_addrs.count(ram_addr) > 0;

      // If func_entry is an internal helper called by JSR $hhll, find the caller
      size_t parent_entry = func_entry;
      uint16_t func_cpu = (uint16_t)(rom_to_snes(rom, func_entry) & 0xFFFF);
      for (size_t ci = bank_start; ci + 3 <= off; ci++)
      {
         if (d[ci] == 0x20 && (uint16_t)(d[ci + 1] | (d[ci + 2] << 8)) == func_cpu)
         {
            // Found caller at ci! Find start of caller function
            for (size_t pbi = ci; pbi >= bank_start; pbi--)
            {
               if (d[pbi] == 0x60 || d[pbi] == 0x6B || d[pbi] == 0x40 || pbi == bank_start)
               {
                  parent_entry = (pbi == bank_start) ? bank_start : (pbi + 1);
                  break;
               }
            }
            break;
         }
      }

      // Collect jump table entries at bank_start (common in SNES sound drivers)
      struct JumpEntry
      {
         uint32_t vector_snes = 0;
         uint16_t target = 0;
         size_t target_rom = 0;
      };
      std::vector<JumpEntry> jump_table;
      for (size_t ji = bank_start; ji + 3 <= bank_start + 64 && ji + 3 <= d.size(); ji++)
      {
         if (d[ji] == 0x4C) // JMP $xxxx
         {
            uint16_t tgt = d[ji + 1] | (d[ji + 2] << 8);
            if (lorom && tgt < 0x8000)
            {
               ji += 2;
               continue;   // RAM or I/O, not this bank's ROM
            }
            JumpEntry je;
            je.vector_snes = rom_to_snes(rom, ji);
            je.target = tgt;
            je.target_rom = bank_start | (tgt & bank_mask);
            jump_table.push_back(je);
            ji += 2;
         }
      }

      // Check for public jump table entry (JMP $hhll or JML $bbhhll)
      uint32_t public_entry = rom_to_snes(rom, parent_entry);
      if (!jump_table.empty())
      {
         // Find jump table entry that contains dp_setter_off or parent_entry
         size_t needle = dp_setter_off ? dp_setter_off : parent_entry;
         for (size_t k = 0; k < jump_table.size(); k++)
         {
            size_t start_r = jump_table[k].target_rom;
            size_t end_r = (k + 1 < jump_table.size()) ? jump_table[k + 1].target_rom : (start_r + 0x800);
            if (needle >= start_r && needle < end_r)
            {
               public_entry = jump_table[k].vector_snes;
               is_rtl = true;
               break;
            }
         }
      }

      // Build Candidate
      ApuCandidate cand;
      cand.snes_pc = st.snes_addr;
      cand.rom_offset = (uint32_t)st.rom_offset;
      cand.port = st.port;
      cand.latched = latched;
      cand.is_command_block = in_command_block[si];

      cand.address.known = true;
      cand.address.address = ram_addr;
      cand.address.memory = memory_type;
      cand.address.size = 1;
      cand.address.latch = latched;
      cand.address.debounce = 1;

      cand.start.kind = SongStart::ROUTINE;
      cand.start.address = public_entry;
      cand.start.jsr = !is_rtl;
      cand.start.settle_frames = 60;

      // Command block details
      if (cand.is_command_block)
      {
         if (dp_base >= 0x2000 && dp_base < 0x8000)
            continue;   // direct page on I/O registers
         if (dp_base >= 0)
         {
            cand.address.address = (uint32_t)dp_base;
            ram_addr = (uint32_t)dp_base;
         }
         // A table of commands near the port writes suggests how the block is filled. It may
         // belong to other code, so it only goes to the suggested start, which scans check,
         // not to the song address.
         std::vector<uint8_t> pat;
         int song_off = 0;
         if (find_command_table_pattern(d, off, pat, song_off))
         {
            cand.start.fill_block = true;
            cand.start.block = ram_addr;
            cand.start.bytes = pat;
            cand.start.offset = song_off;
         }
      }

      // Base heuristic score. Which port carries music differs between games (Super Mario
      // World uses $2142 for music and $2140 for sound effects), so ports are not scored.
      int score = 10;
      if (latched)
         score += 35; // Latching is an extremely high confidence indicator
      if (cand.is_command_block)
         score += 30; // 4-port handshake command blocks (e.g. Chrono Trigger)
      if (dp_base >= 0)
         score += 15; // Direct Page setup detected

      if (ram_addr < 0x2000)
         score += 20; // Low WRAM / scratch
      else if (ram_addr < 0x10000)
         score += 10;

      cand.score = score;

      char sbuf[128];
      snprintf(sbuf, sizeof(sbuf), "LDA $%s (Port $%04X, latched=%s)",
               hex4(ram_addr).c_str(), 0x2140 + st.port, latched ? "yes" : "no");
      cand.source_desc = sbuf;

      char rbuf[128];
      snprintf(rbuf, sizeof(rbuf), "%s $%s (subroutine at $%s)",
               is_rtl ? "JSL" : "JSR", hex6(public_entry).c_str(), hex6(rom_to_snes(rom, func_entry)).c_str());
      cand.routine_desc = rbuf;

      result.candidates.push_back(cand);
   }

   // Sort candidates by score descending
   std::sort(result.candidates.begin(), result.candidates.end(),
             [](const ApuCandidate &a, const ApuCandidate &b) {
                if (a.score != b.score)
                   return a.score > b.score;
                return a.port < b.port;
             });

   // Deduplicate candidates that point to the exact same RAM address & pattern
   std::vector<ApuCandidate> unique;
   for (const auto &c : result.candidates)
   {
      bool dup = false;
      for (const auto &u : unique)
      {
         if (u.address.address == c.address.address && u.address.bytes == c.address.bytes)
         {
            dup = true;
            break;
         }
      }
      if (!dup)
         unique.push_back(c);
   }
   result.candidates = unique;

   if (!result.candidates.empty())
   {
      result.found = true;
      result.best = result.candidates.front();
   }

   return result;
}
