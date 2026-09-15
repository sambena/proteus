// SPDX-License-Identifier: LGPL-2.1-or-later
#include "nsf_init.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace {

struct Nsf
{
   uint16_t load = 0, init = 0, play = 0;
   uint8_t banks[8] = { 0 };
   bool banked = false;
   int songs = 0;
   std::vector<uint8_t> data;
};

bool parse(const std::vector<uint8_t> &d, Nsf &n, std::string &error)
{
   auto word = [&](size_t at) { return (uint16_t)(d[at] | d[at + 1] << 8); };
   if (d.size() > 0x80 && !memcmp(d.data(), "NESM\x1A", 5))
   {
      n.songs = d[6];
      n.load = word(8);
      n.init = word(10);
      n.play = word(12);
      memcpy(n.banks, &d[0x70], 8);
      n.data.assign(d.begin() + 0x80, d.end());
   }
   else if (d.size() > 4 && !memcmp(d.data(), "NSFE", 4))
   {
      for (size_t pos = 4; pos + 8 <= d.size();)
      {
         uint32_t size = d[pos] | d[pos + 1] << 8 | d[pos + 2] << 16 | (uint32_t)d[pos + 3] << 24;
         const char *id = (const char*)&d[pos + 4];
         size_t body = pos + 8;
         if (body + size > d.size() || !memcmp(id, "NEND", 4))
            break;
         if (!memcmp(id, "INFO", 4) && size >= 9)
         {
            n.load = word(body);
            n.init = word(body + 2);
            n.play = word(body + 4);
            n.songs = d[body + 8];
         }
         else if (!memcmp(id, "BANK", 4))
            memcpy(n.banks, &d[body], std::min<size_t>(8, size));
         else if (!memcmp(id, "DATA", 4))
            n.data.assign(d.begin() + body, d.begin() + body + size);
         pos = body + size;
      }
   }
   else
   {
      error = "not an .nsf file";
      return false;
   }
   for (uint8_t b : n.banks)
      n.banked = n.banked || b;
   if (n.data.empty() || n.songs <= 0 || (!n.banked && n.load < 0x8000))
   {
      error = "an .nsf file without its data";
      return false;
   }
   return true;
}

// Just enough NES for an NSF: RAM, the cartridge's $6000-$7FFF RAM, and bank-switched ROM.
class Machine
{
public:
   explicit Machine(const Nsf &nsf) : nsf_(nsf)
   {
      memset(ram_, 0, sizeof(ram_));
      memset(wram_, 0, sizeof(wram_));
      memcpy(page_, nsf.banks, 8);
      if (!nsf.banked)
         for (int i = 0; i < 8; i++)
            page_[i] = (uint8_t)i;
   }

   uint8_t read(uint16_t a) const
   {
      if (a >= 0x8000 && !patches_.empty())
      {
         auto p = patches_.find(a);
         if (p != patches_.end())
            return p->second;
      }
      return read_raw(a);
   }

   // Subroutine calls made so far: target -> A at each call, in order.
   std::vector<std::pair<uint16_t, uint8_t>> calls;
   bool record_calls = false;

   // Code patches, like a cheat code's: the byte read at a CPU address.
   void patch(uint16_t address, uint8_t value) { patches_[address] = value; }
   // Addresses of the branches and subroutine calls executed so far.
   const std::set<uint16_t> &branches() const { return branches_; }
   void record_branches(bool on) { record_ = on; }

   uint8_t read_raw(uint16_t a) const
   {
      if (a < 0x2000)
         return ram_[a & 0x7FF];
      if (a == 0x2002)
         return 0x80;   // "in vertical blank", for code that waits for it
      if (a >= 0x6000 && a < 0x8000)
         return wram_[a - 0x6000];
      if (a < 0x8000)
         return 0;
      size_t at;
      if (nsf_.banked)
         at = (size_t)page_[(a - 0x8000) >> 12] * 0x1000 + (a & 0xFFF);
      else
         at = (size_t)a;
      size_t base = nsf_.banked ? (nsf_.load & 0xFFF) : nsf_.load;
      return at >= base && at - base < nsf_.data.size() ? nsf_.data[at - base] : 0;
   }

   void write(uint16_t a, uint8_t v)
   {
      if (a < 0x2000)
      {
         ram_[a & 0x7FF] = v;
         written_.insert(a & 0x7FF);
      }
      else if (a >= 0x4000 && a <= 0x4015)
         apu_[a - 0x4000] = v;
      else if (a >= 0x5FF8 && a <= 0x5FFF)
         page_[a - 0x5FF8] = v;
      else if (a >= 0x6000 && a < 0x8000)
         wram_[a - 0x6000] = v;
   }

   // Calls `address` with A and X set, until it returns or runs too long.
   void call(uint16_t address, uint8_t a_in, uint8_t x_in)
   {
      a = a_in;
      x = x_in;
      y = 0;
      p = 0x24;
      s = 0xFD;
      push16(0xFFFE);   // RTS returns to $FFFF, where the call ends
      pc = address;
      for (long steps = 0; steps < 2000000 && pc != 0xFFFF; steps++)
         if (!step())
            break;
   }

   const uint8_t *ram() const { return ram_; }
   uint8_t *ram_mut() { return ram_; }
   const std::set<uint16_t> &written() const { return written_; }
   // Channels the sound registers leave audible (bit 0 pulse 1 .. bit 4 samples): enabled, with
   // a volume (or envelope) and a pitch. Length counters are not emulated, so a note left
   // enabled counts as sounding, which is what matters here.
   int audible() const
   {
      int on = 0, enabled = apu_[0x15];
      auto volume = [&](int reg) { return (apu_[reg] & 0x10) ? (apu_[reg] & 0x0F) != 0 : true; };
      for (int c = 0; c < 2; c++)
      {
         int base = c * 4, period = apu_[base + 2] | (apu_[base + 3] & 7) << 8;
         if ((enabled >> c & 1) && volume(base) && period >= 8)
            on |= 1 << c;
      }
      if ((enabled & 4) && (apu_[8] & 0x7F) && (apu_[0x0A] | (apu_[0x0B] & 7) << 8) >= 2)
         on |= 4;
      if ((enabled & 8) && volume(0x0C))
         on |= 8;
      if (enabled & 0x10)
         on |= 0x10;
      return on;
   }

private:
   const Nsf &nsf_;
   uint8_t ram_[0x800], wram_[0x2000], page_[8], apu_[0x16] = { 0 };
   std::map<uint16_t, uint8_t> patches_;
   std::set<uint16_t> branches_;
   bool record_ = false;
   std::set<uint16_t> written_;
   uint8_t a = 0, x = 0, y = 0, p = 0x24, s = 0xFD;
   uint16_t pc = 0;

   void push(uint8_t v) { write(0x100 | s--, v); }
   uint8_t pull() { return read(0x100 | ++s); }
   void push16(uint16_t v) { push(v >> 8); push(v & 0xFF); }
   uint16_t pull16() { uint8_t lo = pull(); return (uint16_t)(lo | pull() << 8); }
   uint8_t fetch() { return read(pc++); }
   uint16_t fetch16() { uint8_t lo = fetch(); return (uint16_t)(lo | fetch() << 8); }
   uint16_t read16_zp(uint8_t z) const { return (uint16_t)(read(z) | read((uint8_t)(z + 1)) << 8); }
   void nz(uint8_t v) { p = (uint8_t)((p & ~0x82) | (v & 0x80) | (v ? 0 : 0x02)); }
   void flag(uint8_t f, bool on) { p = on ? (p | f) : (p & ~f); }

   void compare(uint8_t r, uint8_t v)
   {
      flag(0x01, r >= v);
      nz((uint8_t)(r - v));
   }

   void adc(uint8_t v)
   {
      unsigned sum = (unsigned)a + v + (p & 1);
      flag(0x40, (~(a ^ v) & (a ^ sum) & 0x80) != 0);
      flag(0x01, sum > 0xFF);
      a = (uint8_t)sum;
      nz(a);
   }

   // The effective address of a group-1 style addressing mode `mode` (bbb of aaabbbcc).
   uint16_t address(int mode, bool index_y = false)
   {
      switch (mode)
      {
         case 0: { uint8_t z = (uint8_t)(fetch() + x); return read16_zp(z); }   // (zp,X)
         case 1: return fetch();                                               // zp
         case 3: return fetch16();                                             // abs
         case 4: return (uint16_t)(read16_zp(fetch()) + y);                    // (zp),Y
         case 5: return (uint8_t)(fetch() + (index_y ? y : x));                // zp,X / zp,Y
         case 6: return (uint16_t)(fetch16() + y);                             // abs,Y
         case 7: return (uint16_t)(fetch16() + (index_y ? y : x));             // abs,X / abs,Y
      }
      return 0;
   }

   uint8_t shift(int op, uint8_t v)
   {
      uint8_t carry = p & 1;
      switch (op)
      {
         case 0: flag(0x01, v & 0x80); v = (uint8_t)(v << 1); break;               // ASL
         case 1: flag(0x01, v & 0x80); v = (uint8_t)(v << 1 | carry); break;       // ROL
         case 2: flag(0x01, v & 0x01); v = (uint8_t)(v >> 1); break;               // LSR
         case 3: flag(0x01, v & 0x01); v = (uint8_t)(v >> 1 | carry << 7); break;  // ROR
      }
      nz(v);
      return v;
   }

   bool step()
   {
      if (record_ && pc >= 0x8000)
      {
         uint8_t at = read(pc);
         if ((at & 0x1F) == 0x10 || at == 0x20)
            branches_.insert(pc);
      }
      uint8_t op = fetch();
      // Branches: xxy10000.
      if ((op & 0x1F) == 0x10)
      {
         static const uint8_t kFlags[] = { 0x80, 0x40, 0x01, 0x02 };
         bool set = (p & kFlags[op >> 6]) != 0;
         int8_t offset = (int8_t)fetch();
         if (set == ((op & 0x20) != 0))
            pc = (uint16_t)(pc + offset);
         return true;
      }
      switch (op)
      {
         case 0x00: return false;                                   // BRK: not in music code
         case 0x20:
         {
            uint16_t t = fetch16();
            if (record_calls && t >= 0x8000)
               calls.push_back({ t, a });
            push16((uint16_t)(pc - 1));
            pc = t;
            return true;
         }
         case 0x40: p = pull(); pc = pull16(); return true;
         case 0x60: pc = (uint16_t)(pull16() + 1); return true;
         case 0x08: push(p | 0x30); return true;
         case 0x28: p = pull(); return true;
         case 0x48: push(a); return true;
         case 0x68: a = pull(); nz(a); return true;
         case 0x88: y--; nz(y); return true;
         case 0xA8: y = a; nz(y); return true;
         case 0xC8: y++; nz(y); return true;
         case 0xE8: x++; nz(x); return true;
         case 0xCA: x--; nz(x); return true;
         case 0xAA: x = a; nz(x); return true;
         case 0x8A: a = x; nz(a); return true;
         case 0x98: a = y; nz(a); return true;
         case 0x9A: s = x; return true;
         case 0xBA: x = s; nz(x); return true;
         case 0x18: flag(0x01, false); return true;
         case 0x38: flag(0x01, true); return true;
         case 0x58: flag(0x04, false); return true;
         case 0x78: flag(0x04, true); return true;
         case 0xB8: flag(0x40, false); return true;
         case 0xD8: flag(0x08, false); return true;
         case 0xF8: flag(0x08, true); return true;
         case 0xEA: return true;
         case 0x0A: case 0x2A: case 0x4A: case 0x6A: a = shift(op >> 5, a); return true;
         case 0x4C: pc = fetch16(); return true;
         case 0x6C: { uint16_t t = fetch16(); pc = (uint16_t)(read(t) | read((uint16_t)((t & 0xFF00) | ((t + 1) & 0xFF))) << 8); return true; }
      }

      int group = op >> 5, mode = (op >> 2) & 7;
      switch (op & 3)
      {
         case 1:
         {
            if (group == 4 && mode == 2)
               return false;                                        // no STA #imm
            if (group == 4)
            {
               write(address(mode), a);
               return true;
            }
            uint8_t v = mode == 2 ? fetch() : read(address(mode));
            switch (group)
            {
               case 0: a |= v; nz(a); break;
               case 1: a &= v; nz(a); break;
               case 2: a ^= v; nz(a); break;
               case 3: adc(v); break;
               case 5: a = v; nz(a); break;
               case 6: compare(a, v); break;
               case 7: adc((uint8_t)~v); break;
            }
            return true;
         }
         case 2:
         {
            if (mode == 2 || mode == 4 || mode == 6 || (group == 4 && mode == 7))
               return false;                                        // handled above, or not a real opcode
            if (group == 5 && mode == 0)
            {
               x = fetch();                                         // LDX #imm
               nz(x);
               return true;
            }
            if (mode == 0)
               return false;
            bool by_y = group == 4 || group == 5;                   // STX, LDX index with Y
            uint16_t at = address(mode, by_y);
            switch (group)
            {
               case 4: write(at, x); break;
               case 5: x = read(at); nz(x); break;
               case 6: { uint8_t v = (uint8_t)(read(at) - 1); write(at, v); nz(v); break; }
               case 7: { uint8_t v = (uint8_t)(read(at) + 1); write(at, v); nz(v); break; }
               default: write(at, shift(group, read(at))); break;
            }
            return true;
         }
         case 0:
         {
            if (mode == 2 || mode == 4 || mode == 6 || group < 1 || group == 2 || group == 3 || (mode == 0 && group < 5) ||
                  (group == 1 && mode != 1 && mode != 3) || (group == 4 && mode == 7) || (group >= 6 && mode > 3))
               return false;
            uint8_t v = 0;
            uint16_t at = 0;
            if (mode == 0)
               v = fetch();
            else
            {
               at = address(mode);
               if (group != 4)
                  v = read(at);
            }
            switch (group)
            {
               case 1: flag(0x80, v & 0x80); flag(0x40, v & 0x40); flag(0x02, (a & v) == 0); break;   // BIT
               case 4: write(at, y); break;
               case 5: y = v; nz(y); break;
               case 6: compare(y, v); break;
               case 7: compare(x, v); break;
            }
            return true;
         }
      }
      return false;   // unofficial opcode
   }
};

} // namespace

bool nsf_init_ram(const std::vector<uint8_t> &data, int song, std::map<uint16_t, uint8_t> &writes, std::string &error)
{
   Nsf nsf;
   if (!parse(data, nsf, error))
      return false;
   Machine m(nsf);
   m.call(nsf.init, (uint8_t)song, 0);
   writes.clear();
   for (uint16_t at : m.written())
      if (at < 0x100 || at >= 0x200)   // the stack changes with every call
         writes[at] = m.ram()[at];
   return true;
}

// Runs init for `song`, then play once a frame with `holds` applied; `on_frame` sees each frame's
// channel changes.
static bool run_song(const std::vector<uint8_t> &data, int song, int frames,
      const std::vector<std::pair<uint16_t, uint8_t>> &holds, int activity[NSF_CHANNELS],
      std::set<uint16_t> *written, std::string &error,
      const std::vector<std::pair<uint16_t, uint8_t>> &patches = {}, std::set<uint16_t> *branches = nullptr)
{
   Nsf nsf;
   if (!parse(data, nsf, error))
      return false;
   Machine m(nsf);
   m.call(nsf.init, (uint8_t)song, 0);
   // Patches apply to the music code as it runs, like cheat codes set once the game has started.
   for (const auto &p : patches)
      m.patch(p.first, p.second);
   m.record_branches(branches != nullptr);
   for (int c = 0; c < NSF_CHANNELS; c++)
      activity[c] = 0;
   for (int f = 0; f < frames; f++)
   {
      for (const auto &h : holds)
         if (h.first < 0x800)
            m.ram_mut()[h.first] = h.second;
      m.call(nsf.play, 0, 0);
      int on = m.audible();
      for (int c = 0; c < NSF_CHANNELS; c++)
         activity[c] += (on >> c) & 1;
   }
   if (written)
      *written = m.written();
   if (branches)
      *branches = m.branches();
   return true;
}

std::vector<NsfPatch> nsf_music_patches(const std::vector<uint8_t> &data, int song, int &used, std::string &error)
{
   const int kFrames = 240, kUsed = 24, kSilent = 12;
   std::vector<NsfPatch> out;
   int base[NSF_CHANNELS];
   std::set<uint16_t> branches;
   used = 0;
   if (!run_song(data, song, kFrames, {}, base, nullptr, error, {}, &branches))
      return out;
   for (int c = 0; c < NSF_CHANNELS; c++)
      if (base[c] >= kUsed)
         used |= 1 << c;
   if (!used)
      return out;
   Nsf nsf;
   parse(data, nsf, error);
   for (uint16_t pc : branches)
   {
      // The code bytes as the song leaves them banked in (the play routine's own banks).
      Machine m(nsf);
      m.call(nsf.init, (uint8_t)song, 0);
      uint8_t op = m.read_raw(pc);
      std::vector<std::vector<uint8_t>> variants;
      if (op == 0x20)
         variants.push_back({ 0xEA, 0xEA, 0xEA });   // skip the call
      else
      {
         variants.push_back({ 0x24 });                // never branch (BIT zp)
         variants.push_back({ (uint8_t)(op ^ 0x20) }); // branch on the opposite condition
      }
      int best = 0;
      const std::vector<uint8_t> *best_bytes = nullptr;
      for (const auto &v : variants)
      {
         std::vector<std::pair<uint16_t, uint8_t>> patches;
         for (size_t i = 0; i < v.size(); i++)
            patches.push_back({ (uint16_t)(pc + i), v[i] });
         int act[NSF_CHANNELS];
         if (!run_song(data, song, kFrames, {}, act, nullptr, error, patches))
            return out;
         int silenced = 0;
         for (int c = 0; c < NSF_CHANNELS; c++)
            if ((used >> c & 1) && act[c] <= kSilent)
               silenced |= 1 << c;
         if (__builtin_popcount(silenced) > __builtin_popcount(best))
         {
            best = silenced;
            best_bytes = &v;
         }
      }
      if (best)
      {
         NsfPatch p;
         p.address = pc;
         p.bytes = *best_bytes;
         for (size_t i = 0; i < p.bytes.size(); i++)
            p.original.push_back(m.read_raw((uint16_t)(pc + i)));
         p.silenced = best;
         out.push_back(p);
      }
   }
   std::stable_sort(out.begin(), out.end(), [](const NsfPatch &x, const NsfPatch &y) {
      return __builtin_popcount(x.silenced) > __builtin_popcount(y.silenced);
   });
   return out;
}

std::vector<uint8_t> nsf_code_at(const std::vector<uint8_t> &data, uint16_t address, size_t length, std::string &error)
{
   std::vector<uint8_t> out;
   Nsf nsf;
   if (!parse(data, nsf, error))
      return out;
   Machine m(nsf);
   m.call(nsf.init, 0, 0);
   for (size_t i = 0; i < length; i++)
      out.push_back(m.read_raw((uint16_t)(address + i)));
   return out;
}

std::vector<NsfCall> nsf_init_calls(const std::vector<uint8_t> &data, std::string &error)
{
   std::vector<NsfCall> out;
   Nsf nsf;
   if (!parse(data, nsf, error))
      return out;
   std::map<uint16_t, NsfCall> by_routine;
   for (int song = 0; song < nsf.songs; song++)
   {
      Machine m(nsf);
      m.record_calls = true;
      m.call(nsf.init, (uint8_t)song, 0);
      // The last call to each routine carries the song (earlier ones may reset the driver).
      for (const auto &c : m.calls)
      {
         NsfCall &r = by_routine[c.first];
         r.routine = c.first;
         r.song_values[song] = c.second;
      }
   }
   for (auto &e : by_routine)
   {
      std::set<uint8_t> values;
      for (const auto &v : e.second.song_values)
         values.insert(v.second);
      e.second.distinct = (int)values.size();
      if (e.second.distinct >= 2)
         out.push_back(e.second);
   }
   std::stable_sort(out.begin(), out.end(), [](const NsfCall &x, const NsfCall &y) { return x.distinct > y.distinct; });
   return out;
}

std::vector<NsfVariable> nsf_song_variables(const std::vector<uint8_t> &data, const std::vector<int> &songs, std::string &error)
{
   std::vector<NsfVariable> out;
   Nsf nsf;
   if (!parse(data, nsf, error))
      return out;
   std::map<uint16_t, NsfVariable> by_address;
   std::set<uint16_t> unsteady;
   for (int song : songs)
   {
      Machine m(nsf);
      m.call(nsf.init, (uint8_t)song, 0);
      uint8_t samples[3][0x800];
      for (int f = 1, k = 0; f <= 180; f++)
      {
         m.call(nsf.play, 0, 0);
         if (f % 60 == 0)
            memcpy(samples[k++], m.ram(), 0x800);
      }
      for (uint16_t at = 0; at < 0x800; at++)
      {
         if (at >= 0x100 && at < 0x200)
            continue;
         if (samples[0][at] != samples[1][at] || samples[1][at] != samples[2][at])
         {
            unsteady.insert(at);
            continue;
         }
         NsfVariable &v = by_address[at];
         v.address = at;
         v.song_values[song] = samples[2][at];
      }
   }
   for (auto &e : by_address)
   {
      if (unsteady.count(e.first) || e.second.song_values.size() != songs.size())
         continue;
      std::set<uint8_t> values;
      for (const auto &v : e.second.song_values)
         values.insert(v.second);
      e.second.distinct = (int)values.size();
      if (e.second.distinct >= 2)
         out.push_back(e.second);
   }
   std::stable_sort(out.begin(), out.end(), [](const NsfVariable &x, const NsfVariable &y) { return x.distinct > y.distinct; });
   return out;
}

bool nsf_channel_activity_patched(const std::vector<uint8_t> &data, int song, int frames,
      const std::vector<std::pair<uint16_t, uint8_t>> &patches, int activity[NSF_CHANNELS], std::string &error)
{
   return run_song(data, song, frames, {}, activity, nullptr, error, patches);
}

bool nsf_channel_activity(const std::vector<uint8_t> &data, int song, int frames,
      const std::vector<std::pair<uint16_t, uint8_t>> &holds, int activity[NSF_CHANNELS], std::string &error)
{
   return run_song(data, song, frames, holds, activity, nullptr, error);
}

std::vector<NsfSwitch> nsf_music_switches(const std::vector<uint8_t> &data, int song, int &used, std::string &error)
{
   const int kFrames = 240, kUsed = 24, kSilent = 12;
   std::vector<NsfSwitch> out;
   int base[NSF_CHANNELS];
   std::set<uint16_t> written;
   used = 0;
   if (!run_song(data, song, kFrames, {}, base, &written, error))
      return out;
   for (int c = 0; c < NSF_CHANNELS; c++)
      if (base[c] >= kUsed)
         used |= 1 << c;
   if (!used)
      return out;
   for (uint16_t at : written)
   {
      if (at >= 0x100 && at < 0x200)
         continue;
      int best = 0;
      uint8_t best_value = 0;
      for (uint8_t v : { (uint8_t)0x00, (uint8_t)0xFF, (uint8_t)0x80 })
      {
         int act[NSF_CHANNELS];
         if (!run_song(data, song, kFrames, { { at, v } }, act, nullptr, error))
            return out;
         int silenced = 0;
         for (int c = 0; c < NSF_CHANNELS; c++)
            if ((used >> c & 1) && act[c] <= kSilent)
               silenced |= 1 << c;
         if (__builtin_popcount(silenced) > __builtin_popcount(best))
         {
            best = silenced;
            best_value = v;
         }
      }
      if (best)
      {
         NsfSwitch s;
         s.address = at;
         s.value = best_value;
         s.silenced = best;
         out.push_back(s);
      }
   }
   std::stable_sort(out.begin(), out.end(), [](const NsfSwitch &x, const NsfSwitch &y) {
      return __builtin_popcount(x.silenced) > __builtin_popcount(y.silenced);
   });
   return out;
}

std::vector<NsfRequest> nsf_song_requests(const std::vector<uint8_t> &data, std::string &error)
{
   Nsf nsf;
   std::vector<NsfRequest> out;
   if (!parse(data, nsf, error))
      return out;
   std::map<uint16_t, NsfRequest> by_address;
   for (int song = 0; song < nsf.songs; song++)
   {
      std::map<uint16_t, uint8_t> writes;
      if (!nsf_init_ram(data, song, writes, error))
         return out;
      for (const auto &w : writes)
      {
         NsfRequest &r = by_address[w.first];
         r.address = w.first;
         r.song_values[song] = w.second;
      }
   }
   for (auto &e : by_address)
   {
      std::set<uint8_t> values;
      for (const auto &v : e.second.song_values)
         values.insert(v.second);
      if (values.size() >= 2)
         out.push_back(e.second);
   }
   std::stable_sort(out.begin(), out.end(), [](const NsfRequest &x, const NsfRequest &y) {
      return x.song_values.size() > y.song_values.size();
   });
   return out;
}
