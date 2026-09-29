/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Plays Nintendo DS rips (2SF through vio2sf, NCSF through SSEQPlayer) via px_source: each song
 * must sound (RMS over its first 10 s), seek forward and back to the same audio it plays straight
 * through, and end after its tagged length and fade when not looped.
 *
 *   test_nds <song.mini2sf|song.minincsf>...
 *
 * The rips are not in the repository (see the Makefile's test-nds target). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"

#define RATE 48000
#define CHUNK 4096

static int failures;

static void check(int ok, const char *what, const char *path)
{
   if (!ok)
   {
      printf("  FAIL %s (%s)\n", what, path);
      failures++;
   }
}

/* Reads exactly `frames` frames; returns how many came. */
static size_t read_all(px_source *s, int16_t *out, size_t frames)
{
   size_t got = 0;
   while (got < frames)
   {
      size_t want = frames - got > CHUNK ? CHUNK : frames - got;
      size_t n = px_source_read(s, out + got * 2, want);
      if (!n)
         break;
      got += n;
   }
   return got;
}

static double rms(const int16_t *x, size_t samples)
{
   double sum = 0;
   for (size_t i = 0; i < samples; i++)
      sum += (double)x[i] * x[i];
   return samples ? sqrt(sum / samples) : 0;
}

static double diff_rms(const int16_t *a, const int16_t *b, size_t samples)
{
   double sum = 0;
   for (size_t i = 0; i < samples; i++)
      sum += (double)(a[i] - b[i]) * (a[i] - b[i]);
   return samples ? sqrt(sum / samples) : 0;
}

static void test_song(const char *path)
{
   char err[512];
   size_t ten = 10u * RATE, one = 1u * RATE; /* in frames at the source's rate, set once open */
   int16_t *ref = (int16_t*)malloc(ten * 2 * sizeof(int16_t));
   int16_t *got = (int16_t*)malloc(one * 2 * sizeof(int16_t));
   px_source *s;
   unsigned rate;

   printf("%s\n", path);
   check(px_source_supported(path), "px_source_supported", path);
   check(px_source_song_count(path) == 1, "px_source_song_count == 1", path);

   /* Straight through: 10 s that must not be silent. */
   s = px_source_open(path, 0, true, RATE, err, sizeof(err));
   if (!s)
   {
      printf("  FAIL open: %s\n", err);
      failures++;
      free(ref);
      free(got);
      return;
   }
   rate = px_source_rate(s);
   check(rate >= 8000 && rate <= RATE, "rate in range", path);
   if (rate <= RATE)
   {
      ten = 10u * rate;
      one = rate;
   }
   printf("  rate %u Hz\n", rate);
   {
      size_t n = read_all(s, ref, ten);
      double r = rms(ref, n * 2);
      printf("  10 s rendered: %zu frames, RMS %.1f\n", n, r);
      check(n == ten, "renders 10 s", path);
      check(r > 100.0, "RMS above 100 (not silent)", path);
   }

   /* Seek back from 10 s to 5 s (a restart, then rendering forward), then forward to 8 s: each
    * must give what the straight run gave there. */
   {
      struct { double at; const char *what; } seeks[] = { { 5.0, "seek back to 5 s" }, { 8.0, "seek forward to 8 s" } };
      for (int i = 0; i < 2; i++)
      {
         size_t at = (size_t)(seeks[i].at * rate), len = one < ten - at ? one : ten - at, n;
         double d, r;
         check(px_source_seek(s, at), seeks[i].what, path);
         n = read_all(s, got, len);
         d = diff_rms(got, ref + at * 2, n * 2);
         r = rms(ref + at * 2, n * 2);
         printf("  %s: %zu frames, RMS %.1f, difference from straight run RMS %.2f\n", seeks[i].what, n, rms(got, n * 2), d);
         check(n == len, "reads after seek", path);
         check(d <= r * 0.05 + 1.0, "seeked audio matches the straight run", path);
      }
   }
   px_source_close(s);

   /* Not looped: ends after the tagged length and fade. */
   s = px_source_open(path, 0, false, RATE, err, sizeof(err));
   if (s)
   {
      size_t total = 0, n;
      int16_t *buf = (int16_t*)malloc(CHUNK * 2 * sizeof(int16_t));
      double limit = 15.0 * 60 * rate;
      while ((n = px_source_read(s, buf, CHUNK)) > 0 && total < limit)
         total += n;
      printf("  unlooped: ends after %.2f s\n", (double)total / rate);
      check(total > 0 && total < limit, "ends when not looped", path);
      free(buf);
      px_source_close(s);
   }
   else
   {
      printf("  FAIL reopen: %s\n", err);
      failures++;
   }
   free(ref);
   free(got);
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "usage: %s <song.mini2sf|song.minincsf>...\n", argv[0]);
      return 2;
   }
   for (int i = 1; i < argc; i++)
      test_song(argv[i]);
   printf(failures ? "%d FAILED\n" : "all passed\n", failures);
   return failures ? 1 : 0;
}
