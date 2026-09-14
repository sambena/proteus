// SPDX-License-Identifier: LGPL-2.1-or-later
#include "tas_movie.h"

#include <cstring>

#include "platform.h"
#include "zip_read.h"

// libretro joypad ids in the order BizHawk logs SNES buttons: Up Down Left Right Select Start Y B X A L R
static const int kBk2Buttons[12] = { 4, 5, 6, 7, 2, 3, 1, 0, 9, 8, 10, 11 };

static std::string line_value(const std::string &text, const char *key)
{
   size_t n = strlen(key);
   for (size_t p = 0; p < text.size();)
   {
      size_t e = text.find('\n', p);
      if (e == std::string::npos)
         e = text.size();
      if (text.compare(p, n, key) == 0 && p + n < e && text[p + n] == ' ')
      {
         std::string v = text.substr(p + n + 1, e - p - n - 1);
         while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
            v.pop_back();
         return v;
      }
      p = e + 1;
   }
   return "";
}

static bool load_bk2(const std::vector<uint8_t> &zip, TasMovie &m, std::string &err)
{
   std::string header, log;
   zip_read(zip, [](const std::string &n) { return n == "Header.txt" || n == "Input Log.txt"; },
         [&](const std::string &n, std::vector<uint8_t> &d) {
            (n == "Header.txt" ? header : log).assign((const char *)d.data(), d.size());
            return true;
         }, err);
   if (log.empty())
   {
      err = "the .bk2 has no input log";
      return false;
   }
   if (line_value(header, "Platform") != "SNES")
   {
      err = "the movie is for " + (header.empty() ? std::string("another system") : line_value(header, "Platform")) + ", not the SNES";
      return false;
   }
   m.format = "bk2";
   m.core = line_value(header, "Core");
   m.game = line_value(header, "GameName");
   m.sha1 = line_value(header, "SHA1");

   // LogKey:#Reset|Power|#P1 Up|...|P1 R|#P2 Up|... names every column; each frame is a line of
   // |..|............|: a '.' for a button up, anything else for down.
   std::vector<std::string> keys;
   size_t k = log.find("LogKey:");
   if (k != std::string::npos)
   {
      size_t e = log.find('\n', k);
      std::string key = log.substr(k + 7, e - k - 7);
      std::string cur;
      for (char c : key)
      {
         if (c == '|' || c == '#' || c == '\r')
         {
            if (!cur.empty())
               keys.push_back(cur);
            cur.clear();
         }
         else
            cur += c;
      }
      if (!cur.empty())
         keys.push_back(cur);
   }
   static const char *kP1[12] = { "P1 Up", "P1 Down", "P1 Left", "P1 Right", "P1 Select", "P1 Start",
      "P1 Y", "P1 B", "P1 X", "P1 A", "P1 L", "P1 R" };
   std::vector<int> column(keys.size(), -1);   // column -> libretro id, or -2 reset, -3 power
   for (size_t i = 0; i < keys.size(); i++)
   {
      if (keys[i] == "Reset")
         column[i] = -2;
      else if (keys[i] == "Power")
         column[i] = -3;
      for (int b = 0; b < 12; b++)
         if (keys[i] == kP1[b])
            column[i] = kBk2Buttons[b];
   }
   if (keys.empty())
   {
      // Without a key, assume the usual one controller layout
      column = { -2, -3 };
      for (int b = 0; b < 12; b++)
         column.push_back(kBk2Buttons[b]);
   }

   for (size_t p = 0; p < log.size();)
   {
      size_t e = log.find('\n', p);
      if (e == std::string::npos)
         e = log.size();
      if (log[p] == '|')
      {
         TasMovie::Frame f = { 0, TasMovie::PAD };
         size_t col = 0;
         for (size_t i = p; i < e && col < column.size(); i++)
         {
            char c = log[i];
            if (c == '|' || c == '\r')
               continue;
            if (c != '.' && c != ' ')
            {
               if (column[col] >= 0)
                  f.buttons |= (uint16_t)(1 << column[col]);
               else if (column[col] == -2)
                  f.flag = TasMovie::RESET;
               else if (column[col] == -3)
                  f.flag = TasMovie::POWER;
            }
            col++;
         }
         m.frames.push_back(f);
      }
      p = e + 1;
   }
   if (m.frames.empty())
   {
      err = "the .bk2 input log has no frames";
      return false;
   }
   return true;
}

static bool load_smv(const std::vector<uint8_t> &mv, TasMovie &m, std::string &err)
{
   auto rd32 = [&](size_t at) { return (uint32_t)mv[at] | mv[at + 1] << 8 | mv[at + 2] << 16 | (uint32_t)mv[at + 3] << 24; };
   uint32_t version = rd32(4), data_at = rd32(28);
   uint8_t mask = mv[20], opts = mv[21];
   if (!(opts & 1))
   {
      err = "the movie starts from a Snes9x save state, which only Snes9x can load";
      return false;
   }
   int pads = 0;
   for (int i = 0; i < 8; i++)
      pads += (mask >> i) & 1;
   int per = 2 * pads;
   if (version >= 4)
      for (int p = 0; p < 2; p++)
         per += mv[36 + p] == 2 ? 5 : mv[36 + p] == 3 ? 6 : mv[36 + p] == 4 ? 11 : 0;   // mouse, scope, justifier
   if (!per || data_at >= mv.size())
   {
      err = "the .smv has no controller data";
      return false;
   }
   m.format = "smv";
   m.core = version >= 4 ? "Snes9x 1.5x" : "Snes9x 1.43";
   for (size_t at = data_at; at + per <= mv.size(); at += per)
   {
      bool reset = true;
      for (int i = 0; i < per; i++)
         reset = reset && mv[at + i] == 0xFF;
      TasMovie::Frame f = { 0, reset ? TasMovie::RESET : TasMovie::PAD };
      if (!reset && (mask & 1))
      {
         // Snes9x keeps B Y Select Start Up Down Left Right A X L R from bit 15 down; libretro from bit 0 up
         uint16_t pad = (uint16_t)(mv[at] | mv[at + 1] << 8);
         for (int b = 0; b < 12; b++)
            if (pad & (0x8000 >> b))
               f.buttons |= (uint16_t)(1 << b);
      }
      m.frames.push_back(f);
   }
   return true;
}

bool TasMovie::load(const std::vector<uint8_t> &data, const std::string &name, std::string &err)
{
   *this = TasMovie();
   if (data.size() >= 32 && memcmp(data.data(), "SMV\x1a", 4) == 0)
      return load_smv(data, *this, err);
   if (!is_zip(data))
   {
      err = name + " is not a .bk2 or .smv movie";
      return false;
   }
   // A .bk2 is itself a zip; TASVideos wraps movies in another one.
   bool bk2 = false;
   zip_read(data, [&](const std::string &n) { bk2 = bk2 || n == "Input Log.txt"; return false; }, [](const std::string &, std::vector<uint8_t> &) { return true; }, err);
   if (bk2)
      return load_bk2(data, *this, err);
   std::vector<uint8_t> inner;
   std::string inner_name;
   zip_read(data, [](const std::string &n) { std::string e = lower_ext(n); return e == "bk2" || e == "smv"; },
         [&](const std::string &n, std::vector<uint8_t> &d) { inner.swap(d); inner_name = n; return false; }, err);
   if (inner.empty())
   {
      err = name + " holds no .bk2 or .smv movie";
      return false;
   }
   err.clear();
   return load(inner, inner_name, err);
}

bool TasMovie::load(const std::string &path, std::string &err)
{
   std::vector<uint8_t> data;
   if (!read_file_bytes(path, data))
   {
      err = "cannot read " + path;
      return false;
   }
   return load(data, path, err);
}
