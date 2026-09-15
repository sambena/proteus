// SPDX-License-Identifier: LGPL-2.1-or-later
// Reference song sets: a game's soundtrack from an archive such as SNESmusic.org or Zophar's
// Domain. SNES sets are .spc files: each holds the sound CPU's RAM while its song plays, so it
// names rips (the song data a rip loaded matches one reference) and shows where the songs sit
// in the ROM when the game stores them uncompressed. NES sets are an .nsf (or .nsfe) holding
// every song, with an .m3u playlist naming them; those name rips by their notes alone.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "snes_rom.h"

struct ReferenceSong
{
   std::string title;   // ID666 or playlist song title, or the file name
   std::string path;
   std::vector<uint8_t> data;   // the .spc, or the whole .nsf the song is in
   int track = 0;               // song within a multi-song file, from 0
};

// A song table found in the ROM: pointers to each song's data, indexed by song number.
struct SongTable
{
   bool found = false;
   size_t rom_offset = 0;       // entry 0
   uint32_t cpu_address = 0;    // entry 0, as the game's code addresses it (when referenced)
   int width = 0;               // bytes per pointer: 2 or 3
   std::vector<int> entries;    // reference index for each entry, -1 when no reference matches it
   std::vector<std::vector<int>> versions;   // every reference whose song data an entry points to
   int matched = 0;             // references that have their own entry
};

class ReferenceSet
{
public:
   // Loads every .spc, and every song of each .nsf/.nsfe, in `dir`. An empty or missing folder
   // is an empty set.
   bool load(const std::string &dir, std::string &error);
   // Uses `songs` directly (tests, and sets built in memory).
   void assign(std::vector<ReferenceSong> songs);
   void clear();

   bool empty() const { return songs_.empty(); }
   size_t size() const { return songs_.size(); }
   const ReferenceSong &song(size_t i) const { return songs_[i]; }
   const std::string &dir() const { return dir_; }

   struct Match
   {
      int index = -1;       // best reference, -1 when none is convincing
      double score = 0;     // song data it shares with the rip, weighted by how rare that data is
      double second = 0;    // the best score of a reference with different song data
      std::vector<int> close;   // references scoring nearly as well: versions of the same song
   };
   // Compares a rip's sound CPU RAM (64 KB) with every reference. Only RAM that changed since
   // `before` (the sound CPU before the song started; may be null) counts, so what an earlier
   // song left behind does not match that song.
   Match match(const uint8_t *ram, const uint8_t *before) const;
   // Same, for .spc files.
   Match match_spc(const std::vector<uint8_t> &spc, const std::vector<uint8_t> *before) const;

   // Looks for a table of pointers to the references' song data in the ROM.
   SongTable find_song_table(const SnesRom &rom) const;

private:
   static const size_t kBlock = 16, kBlocks = 0x10000 / kBlock;
   std::string dir_;
   std::vector<ReferenceSong> songs_;
   std::vector<std::vector<uint32_t>> hashes_;   // per song, per RAM block
   std::vector<std::vector<uint16_t>> share_;    // per song, per block: songs with the same block
   std::vector<uint64_t> content_;               // hash << 16 | song, for every 16 bytes anywhere in each song
   int content_share(uint32_t hash) const;       // songs holding these 16 bytes anywhere
   void index();
};

// The SPC's sound CPU RAM (64 KB), or null when the data is not an .spc file.
const uint8_t *spc_ram(const std::vector<uint8_t> &spc);
// The ID666 song title, trimmed; empty when there is none.
std::string spc_song_title(const std::vector<uint8_t> &spc);

// True for the files a reference set is made of: .spc, .nsf, .nsfe and .m3u playlists.
bool is_reference_file(const std::string &name);
// Copies the reference files from `source` (a folder, a zip archive, an .rsn/.rar/.7z archive
// through 7-Zip, or a single file) into `dir`. Returns how many songs files were copied
// (playlists do not count).
int import_reference_songs(const std::string &source, const std::string &dir, std::string &error);
// 7z.exe, or empty when 7-Zip is not installed.
std::string find_7zip();

// Downloads a game's set into `dir`, finding it by any of `names` (the ROM file name, the
// game's title): SNES sets from Zophar's Domain, else from SNESmusic.org (needs 7-Zip); NES
// sets from Zophar's Domain. `progress` receives messages while it works.
enum { REFERENCES_ZOPHAR = 1, REFERENCES_SNESMUSIC = 2, REFERENCES_ZOPHAR_NES = 4 };
int download_reference_songs(const std::vector<std::string> &names, const std::string &dir,
      const std::function<void(const std::string &)> &progress, std::string &error,
      int sources = REFERENCES_ZOPHAR | REFERENCES_SNESMUSIC);
// How alike two game names are, 0..1, ignoring case, punctuation, articles and region tags.
double game_name_similarity(const std::string &a, const std::string &b);
// The name Zophar's Domain uses in its addresses: "Addams Family, The (USA)" -> "addams-family-the".
std::string zophar_slug(const std::string &game_name);
