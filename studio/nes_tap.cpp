// SPDX-License-Identifier: LGPL-2.1-or-later
#include "nes_tap.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// 6502 instruction lengths (official opcodes; 0 for the rest).
int op_length(uint8_t op)
{
   int mode = (op >> 2) & 7;
   switch (op & 3)
   {
      case 1:   // ORA AND EOR ADC STA LDA CMP SBC
         return mode == 3 || mode == 6 || mode == 7 ? 3 : 2;
      case 2:   // ASL ROL LSR ROR STX LDX DEC INC
         if (mode == 0)
            return op == 0xA2 ? 2 : 0;
         if (mode == 1 || mode == 5)
            return 2;
         if (mode == 2 || mode == 6)
            return 1;
         return mode == 3 || mode == 7 ? 3 : 0;
      case 0:
         if (mode == 0)
         {
            if (op == 0x20)
               return 3;
            if (op == 0x00 || op == 0x40 || op == 0x60)
               return 1;
            return op == 0xA0 || op == 0xC0 || op == 0xE0 ? 2 : 0;
         }
         if (mode == 1 || mode == 4 || mode == 5)
            return 2;
         if (mode == 2 || mode == 6)
            return 1;
         return 3;
   }
   return 0;
}

bool is_branch(uint8_t op) { return (op & 0x1F) == 0x10; }

// Instructions with a 16-bit address operand.
bool takes_address(uint8_t op)
{
   int mode = (op >> 2) & 7;
   return op_length(op) == 3 && op != 0x20 && op != 0x4C && op != 0x6C && mode != 0 &&
         !((op & 3) == 1 && mode == 0);
}
bool indexed(uint8_t op)
{
   int mode = (op >> 2) & 7;
   return mode == 6 || mode == 7;
}

struct Prg
{
   const uint8_t *data = nullptr;
   size_t size = 0;
   int mapper = 0;
};

bool parse_ines(const std::vector<uint8_t> &ines, Prg &prg, std::string &error)
{
   if (ines.size() < 16 || memcmp(ines.data(), "NES\x1A", 4))
   {
      error = "not an iNES ROM";
      return false;
   }
   size_t trainer = (ines[6] & 4) ? 512 : 0;
   prg.size = (size_t)ines[4] * 0x4000;
   prg.mapper = (ines[6] >> 4) | (ines[7] & 0xF0);
   if (!prg.size || 16 + trainer + prg.size > ines.size())
   {
      error = "the ROM is shorter than its header says";
      return false;
   }
   prg.data = ines.data() + 16 + trainer;
   return true;
}

struct Region
{
   size_t prg = 0;      // PRG offset of its first byte
   uint16_t cpu = 0;    // CPU address it is read at
   size_t length = 0;
};

// Parts of PRG ROM mapped where the tap can count on them while the routine runs: the bank
// the mapper keeps in place, then the routine's own 8KB.
std::vector<Region> stub_regions(const Prg &prg, size_t routine_prg, uint16_t routine)
{
   std::vector<Region> out;
   switch (prg.mapper)
   {
      case 4: case 118: case 119:   // MMC3: the last 8KB at $E000
         out.push_back({ prg.size - 0x2000, 0xE000, 0x2000 });
         break;
      case 1: case 2: case 71:      // MMC1 (usual mode), UxROM: the last 16KB at $C000
         out.push_back({ prg.size - 0x4000, 0xC000, 0x4000 });
         break;
      case 0: case 3:               // no PRG banking
         if (prg.size == 0x4000)
            out.push_back({ 0, 0xC000, 0x4000 });
         else
            out.push_back({ 0, 0x8000, std::min<size_t>(prg.size, 0x8000) });
         break;
   }
   out.push_back({ routine_prg & ~(size_t)0x1FFF, (uint16_t)(routine & 0xE000), 0x2000 });
   return out;
}

}  // namespace

std::string NesTap::fceumm_cheat() const
{
   std::string out;
   char buf[16];
   for (const auto &p : patches)
   {
      snprintf(buf, sizeof(buf), "%s%04X?%02X:%02X", out.empty() ? "" : "+", p.address, p.compare, p.value);
      out += buf;
   }
   return out;
}

std::string fceumm_cheat(uint16_t address, const std::vector<uint8_t> &original, const std::vector<uint8_t> &bytes)
{
   NesTap t;
   for (size_t i = 0; i < bytes.size() && i < original.size(); i++)
      t.patches.push_back({ (uint16_t)(address + i), original[i], bytes[i] });
   return t.fceumm_cheat();
}

bool nes_rom_holds(const std::vector<uint8_t> &ines, uint16_t address, const std::vector<uint8_t> &code)
{
   Prg prg;
   std::string error;
   if (!parse_ines(ines, prg, error) || code.empty() || address < 0x8000)
      return false;
   for (size_t o = address & 0x1FFF; o + code.size() <= prg.size; o += 0x2000)
      if (!memcmp(prg.data + o, code.data(), code.size()))
         return true;
   return false;
}

bool nes_tap_design(const std::vector<uint8_t> &ines, uint16_t routine, const std::vector<uint8_t> &routine_code,
      uint16_t ram, NesTap &tap, std::string &error)
{
   Prg prg;
   if (!parse_ines(ines, prg, error))
      return false;
   if (routine < 0x8000 || ram >= 0x0800)
   {
      error = "the routine must be in ROM and the tap byte in RAM";
      return false;
   }
   size_t probe = std::min<size_t>(routine_code.size(), 16);
   if (probe < 8)
   {
      error = "too little of the routine's code to find it";
      return false;
   }

   // The routine's code, where its 8KB bank lines up with its CPU address.
   size_t at = SIZE_MAX;
   for (size_t o = 0; o + probe <= prg.size && at == SIZE_MAX; o++)
      if ((o & 0x1FFF) == (routine & 0x1FFF) && !memcmp(prg.data + o, routine_code.data(), probe))
         at = o;
   if (at == SIZE_MAX)
   {
      error = "the routine's code is not in the ROM";
      return false;
   }

   // Move whole instructions until the jump to the stub (3 bytes) fits.
   std::vector<uint8_t> moved;
   std::vector<uint16_t> starts;   // where moved instructions other than the first begin
   size_t covered = 0;
   bool ends = false;
   while (covered < 3)
   {
      if (covered)
         starts.push_back((uint16_t)(routine + covered));
      if (at + covered >= prg.size)
         break;
      uint8_t op = prg.data[at + covered];
      int len = op_length(op);
      if (!len || at + covered + len > prg.size)
      {
         error = "the routine starts with an instruction the tap does not know";
         return false;
      }
      if (is_branch(op))
      {
         // A relative branch moves as its opposite over a jump to the same place.
         uint16_t target = (uint16_t)(routine + covered + 2 + (int8_t)prg.data[at + covered + 1]);
         moved.insert(moved.end(), { (uint8_t)(op ^ 0x20), 0x03, 0x4C, (uint8_t)target, (uint8_t)(target >> 8) });
      }
      else
         moved.insert(moved.end(), prg.data + at + covered, prg.data + at + covered + len);
      covered += len;
      if (op == 0x4C || op == 0x6C || op == 0x60 || op == 0x40)
      {
         ends = true;
         break;
      }
   }
   if (covered < 3)
   {
      error = "the routine returns before there is room for a jump";
      return false;
   }

   // Nothing may jump to a moved instruction but the first: code in the routine's 16KB or in the
   // last 16KB (a fixed bank for most mappers) that jumps there. A jump into the middle of one
   // is to another bank mapped at the same address.
   auto moved_start = [&](long address) { return std::find(starts.begin(), starts.end(), address) != starts.end(); };
   for (size_t o = 0; o + 3 <= prg.size; o++)
   {
      if ((o & ~(size_t)0x3FFF) != (at & ~(size_t)0x3FFF) && o < prg.size - 0x4000)
         continue;
      uint8_t op = prg.data[o];
      uint16_t operand = (uint16_t)(prg.data[o + 1] | prg.data[o + 2] << 8);
      if ((op == 0x20 || op == 0x4C) && moved_start(operand))
      {
         error = "other code jumps into the routine's first instructions";
         return false;
      }
   }
   for (size_t o = at > 128 ? at - 128 : 0; o + 2 <= prg.size && o < at + covered + 128; o++)
   {
      if (!is_branch(prg.data[o]) || (o >= at && o < at + covered))
         continue;
      long target = (long)routine + ((long)o - (long)at) + 2 + (int8_t)prg.data[o + 1];
      if (moved_start(target))
      {
         error = "a branch lands in the routine's first instructions";
         return false;
      }
   }

   // PHP, STA ram, INC ram, PLP (the request + 1, A and flags kept); the moved instructions; JMP back.
   std::vector<uint8_t> stub = { 0x08, 0x8D, (uint8_t)ram, (uint8_t)(ram >> 8), 0xEE, (uint8_t)ram, (uint8_t)(ram >> 8), 0x28 };
   stub.insert(stub.end(), moved.begin(), moved.end());
   if (!ends)
   {
      uint16_t back = (uint16_t)(routine + covered);
      stub.insert(stub.end(), { 0x4C, (uint8_t)back, (uint8_t)(back >> 8) });
   }

   // Blank ROM (a run of FF or 00) with room to spare on both sides.
   const size_t margin = 2;
   for (const Region &r : stub_regions(prg, at, routine))
   {
      for (uint8_t fill : { (uint8_t)0xFF, (uint8_t)0x00 })
      {
         size_t run = 0;
         for (size_t i = 0; i < r.length; i++)
         {
            uint16_t cpu = (uint16_t)(r.cpu + i);
            run = prg.data[r.prg + i] == fill && cpu < 0xFFFA ? run + 1 : 0;
            if (run < stub.size() + 2 * margin)
               continue;
            size_t first = i + 1 - run + margin;
            tap = NesTap();
            tap.routine = routine;
            tap.stub = (uint16_t)(r.cpu + first);
            tap.ram = ram;
            uint8_t jump[3] = { 0x4C, (uint8_t)tap.stub, (uint8_t)(tap.stub >> 8) };
            for (int k = 0; k < 3; k++)
               tap.patches.push_back({ (uint16_t)(routine + k), prg.data[at + k], jump[k] });
            for (size_t k = 0; k < stub.size(); k++)
               tap.patches.push_back({ (uint16_t)(tap.stub + k), fill, stub[k] });
            return true;
         }
      }
   }
   error = "no blank ROM space for the stub";
   return false;
}

std::vector<uint16_t> nes_unnamed_ram(const std::vector<uint8_t> &ines)
{
   Prg prg;
   std::string error;
   std::vector<uint16_t> out;
   if (!parse_ines(ines, prg, error))
      return out;
   std::vector<bool> named(0x800, false);
   // Without decoding, data bytes also count as instructions: that only names more bytes.
   for (size_t o = 0; o + 3 <= prg.size; o++)
   {
      uint8_t op = prg.data[o];
      if (!takes_address(op))
         continue;
      uint16_t a = (uint16_t)(prg.data[o + 1] | prg.data[o + 2] << 8);
      if (a >= 0x0800)
         continue;
      // An indexed access walks a table from its address; the first bytes of it count.
      size_t reach = indexed(op) ? 8 : 1;
      for (size_t k = 0; k < reach && a + k < 0x800; k++)
         named[a + k] = true;
   }
   for (int a = 0x7FF; a >= 0x200; a--)
      if (!named[a])
         out.push_back((uint16_t)a);
   return out;
}
