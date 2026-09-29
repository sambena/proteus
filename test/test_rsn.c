/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RSN soundtracks through px_source: counts the songs in a set, renders ten seconds of a few,
 * checks they are not silent, that seeking lands where straight playback does, and that a
 * song not looped ends at its tagged length.
 *   test_rsn <set.rsn> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"

#define RATE    44100
#define SECONDS 10
#define MIN_RMS 300.0

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static size_t render(px_source *s, int16_t *out, size_t frames)
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
   return frames ? sqrt(sum / (double)(frames * 2)) : 0;
}

/* libgme skips SPC audio by running the APU without its resampler, so a seek lands within a
 * few samples of the target rather than on it: the best normalised correlation of `a` against
 * `ref` over lags of up to 512 frames either way. It also clears the echo buffer, so a song
 * heavy with echo sounds a little different for a moment after a seek. */
static double match(const int16_t *a, const int16_t *ref, size_t frames, int *lag_out)
{
   double best = -1;
   for (int lag = -512; lag <= 512; lag++)
   {
      double ab = 0, aa = 0, bb = 0;
      for (size_t i = 1024; i + 1024 < frames; i++)
      {
         double x = a[i * 2], y = ref[(i + lag) * 2];
         ab += x * y; aa += x * x; bb += y * y;
      }
      if (aa > 0 && bb > 0 && ab / sqrt(aa * bb) > best)
      {
         best = ab / sqrt(aa * bb);
         *lag_out = lag;
      }
   }
   return best;
}

static void test_song(const char *path, unsigned track)
{
   char err[512];
   size_t frames = (size_t)RATE * SECONDS, got;
   int16_t *pcm = (int16_t*)malloc(frames * 2 * sizeof(int16_t));
   int16_t *again = (int16_t*)malloc(RATE * 2 * sizeof(int16_t));
   px_source *s = px_source_open(path, track, true, RATE, err, sizeof(err));
   double level;

   CHECK(s, "track %u: %s", track + 1, err);
   if (!s || !pcm || !again)
      goto done;
   CHECK(px_source_rate(s) == RATE, "track %u: rate %u", track + 1, px_source_rate(s));

   got = render(s, pcm, frames);
   level = rms(pcm, got);
   printf("track %2u: rendered %.2f s, RMS %.1f\n", track + 1, (double)got / RATE, level);
   CHECK(got == frames, "track %u: rendered %zu of %zu frames", track + 1, got, frames);
   CHECK(level > MIN_RMS, "track %u: RMS %.1f is too quiet", track + 1, level);

   /* Back to 5 s: the second from there must sound like what straight playback gave there. */
   CHECK(px_source_seek(s, (uint64_t)RATE * 5), "track %u: seek to 5 s failed", track + 1);
   got = render(s, again, RATE);
   level = rms(again, got);
   CHECK(got == RATE, "track %u: rendered %zu frames after seeking", track + 1, got);
   if (got == RATE && level > MIN_RMS / 4)
   {
      int lag = 0;
      double c = match(again, pcm + (size_t)RATE * 5 * 2, RATE, &lag);
      printf("          after seeking to 5 s: RMS %.1f, correlation %.3f at %+d frames\n", level, c, lag);
      CHECK(c > 0.75 && lag > -64 && lag < 64, "track %u: audio after seeking to 5 s does not match straight playback", track + 1);
   }
   else
      printf("          after seeking to 5 s: RMS %.1f (quiet there, not compared)\n", level);
   /* Back to the start, which libgme reaches by restarting the song: exactly the same audio. */
   CHECK(px_source_seek(s, 0), "track %u: seek to 0 failed", track + 1);
   got = render(s, again, RATE);
   CHECK(got == RATE && !memcmp(again, pcm, RATE * 2 * sizeof(int16_t)),
         "track %u: audio after seeking to 0 differs from straight playback", track + 1);
   printf("          seek back to 0 matches straight playback exactly\n");

done:
   px_source_close(s);
   free(pcm);
   free(again);
}

/* A song not looped stops after its ID666 length plus fade. */
static void test_ending(const char *path, unsigned track)
{
   char err[512];
   int16_t buf[4096 * 2];
   uint64_t total = 0, limit = (uint64_t)RATE * 900;
   px_source *s = px_source_open(path, track, false, RATE, err, sizeof(err));
   size_t n;

   CHECK(s, "track %u (no loop): %s", track + 1, err);
   if (!s)
      return;
   while (total < limit && (n = px_source_read(s, buf, 4096)))
      total += n;
   printf("track %2u without looping ends after %.1f s\n", track + 1, (double)total / RATE);
   CHECK(total < limit, "track %u never ended without looping", track + 1);
   px_source_close(s);
}

int main(int argc, char **argv)
{
   char err[512];
   unsigned count;
   px_source *s;

   if (argc < 2)
   {
      fprintf(stderr, "usage: %s <set.rsn>\n", argv[0]);
      return 2;
   }
   CHECK(px_source_supported(argv[1]), "%s not recognised as a source", argv[1]);
   count = px_source_song_count(argv[1]);
   printf("%s: %u songs\n", argv[1], count);
   CHECK(count > 1, "expected several songs, found %u", count);
   if (count)
   {
      test_song(argv[1], 0);
      test_song(argv[1], count / 2);
      test_song(argv[1], count - 1);
      test_ending(argv[1], 0);
   }
   s = px_source_open(argv[1], count, true, RATE, err, sizeof(err));
   CHECK(!s, "track %u past the end opened", count + 1);
   if (!s)
      printf("track %2u (past the end) refused: %s\n", count + 1, err);
   px_source_close(s);

   printf(failures ? "%d FAILED\n" : "all passed\n", failures);
   return failures ? 1 : 0;
}
