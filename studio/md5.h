// SPDX-License-Identifier: LGPL-2.1-or-later
// Standard MD5 message-digest algorithm (RFC 1321).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct Md5Context
{
   uint32_t state[4];
   uint32_t count[2];
   uint8_t buffer[64];
};

void md5_init(Md5Context *ctx);
void md5_update(Md5Context *ctx, const uint8_t *input, size_t input_len);
void md5_final(Md5Context *ctx, uint8_t digest[16]);

// Computes the 32-character lowercase hex MD5 string for the given buffer.
std::string md5_hex(const uint8_t *data, size_t len);
