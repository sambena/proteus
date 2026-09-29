/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SNSF playback through px_source: each rip named on the command line must play (10 s with an
 * RMS above a floor), seek back and forward to the same audio it played, and play alongside a
 * second copy of itself without either disturbing the other.
 *   test_snsf song.minisnsf [...] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "music.h"

#define RATE     44100u
#define SECONDS  10u
#define FRAMES   (RATE * SECONDS)
#define MIN_RMS  300.0

static int failures;

static void check(int ok, const char *what, const char *path)
{
   if (!ok)
   {
      printf("  FAIL %s: %s\n", what, path);
      failures++;
   }
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

static double rms(const int16_t *pcm, size_t frames)
{
   double sum = 0;
   for (size_t i = 0; i < frames * 2; i++)
      sum += (double)pcm[i] * pcm[i];
   return frames ? sqrt(sum / (frames * 2)) : 0;
}

static void test_rip(const char *path)
{
   char err[512] = "";
   int16_t *pcm = (int16_t*)malloc(FRAMES * 2 * sizeof(int16_t));
   int16_t *again = (int16_t*)malloc(RATE * 2 * sizeof(int16_t));
   int16_t *pair = (int16_t*)malloc(RATE * 2 * 2 * sizeof(int16_t));
   px_source *s, *t;
   clock_t start;
   double took, level;
   size_t got;
   int failed_before = failures;

   printf("%s\n", path);
   check(px_source_supported(path), "px_source_supported", path);
   if (!(s = px_source_open(path, 0, true, RATE, err, sizeof(err))))
   {
      printf("  FAIL open: %s\n", err);
      failures++;
      goto done;
   }
   check(px_source_rate(s) == RATE, "output rate", path);
   check(px_source_length_ms(s) == 0, "no length while looping", path);

   start = clock();
   got   = read_all(s, pcm, FRAMES);
   took  = (double)(clock() - start) / CLOCKS_PER_SEC;
   level = rms(pcm, got);
   printf("  rendered %.1f s in %.2f s (%.0fx real time), RMS %.1f (first 5 s %.1f, last 5 s %.1f)\n",
      (double)got / RATE, took, took > 0 ? got / (double)RATE / took : 0, level,
      rms(pcm, got / 2), rms(pcm + (got / 2) * 2, got - got / 2));
   check(got == FRAMES, "renders 10 s", path);
   check(level > MIN_RMS, "not silent", path);

   /* Back to 4 s (a restart and replay), then the second after must match what played. */
   check(px_source_seek(s, 4 * RATE), "seek back", path);
   got = read_all(s, again, RATE);
   check(got == RATE && !memcmp(again, pcm + 4 * RATE * 2, RATE * 2 * sizeof(int16_t)),
      "seek back to 4 s replays the same audio", path);
   /* Forward to 8 s from 5 s. */
   check(px_source_seek(s, 8 * RATE), "seek forward", path);
   got = read_all(s, again, RATE);
   check(got == RATE && !memcmp(again, pcm + 8 * RATE * 2, RATE * 2 * sizeof(int16_t)),
      "seek forward to 8 s plays the same audio", path);
   printf("  seek back/forward: %s\n", failures > failed_before ? "MISMATCH" : "matches");

   /* Two players at once, interleaved, each must still play what one alone played. */
   check(px_source_seek(s, 0), "seek to start", path);
   if (!(t = px_source_open(path, 0, true, RATE, err, sizeof(err))))
      check(0, "second open", path);
   else
   {
      for (size_t at = 0; at < RATE; at += 735)
      {
         size_t n = RATE - at < 735 ? RATE - at : 735;
         read_all(s, pair + at * 2, n);
         read_all(t, pair + (RATE + at) * 2, n);
      }
      check(!memcmp(pair, pcm, RATE * 2 * sizeof(int16_t)), "first of two players unchanged", path);
      check(!memcmp(pair + RATE * 2, pcm, RATE * 2 * sizeof(int16_t)), "second of two players unchanged", path);
      px_source_close(t);
   }
   px_source_close(s);

   /* Not looped and untagged: plays on (no length to stop at). */
   if ((s = px_source_open(path, 0, false, RATE, err, sizeof(err))))
   {
      check(read_all(s, again, RATE) == RATE, "untagged, unlooped song plays", path);
      px_source_close(s);
   }

done:
   free(pcm);
   free(again);
   free(pair);
}

/* A copy of a tagged-less miniSNSF with length and fade tags added (beside it, so its _lib
 * resolves) must stop, unlooped, after length + fade, fading to silence. */
static void test_tags(const char *path)
{
   char tagged[1024], err[512];
   const char *slash = strrchr(path, '/');
   FILE *in, *out;
   int c;
   px_source *s;
   int16_t *pcm;
   size_t got, want = 4 * RATE;

   if (!slash)
      slash = strrchr(path, '\\');
   snprintf(tagged, sizeof(tagged), "%.*spx_tagged.minisnsf", slash ? (int)(slash - path + 1) : 0, path);
   if (!(in = fopen(path, "rb")) || !(out = fopen(tagged, "wb")))
   {
      check(0, "write tagged copy", tagged);
      if (in)
         fclose(in);
      return;
   }
   while ((c = fgetc(in)) != EOF)
      fputc(c, out);
   fclose(in);
   fputs("length=0:03\nfade=1\ntitle=Tagged\n", out);
   fclose(out);

   printf("%s (length 3 s, fade 1 s)\n", tagged);
   pcm = (int16_t*)malloc((want + RATE) * 2 * sizeof(int16_t));
   if ((s = px_source_open(tagged, 0, false, RATE, err, sizeof(err))))
   {
      check(px_source_length_ms(s) == 4000, "reports 4000 ms (length + fade)", tagged);
      got = read_all(s, pcm, want + RATE);
      printf("  played %zu frames (want %zu), px_source_length_ms %llu; RMS 2-3 s %.1f, last 0.1 s %.1f\n", got, want,
         (unsigned long long)px_source_length_ms(s),
         rms(pcm + 2 * RATE * 2, RATE), got >= RATE / 10 ? rms(pcm + (got - RATE / 10) * 2, RATE / 10) : 0);
      check(got == want, "ends after length + fade", tagged);
      check(got == want && rms(pcm + (got - RATE / 10) * 2, RATE / 10) < rms(pcm + 2 * RATE * 2, RATE) / 4,
         "fades out", tagged);
      px_source_close(s);
   }
   else
      check(0, err, tagged);
   free(pcm);
   remove(tagged);
}

int main(int argc, char **argv)
{
   char err[256];
   if (argc < 2)
   {
      fprintf(stderr, "usage: test_snsf song.minisnsf [...]\n");
      return 2;
   }
   /* A library alone is not a song, and a missing file fails cleanly. */
   check(!px_source_supported("x.snsflib"), "snsflib is not a song", "x.snsflib");
   check(!px_source_open("missing.minisnsf", 0, true, RATE, err, sizeof(err)), "missing file fails", "missing.minisnsf");
   for (int i = 1; i < argc; i++)
      test_rip(argv[i]);
   test_tags(argv[1]);
   printf(failures ? "test_snsf: %d FAILED\n" : "test_snsf: all passed\n", failures);
   return failures ? 1 : 0;
}
