/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vgmstream playback through px_source.
 *   test_vgm dir stream.bfstm [...]
 * Each stream named must be listed as playable, count its songs, play (an RMS above a floor),
 * seek back and forward to the same audio it played, report no length while looping, and, not
 * looped, stop after the length it reports: for a looping stream two loops and a fade to near
 * silence. In `dir` the test also writes a GameCube-style pair of mono DSP files, song_L.dsp and
 * song_R.dsp, which must play as one stereo song listed once, and a lone mono DSP, which must
 * play the same on both sides. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"

#define SECONDS  10u
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

/* One side of the stereo output: 0 left, 1 right. */
static double rms_side(const int16_t *pcm, size_t frames, int side)
{
   double sum = 0;
   for (size_t i = 0; i < frames; i++)
      sum += (double)pcm[i * 2 + side] * pcm[i * 2 + side];
   return frames ? sqrt(sum / frames) : 0;
}

static void test_stream(const char *path)
{
   char err[512] = "";
   px_source *s;
   unsigned songs, rate;
   size_t want, got, left, sec;
   int16_t *pcm = NULL, *again = NULL;
   uint64_t length_ms;
   double level;

   printf("%s\n", path);
   check(px_source_supported(path), "px_source_supported", path);
   songs = px_source_song_count(path);
   printf("  %u song(s)\n", songs);
   check(songs >= 1, "has songs", path);
   check(!px_source_open(path, songs, true, 44100, err, sizeof(err)), "a song past the last fails", path);

   if (!(s = px_source_open(path, 0, true, 44100, err, sizeof(err))))
   {
      printf("  FAIL open: %s\n", err);
      failures++;
      return;
   }
   rate = px_source_rate(s);
   sec  = rate;
   want = SECONDS * sec;
   pcm   = (int16_t*)malloc(want * 2 * sizeof(int16_t));
   again = (int16_t*)malloc(sec * 2 * sizeof(int16_t));
   got   = read_all(s, pcm, want);
   level = rms(pcm, got);
   printf("  %u Hz; looping: played %.2f s, RMS %.1f (L %.1f, R %.1f), length %llu ms\n", rate,
      (double)got / rate, level, rms_side(pcm, got, 0), rms_side(pcm, got, 1),
      (unsigned long long)px_source_length_ms(s));
   check(level > MIN_RMS, "not silent", path);

   /* The left half of a "_L"/"_R" pair plays both, one on each side; the right is not listed. */
   {
      const char *l = strstr(path, "_L.");
      if (l && !strchr(l + 3, '/') && !strchr(l + 3, '\\'))
      {
         char right[1024];
         double diff = 0;
         snprintf(right, sizeof(right), "%s", path);
         right[l - path + 1] = 'R';
         for (size_t i = 0; i < got; i++)
            diff += fabs((double)pcm[i * 2] - pcm[i * 2 + 1]);
         printf("  pair with %s: listed %u time(s), sides differ by %.1f on average\n", right,
            px_source_song_count(right), got ? diff / got : 0);
         check(px_source_song_count(right) == 0, "the right half is not listed apart", right);
         check(got && diff / got > 10, "left and right sides differ", path);
      }
   }

   /* A stream with loop points loops forever and has no length to report; one without plays once. */
   if (got == want)
      check(px_source_length_ms(s) == 0, "no length while looping", path);
   else
      check(got > sec && got >= px_source_length_ms(s) * rate / 1000 && got <= (px_source_length_ms(s) + 1) * rate / 1000,
         "a stream without loops plays once, for the length it reports", path);

   /* Back to 40% of what played, then forward to 80%: the same audio as the first time. */
   if (got >= 2 * sec)
   {
      int before = failures;
      size_t back = got * 4 / 10, fwd = got * 8 / 10, n = got / 10 < sec ? got / 10 : sec;
      check(px_source_seek(s, back) && read_all(s, again, n) == n
         && !memcmp(again, pcm + back * 2, n * 2 * sizeof(int16_t)), "seek back replays the same audio", path);
      check(px_source_seek(s, fwd) && read_all(s, again, n) == n
         && !memcmp(again, pcm + fwd * 2, n * 2 * sizeof(int16_t)), "seek forward plays the same audio", path);
      printf("  seek back to %.2f s, forward to %.2f s: %s\n", (double)back / rate, (double)fwd / rate,
         failures > before ? "MISMATCH" : "same audio");
   }
   px_source_close(s);

   /* Not looped: plays to the length it reports, and a looping stream fades out at its end. */
   if (!(s = px_source_open(path, 0, false, 44100, err, sizeof(err))))
   {
      check(0, err, path);
      goto done;
   }
   length_ms = px_source_length_ms(s);
   free(pcm);
   want = (size_t)(length_ms * rate / 1000) + sec;
   pcm  = (int16_t*)malloc(want * 2 * sizeof(int16_t));
   got  = 0;
   while ((left = read_all(s, pcm + got * 2, want - got > sec ? sec : want - got)) > 0)
      got += left;
   printf("  not looped: length %llu ms, played %.3f s", (unsigned long long)length_ms, (double)got / rate);
   check(length_ms > 0, "reports a length", path);
   check(got + rate / 1000 + 1 >= length_ms * rate / 1000 && got <= length_ms * rate / 1000 + rate / 1000 + 1,
      "plays for the length it reports", path);
   if (got > 3 * sec)
   {
      double mid = rms(pcm + (got / 2) * 2, sec), end = rms(pcm + (got - sec / 10) * 2, sec / 10);
      printf(", RMS mid %.1f, last 0.1 s %.1f", mid, end);
      check(end < mid / 4 || end < 50, "ends quiet (faded, or the stream's own ending)", path);
   }
   printf("\n");
   /* A seek back past the end starts it playing again. */
   check(px_source_seek(s, 0) && read_all(s, again, sec / 10) == sec / 10, "plays again after seeking to 0", path);
   px_source_close(s);

done:
   free(pcm);
   free(again);
}

/* A mono GameCube DSP file: the standard 0x60-byte header, then 8-byte frames of 14 samples each
 * that use predictor pair 0, (0, 0), so every sample is its nibble scaled by 2^scale.
 * The tone is a square wave of `period` samples and amplitude `nibble` << 10, looping whole. */
static int write_dsp(const char *path, unsigned rate, unsigned frames, unsigned period, int nibble)
{
   unsigned samples = frames * 14, nibbles = frames * 16;
   unsigned char h[0x60] = { 0 };
   FILE *f = fopen(path, "wb");
   unsigned n = 0;
   if (!f)
      return 0;
#define BE32(o, v) (h[o] = (unsigned char)((v) >> 24), h[o + 1] = (unsigned char)((v) >> 16), \
                    h[o + 2] = (unsigned char)((v) >> 8), h[o + 3] = (unsigned char)(v))
   BE32(0x00, samples);
   BE32(0x04, nibbles);
   BE32(0x08, rate);
   h[0x0D] = 1;                /* loops */
   BE32(0x10, 2u);             /* loop start: the first sample, as a nibble address */
   BE32(0x14, nibbles - 1);    /* loop end: the last */
   BE32(0x18, 2u);             /* current address */
   for (int i = 2; i < 16; i++) /* pairs 1-7 unused, but a file with no coefficients is refused */
      h[0x1C + i * 2] = 0x08;
   h[0x3F] = 0x0A;             /* first frame's predictor/scale */
   h[0x45] = 0x0A;             /* loop frame's predictor/scale */
   fwrite(h, 1, sizeof(h), f);
   for (unsigned fr = 0; fr < frames; fr++)
   {
      unsigned char b[8];
      b[0] = 0x0A; /* coefficient pair 0, scale 2^10 */
      for (int i = 0; i < 7; i++)
      {
         int hi = (n++ / (period / 2)) % 2 ? -nibble : nibble;
         int lo = (n++ / (period / 2)) % 2 ? -nibble : nibble;
         b[1 + i] = (unsigned char)(((hi & 15) << 4) | (lo & 15));
      }
      fwrite(b, 1, 8, f);
   }
   fclose(f);
   return 1;
}

static void test_pair(const char *dir)
{
   char left[1024], right[1024], lone[1024], err[512];
   const unsigned rate = 32000, frames = 32000 * 3 / 14; /* about 3 s */
   int16_t *pcm = (int16_t*)malloc(rate * 2 * sizeof(int16_t));
   px_source *s;
   size_t got;

   snprintf(left, sizeof(left), "%s/pair_L.dsp", dir);
   snprintf(right, sizeof(right), "%s/pair_R.dsp", dir);
   snprintf(lone, sizeof(lone), "%s/lone.dsp", dir);
   printf("%s + _R (a DSP pair written by the test)\n", left);
   if (!write_dsp(left, rate, frames, 64, 6) || !write_dsp(right, rate, frames, 40, 2) || !write_dsp(lone, rate, frames, 64, 6))
   {
      check(0, "write DSP files", dir);
      free(pcm);
      return;
   }
   check(px_source_song_count(left) == 1, "the left half is one song", left);
   check(px_source_song_count(right) == 0, "the right half is not listed apart", right);
   check(px_source_song_count(lone) == 1, "a lone mono DSP is one song", lone);

   if ((s = px_source_open(left, 0, true, 44100, err, sizeof(err))))
   {
      double l, r;
      got = read_all(s, pcm, rate);
      l = rms_side(pcm, got, 0);
      r = rms_side(pcm, got, 1);
      printf("  pair: %u Hz, played %.2f s, RMS L %.1f, R %.1f\n", px_source_rate(s), (double)got / rate, l, r);
      check(got == rate && px_source_rate(s) == rate, "plays at its own rate", left);
      /* The left file is 6 << 10 loud, the right 2 << 10: about 6144 and 2048 RMS. */
      check(l > 5500 && l < 6700 && r > 1800 && r < 2300, "left and right come from their own files", left);
      px_source_close(s);
   }
   else
      check(0, err, left);

   if ((s = px_source_open(lone, 0, true, 44100, err, sizeof(err))))
   {
      int same = 1;
      got = read_all(s, pcm, rate);
      for (size_t i = 0; i < got; i++)
         same = same && pcm[i * 2] == pcm[i * 2 + 1];
      printf("  lone: played %.2f s, RMS %.1f, both sides the same: %s\n", (double)got / rate, rms(pcm, got), same ? "yes" : "no");
      check(got == rate && same && rms(pcm, got) > 5500, "mono plays on both sides", lone);
      px_source_close(s);
   }
   else
      check(0, err, lone);

   /* Not looped: two loops of the ~3 s tone, then ten seconds of fade. */
   if ((s = px_source_open(lone, 0, false, 44100, err, sizeof(err))))
   {
      uint64_t expect = ((uint64_t)frames * 14 * 2 + 10 * rate) * 1000 / rate;
      printf("  lone, not looped: length %llu ms (two loops and a 10 s fade: %llu ms)\n",
         (unsigned long long)px_source_length_ms(s), (unsigned long long)expect);
      check(px_source_length_ms(s) == expect, "two loops then a ten-second fade", lone);
      px_source_close(s);
   }
   free(pcm);
   remove(left);
   remove(right);
   remove(lone);
}

int main(int argc, char **argv)
{
   char err[256];
   if (argc < 2)
   {
      fprintf(stderr, "usage: test_vgm scratch-dir [stream ...]\n");
      return 2;
   }
   /* vgmstream takes Nintendo's streams; other players keep theirs; codecs not built are not claimed. */
   check(px_source_supported("x.brstm") && px_source_supported("x.BFSTM") && px_source_supported("x.dsp")
      && px_source_supported("x.adx") && px_source_supported("x.hps"), "stream extensions", "x.*");
   check(!px_source_supported("x.lopus") && !px_source_supported("x.ktss"), "codecs not built are not claimed", "x.lopus");
   check(!px_source_open("missing.brstm", 0, true, 44100, err, sizeof(err)), "missing file fails", "missing.brstm");
   check(px_source_song_count("missing.brstm") == 0, "missing file has no songs", "missing.brstm");

   test_pair(argv[1]);
   for (int i = 2; i < argc; i++)
      test_stream(argv[i]);
   printf(failures ? "test_vgm: %d FAILED\n" : "test_vgm: all passed\n", failures);
   return failures ? 1 : 0;
}
