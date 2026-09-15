// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for ROM MD5 calculation and RetroAchievements code notes parsing.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "md5.h"
#include "ra_client.h"

static int g_failures = 0;

#define TEST(expr, msg) do { \
   if (!(expr)) { \
      printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_failures++; \
   } else { \
      printf("  ok: %s\n", msg); \
   } \
} while (0)

static void test_md5()
{
   printf("scenario: MD5 test vectors (RFC 1321)\n");

   // RFC 1321 test suite
   TEST(md5_hex((const uint8_t*)"", 0) == "d41d8cd98f00b204e9800998ecf8427e", "MD5(\"\")");
   TEST(md5_hex((const uint8_t*)"a", 1) == "0cc175b9c0f1b6a831c399e269772661", "MD5(\"a\")");
   TEST(md5_hex((const uint8_t*)"abc", 3) == "900150983cd24fb0d6963f7d28e17f72", "MD5(\"abc\")");
   TEST(md5_hex((const uint8_t*)"message digest", 14) == "f96b697d7cb7938d525a2f31aaf161d0",
        "MD5(\"message digest\")");
   TEST(md5_hex((const uint8_t*)"abcdefghijklmnopqrstuvwxyz", 26) == "c3fcd3d76192e4007dfb496cca67e13b",
        "MD5(alphabet)");
   TEST(md5_hex((const uint8_t*)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62) ==
        "d174ab98d277d9f5a5611c2c9f419d9f", "MD5(62 characters: padding spills into a second block)");
   {
      const char *eighty = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
      TEST(md5_hex((const uint8_t*)eighty, 80) == "57edf4a22be3c955ac49da2e2107b67a", "MD5(80 digits: more than one block)");
      // Fed in uneven pieces, the same digest.
      Md5Context ctx;
      uint8_t d[16];
      char hex[33];
      md5_init(&ctx);
      md5_update(&ctx, (const uint8_t*)eighty, 3);
      md5_update(&ctx, (const uint8_t*)eighty + 3, 64);
      md5_update(&ctx, (const uint8_t*)eighty + 67, 13);
      md5_final(&ctx, d);
      for (int i = 0; i < 16; i++)
         snprintf(hex + i * 2, 3, "%02x", d[i]);
      TEST(std::string(hex) == "57edf4a22be3c955ac49da2e2107b67a", "MD5(80 digits in pieces)");
   }
}

static void test_ra_json_parser()
{
   printf("scenario: RetroAchievements JSON response parsing\n");

   // Test game ID resolution
   std::string game_res = "{\"Success\":true,\"GameID\":228}";
   TEST(ra_parse_game_id_json(game_res) == 228, "parse GameID 228 (Super Mario World)");

   std::string no_game = "{\"Success\":true,\"GameID\":0}";
   TEST(ra_parse_game_id_json(no_game) == 0, "parse GameID 0 (not found)");

   // Test code notes response
   std::string mock_notes = R"JSON({
      "Success": true,
      "CodeNotes": [
         {
            "User": "authorblues",
            "Address": "0x001f08",
            "Note": "Overworld Event Flags [8 bit]"
         },
         {
            "User": "authorblues",
            "Address": "0x000dda",
            "Note": "Music Register Backup [8bit]"
         },
         {
            "User": "developer_x",
            "Address": "0x7e0523",
            "Note": "Music track playing [8bit]"
         },
         {
            "User": "developer_x",
            "Address": "0x000521",
            "Note": "Music track selected [16bit]"
         },
         {
            "User": "sfx_dev",
            "Address": "0x001dfc",
            "Note": "Sound effect trigger (SFX 1) [8bit]"
         }
      ]
   })JSON";

   std::vector<RaCodeNote> notes = ra_parse_music_notes_json(mock_notes);
   TEST(notes.size() == 3, "filters out non-music notes and SFX-only notes");

   // Verify normalized addresses and ranking
   // 1. "Music track playing" ($0523, 24-bit normalized from $7E0523) or "Music track selected"
   bool found_dda = false, found_523 = false, found_521 = false;
   for (const auto &n : notes)
   {
      if (n.address == 0x0DDA)
      {
         found_dda = true;
         TEST(n.size == 1, "0x0DDA size is 1 byte");
         TEST(n.address_hex == "$0DDA", "0x0DDA hex display string");
         TEST(n.author == "authorblues", "0x0DDA author");
      }
      else if (n.address == 0x0523)
      {
         found_523 = true;
         TEST(n.size == 1, "0x0523 (from 0x7e0523) size is 1 byte");
         TEST(n.address_hex == "$0523", "0x7e0523 correctly stripped to $0523 in system_ram");
      }
      else if (n.address == 0x0521)
      {
         found_521 = true;
         TEST(n.size == 2, "0x0521 size detected as 2 bytes from [16bit]");
      }
   }
   TEST(found_dda, "found 0x0DDA (Music Register Backup)");
   TEST(found_523, "found 0x0523 (Music track playing)");
   TEST(found_521, "found 0x0521 (Music track selected)");

   // Verify highest ranked is a high-confidence phrase (Music track playing / selected)
   if (!notes.empty())
   {
      TEST(notes[0].score >= 100, "top note has high confidence score");
   }
}

int main()
{
   test_md5();
   test_ra_json_parser();

   if (g_failures == 0)
   {
      printf("\nPASSED (0 failures)\n");
      return 0;
   }
   else
   {
      printf("\nFAILED (%d failure%s)\n", g_failures, g_failures == 1 ? "" : "s");
      return 1;
   }
}
