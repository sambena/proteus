// SPDX-License-Identifier: GPL-3.0-or-later
// MD5 written from the algorithm's description: 64 rounds over 512-bit blocks, in four passes that
// each mix a different function of three state words with a message word and a sine constant.
#include "md5.h"

#include <cstdio>

namespace {

// K[i] = floor(|sin(i + 1)| * 2^32)
const uint32_t K[64] = {
   0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
   0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
   0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
   0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
   0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
   0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
   0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
   0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

// Left-rotation amounts, four per pass.
const uint8_t kShift[4][4] = { { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 } };

uint32_t rotl(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

void compress(uint32_t h[4], const uint8_t *p)
{
   uint32_t m[16];
   for (int i = 0; i < 16; i++)
      m[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;

   uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
   for (int i = 0; i < 64; i++)
   {
      int pass = i / 16;
      uint32_t f;
      int word;
      switch (pass)
      {
         case 0:  f = (b & c) | (~b & d); word = i;                 break;
         case 1:  f = (d & b) | (~d & c); word = (5 * i + 1) % 16;  break;
         case 2:  f = b ^ c ^ d;          word = (3 * i + 5) % 16;  break;
         default: f = c ^ (b | ~d);       word = (7 * i) % 16;      break;
      }
      uint32_t next = b + rotl(a + f + K[i] + m[word], kShift[pass][i % 4]);
      a = d;
      d = c;
      c = b;
      b = next;
   }
   h[0] += a;
   h[1] += b;
   h[2] += c;
   h[3] += d;
}

} // namespace

void md5_init(Md5Context *ctx)
{
   ctx->h[0] = 0x67452301;
   ctx->h[1] = 0xefcdab89;
   ctx->h[2] = 0x98badcfe;
   ctx->h[3] = 0x10325476;
   ctx->length = 0;
   ctx->used = 0;
}

void md5_update(Md5Context *ctx, const uint8_t *input, size_t input_len)
{
   ctx->length += input_len;
   while (input_len > 0)
   {
      if (ctx->used == 0 && input_len >= 64)
      {
         compress(ctx->h, input);
         input += 64;
         input_len -= 64;
         continue;
      }
      size_t take = 64 - ctx->used < input_len ? 64 - ctx->used : input_len;
      for (size_t i = 0; i < take; i++)
         ctx->block[ctx->used + i] = input[i];
      ctx->used += take;
      input += take;
      input_len -= take;
      if (ctx->used == 64)
      {
         compress(ctx->h, ctx->block);
         ctx->used = 0;
      }
   }
}

void md5_final(Md5Context *ctx, uint8_t digest[16])
{
   // A 1 bit, zeros up to 56 bytes into a block, then the message length in bits (little endian).
   uint64_t bits = ctx->length * 8;
   uint8_t tail[72] = { 0x80 };
   size_t pad = (ctx->used < 56 ? 56 : 120) - ctx->used;
   for (int i = 0; i < 8; i++)
      tail[pad + i] = (uint8_t)(bits >> (8 * i));
   md5_update(ctx, tail, pad + 8);
   for (int i = 0; i < 16; i++)
      digest[i] = (uint8_t)(ctx->h[i / 4] >> (8 * (i % 4)));
}

std::string md5_hex(const uint8_t *data, size_t len)
{
   Md5Context ctx;
   uint8_t digest[16];
   char hex[33];
   md5_init(&ctx);
   md5_update(&ctx, data, len);
   md5_final(&ctx, digest);
   for (int i = 0; i < 16; i++)
      snprintf(hex + i * 2, 3, "%02x", digest[i]);
   return std::string(hex, 32);
}
