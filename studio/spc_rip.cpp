// SPDX-License-Identifier: LGPL-2.1-or-later
#include "spc_rip.h"

#include <atomic>
#include <cstring>
#include <ctime>

// snes9x (libretro) state layout, from apu/apu.cpp S9xAPUSaveState and
// apu/bapu/smp/smp_state.cpp:
//   "SND" block: 64 KB sound RAM, 41 little-endian int32 SMP fields,
//   the SPC_DSP state (128 register bytes first; 642 bytes in all in snes9x 1.61),
//   reference_time, remainder, dsp clock, and the 4 CPU->APU port bytes,
//   then zero padding.
static const size_t kRam        = 0x10000;
static const size_t kSmpFields  = 41;
static std::atomic<size_t> g_dsp_state{642};

enum SmpField
{
   F_CLOCK, F_OPCODE_NUMBER, F_OPCODE_CYCLE,
   F_PC, F_SP, F_A, F_X, F_Y,
   F_N, F_V, F_P, F_B, F_H, F_I, F_Z, F_C,
   F_IPLROM,
   F_DSP_ADDR,
   F_F8, F_F9,
   F_T0_ENABLE, F_T0_TARGET, F_T0_S1, F_T0_S2, F_T0_S3,
   F_T1_ENABLE, F_T1_TARGET, F_T1_S1, F_T1_S2, F_T1_S3,
   F_T2_ENABLE, F_T2_TARGET, F_T2_S1, F_T2_S2, F_T2_S3,
};

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Finds a block ("SND") in a snes9x snapshot: "#!s9xsnp:NNNN\n" then "TAG:LLLLLL:" + data.
static const uint8_t *find_block(const std::vector<uint8_t> &st, const char *tag, size_t *len)
{
   if (st.size() < 14 || memcmp(st.data(), "#!s9xsnp:", 9) != 0)
      return nullptr;
   size_t pos = 14;
   while (pos + 11 <= st.size() && st[pos + 3] == ':' && st[pos + 10] == ':')
   {
      size_t n = 0;
      for (int i = 4; i < 10; i++)
      {
         if (st[pos + i] < '0' || st[pos + i] > '9')
            return nullptr;
         n = n * 10 + (st[pos + i] - '0');
      }
      if (pos + 11 + n > st.size())
         return nullptr;
      if (memcmp(&st[pos], tag, 3) == 0)
      {
         *len = n;
         return &st[pos + 11];
      }
      pos += 11 + n;
   }
   return nullptr;
}

static void put_text(uint8_t *dst, size_t size, const std::string &s)
{
   memset(dst, 0, size);
   memcpy(dst, s.data(), s.size() < size ? s.size() : size);
}

// The IPL boot ROM mapped at $FFC0 while control bit 7 is set.
static const uint8_t kIplRom[64] = {
   0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
   0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
   0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
   0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF,
};

bool spc_calibrate_snes9x(const std::vector<uint8_t> &state,
      const std::function<std::vector<uint8_t>(const std::vector<uint8_t> &)> &round_trip)
{
   size_t len = 0;
   const uint8_t *snd = find_block(state, "SND", &len);
   if (!snd)
      return false;
   size_t snd_at = snd - state.data();

   // A byte is a saved field if a change to it survives loading and saving again.
   auto live = [&](size_t off) {
      std::vector<uint8_t> st = state;
      st[snd_at + off] ^= 0x5A;
      std::vector<uint8_t> back = round_trip(st);
      return back.size() == st.size() && back[snd_at + off] == st[snd_at + off];
   };
   // The port bytes are the last field: live, followed by padding.
   auto fits = [&](size_t dsp) {
      size_t ports = kRam + kSmpFields * 4 + dsp + 12;
      return ports + 5 <= len && live(ports + 3) && !live(ports + 4);
   };
   std::vector<size_t> sizes = { 642, 486 };
   for (size_t s = 400; s <= 900; s++)
      sizes.push_back(s);
   for (size_t dsp : sizes)
      if (fits(dsp))
      {
         g_dsp_state = dsp;
         return true;
      }
   return false;
}

bool spc_snes9x_ports(const std::vector<uint8_t> &state, uint8_t ports[4])
{
   size_t len = 0;
   const uint8_t *snd = find_block(state, "SND", &len);
   const size_t at = kRam + kSmpFields * 4 + g_dsp_state + 12;
   if (!snd || len < at + 4)
      return false;
   memcpy(ports, snd + at, 4);
   return true;
}

bool spc_from_snes9x_state(const std::vector<uint8_t> &state, const SpcTags &tags,
      std::vector<uint8_t> &spc, SpcState *info, std::string &error)
{
   size_t len = 0;
   const uint8_t *snd = find_block(state, "SND", &len);
   if (!snd)
   {
      error = "not a snes9x save state (no SND block)";
      return false;
   }
   const size_t ports_at = kRam + kSmpFields * 4 + g_dsp_state + 12;
   if (len < ports_at + 4)
   {
      error = "unexpected snes9x SND block size " + std::to_string(len);
      return false;
   }

   const uint8_t *ram = snd;
   const uint8_t *smp = snd + kRam;
   const uint8_t *dsp = smp + kSmpFields * 4;
   const uint8_t *ports = snd + ports_at;
   auto field = [&](SmpField f) { return rd32(smp + f * 4); };

   if (info)
      info->mid_opcode = field(F_OPCODE_CYCLE) != 0;

   spc.assign(0x10200, 0);
   uint8_t *h = spc.data();
   memcpy(h, "SNES-SPC700 Sound File Data v0.30", 33);
   h[0x21] = 26;
   h[0x22] = 26;
   h[0x23] = 26;   // has ID666 tags
   h[0x24] = 30;

   uint32_t pc = field(F_PC);
   h[0x25] = pc & 0xFF;
   h[0x26] = (pc >> 8) & 0xFF;
   h[0x27] = (uint8_t)field(F_A);
   h[0x28] = (uint8_t)field(F_X);
   h[0x29] = (uint8_t)field(F_Y);
   h[0x2A] = (field(F_N) ? 0x80 : 0) | (field(F_V) ? 0x40 : 0) | (field(F_P) ? 0x20 : 0) |
             (field(F_B) ? 0x10 : 0) | (field(F_H) ? 0x08 : 0) | (field(F_I) ? 0x04 : 0) |
             (field(F_Z) ? 0x02 : 0) | (field(F_C) ? 0x01 : 0);
   h[0x2B] = (uint8_t)field(F_SP);

   // Text ID666 tags. No play length, so players loop the song indefinitely.
   put_text(h + 0x2E, 32, tags.song);
   put_text(h + 0x4E, 32, tags.game);
   put_text(h + 0x6E, 16, tags.dumper);
   put_text(h + 0x7E, 32, tags.comment);
   char date[16];
   time_t now = time(nullptr);
   strftime(date, sizeof(date), "%m/%d/%Y", localtime(&now));
   put_text(h + 0x9E, 11, date);

   uint8_t *out_ram = h + 0x100;
   memcpy(out_ram, ram, kRam);

   // The SMP keeps its I/O registers outside RAM; write them where .spc expects.
   bool iplrom = field(F_IPLROM) != 0;
   out_ram[0xF1] = (iplrom ? 0x80 : 0) | (field(F_T2_ENABLE) ? 4 : 0) |
                   (field(F_T1_ENABLE) ? 2 : 0) | (field(F_T0_ENABLE) ? 1 : 0);
   out_ram[0xF2] = (uint8_t)field(F_DSP_ADDR);
   memcpy(out_ram + 0xF4, ports, 4);   // what the sound CPU reads from the game
   out_ram[0xF8] = (uint8_t)field(F_F8);
   out_ram[0xF9] = (uint8_t)field(F_F9);
   out_ram[0xFA] = (uint8_t)field(F_T0_TARGET);
   out_ram[0xFB] = (uint8_t)field(F_T1_TARGET);
   out_ram[0xFC] = (uint8_t)field(F_T2_TARGET);
   out_ram[0xFD] = field(F_T0_S3) & 0x0F;
   out_ram[0xFE] = field(F_T1_S3) & 0x0F;
   out_ram[0xFF] = field(F_T2_S3) & 0x0F;

   // RAM hidden under the IPL ROM goes in the "extra RAM" area.
   memcpy(h + 0x101C0, ram + 0xFFC0, 64);
   if (iplrom)
      memcpy(out_ram + 0xFFC0, kIplRom, 64);

   memcpy(h + 0x10100, dsp, 128);
   return true;
}
