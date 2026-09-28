/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The games Proteus knows in particular, found by the MD5 of their ROMs, which is what the
 * Atari 2600's games go by (Stella's properties do the same). */
#include "fx.h"
#include "games/games.h"

#include <string.h>

#define X(name) extern const px_game px_game_##name;
PX_GAMES(X)
#undef X

static const px_game *const games[] = {
#define X(name) &px_game_##name,
   PX_GAMES(X)
#undef X
};

unsigned px_game_count(void)
{
   return (unsigned)(sizeof(games) / sizeof(games[0]));
}

const px_game *px_game_at(unsigned index)
{
   return index < px_game_count() ? games[index] : NULL;
}

const px_game *px_game_find(const char *md5)
{
   if (!md5 || !*md5)
      return NULL;
   for (unsigned i = 0; i < sizeof(games) / sizeof(games[0]); i++)
      for (const char *const *m = games[i]->md5; m && *m; m++)
         if (!strcmp(*m, md5))
            return games[i];
   return NULL;
}

const char *px_game_fx(const px_game *g, const char *key)
{
   if (!g || !g->fx || !key)
      return NULL;
   for (const char *const *p = g->fx; p[0] && p[1]; p += 2)
      if (!strcmp(p[0], key))
         return p[1];
   return NULL;
}

/* ---------------------------------------------------------------------------
 * MD5 (RFC 1321)
 * ------------------------------------------------------------------------- */

static const uint32_t md5_k[64] = {
   0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
   0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
   0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
   0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
   0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
   0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
   0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
   0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const uint8_t md5_s[64] = {
   7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_block(uint32_t h[4], const uint8_t *p)
{
   uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
   for (unsigned i = 0; i < 16; i++)
      m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16)
            | ((uint32_t)p[i * 4 + 3] << 24);
   for (unsigned i = 0; i < 64; i++)
   {
      uint32_t f, t;
      unsigned g;
      if (i < 16)      { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
      else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
      t = a + f + md5_k[i] + m[g];
      a = d;
      d = c;
      c = b;
      b = b + ((t << md5_s[i]) | (t >> (32 - md5_s[i])));
   }
   h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

void px_md5(const void *data, size_t size, char out[33])
{
   static const char digits[] = "0123456789abcdef";
   uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
   const uint8_t *p = (const uint8_t*)data;
   uint8_t tail[128];
   size_t whole = size / 64, rest = size % 64, n;
   uint64_t bits = (uint64_t)size * 8;

   for (size_t i = 0; i < whole; i++)
      md5_block(h, p + i * 64);

   memset(tail, 0, sizeof(tail));
   if (rest)
      memcpy(tail, p + whole * 64, rest);
   tail[rest] = 0x80;
   n = rest < 56 ? 64 : 128;
   for (unsigned i = 0; i < 8; i++)
      tail[n - 8 + i] = (uint8_t)(bits >> (8 * i));
   md5_block(h, tail);
   if (n == 128)
      md5_block(h, tail + 64);

   for (unsigned i = 0; i < 16; i++)
   {
      uint8_t byte = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
      out[i * 2]     = digits[byte >> 4];
      out[i * 2 + 1] = digits[byte & 15];
   }
   out[32] = '\0';
}
