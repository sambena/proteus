// SPDX-License-Identifier: LGPL-2.1-or-later
// Reference songs: matching rips, finding a song table in a ROM, names and imports.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nes_tap.h"
#include "nsf_init.h"
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
      r.data = make_spc(f, i, i > 0 ? i - 1 : kSongs - 1, r.title.c_str());
      songs.push_back(r);
   }
   TEST(spc_song_title(songs[3].data) == "Song 3", "ID666 song title");
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
   TEST(game_name_similarity("Legend of Zelda - A Link to the Past, The (USA)", "Legend of Zelda: A Link to the Past") == 1.0,
        "names match despite articles and punctuation");
   TEST(game_name_similarity("Addams Family, The", "Addams Family Values") < 0.75, "different games stay apart");
   TEST(game_name_similarity("Super Mario World", "Super Mario World 2: Yoshi's Island") < 0.75, "sequels stay apart");
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
   TEST(import_reference_songs(work + "/missing.rsn", dir, err) < 0 && !err.empty(), "a missing .rsn set fails with a reason");
}

static void test_nsf_sets(const std::string &work)
{
   printf("scenario: reference songs - NES sets\n");
   std::vector<uint8_t> nsf(0x80 + 16, 0);
   memcpy(nsf.data(), "NESM\x1A\x01", 6);
   nsf[6] = 5;   // songs
   nsf[7] = 1;
   std::string playlist = "# Game\n\n"
         "Game.nsf::NSF,03,Castle\\, Part 1,90,,7\n"
         "Game.nsf::NSF,$00,Title,12\n"
         "Game.nsf::NSF,9,Past the end\n";
   std::vector<uint8_t> m3u(playlist.begin(), playlist.end());
   std::vector<uint8_t> zip = make_zip({ { "Game (EMU)/Game.nsf", nsf }, { "Game (EMU)/Game.m3u", m3u } });
   std::string zip_path = work + "/nsf.zip", dir = work + "/nsf_refs", err;
   write_text(zip_path, std::string(zip.begin(), zip.end()));
   TEST(import_reference_songs(zip_path, dir, err) == 1, "imports an .nsf and its playlist from a zip");

   ReferenceSet refs;
   refs.load(dir, err);
   TEST(refs.size() == 2, "lists the playlist's songs that the file holds");
   TEST(refs.size() == 2 && refs.song(0).title == "Castle, Part 1" && refs.song(0).track == 2,
         "decimal playlist tracks count from 1; escaped commas stay in titles");
   TEST(refs.size() == 2 && refs.song(1).title == "Title" && refs.song(1).track == 0, "$hex playlist tracks count from 0");
   TEST(!refs.find_song_table(SnesRom()).found, "no ROM song table from an .nsf set");

   std::string bare = work + "/nsf_bare";
   make_dirs(bare);
   write_text(bare + "/Other.nsf", std::string(nsf.begin(), nsf.end()));
   refs.load(bare, err);
   TEST(refs.size() == 5 && refs.song(4).track == 4 && refs.song(4).title == "Other #5", "without a playlist, every song is listed");
}

static void test_nsf_init()
{
   printf("scenario: reference songs - what an .nsf's init writes\n");
   // Like The Legend of Zelda's rip: song -> (offset, value) pairs written to $0600 + offset, after
   // clearing a work byte and calling a subroutine that sets another.
   const uint8_t code[] = {
      0x48,                   // 8000 PHA
      0xA9, 0x00, 0x85, 0x10, // LDA #0 / STA $10       (the same for every song)
      0x20, 0x20, 0x80,       // JSR $8020
      0x68, 0x0A, 0xA8,       // PLA / ASL A / TAY
      0xB9, 0x30, 0x80, 0xAA, // LDA $8030,Y / TAX
      0xB9, 0x31, 0x80,       // LDA $8031,Y
      0x9D, 0x00, 0x06,       // STA $0600,X
      0x60,                   // RTS
   };
   const uint8_t sub[] = { 0xA9, 0x05, 0x8D, 0x20, 0x07, 0x60 };   // LDA #5 / STA $0720 / RTS
   const uint8_t table[] = { 0x00, 0x80, 0x00, 0x10, 0x02, 0x40, 0x02, 0x80 };
   std::vector<uint8_t> nsf(0x80 + 0x40, 0);
   memcpy(nsf.data(), "NESM\x1A\x01", 6);
   nsf[6] = 4;
   nsf[7] = 1;
   nsf[8] = 0x00; nsf[9] = 0x80;     // load
   nsf[10] = 0x00; nsf[11] = 0x80;   // init
   nsf[12] = 0x20; nsf[13] = 0x80;   // play
   memcpy(&nsf[0x80], code, sizeof(code));
   memcpy(&nsf[0x80 + 0x20], sub, sizeof(sub));
   memcpy(&nsf[0x80 + 0x30], table, sizeof(table));

   std::string err;
   std::map<uint16_t, uint8_t> writes;
   TEST(nsf_init_ram(nsf, 1, writes, err) && writes.count(0x600) && writes[0x600] == 0x10 && writes[0x720] == 5,
         "runs init for a song and returns the RAM it writes");
   std::vector<NsfRequest> requests = nsf_song_requests(nsf, err);
   TEST(requests.size() == 2, "only bytes written differently from song to song are requests");
   TEST(requests.size() == 2 && requests[0].address == 0x600 && requests[0].song_values.size() == 2 &&
         requests[0].song_values[0] == 0x80 && requests[0].song_values[1] == 0x10, "the request byte and each song's value");
   TEST(requests.size() == 2 && requests[1].address == 0x602 && requests[1].song_values[2] == 0x40, "a second request byte");
}

static void test_nes_tap()
{
   printf("scenario: a tap on an NES game's sound routine\n");
   // The rip's init calls the sound routine with the song in A, like Mega Man 3's:
   //   8000 JSR $8100 / RTS
   //   8100 CMP #$F0 / BCC +3 / JMP $8120 / STA $0600 / RTS      8120 RTS
   const uint8_t init[] = { 0x20, 0x00, 0x81, 0x60 };
   const uint8_t routine[] = { 0xC9, 0xF0, 0x90, 0x03, 0x4C, 0x20, 0x81, 0x8D, 0x00, 0x06, 0x60 };
   std::vector<uint8_t> nsf(0x80 + 0x200, 0);
   memcpy(nsf.data(), "NESM\x1A\x01", 6);
   nsf[6] = 3;
   nsf[7] = 1;
   nsf[8] = 0x00; nsf[9] = 0x80;
   nsf[10] = 0x00; nsf[11] = 0x80;
   nsf[12] = 0x20; nsf[13] = 0x81;
   memcpy(&nsf[0x80], init, sizeof(init));
   memcpy(&nsf[0x80 + 0x100], routine, sizeof(routine));
   nsf[0x80 + 0x120] = 0x60;

   std::string err;
   std::vector<NsfCall> calls = nsf_init_calls(nsf, err);
   TEST(calls.size() == 1 && calls[0].routine == 0x8100 && calls[0].song_values[2] == 2, "init calls the sound routine with the song");

   // The game: one 16KB bank (NROM), blank but for the routine and code naming RAM.
   std::vector<uint8_t> rom(16 + 0x4000, 0xFF);
   memcpy(rom.data(), "NES\x1A", 4);
   rom[4] = 1;
   rom[5] = 0;
   rom[6] = 0;
   rom[7] = 0;
   for (int i = 8; i < 16; i++)
      rom[i] = 0;
   memcpy(&rom[16 + 0x100], routine, sizeof(routine));
   const uint8_t names[] = { 0x9D, 0xF8, 0x07, 0x8D, 0xF5, 0x07 };   // STA $07F8,X / STA $07F5
   memcpy(&rom[16 + 0x3000], names, sizeof(names));

   std::vector<uint16_t> free_ram = nes_unnamed_ram(rom);
   TEST(!free_ram.empty() && free_ram[0] == 0x07F7, "RAM no instruction names, from the end");

   NesTap tap;
   bool ok = nes_tap_design(rom, 0x8100, nsf_code_at(nsf, 0x8100, sizeof(routine), err), 0x07F7, tap, err);
   TEST(ok, "designs a tap");
   if (!ok)
   {
      printf("    %s\n", err.c_str());
      return;
   }
   TEST(tap.stub == 0xC004, "the stub goes in blank ROM of the fixed bank");
   const uint8_t stub[] = { 0x08, 0x48, 0x18, 0x69, 0x01, 0x8D, 0xF7, 0x07, 0x68, 0x28,   // store request + 1
                            0xC9, 0xF0, 0xB0, 0x03, 0x4C, 0x07, 0x81,                     // CMP, the branch moved
                            0x4C, 0x04, 0x81 };                                           // back into the routine
   bool same = tap.patches.size() == 3 + sizeof(stub);
   for (size_t i = 0; same && i < sizeof(stub); i++)
      same = tap.patches[3 + i].address == 0xC004 + i && tap.patches[3 + i].compare == 0xFF && tap.patches[3 + i].value == stub[i];
   TEST(same, "the stub stores the request and runs the moved instructions");
   TEST(tap.fceumm_cheat().compare(0, 33, "8100?C9:4C+8101?F0:04+8102?90:C0+") == 0, "the routine jumps to the stub");
   TEST(nes_rom_holds(rom, 0x8100, std::vector<uint8_t>(routine, routine + sizeof(routine))), "finds the rip's code in the ROM");
   std::vector<uint8_t> other(routine, routine + sizeof(routine));
   other[5] = 0x30;
   TEST(!nes_rom_holds(rom, 0x8100, other), "code the ROM lacks is not found");
}

int main(int argc, char **argv)
{
   std::string work = argc > 1 ? argv[1] : ".";
   make_dirs(work);
   test_table_and_matching();
   test_names();
   test_import(work);
   test_nsf_sets(work);
   test_nsf_init();
   test_nes_tap();
   printf(g_failures ? "\nFAILED (%d failures)\n" : "\nPASSED (0 failures)\n", g_failures);
   return g_failures ? 1 : 0;
}
