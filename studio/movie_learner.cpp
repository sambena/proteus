// SPDX-License-Identifier: GPL-3.0-or-later
#include "movie_learner.h"

#include <algorithm>

struct Candidate { uint32_t address; int apart; int bad; int top; };

int MovieLearner::add(const uint8_t *apu, const uint8_t *ram, size_t ram_size)
{
   moments_++;
   ReferenceSet::Match m = refs_.match(apu, nullptr);
   int name = m.index;
   if (name >= 0 && std::find(heard_.begin(), heard_.end(), name) == heard_.end())
      heard_.push_back(name);
   if (name >= 0 && name == cur_name_ && name == prev_name_)
      learn(cur_name_, cur_ram_);
   prev_name_ = cur_name_;
   cur_name_ = name;
   cur_ram_.assign(ram, ram + ram_size);
   return name;
}

void MovieLearner::learn(int reference, const std::vector<uint8_t> &ram)
{
   size_t n = ram.size();
   auto it = std::find_if(rows_.begin(), rows_.end(), [&](const Row &r) { return r.reference == reference; });
   if (it == rows_.end() || it->value.size() != n)
   {
      if (it != rows_.end())
         rows_.erase(it);
      Row r;
      r.reference = reference;
      r.count = 1;
      r.value = ram;
      r.bad.assign(n, 0);
      rows_.push_back(std::move(r));
      return;
   }
   it->count++;
   for (size_t a = 0; a < n; a++)
      if (ram[a] != it->value[a] && it->bad[a] < 0xFFFF)
         it->bad[a]++;
}

MovieLearner::Result MovieLearner::result() const
{
   Result res;
   // Songs heard for at least two settled moments; a byte may disagree now and then (a menu,
   // a fanfare over the music), not often.
   std::vector<const Row *> rows;
   for (const Row &r : rows_)
      if (r.count >= 2)
         rows.push_back(&r);
   res.songs = (int)rows.size();
   if (rows.size() < 2)
      return res;
   size_t n = rows[0]->value.size();
   for (const Row *r : rows)
      n = std::min(n, r->value.size());

   // Each byte scores the songs it tells apart: songs where it keeps one value, and no other song
   // has that value. Sound effects, versions of one song and scenes that change the music
   // without the song number keep the best byte from a perfect score.
   std::vector<Candidate> all;
   for (size_t a = 0; a < n; a++)
   {
      int owners[256] = { 0 };
      int steady = 0, bad = 0, top = 0;
      for (const Row *r : rows)
         if (r->bad[a] * 10 <= r->count)
         {
            owners[r->value[a]]++;
            steady++;
            bad += r->bad[a];
            top = std::max(top, (int)r->value[a]);
         }
      if (steady * 2 < (int)rows.size())
         continue;
      int apart = 0;
      for (const Row *r : rows)
         if (r->bad[a] * 10 <= r->count && owners[r->value[a]] == 1)
            apart++;
      all.push_back({ (uint32_t)a, apart, bad, top });
   }
   std::stable_sort(all.begin(), all.end(), [](const Candidate &x, const Candidate &y) {
      // Then song numbers over pointers and such: the smaller the largest value, the likelier.
      if (x.apart != y.apart)
         return x.apart > y.apart;
      return x.bad != y.bad ? x.bad < y.bad : x.top < y.top;
   });
   for (size_t i = 0; i < all.size() && i < 12; i++)
      res.top.push_back({ all[i].address, all[i].apart });
   if (all.empty() || rows.size() < 3)
      return res;
   const Candidate &best = all[0];
   res.candidates = 0;
   for (const Candidate &c : all)
      if (c.apart == best.apart)
         res.candidates++;
   // Most songs told apart, and copies of the song number (the song to return to) tie.
   res.found = best.apart * 4 >= (int)rows.size() * 3 && best.apart >= 3 && res.candidates <= 8;
   res.address = best.address;
   for (size_t i = 1; i < all.size() && all[i].apart == best.apart && i < 8; i++)
      res.ties.push_back(all[i].address);
   for (const Row *r : rows)
      if (r->bad[best.address] * 10 <= r->count)
         res.values[r->reference] = r->value[best.address];
   return res;
}

std::string MovieLearner::describe(uint32_t address) const
{
   std::string s;
   char buf[256];
   for (const Row &r : rows_)
      if (address < r.value.size())
      {
         snprintf(buf, sizeof(buf), "    %02X  %3d/%-3d  %s\n", r.value[address], r.bad[address], r.count, refs_.song(r.reference).title.c_str());
         s += buf;
      }
   return s;
}
