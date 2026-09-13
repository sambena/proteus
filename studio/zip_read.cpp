// SPDX-License-Identifier: LGPL-2.1-or-later
#include "zip_read.h"

#include <cstring>

#include <zlib.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool is_zip(const std::vector<uint8_t> &data)
{
   return data.size() >= 4 && rd32(data.data()) == 0x04034b50;
}

bool zip_read(const std::vector<uint8_t> &zip, const std::function<bool(const std::string &name)> &want,
      const std::function<bool(const std::string &name, std::vector<uint8_t> &data)> &take,
      std::string &error)
{
   if (zip.size() < 22)
   {
      error = "not a zip archive";
      return false;
   }
   size_t eocd = std::string::npos;
   for (size_t i = zip.size() - 22 + 1; i-- > 0 && zip.size() - i <= 65557;)
      if (rd32(&zip[i]) == 0x06054b50) { eocd = i; break; }
   if (eocd == std::string::npos)
   {
      error = "not a zip archive";
      return false;
   }

   uint16_t entries = rd16(&zip[eocd + 10]);
   size_t pos = rd32(&zip[eocd + 16]);
   for (unsigned e = 0; e < entries && pos + 46 <= zip.size(); e++)
   {
      if (rd32(&zip[pos]) != 0x02014b50)
         break;
      uint16_t method = rd16(&zip[pos + 10]);
      uint32_t csize = rd32(&zip[pos + 20]), usize = rd32(&zip[pos + 24]);
      uint16_t nlen = rd16(&zip[pos + 28]), xlen = rd16(&zip[pos + 30]), clen = rd16(&zip[pos + 32]);
      uint32_t local = rd32(&zip[pos + 42]);
      if (pos + 46 + nlen > zip.size())
         break;
      std::string entry((const char*)&zip[pos + 46], nlen);
      pos += 46 + nlen + xlen + clen;

      if (entry.empty() || entry.back() == '/' || !want(entry))
         continue;
      if ((uint64_t)local + 30 > zip.size())
         break;
      size_t data = local + 30 + rd16(&zip[local + 26]) + rd16(&zip[local + 28]);
      if ((uint64_t)data + csize > zip.size())
         break;

      std::vector<uint8_t> out(usize);
      if (method == 0 && csize == usize)
         memcpy(out.data(), &zip[data], usize);
      else if (method == 8)
      {
         z_stream zs{};
         if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
         {
            error = "zlib failed to start";
            return false;
         }
         zs.next_in   = (Bytef*)&zip[data];
         zs.avail_in  = csize;
         zs.next_out  = out.data();
         zs.avail_out = usize;
         int r = inflate(&zs, Z_FINISH);
         inflateEnd(&zs);
         if (r != Z_STREAM_END)
         {
            error = "corrupt zip entry " + entry;
            return false;
         }
      }
      else
      {
         error = "unsupported zip compression in " + entry;
         return false;
      }
      if (!take(entry, out))
         return true;
   }
   return true;
}
