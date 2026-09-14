// SPDX-License-Identifier: LGPL-2.1-or-later
#include "song_notes.h"

#include <cmath>
#include <complex>

#include "gme.h"

static const int kRate = 32000;
static const int kSeconds = 10;
static const int kFft = 4096;      // 128 ms: separates semitones down to about 100 Hz
static const int kHop = 1600;      // 50 ms per frame
static const int kMaxShift = 40;   // frames: 2 seconds

static void fft(std::vector<std::complex<double>> &a)
{
   size_t n = a.size();
   for (size_t i = 1, j = 0; i < n; i++)
   {
      size_t bit = n >> 1;
      for (; j & bit; bit >>= 1)
         j ^= bit;
      j ^= bit;
      if (i < j)
         std::swap(a[i], a[j]);
   }
   for (size_t len = 2; len <= n; len <<= 1)
   {
      double ang = -2 * M_PI / (double)len;
      std::complex<double> wl(std::cos(ang), std::sin(ang));
      for (size_t i = 0; i < n; i += len)
      {
         std::complex<double> w(1);
         for (size_t k = 0; k < len / 2; k++, w *= wl)
         {
            std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
            a[i + k] = u + v;
            a[i + k + len / 2] = u - v;
         }
      }
   }
}

bool spc_notes(const std::vector<uint8_t> &spc, SongNotes &notes, std::string &error)
{
   Music_Emu *emu = nullptr;
   if (gme_err_t e = gme_open_data(spc.data(), (long)spc.size(), &emu, kRate))
   {
      error = e;
      return false;
   }
   gme_ignore_silence(emu, 1);
   if (gme_err_t e = gme_start_track(emu, 0))
   {
      error = e;
      gme_delete(emu);
      return false;
   }
   std::vector<short> stereo(kRate * kSeconds * 2);
   gme_play(emu, (int)stereo.size(), stereo.data());
   gme_delete(emu);

   std::vector<double> mono(stereo.size() / 2);
   for (size_t i = 0; i < mono.size(); i++)
      mono[i] = (stereo[2 * i] + stereo[2 * i + 1]) / 65536.0;

   // Pitch class of each FFT bin between 80 Hz and 4 kHz (A = 0), or -1.
   std::vector<int> bin_class(kFft / 2, -1);
   for (int b = 1; b < kFft / 2; b++)
   {
      double hz = (double)b * kRate / kFft;
      if (hz < 80 || hz > 4000)
         continue;
      int semis = (int)std::lround(12 * std::log2(hz / 440.0));
      bin_class[b] = ((semis % 12) + 12) % 12;
   }
   std::vector<double> window(kFft);
   for (int i = 0; i < kFft; i++)
      window[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / (kFft - 1));

   notes.chroma.clear();
   notes.frames = 0;
   std::vector<std::complex<double>> buf(kFft);
   for (size_t start = 0; start + kFft <= mono.size(); start += kHop)
   {
      for (int i = 0; i < kFft; i++)
         buf[i] = mono[start + i] * window[i];
      fft(buf);
      double c[12] = { 0 };
      for (int b = 1; b < kFft / 2; b++)
         if (bin_class[b] >= 0)
            c[bin_class[b]] += std::abs(buf[b]);
      double norm = 0;
      for (double v : c)
         norm += v * v;
      norm = std::sqrt(norm);
      for (double v : c)
         notes.chroma.push_back(norm > 1e-3 ? (float)(v / norm) : 0.0f);
      notes.frames++;
   }
   return true;
}

double notes_distance(const SongNotes &a, const SongNotes &b)
{
   double best = 1.0;
   for (int shift = -kMaxShift; shift <= kMaxShift; shift++)
   {
      double sum = 0;
      int counted = 0;
      for (int i = 0; i < a.frames; i++)
      {
         int j = i + shift;
         if (j < 0 || j >= b.frames)
            continue;
         const float *x = &a.chroma[12 * i], *y = &b.chroma[12 * j];
         double dot = 0, nx = 0, ny = 0;
         for (int k = 0; k < 12; k++)
         {
            dot += x[k] * y[k];
            nx += x[k] * x[k];
            ny += y[k] * y[k];
         }
         if (nx < 0.5 && ny < 0.5)
            continue;               // both silent
         sum += 1.0 - dot;          // silent against sound counts as fully different
         counted++;
      }
      if (counted >= a.frames / 2)
         best = std::min(best, sum / counted);
   }
   return best;
}
