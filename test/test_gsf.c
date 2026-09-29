/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GBA GSF through px_source: a miniGSF (with its .gsflib) renders ten seconds of sound, seeks
 * forward and back to the same samples a straight render gives, and plays at other rates.
 *   test_gsf <song.minigsf> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"

#define RATE    44100
#define SECONDS 10
#define MIN_RMS 300.0

static int failures;

static void check(int ok, const char *what)
{
   printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok)
      failures++;
}

static double rms(const int16_t *s, size_t samples)
{
   double sum = 0;
   for (size_t i = 0; i < samples; i++)
      sum += (double)s[i] * s[i];
   return samples ? sqrt(sum / samples) : 0;
}

static size_t read_all(px_source *s, int16_t *out, size_t frames)
{
   size_t got = 0;
   while (got < frames)
   {
      size_t n = px_source_read(s, out + got * 2, frames - got > 4096 ? 4096 : frames - got);
      if (!n)
         break;
      got += n;
   }
   return got;
}

int main(int argc, char **argv)
{
   char err[512], line[128];
   const size_t frames = (size_t)RATE * SECONDS, part = RATE; /* one second */
   int16_t *all, *buf;
   px_source *s;

   if (argc < 2)
   {
      fprintf(stderr, "usage: %s <song.minigsf>\n", argv[0]);
      return 2;
   }
   check(px_source_supported(argv[1]), "a .minigsf is a supported source");
   check(!px_source_supported("x.gsflib"), "a .gsflib is not a song");

   all = (int16_t*)malloc(frames * 2 * sizeof(int16_t));
   buf = (int16_t*)malloc(part * 2 * sizeof(int16_t));
   if (!(s = px_source_open(argv[1], 0, false, RATE, err, sizeof(err))))
   {
      printf("FAIL  open: %s\n", err);
      return 1;
   }
   check(px_source_rate(s) == RATE, "renders at the requested rate");
   printf("      not looped: px_source_length_ms %llu\n", (unsigned long long)px_source_length_ms(s));

   /* Ten seconds, straight through. */
   {
      size_t got = read_all(s, all, frames);
      double r = rms(all, got * 2), rl = 0, rr = 0;
      size_t differ = 0;
      for (size_t i = 0; i < got; i++)
      {
         rl += (double)all[i * 2] * all[i * 2];
         rr += (double)all[i * 2 + 1] * all[i * 2 + 1];
         differ += all[i * 2] != all[i * 2 + 1];
      }
      printf("      %zu frames, RMS %.1f (left %.1f, right %.1f), %zu frames differ between channels\n",
             got, r, sqrt(rl / got), sqrt(rr / got), differ);
      check(got == frames, "renders ten seconds");
      snprintf(line, sizeof(line), "RMS above %.0f", MIN_RMS);
      check(r > MIN_RMS, line);
   }

   /* Forward: from 10 s back to 3 s restarts the console; then 3 s -> 6 s renders ahead. */
   check(px_source_seek(s, 3 * (uint64_t)RATE), "seeks back to 3 s");
   check(read_all(s, buf, part) == part && !memcmp(buf, all + 3 * RATE * 2, part * 4),
         "3 s after a seek back matches the straight render");
   check(px_source_seek(s, 6 * (uint64_t)RATE), "seeks forward to 6 s");
   check(read_all(s, buf, part) == part && !memcmp(buf, all + 6 * RATE * 2, part * 4),
         "6 s after a seek forward matches the straight render");
   check(px_source_seek(s, 0), "seeks to the start");
   check(read_all(s, buf, part) == part && !memcmp(buf, all, part * 4), "the start renders again the same");
   px_source_close(s);

   /* Other output rates, up to the top of the range. */
   {
      static const unsigned rates[] = { 22050, 48000, 96000, 192000 };
      for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
      {
         size_t n = rates[i] * 2;
         int16_t *b = (int16_t*)malloc(n * 2 * sizeof(int16_t));
         s = px_source_open(argv[1], 0, true, rates[i], err, sizeof(err));
         size_t got = s ? read_all(s, b, n) : 0;
         double r = rms(b, got * 2);
         snprintf(line, sizeof(line), "%u Hz: 2 s, RMS %.1f", rates[i], r);
         check(s && px_source_rate(s) == rates[i] && got == n && r > MIN_RMS, line);
         check(s && px_source_length_ms(s) == 0, "looping: no length");
         px_source_close(s);
         free(b);
      }
   }

   /* A missing library is an error, not a crash. */
   check(!px_source_open("build/test/no-such.minigsf", 0, false, RATE, err, sizeof(err)), "a missing file fails to open");

   free(all);
   free(buf);
   printf(failures ? "%d failed\n" : "all passed\n", failures);
   return failures ? 1 : 0;
}
