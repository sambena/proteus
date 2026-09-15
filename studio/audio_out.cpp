// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_out.h"

#include <algorithm>
#include <cmath>
#include <vector>

extern "C" {
#include "music.h"
}

AudioOut::~AudioOut()
{
   if (dev_)
      SDL_CloseAudioDevice(dev_);
   std::lock_guard<std::mutex> lock(mutex_);
   if (song_)
      px_source_close(song_);
   if (song_stream_)
      SDL_FreeAudioStream(song_stream_);
   if (game_stream_)
      SDL_FreeAudioStream(game_stream_);
}

bool AudioOut::open()
{
   SDL_AudioSpec want{}, have{};
   want.freq = kRate;
   want.format = AUDIO_S16SYS;
   want.channels = 2;
   want.samples = 1024;
   want.callback = callback;
   want.userdata = this;
   dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
   if (dev_)
      SDL_PauseAudioDevice(dev_, 0);
   return dev_ != 0;
}

bool AudioOut::play(const std::string &path, unsigned subtrack, const std::string &id, std::string &error)
{
   char err[1200] = "";
   px_source *src = px_source_open(path.c_str(), subtrack, true, kRate, err, sizeof(err));
   if (!src)
   {
      error = err;
      return false;
   }
   unsigned rate = px_source_rate(src);
   SDL_AudioStream *stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, (int)rate, AUDIO_S16SYS, 2, kRate);

   std::lock_guard<std::mutex> lock(mutex_);
   if (song_)
      px_source_close(song_);
   if (song_stream_)
      SDL_FreeAudioStream(song_stream_);
   song_ = src;
   song_stream_ = stream;
   song_rate_ = rate;
   song_frames_ = 0;
   song_id_ = id;
   return true;
}

void AudioOut::stop()
{
   std::lock_guard<std::mutex> lock(mutex_);
   if (song_)
      px_source_close(song_);
   if (song_stream_)
      SDL_FreeAudioStream(song_stream_);
   song_ = nullptr;
   song_stream_ = nullptr;
   song_id_.clear();
}

std::string AudioOut::playing_id()
{
   std::lock_guard<std::mutex> lock(mutex_);
   return song_id_;
}

double AudioOut::position_seconds()
{
   std::lock_guard<std::mutex> lock(mutex_);
   return song_rate_ ? (double)song_frames_ / song_rate_ : 0;
}

void AudioOut::push_game(const int16_t *frames, size_t count, double rate)
{
   if (!count)
      return;
   std::lock_guard<std::mutex> lock(mutex_);
   int r = (int)std::lround(rate);
   if (!game_stream_ || r != game_rate_)
   {
      if (game_stream_)
         SDL_FreeAudioStream(game_stream_);
      game_stream_ = SDL_NewAudioStream(AUDIO_S16SYS, 2, r, AUDIO_S16SYS, 2, kRate);
      game_rate_ = r;
   }
   if (game_stream_)
      SDL_AudioStreamPut(game_stream_, frames, (int)(count * 4));
}

size_t AudioOut::game_queued()
{
   std::lock_guard<std::mutex> lock(mutex_);
   return game_stream_ ? (size_t)SDL_AudioStreamAvailable(game_stream_) / 4 : 0;
}

void AudioOut::clear_game()
{
   std::lock_guard<std::mutex> lock(mutex_);
   if (game_stream_)
      SDL_AudioStreamClear(game_stream_);
}

void SDLCALL AudioOut::callback(void *self, Uint8 *stream, int len)
{
   ((AudioOut*)self)->fill((int16_t*)stream, (size_t)len / 4);
}

void AudioOut::fill(int16_t *out, size_t frames)
{
   std::fill(out, out + frames * 2, (int16_t)0);
   std::lock_guard<std::mutex> lock(mutex_);
   // The device asks for the same number of frames each time: the buffers grow once, not per call.
   if (mix_.size() < frames * 2)
   {
      mix_.resize(frames * 2);
      tmp_.resize(frames * 2);
   }
   std::fill(mix_.begin(), mix_.begin() + frames * 2, 0);
   int32_t *mix = mix_.data();
   int16_t *tmp = tmp_.data();

   if (song_ && song_stream_)
   {
      int16_t buf[2048];
      while (SDL_AudioStreamAvailable(song_stream_) < (int)(frames * 4))
      {
         size_t got = px_source_read(song_, buf, 1024);
         // Songs loop, as they do in the game.
         if (!got && song_frames_ && px_source_seek(song_, 0))
         {
            song_frames_ = 0;
            got = px_source_read(song_, buf, 1024);
         }
         if (!got)
         {
            // Nothing more to play: finish what is queued, then stop.
            if (SDL_AudioStreamAvailable(song_stream_) == 0)
            {
               px_source_close(song_);
               SDL_FreeAudioStream(song_stream_);
               song_ = nullptr;
               song_stream_ = nullptr;
               song_id_.clear();
            }
            break;
         }
         song_frames_ += got;
         SDL_AudioStreamPut(song_stream_, buf, (int)(got * 4));
      }
      if (song_stream_)
      {
         int got = SDL_AudioStreamGet(song_stream_, tmp, (int)(frames * 4));
         for (int i = 0; i < std::max(0, got) / 2; i++)
            mix[i] += tmp[i];
      }
   }
   if (game_stream_)
   {
      int got = SDL_AudioStreamGet(game_stream_, tmp, (int)(frames * 4));
      for (int i = 0; i < std::max(0, got) / 2; i++)
         mix[i] += tmp[i];
   }

   int vol = std::clamp(volume.load(), 0, 100);
   for (size_t i = 0; i < frames * 2; i++)
      out[i] = (int16_t)std::clamp(mix[i] * vol / 100, -32768, 32767);
}
