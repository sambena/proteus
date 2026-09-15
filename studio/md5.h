// SPDX-License-Identifier: GPL-3.0-or-later
// The MD5 message digest (RFC 1321), for ROM and file checksums.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct Md5Context
{
   uint32_t h[4];
   uint64_t length;     // bytes hashed so far
   uint8_t block[64];
   size_t used;         // bytes waiting in `block`
};

void md5_init(Md5Context *ctx);
void md5_update(Md5Context *ctx, const uint8_t *input, size_t input_len);
void md5_final(Md5Context *ctx, uint8_t digest[16]);

// Computes the 32-character lowercase hex MD5 string for the given buffer.
std::string md5_hex(const uint8_t *data, size_t len);
