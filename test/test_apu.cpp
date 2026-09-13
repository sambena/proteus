// SPDX-License-Identifier: LGPL-2.1-or-later
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "apu_analyzer.h"
#include "snes_rom.h"
#include <zlib.h>

static int g_failures = 0;

#define TEST(expr, msg) do { \
   if (!(expr)) { \
      printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_failures++; \
   } else { \
      printf("  ok: %s\n", msg); \
   } \
} while (0)

static void test_synthetic_lorom()
{
   printf("scenario: Static 65816 APU analysis - LoROM latching (Zelda style)\n");

   // Construct a 32KB LoROM image with internal header
   std::vector<uint8_t> rom_data(0x8000, 0xEA); // fill with NOPs

   // Internal header at 0x7FC0
   const char *title = "ZELDA TEST           ";
   for (int i = 0; i < 21; i++)
      rom_data[0x7FC0 + i] = (uint8_t)title[i];
   rom_data[0x7FD5] = 0x20; // LoROM
   rom_data[0x7FDC] = 0x00; // Checksum complement
   rom_data[0x7FDD] = 0x00;
   rom_data[0x7FDE] = 0xFF; // Checksum
   rom_data[0x7FDF] = 0xFF;
   rom_data[0x7FFC] = 0x00; // Reset vector -> $8000
   rom_data[0x7FFD] = 0x80;

   // Put APU code at offset 0x0100 (SNES $008100)
   // AD 2C 01   LDA $012C
   // 8D 40 21   STA $2140
   // 9C 2C 01   STZ $012C
   // 60         RTS
   size_t code_off = 0x0100;
   rom_data[code_off + 0] = 0xAD; rom_data[code_off + 1] = 0x2C; rom_data[code_off + 2] = 0x01;
   rom_data[code_off + 3] = 0x8D; rom_data[code_off + 4] = 0x40; rom_data[code_off + 5] = 0x21;
   rom_data[code_off + 6] = 0x9C; rom_data[code_off + 7] = 0x2C; rom_data[code_off + 8] = 0x01;
   rom_data[code_off + 9] = 0x60;

   SnesRom rom;
   TEST(rom.load(rom_data), "load synthetic LoROM");
   TEST(rom.map == SnesRom::LOROM, "detected LoROM");

   ApuAnalysisResult res = analyze_snes_apu(rom);
   TEST(res.found, "found APU communication");
   TEST(!res.candidates.empty(), "has candidates");
   if (!res.candidates.empty())
   {
      const auto &top = res.candidates.front();
      TEST(top.address.address == 0x012C, "discovered song address $012C");
      TEST(top.address.latch == true, "discovered latching (STZ $012C)");
      TEST(top.port == 0, "identified port $2140");
      TEST(top.score >= 60, "high confidence score");
   }
}

static void test_synthetic_hirom_command_block()
{
   printf("scenario: Static 65816 APU analysis - HiROM command block & Direct Page\n");

   // Construct a 64KB HiROM image with internal header
   std::vector<uint8_t> rom_data(0x10000, 0xEA);

   // Internal header at 0xFFC0
   const char *title = "CHRONO TEST          ";
   for (int i = 0; i < 21; i++)
      rom_data[0xFFC0 + i] = (uint8_t)title[i];
   rom_data[0xFFD5] = 0x21; // HiROM
   rom_data[0xFFDC] = 0x00;
   rom_data[0xFFDD] = 0x00;
   rom_data[0xFFDE] = 0xFF;
   rom_data[0xFFDF] = 0xFF;
   rom_data[0xFFFC] = 0x00;
   rom_data[0xFFFD] = 0x80;

   // Sound routine entry at offset 0x0140 (SNES $C00140):
   // A2 00 1E   LDX #$1E00
   // DA         PHX
   // 2B         PLD         (Direct page = $1E00)
   size_t code_off = 0x0140;
   rom_data[code_off + 0] = 0xA2; rom_data[code_off + 1] = 0x00; rom_data[code_off + 2] = 0x1E;
   rom_data[code_off + 3] = 0xDA;
   rom_data[code_off + 4] = 0x2B;

   // Port copy routine at offset 0x0200:
   // A5 03      LDA $03
   // 8D 43 21   STA $2143
   // A5 02      LDA $02
   // 8D 42 21   STA $2142
   // A5 01      LDA $01
   // 8D 41 21   STA $2141
   // A5 00      LDA $00
   // 8D 40 21   STA $2140
   // 6B         RTL
   size_t port_off = 0x0200;
   rom_data[port_off + 0] = 0xA5; rom_data[port_off + 1] = 0x03;
   rom_data[port_off + 2] = 0x8D; rom_data[port_off + 3] = 0x43; rom_data[port_off + 4] = 0x21;
   rom_data[port_off + 5] = 0xA5; rom_data[port_off + 6] = 0x02;
   rom_data[port_off + 7] = 0x8D; rom_data[port_off + 8] = 0x42; rom_data[port_off + 9] = 0x21;
   rom_data[port_off + 10] = 0xA5; rom_data[port_off + 11] = 0x01;
   rom_data[port_off + 12] = 0x8D; rom_data[port_off + 13] = 0x41; rom_data[port_off + 14] = 0x21;
   rom_data[port_off + 15] = 0xA5; rom_data[port_off + 16] = 0x00;
   rom_data[port_off + 17] = 0x8D; rom_data[port_off + 18] = 0x40; rom_data[port_off + 19] = 0x21;
   rom_data[port_off + 20] = 0x6B;

   // Nearby command table at offset 0x0300:
   // 10 00 FF FF
   // 10 01 FF FF
   // 10 02 FF FF
   // 10 03 FF FF
   size_t tab_off = 0x0300;
   for (int i = 0; i < 4; i++)
   {
      rom_data[tab_off + i * 4 + 0] = 0x10;
      rom_data[tab_off + i * 4 + 1] = (uint8_t)i;
      rom_data[tab_off + i * 4 + 2] = 0xFF;
      rom_data[tab_off + i * 4 + 3] = 0xFF;
   }

   // Jump table at offset 0x0004:
   // 4C 40 01   JMP $0140
   rom_data[0x0004] = 0x4C; rom_data[0x0005] = 0x40; rom_data[0x0006] = 0x01;

   SnesRom rom;
   TEST(rom.load(rom_data), "load synthetic HiROM");
   TEST(rom.map == SnesRom::HIROM, "detected HiROM");

   ApuAnalysisResult res = analyze_snes_apu(rom);
   TEST(res.found, "found APU communication");
   TEST(!res.candidates.empty(), "has candidates");
   if (!res.candidates.empty())
   {
      const auto &top = res.candidates.front();
      TEST(top.address.address == 0x1E00, "resolved DP to WRAM $1E00");
      TEST(top.is_command_block, "detected 4-port command block");
      // A nearby command table only suggests how songs start; the song address stays unpatterned.
      TEST(top.address.bytes.empty(), "song address has no guessed pattern");
      TEST(top.start.fill_block && top.start.bytes.size() == 4, "suggested start fills a 4-byte command");
      if (top.start.bytes.size() == 4)
      {
         TEST(top.start.bytes[0] == 0x10 && top.start.bytes[2] == 0xFF, "suggested command 10 xx FF FF");
         TEST(top.start.offset == 1, "song offset is 1");
      }
   }
}

static bool load_rom_file(const std::string &path, std::vector<uint8_t> &out)
{
   FILE *f = fopen(path.c_str(), "rb");
   if (!f) return false;
   fseek(f, 0, SEEK_END);
   size_t sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   std::vector<uint8_t> data(sz);
   if (fread(data.data(), 1, sz, f) != sz) { fclose(f); return false; }
   fclose(f);

   if (path.size() > 4 && path.substr(path.size() - 4) == ".zip")
   {
      if (data.size() < 22) return false;
      size_t eocd = 0;
      for (size_t i = data.size() - 22 + 1; i-- > 0 && data.size() - i <= 65557;)
      {
         uint32_t sig = data[i] | (data[i+1]<<8) | (data[i+2]<<16) | (data[i+3]<<24);
         if (sig == 0x06054b50) { eocd = i; break; }
      }
      if (!eocd) return false;
      uint16_t entries = data[eocd + 10] | (data[eocd + 11]<<8);
      size_t pos = data[eocd + 16] | (data[eocd + 17]<<8) | (data[eocd + 18]<<16) | (data[eocd + 19]<<24);
      for (unsigned e = 0; e < entries && pos + 46 <= data.size(); e++)
      {
         uint32_t sig = data[pos] | (data[pos+1]<<8) | (data[pos+2]<<16) | (data[pos+3]<<24);
         if (sig != 0x02014b50) break;
         uint16_t method = data[pos + 10] | (data[pos + 11]<<8);
         uint32_t csize = data[pos + 20] | (data[pos + 21]<<8) | (data[pos + 22]<<16) | (data[pos + 23]<<24);
         uint32_t usize = data[pos + 24] | (data[pos + 25]<<8) | (data[pos + 26]<<16) | (data[pos + 27]<<24);
         uint16_t nlen = data[pos + 28] | (data[pos + 29]<<8);
         uint16_t xlen = data[pos + 30] | (data[pos + 31]<<8);
         uint16_t clen = data[pos + 32] | (data[pos + 33]<<8);
         uint32_t local = data[pos + 42] | (data[pos + 43]<<8) | (data[pos + 44]<<16) | (data[pos + 45]<<24);
         std::string name((const char*)&data[pos + 46], nlen);
         pos += 46 + nlen + xlen + clen;

         if (name.size() >= 4 && (name.substr(name.size() - 4) == ".sfc" || name.substr(name.size() - 4) == ".smc"))
         {
            if (local + 30 > data.size()) return false;
            size_t dpos = local + 30 + (data[local + 26] | (data[local + 27]<<8)) + (data[local + 28] | (data[local + 29]<<8));
            if (dpos + csize > data.size()) return false;
            out.resize(usize);
            if (method == 0)
            {
               memcpy(out.data(), &data[dpos], usize);
               return true;
            }
            else if (method == 8)
            {
               z_stream zs = {};
               zs.next_in = &data[dpos];
               zs.avail_in = csize;
               zs.next_out = out.data();
               zs.avail_out = usize;
               inflateInit2(&zs, -MAX_WBITS);
               int err = inflate(&zs, Z_FINISH);
               inflateEnd(&zs);
               return err == Z_STREAM_END;
            }
         }
      }
      return false;
   }
   out = data;
   return true;
}

static void test_real_chrono_trigger()
{
   std::vector<uint8_t> data;
   if (!load_rom_file("D:/Roms/Super Nintendo/Chrono Trigger.zip", data))
   {
      printf("scenario: Real ROM - Chrono Trigger (skipped, file not present)\n");
      return;
   }
   printf("scenario: Real ROM - Chrono Trigger\n");
   SnesRom rom;
   TEST(rom.load(data), "load Chrono Trigger ROM");
   ApuAnalysisResult res = analyze_snes_apu(rom);
   TEST(res.found, "found APU communication");
   TEST(!res.candidates.empty(), "has candidates");
   if (!res.candidates.empty())
   {
      const auto &top = res.candidates.front();
      TEST(top.address.address == 0x1E00, "top candidate is the command block $1E00");
      TEST(top.is_command_block, "identified as 4-port command block");
      TEST(top.address.bytes.empty(), "no pattern guessed from unrelated tables");
      TEST(top.start.address == 0xC70004, "identified public routine JSL $C70004");
   }
   bool io = false;
   for (const auto &c : res.candidates)
      io = io || (c.address.address >= 0x2000 && c.address.address < 0x8000);
   TEST(!io, "no I/O registers suggested as song addresses");
}

// Finds `address` among the candidates; returns its rank or -1.
static int rank_of(const ApuAnalysisResult &res, uint32_t address)
{
   for (size_t i = 0; i < res.candidates.size(); i++)
      if (res.candidates[i].address.address == address)
         return (int)i;
   return -1;
}

static void test_real_link_to_the_past()
{
   std::vector<uint8_t> data;
   if (!load_rom_file("D:/Roms/Super Nintendo/Legend of Zelda - A Link to the Past.zip", data))
   {
      printf("scenario: Real ROM - A Link to the Past (skipped, file not present)\n");
      return;
   }
   printf("scenario: Real ROM - A Link to the Past\n");
   SnesRom rom;
   TEST(rom.load(data), "load A Link to the Past ROM");
   ApuAnalysisResult res = analyze_snes_apu(rom);
   int music = rank_of(res, 0x012C);
   TEST(music >= 0, "suggests the music command $012C");
   if (music >= 0)
   {
      TEST(res.candidates[music].address.latch, "$012C is a command register");
      TEST(music < 2, "$012C is among the top two");
   }
}

static void test_real_super_mario_world()
{
   std::vector<uint8_t> data;
   if (!load_rom_file("D:/Roms/Super Nintendo/Super Mario World.zip", data))
   {
      printf("scenario: Real ROM - Super Mario World (skipped, file not present)\n");
      return;
   }
   printf("scenario: Real ROM - Super Mario World\n");
   SnesRom rom;
   TEST(rom.load(data), "load Super Mario World ROM");
   ApuAnalysisResult res = analyze_snes_apu(rom);
   TEST(res.found, "found APU communication");
   TEST(!res.candidates.empty(), "has candidates");
   int music = rank_of(res, 0x1DFB), sfx = rank_of(res, 0x1DF9);
   TEST(music >= 0, "suggests the music command $1DFB");
   if (music >= 0)
   {
      TEST(res.candidates[music].address.latch, "$1DFB is a command register");
      // $2140 carries sound effects in this game: port 0 must not outscore the music port.
      TEST(sfx < 0 || res.candidates[music].score >= res.candidates[sfx].score, "$1DFB scores at least as high as sound effects $1DF9");
   }
}

int main()
{
   test_synthetic_lorom();
   test_synthetic_hirom_command_block();
   test_real_chrono_trigger();
   test_real_link_to_the_past();
   test_real_super_mario_world();

   if (g_failures > 0)
   {
      printf("\nFAILED: %d tests failed\n", g_failures);
      return 1;
   }
   printf("\nPASSED (0 failures)\n");
   return 0;
}
