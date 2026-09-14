// SPDX-License-Identifier: LGPL-2.1-or-later
#include "song_notes.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "gme.h"

static const int kRate = 32000;
static const int kSeconds = 30;
static const int kFft = 4096;      // 128 ms: separates semitones down to about 100 Hz
static const int kHop = 1600;      // 50 ms per frame
static const int kProbe = 60;      // frames: 3 seconds of the rip, slid along the reference
static const int kMinOverlap = 200; // frames: 10 seconds compared at the chosen alignment

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

// Mean of (1 - similarity) over frames i of a against i + shift of b; frames silent in both
// are skipped, and silence against sound counts as fully different.
static double aligned_distance(const SongNotes &a, int from, int to, const SongNotes &b, int shift, int *counted_out)
{
   double sum = 0;
   int counted = 0;
   for (int i = std::max(from, -shift); i < to && i + shift < b.frames; i++)
   {
      const float *x = &a.chroma[12 * i], *y = &b.chroma[12 * (i + shift)];
      double dot = 0, nx = 0, ny = 0;
      for (int k = 0; k < 12; k++)
      {
         dot += x[k] * y[k];
         nx += x[k] * x[k];
         ny += y[k] * y[k];
      }
      if (nx < 0.5 && ny < 0.5)
         continue;
      sum += 1.0 - dot;
      counted++;
   }
   *counted_out = counted;
   return counted ? sum / counted : 1.0;
}

// References are often dumped seconds into their songs, or after an intro, so the two may be
// many seconds apart. A few 3-second probes of the rip are slid along the whole reference; the
// best alignment is then measured over everything the two share.
double notes_frame_seconds()
{
   return (double)kHop / kRate;
}

double notes_distance(const SongNotes &a, const SongNotes &b, int *shift)
{
   if (shift)
      *shift = 0;
   if (a.frames < kProbe || b.frames < kProbe)
      return 1.0;
   double best = 1.0;
   for (int probe = 20; probe + kProbe <= a.frames; probe += 140)
   {
      // A probe must have sound in most frames to say anything.
      int loud = 0;
      for (int i = probe; i < probe + kProbe; i++)
      {
         double n = 0;
         for (int k = 0; k < 12; k++)
            n += a.chroma[12 * i + k] * a.chroma[12 * i + k];
         loud += n > 0.5;
      }
      if (loud < kProbe * 2 / 3)
         continue;
      int best_shift = 0, counted = 0;
      double probe_best = 1.0;
      for (int start = 0; start + kProbe <= b.frames; start++)
      {
         double d = aligned_distance(a, probe, probe + kProbe, b, start - probe, &counted);
         if (counted >= kProbe / 2 && d < probe_best)
         {
            probe_best = d;
            best_shift = start - probe;
         }
      }
      if (probe_best >= 0.5)
         continue;
      double d = aligned_distance(a, 0, a.frames, b, best_shift, &counted);
      if (counted >= std::min(kMinOverlap, a.frames / 2) && d < best)
      {
         best = d;
         if (shift)
            *shift = best_shift;
      }
   }
   return best;
}
