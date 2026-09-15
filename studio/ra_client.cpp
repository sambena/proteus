// SPDX-License-Identifier: GPL-3.0-or-later
#include "ra_client.h"
#include "http.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

static std::string to_lower(std::string s)
{
   for (char &c : s)
      c = (char)std::tolower((unsigned char)c);
   return s;
}

int ra_parse_game_id_json(const std::string &json_str)
{
   size_t pos = json_str.find("\"GameID\"");
   if (pos == std::string::npos)
      return 0;
   pos = json_str.find(':', pos);
   if (pos == std::string::npos)
      return 0;
   pos++;
   while (pos < json_str.size() && (json_str[pos] == ' ' || json_str[pos] == '\t'))
      pos++;
   return atoi(json_str.c_str() + pos);
}

// Unescapes JSON string characters like \r, \n, quotes, and backslashes
static std::string unescape_json(const std::string &s)
{
   std::string out;
   out.reserve(s.size());
   for (size_t i = 0; i < s.size(); i++)
   {
      if (s[i] == '\\' && i + 1 < s.size())
      {
         i++;
         switch (s[i])
         {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            default: out += s[i]; break;
         }
      }
      else
         out += s[i];
   }
   return out;
}

// Extract the string value of a JSON field key within [start, end)
static bool extract_json_field(const std::string &json, size_t start, size_t end,
      const std::string &key, std::string &val)
{
   std::string pattern = "\"" + key + "\"";
   size_t pos = json.find(pattern, start);
   if (pos == std::string::npos || pos >= end)
      return false;
   pos = json.find(':', pos);
   if (pos == std::string::npos || pos >= end)
      return false;
   pos = json.find('"', pos);
   if (pos == std::string::npos || pos >= end)
      return false;
   pos++; // skip opening quote
   size_t val_start = pos;
   while (pos < end)
   {
      if (json[pos] == '\\')
      {
         pos += 2;
         continue;
      }
      if (json[pos] == '"')
         break;
      pos++;
   }
   if (pos > end)
      pos = end;
   val = unescape_json(json.substr(val_start, pos - val_start));
   return true;
}

static uint32_t normalize_snes_ram_address(uint32_t raw)
{
   // SNES 24-bit bus address mappings to system RAM (WRAM, 128KB, banks $7E-$7F):
   if (raw >= 0x7E0000 && raw <= 0x7FFFFF)
      return raw - 0x7E0000;
   if (raw <= 0x01FFFF)
      return raw;
   return raw & 0x01FFFF;
}

static int score_note(const std::string &note_text, int &size)
{
   std::string l = to_lower(note_text);

   // Size detection
   if (l.find("16bit") != std::string::npos || l.find("16-bit") != std::string::npos ||
       l.find("16 bit") != std::string::npos || l.find("2 byte") != std::string::npos ||
       l.find("2-byte") != std::string::npos || l.find("word") != std::string::npos)
      size = 2;
   else
      size = 1;

   bool has_music = l.find("music") != std::string::npos;
   bool has_bgm   = l.find("bgm") != std::string::npos;
   bool has_song  = l.find("song") != std::string::npos;
   bool has_track = l.find("track") != std::string::npos;

   // Skip if no music-related terms
   if (!has_music && !has_bgm && !has_song && !has_track)
      return -1000;

   // If only "sound effect" or "sfx" without music/bgm/song, skip
   bool has_sfx = l.find("sound effect") != std::string::npos || l.find("sfx") != std::string::npos;
   if (has_sfx && !has_music && !has_bgm && !has_song)
      return -1000;

   int score = 0;
   if (has_bgm)
      score += 50;
   if (has_music)
      score += 40;
   if (has_song)
      score += 35;
   if (has_track)
      score += 25;

   // High-confidence phrases
   if (l.find("music id") != std::string::npos || l.find("bgm id") != std::string::npos ||
       l.find("song id") != std::string::npos || l.find("music index") != std::string::npos)
      score += 80;
   if (l.find("music track") != std::string::npos || l.find("song track") != std::string::npos)
      score += 60;
   if (l.find("music playing") != std::string::npos || l.find("song playing") != std::string::npos ||
       l.find("current music") != std::string::npos || l.find("current song") != std::string::npos ||
       l.find("current bgm") != std::string::npos)
      score += 50;
   if (l.find("music register") != std::string::npos || l.find("bgm register") != std::string::npos)
      score += 50;

   // Demote secondary properties unless explicit ID is mentioned
   bool explicit_id = l.find("id") != std::string::npos || l.find("index") != std::string::npos ||
                      l.find("track") != std::string::npos || l.find("number") != std::string::npos;
   if (!explicit_id)
   {
      if (l.find("volume") != std::string::npos || l.find("fade") != std::string::npos ||
          l.find("mute") != std::string::npos || l.find("tempo") != std::string::npos ||
          l.find("pan") != std::string::npos || l.find("pause") != std::string::npos ||
          l.find("speed") != std::string::npos)
         score -= 40;
   }

   return score;
}

std::vector<RaCodeNote> ra_parse_music_notes_json(const std::string &json_str)
{
   std::vector<RaCodeNote> results;

   size_t array_pos = json_str.find("\"CodeNotes\"");
   if (array_pos == std::string::npos)
      return results;
   array_pos = json_str.find('[', array_pos);
   if (array_pos == std::string::npos)
      return results;

   size_t curr = array_pos + 1;
   while (curr < json_str.size())
   {
      size_t obj_start = json_str.find('{', curr);
      if (obj_start == std::string::npos)
         break;

      // Find matching closing brace
      size_t obj_end = obj_start + 1;
      bool in_quote = false;
      while (obj_end < json_str.size())
      {
         if (json_str[obj_end] == '"' && json_str[obj_end - 1] != '\\')
            in_quote = !in_quote;
         else if (json_str[obj_end] == '}' && !in_quote)
            break;
         obj_end++;
      }
      if (obj_end >= json_str.size())
         break;

      std::string addr_str, note_str, user_str;
      extract_json_field(json_str, obj_start, obj_end, "Address", addr_str);
      extract_json_field(json_str, obj_start, obj_end, "Note", note_str);
      extract_json_field(json_str, obj_start, obj_end, "User", user_str);

      if (!addr_str.empty() && !note_str.empty())
      {
         int size = 1;
         int score = score_note(note_str, size);
         if (score > 0)
         {
            uint32_t raw_addr = (uint32_t)strtoul(addr_str.c_str(), nullptr, 0);
            uint32_t norm_addr = normalize_snes_ram_address(raw_addr);

            // Sanitize note for one-line display
            std::string clean_note;
            for (char c : note_str)
            {
               if (c == '\r' || c == '\n')
               {
                  if (!clean_note.empty() && clean_note.back() != ' ')
                     clean_note += " ";
               }
               else
                  clean_note += c;
            }
            while (!clean_note.empty() && clean_note.back() == ' ')
               clean_note.pop_back();

            char hex_buf[16];
            snprintf(hex_buf, sizeof(hex_buf), "$%04X", norm_addr);

            RaCodeNote cn;
            cn.address = norm_addr;
            cn.memory = 0; // system_ram
            cn.size = size;
            cn.address_hex = hex_buf;
            cn.note = clean_note;
            cn.author = user_str;
            cn.score = score;
            results.push_back(cn);
         }
      }

      curr = obj_end + 1;
   }

   // Sort candidates by score descending, then by address ascending
   std::stable_sort(results.begin(), results.end(), [](const RaCodeNote &a, const RaCodeNote &b) {
      if (a.score != b.score)
         return a.score > b.score;
      return a.address < b.address;
   });

   // Deduplicate addresses, keeping the highest-scoring note per address
   std::vector<RaCodeNote> deduped;
   for (const auto &note : results)
   {
      bool exists = false;
      for (const auto &d : deduped)
      {
         if (d.address == note.address)
         {
            exists = true;
            break;
         }
      }
      if (!exists)
         deduped.push_back(note);
   }

   return deduped;
}

RaLookupResult ra_lookup_music_notes(const std::string &md5_hex)
{
   RaLookupResult res;
   if (md5_hex.empty())
   {
      res.status = RaLookupStatus::NO_GAME;
      res.message = "Empty ROM MD5";
      return res;
   }

   const std::string url = "https://retroachievements.org/dorequest.php";

   // 1. Resolve ROM hash to game ID
   std::string post_game = "r=gameid&m=" + md5_hex;
   std::string response_game, err;
   if (!http_fetch(url, post_game, response_game, err))
   {
      res.status = RaLookupStatus::ERROR_NET;
      res.message = err;
      return res;
   }

   int game_id = ra_parse_game_id_json(response_game);
   if (game_id <= 0)
   {
      res.status = RaLookupStatus::NO_GAME;
      res.message = "ROM hash not found on RetroAchievements";
      return res;
   }

   res.game_id = game_id;

   // 2. Fetch code notes for game ID
   std::string post_notes = "r=codenotes2&g=" + std::to_string(game_id);
   std::string response_notes;
   if (!http_fetch(url, post_notes, response_notes, err))
   {
      res.status = RaLookupStatus::ERROR_NET;
      res.message = err;
      return res;
   }

   res.notes = ra_parse_music_notes_json(response_notes);
   if (res.notes.empty())
   {
      res.status = RaLookupStatus::NO_NOTES;
      res.message = "No music notes documented for game " + std::to_string(game_id);
   }
   else
   {
      res.status = RaLookupStatus::SUCCESS;
      res.message = "Found " + std::to_string(res.notes.size()) + " candidate music address" +
                    (res.notes.size() == 1 ? "" : "es");
   }

   return res;
}
