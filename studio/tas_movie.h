// SPDX-License-Identifier: LGPL-2.1-or-later
// Input movies from TASVideos: BizHawk .bk2 and Snes9x .smv, read into one
// libretro joypad mask per frame.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TasMovie
{
   enum Flag : uint8_t { PAD = 0, RESET = 1, POWER = 2 };
   struct Frame
   {
      uint16_t buttons;   // bit n = RETRO_DEVICE_ID_JOYPAD_n, player 1
      uint8_t flag;
   };

   std::string format;     // "bk2" or "smv"
   std::string core;       // BizHawk core ("BSNES", "BSNESv115+", "Snes9x"), or "Snes9x 1.xx"
   std::string game;       // the game name the movie gives
   std::string sha1;       // of the ROM, upper-case hex, when the movie gives it
   std::vector<Frame> frames;

   // Reads a movie (or a zip holding one). False with the reason in err.
   bool load(const std::string &path, std::string &err);
   bool load(const std::vector<uint8_t> &data, const std::string &name, std::string &err);
};
