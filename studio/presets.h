// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct RomPresetSong
{
   uint32_t value;
   std::string title;
};

struct RomPreset
{
   std::string name;            // Game title
   std::string system;          // Platform: "SNES", "Genesis", "NES", "Game Boy", etc.
   uint32_t address;            // Song / command RAM address
   int memory;                  // Index into kMemories (0 = system_ram)
   int size;                    // 1 or 2 bytes
   bool latch;                  // True if one-shot command register
   int debounce;                // Debounce frame count (default 1)
   std::string description;     // Descriptive notes for users
   std::vector<RomPresetSong> songs; // Known songs and their titles
};

const std::vector<RomPreset> &get_rom_presets();
const RomPreset *detect_preset(const uint8_t *data, size_t size, const std::string &rom_path);
std::string detect_rom_header_title(const uint8_t *data, size_t size, std::string &system_out);
