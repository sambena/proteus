// SPDX-License-Identifier: LGPL-2.1-or-later
#include "presets.h"

#include <algorithm>
#include <cctype>
#include <cstring>

static std::string to_lower(const std::string &s)
{
   std::string r = s;
   std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)std::tolower(c); });
   return r;
}

static bool contains_ci(const std::string &haystack, const std::string &needle)
{
   if (needle.empty())
      return true;
   std::string h = to_lower(haystack);
   std::string n = to_lower(needle);
   return h.find(n) != std::string::npos;
}

static const std::vector<RomPreset> s_presets = {
   {
      "Super Mario World",
      "SNES",
      0x1DFB,
      0, // system_ram
      1,
      true, // latch = 1
      1,
      "APU sound command register ($1DFB, latch=1). Pulses song ID to SPC700.",
      {
         { 0x01, "Title Screen / Yoshi's Island" },
         { 0x02, "Overworld (Main)" },
         { 0x03, "Vanilla Dome" },
         { 0x04, "Star World" },
         { 0x05, "Forest of Illusion" },
         { 0x06, "Bowser's Castle (Valley)" },
         { 0x07, "Special World" },
         { 0x08, "Athletic" },
         { 0x09, "Underground" },
         { 0x0A, "Underwater" },
         { 0x0B, "Ghost House" },
         { 0x0C, "Castle / Fortress" },
         { 0x0D, "Bowser Battle" },
         { 0x0E, "Boss Battle" },
         { 0x0F, "Level Clear Fanfare" },
         { 0x10, "Death Fanfare" },
         { 0x11, "Game Over" },
         { 0x12, "Invincible (Starman)" },
         { 0x13, "P-Switch" },
         { 0x14, "Keyhole / Iris Out" }
      }
   },
   {
      "Super Metroid",
      "SNES",
      0x07F3,
      0, // system_ram
      1,
      false, // persistent track index
      2,
      "Current music track index in System RAM ($07F3).",
      {
         { 0x00, "Silence" },
         { 0x03, "Title Screen" },
         { 0x06, "Space Colony Ceres" },
         { 0x09, "Ceres Escape" },
         { 0x0C, "Crateria (Landing Site / Rain)" },
         { 0x0F, "Crateria (Space Pirates Appear)" },
         { 0x12, "Brinstar (Overgrown with Vegetation)" },
         { 0x15, "Brinstar (Red Soil Swamp)" },
         { 0x18, "Norfair (Hot Lava Area)" },
         { 0x1B, "Norfair (Ancient Ruins)" },
         { 0x1E, "Maridia (Watery Area)" },
         { 0x21, "Maridia (Quicksand Area)" },
         { 0x24, "Tourian" },
         { 0x27, "Mother Brain Battle" },
         { 0x2A, "Big Boss Battle (Ridley / Draygon)" },
         { 0x2D, "Small Boss Battle (Spore Spawn / Botwoon)" },
         { 0x30, "Theme of Samus Aran" }
      }
   },
   {
      "The Legend of Zelda: A Link to the Past",
      "SNES",
      0x0130,
      0, // system_ram
      1,
      true, // latch = 1
      1,
      "APU sound command register ($0130, latch=1). Pulses song ID to SPC700.",
      {
         { 0x01, "Title Screen" },
         { 0x02, "Light World Overworld" },
         { 0x03, "Rain (Opening)" },
         { 0x05, "Kakariko Village" },
         { 0x07, "Hyrule Castle" },
         { 0x09, "Dark World Overworld" },
         { 0x0B, "Master Sword Fanfare" },
         { 0x0C, "Flute Boy Song" },
         { 0x13, "Sanctuary" },
         { 0x16, "Boss Clear Fanfare" },
         { 0x17, "Dungeon (Light World)" },
         { 0x19, "Dungeon (Dark World)" },
         { 0x22, "Boss Battle" },
         { 0x23, "Ganon's Battle" }
      }
   },
   {
      "Chrono Trigger",
      "SNES",
      0x0100,
      0, // system_ram
      1,
      true, // latch = 1
      1,
      "APU sound command register ($0100, latch=1).",
      {}
   },
   {
      "Mega Man X",
      "SNES",
      0x0BD7,
      0, // system_ram
      1,
      true, // latch = 1
      1,
      "Music command register ($0BD7, latch=1).",
      {}
   },
   {
      "Donkey Kong Country",
      "SNES",
      0x0513,
      0, // system_ram
      1,
      true, // latch = 1
      1,
      "Music command register ($0513, latch=1).",
      {}
   },
   {
      "Sonic the Hedgehog",
      "Genesis",
      0xF000,
      0, // system_ram (68k RAM)
      1,
      true, // latch = 1
      1,
      "SMPS sound driver music command ($F000, latch=1).",
      {
         { 0x81, "Green Hill Zone" },
         { 0x82, "Labyrinth Zone" },
         { 0x83, "Marble Zone" },
         { 0x84, "Star Light Zone" },
         { 0x85, "Spring Yard Zone" },
         { 0x86, "Scrap Brain Zone" },
         { 0x87, "Invincibility" },
         { 0x88, "Extra Life" },
         { 0x89, "Special Stage" },
         { 0x8A, "Title Screen" },
         { 0x8B, "Ending Theme" },
         { 0x8C, "Boss Theme" },
         { 0x8D, "Final Zone" },
         { 0x8E, "Act Clear" },
         { 0x8F, "Game Over" },
         { 0x90, "Continue Screen" },
         { 0x91, "Drowning Warning" }
      }
   },
   {
      "Sonic the Hedgehog 2",
      "Genesis",
      0xF000,
      0, // system_ram (68k RAM)
      1,
      true, // latch = 1
      1,
      "SMPS sound driver music command ($F000, latch=1).",
      {
         { 0x81, "Emerald Hill Zone" },
         { 0x82, "Chemical Plant Zone" },
         { 0x83, "Aquatic Ruin Zone" },
         { 0x84, "Casino Night Zone" },
         { 0x85, "Hill Top Zone" },
         { 0x86, "Mystic Cave Zone" },
         { 0x87, "Oil Ocean Zone" },
         { 0x88, "Metropolis Zone" },
         { 0x89, "Boss Theme" },
         { 0x8A, "Casino Night (2P)" },
         { 0x8B, "Death Egg Zone" },
         { 0x8C, "Special Stage" }
      }
   },
   {
      "Streets of Rage 2",
      "Genesis",
      0xF000,
      0, // system_ram (68k RAM)
      1,
      true, // latch = 1
      1,
      "Sound driver music command register ($F000, latch=1).",
      {}
   },
   {
      "Super Mario Bros.",
      "NES",
      0x0710,
      0, // system_ram
      1,
      true, // latch = 1
      1,
      "Event sound register ($0710, latch=1).",
      {
         { 0x01, "Ground Theme" },
         { 0x02, "Underground Theme" },
         { 0x04, "Underwater Theme" },
         { 0x08, "Castle Theme" },
         { 0x10, "Starman" },
         { 0x20, "Level Clear Fanfare" }
      }
   },
   {
      "Castlevania",
      "NES",
      0x002C,
      0, // system_ram
      1,
      true, // latch = 1
      1,
      "Music command register ($002C, latch=1).",
      {}
   },
   {
      "Pokemon Red / Blue",
      "Game Boy",
      0xC0EE,
      0, // system_ram
      1,
      false,
      1,
      "Audio bank / current music track register ($C0EE).",
      {}
   }
};

const std::vector<RomPreset> &get_rom_presets()
{
   return s_presets;
}

std::string detect_rom_header_title(const uint8_t *data, size_t size, std::string &system_out)
{
   if (!data || size < 0x200)
      return "";

   // 1. Genesis / Mega Drive check (Fixed header at 0x0100)
   if (size >= 0x200 && memcmp(&data[0x0100], "SEGA", 4) == 0)
   {
      system_out = "Genesis";
      std::string title((const char *)&data[0x0150], 48);
      while (!title.empty() && (title.back() == ' ' || (uint8_t)title.back() < 0x20 || (uint8_t)title.back() > 0x7E))
         title.pop_back();
      if (title.empty())
      {
         title = std::string((const char *)&data[0x0120], 48);
         while (!title.empty() && (title.back() == ' ' || (uint8_t)title.back() < 0x20 || (uint8_t)title.back() > 0x7E))
            title.pop_back();
      }
      return title;
   }

   // 2. SNES Check (LoROM $7FC0, HiROM $FFC0, with or without 512-byte copier header)
   static const size_t snes_offsets[] = { 0x7FC0, 0x81C0, 0xFFC0, 0x101C0, 0x40FFC0 };
   for (size_t off : snes_offsets)
   {
      if (size >= off + 0x30)
      {
         uint16_t csum = (uint16_t)(data[off + 0x1E] | (data[off + 0x1F] << 8));
         uint16_t comp = (uint16_t)(data[off + 0x1C] | (data[off + 0x1D] << 8));
         if ((uint16_t)(csum + comp) == 0xFFFF && csum != 0)
         {
            system_out = "SNES";
            std::string title((const char *)&data[off], 21);
            while (!title.empty() && (title.back() == ' ' || (uint8_t)title.back() < 0x20 || (uint8_t)title.back() > 0x7E))
               title.pop_back();
            return title;
         }
      }
   }

   // 3. Game Boy / GBC Check (Header at 0x0134, checksum at 0x014D)
   if (size >= 0x150)
   {
      uint8_t chk = 0;
      for (int i = 0x134; i <= 0x14C; i++)
         chk = chk - data[i] - 1;
      if (chk == data[0x014D])
      {
         system_out = (data[0x0143] & 0x80) ? "GBC" : "Game Boy";
         std::string title((const char *)&data[0x0134], 16);
         while (!title.empty() && (title.back() == ' ' || (uint8_t)title.back() < 0x20 || (uint8_t)title.back() > 0x7E))
            title.pop_back();
         return title;
      }
   }

   // 4. Game Boy Advance Check (Title at 0x00A0, checksum at 0x00BD)
   if (size >= 0xC0)
   {
      uint8_t chk = 0;
      for (int i = 0xA0; i <= 0xBC; i++)
         chk -= data[i];
      chk -= 0x19;
      if (chk == data[0x00BD])
      {
         system_out = "GBA";
         std::string title((const char *)&data[0x00A0], 12);
         while (!title.empty() && (title.back() == ' ' || (uint8_t)title.back() < 0x20 || (uint8_t)title.back() > 0x7E))
            title.pop_back();
         return title;
      }
   }

   // 5. NES Check (iNES header "NES\x1A")
   if (size >= 16 && memcmp(data, "NES\x1A", 4) == 0)
   {
      system_out = "NES";
      return "";
   }

   return "";
}

const RomPreset *detect_preset(const uint8_t *data, size_t size, const std::string &rom_path)
{
   std::string sys;
   std::string header_title = detect_rom_header_title(data, size, sys);

   // Match each preset against header title or file path
   for (const auto &p : s_presets)
   {
      if (!header_title.empty())
      {
         if (p.name == "Super Mario World" && contains_ci(header_title, "MARIOWORLD")) return &p;
         if (p.name == "Super Metroid" && contains_ci(header_title, "METROID")) return &p;
         if (p.name == "The Legend of Zelda: A Link to the Past" && contains_ci(header_title, "ZELDA")) return &p;
         if (p.name == "Chrono Trigger" && contains_ci(header_title, "CHRONO")) return &p;
         if (p.name == "Mega Man X" && contains_ci(header_title, "MEGA MAN X")) return &p;
         if (p.name == "Donkey Kong Country" && (contains_ci(header_title, "DONKEY KONG") || contains_ci(header_title, "DKC"))) return &p;
         if (p.name == "Sonic the Hedgehog 2" && contains_ci(header_title, "SONIC THE HEDGEHOG 2")) return &p;
         if (p.name == "Sonic the Hedgehog" && contains_ci(header_title, "SONIC THE HEDGEHOG")) return &p;
         if (p.name == "Streets of Rage 2" && (contains_ci(header_title, "STREETS OF RAGE 2") || contains_ci(header_title, "BARE KNUCKLE II"))) return &p;
         if (p.name == "Pokemon Red / Blue" && (contains_ci(header_title, "POKEMON RED") || contains_ci(header_title, "POKEMON BLUE"))) return &p;
      }

      // Fallback matching against rom_path
      if (!rom_path.empty())
      {
         if (p.name == "Super Mario World" && (contains_ci(rom_path, "mario world") || contains_ci(rom_path, "smw"))) return &p;
         if (p.name == "Super Metroid" && contains_ci(rom_path, "super metroid")) return &p;
         if (p.name == "The Legend of Zelda: A Link to the Past" && (contains_ci(rom_path, "link to the past") || contains_ci(rom_path, "alttp"))) return &p;
         if (p.name == "Chrono Trigger" && contains_ci(rom_path, "chrono trigger")) return &p;
         if (p.name == "Mega Man X" && contains_ci(rom_path, "mega man x")) return &p;
         if (p.name == "Donkey Kong Country" && contains_ci(rom_path, "donkey kong country")) return &p;
         if (p.name == "Sonic the Hedgehog 2" && contains_ci(rom_path, "sonic 2")) return &p;
         if (p.name == "Sonic the Hedgehog" && (contains_ci(rom_path, "sonic the hedgehog") || contains_ci(rom_path, "sonic 1"))) return &p;
         if (p.name == "Streets of Rage 2" && (contains_ci(rom_path, "streets of rage 2") || contains_ci(rom_path, "sor2") || contains_ci(rom_path, "bare knuckle 2"))) return &p;
         if (p.name == "Super Mario Bros." && (contains_ci(rom_path, "super mario bros") || contains_ci(rom_path, "super mario brothers"))) return &p;
         if (p.name == "Castlevania" && contains_ci(rom_path, "castlevania")) return &p;
         if (p.name == "Pokemon Red / Blue" && (contains_ci(rom_path, "pokemon red") || contains_ci(rom_path, "pokemon blue"))) return &p;
      }
   }

   return nullptr;
}
