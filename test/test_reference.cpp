// SPDX-License-Identifier: LGPL-2.1-or-later
// Reference songs: matching rips, finding a song table in a ROM, names and imports.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "platform.h"
#include "reference.h"
#include "snes_rom.h"

static int g_failures = 0;

#define TEST(expr, msg) do { \
   if (!(expr)) { \
      printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_failures++; \
   } else { \
      printf("  ok: %s\n", msg); \
   } \
} while (0)

static uint32_t g_seed = 12345;
static uint8_t rnd()
{
   g_seed = g_seed * 1103515245 + 12345;
   return (uint8_t)(g_seed >> 16);
}

static const int kSongs = 8;
static const size_t kTable = 0x1000;      // ROM offset of the song table
static const uint32_t kTableCpu = 0x809000;

struct Fixture
{
   std::vector<uint8_t> rom;
   std::vector<std::vector<uint8_t>> song_data;   // each song's sequence
   std::vector<uint8_t> driver, samples;
};

static uint32_t lorom_cpu(size_t offset)
{
   return 0x800000 | (uint32_t)(offset >> 15) << 16 | (uint32_t)(offset & 0x7FFF) | 0x8000;
}

static Fixture make_fixture()
{
   Fixture f;
   f.rom.assign(0x80000, 0x00);
   const char *title = "REFERENCE TEST       ";
   memcpy(&f.rom[0x7FC0], title, 21);
   f.rom[0x7FD5] = 0x20;
   f.rom[0x7FDC] = 0x00; f.rom[0x7FDD] = 0x00;
   f.rom[0x7FDE] = 0xFF; f.rom[0x7FDF] = 0xFF;
   f.rom[0x7FFC] = 0x00; f.rom[0x7FFD] = 0x80;

   // Code that reads the table: LDA $809000,X
   f.rom[0x0200] = 0xBF;
   f.rom[0x0201] = (uint8_t)kTableCpu;
   f.rom[0x0202] = (uint8_t)(kTableCpu >> 8);
   f.rom[0x0203] = (uint8_t)(kTableCpu >> 16);

   // Songs: a 2-byte length, then the sequence; the table points at the length.
   size_t at = 0x20000;
   for (int i = 0; i < kSongs; i++)
   {
      std::vector<uint8_t> seq(300 + i * 97);
      for (auto &b : seq)
         b = rnd();
      f.rom[at] = (uint8_t)seq.size();
      f.rom[at + 1] = (uint8_t)(seq.size() >> 8);
      memcpy(&f.rom[at + 2], seq.data(), seq.size());
      uint32_t cpu = lorom_cpu(at);
      f.rom[kTable + i * 3] = (uint8_t)cpu;
      f.rom[kTable + i * 3 + 1] = (uint8_t)(cpu >> 8);
      f.rom[kTable + i * 3 + 2] = (uint8_t)(cpu >> 16);
      f.song_data.push_back(seq);
      at += 2 + seq.size() + 64;
   }
   f.driver.resize(0x800);
   for (auto &b : f.driver)
      b = rnd();
   f.samples.resize(0x1000);
   for (auto &b : f.samples)
      b = rnd();
   return f;
}

// An .spc of song `song`, dumped after song `previous` played (its data lingers past the end).
static std::vector<uint8_t> make_spc(const Fixture &f, int song, int previous, const char *title)
{
   std::vector<uint8_t> spc(0x10200, 0);
   memcpy(spc.data(), "SNES-SPC700 Sound File Data v0.30", 33);
   spc[0x21] = 26; spc[0x22] = 26; spc[0x23] = 26; spc[0x24] = 30;
   strncpy((char*)&spc[0x2E], title, 32);
   uint8_t *ram = &spc[0x100];
   memcpy(ram + 0x0200, f.driver.data(), f.driver.size());
   if (previous >= 0)
      memcpy(ram + 0x2000, f.song_data[previous].data(), f.song_data[previous].size());
   memcpy(ram + 0x2000, f.song_data[song].data(), f.song_data[song].size());
   // Instruments load wherever there is room: after whatever played before.
   size_t sample_at = 0x6000 + (previous >= 0 ? (size_t)previous * 0x40 : 0);
   memcpy(ram + sample_at, f.samples.data(), f.samples.size());
   return spc;
}

static void test_table_and_matching()
{
   printf("scenario: reference songs - song table and matching\n");
   Fixture f = make_fixture();
   SnesRom rom;
   TEST(rom.load(f.rom), "load synthetic LoROM");

   std::vector<ReferenceSong> songs;
   for (int i = 0; i < kSongs; i++)
   {
      ReferenceSong r;
      r.title = "Song " + std::to_string(i);
      // Each reference was dumped after the song before it, like real sets.
      r.spc = make_spc(f, i, i > 0 ? i - 1 : kSongs - 1, r.title.c_str());
      songs.push_back(r);
   }
   TEST(spc_song_title(songs[3].spc) == "Song 3", "ID666 song title");
   ReferenceSet refs;
   refs.assign(songs);

   SongTable t = refs.find_song_table(rom);
   TEST(t.found, "song table found");
   TEST(t.rom_offset == kTable, "table starts where the code reads it");
   TEST(t.cpu_address == kTableCpu, "table CPU address from the code");
   TEST(t.width == 3, "3-byte pointers");
   TEST(t.entries.size() == (size_t)kSongs, "one entry per song");
   bool in_order = t.entries.size() == (size_t)kSongs;
   for (int i = 0; in_order && i < kSongs; i++)
      in_order = t.entries[i] == i;
   TEST(in_order, "entries name the right songs");

   // A rip of song 5 taken after song 2 played; before it started, song 2's state.
   std::vector<uint8_t> before = make_spc(f, 2, 1, "");
   std::vector<uint8_t> rip = make_spc(f, 5, 2, "");
   ReferenceSet::Match m = refs.match_spc(rip, &before);
   TEST(m.index == 5, "rip matches its own song despite leftovers");
   TEST(m.score > m.second * 2, "clear margin over other songs");

   std::vector<uint8_t> nothing_new = before;
   m = refs.match_spc(nothing_new, &before);
   TEST(m.index < 0, "a rip with nothing new matches no song");

   // A ROM whose songs are not in a table.
   Fixture g = make_fixture();
   memset(&g.rom[kTable], 0, kSongs * 3);
   SnesRom plain;
   plain.load(g.rom);
   TEST(!refs.find_song_table(plain).found, "no table in a ROM without one");
}

static void test_names()
{
   printf("scenario: reference songs - Zophar's Domain names\n");
   TEST(zophar_slug("Chrono Trigger (USA)") == "chrono-trigger", "tags removed");
   TEST(zophar_slug("Addams Family, The") == "addams-family-the", "article after a comma");
   TEST(zophar_slug("Kirby's Dream Course [!]") == "kirbys-dream-course", "apostrophes dropped");
   TEST(zophar_slug("Super Mario World 2 - Yoshi's Island") == "super-mario-world-2-yoshis-island", "punctuation becomes one dash");
}

static void put16(std::vector<uint8_t> &v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void put32(std::vector<uint8_t> &v, uint32_t x) { put16(v, (uint16_t)x); put16(v, (uint16_t)(x >> 16)); }

// A stored (uncompressed) zip archive.
static std::vector<uint8_t> make_zip(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &files)
{
   std::vector<uint8_t> zip, central;
   for (const auto &f : files)
   {
      uint32_t local = (uint32_t)zip.size();
      put32(zip, 0x04034b50); put16(zip, 20); put16(zip, 0); put16(zip, 0); put16(zip, 0); put16(zip, 0);
      put32(zip, 0); put32(zip, (uint32_t)f.second.size()); put32(zip, (uint32_t)f.second.size());
      put16(zip, (uint16_t)f.first.size()); put16(zip, 0);
      zip.insert(zip.end(), f.first.begin(), f.first.end());
      zip.insert(zip.end(), f.second.begin(), f.second.end());

      put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0); put16(central, 0);
      put16(central, 0); put16(central, 0); put32(central, 0);
      put32(central, (uint32_t)f.second.size()); put32(central, (uint32_t)f.second.size());
      put16(central, (uint16_t)f.first.size()); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
      put32(central, 0); put32(central, local);
      central.insert(central.end(), f.first.begin(), f.first.end());
   }
   uint32_t cd = (uint32_t)zip.size();
   zip.insert(zip.end(), central.begin(), central.end());
   put32(zip, 0x06054b50); put16(zip, 0); put16(zip, 0);
   put16(zip, (uint16_t)files.size()); put16(zip, (uint16_t)files.size());
   put32(zip, (uint32_t)central.size()); put32(zip, cd); put16(zip, 0);
   return zip;
}

static void test_import(const std::string &work)
{
   printf("scenario: reference songs - importing\n");
   Fixture f = make_fixture();
   std::vector<uint8_t> a = make_spc(f, 0, -1, "First"), b = make_spc(f, 1, 0, "Second");
   std::vector<uint8_t> zip = make_zip({ { "Set/01 First.spc", a }, { "Set/02 Second.spc", b }, { "Set/info.txt", { 'h', 'i' } } });
   std::string zip_path = work + "/set.zip";
   write_text(zip_path, std::string(zip.begin(), zip.end()));
   std::string dir = work + "/refs";
   std::string err;
   int n = import_reference_songs(zip_path, dir, err);
   TEST(n == 2, "imports the .spc files of a zip");

   ReferenceSet refs;
   refs.load(dir, err);
   TEST(refs.size() == 2, "loads the imported folder");
   TEST(refs.size() == 2 && refs.song(0).title == "First" && refs.song(1).title == "Second", "titles from the files");

   err.clear();
   TEST(import_reference_songs(work + "/set.rsn", dir, err) < 0 && err.find("RAR") != std::string::npos,
        "RAR sets are refused with advice");
}

int main(int argc, char **argv)
{
   std::string work = argc > 1 ? argv[1] : ".";
   make_dirs(work);
   test_table_and_matching();
   test_names();
   test_import(work);
   printf(g_failures ? "\nFAILED (%d failures)\n" : "\nPASSED (0 failures)\n", g_failures);
   return g_failures ? 1 : 0;
}
