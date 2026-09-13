// SPDX-License-Identifier: LGPL-2.1-or-later
// Audio output for Proteus Studio: plays one song file at a time (any format Proteus
// plays) mixed with live game audio.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <SDL.h>

struct px_source;

class AudioOut
{
public:
   static constexpr int kRate = 48000;

   ~AudioOut();
   bool open();

   // Plays a song file from the start; `id` names what is playing for the UI.
   bool play(const std::string &path, unsigned subtrack, const std::string &id, std::string &error);
   void stop();
   std::string playing_id();
   double position_seconds();

   // Game audio, paced by the caller: push while game_queued() is low.
   void push_game(const int16_t *frames, size_t count, double rate);
   size_t game_queued();
   void clear_game();

   int volume = 80;   // percent

private:
   static void SDLCALL callback(void *self, Uint8 *stream, int len);
   void fill(int16_t *out, size_t frames);

   SDL_AudioDeviceID dev_ = 0;
   std::mutex mutex_;

   px_source *song_ = nullptr;
   SDL_AudioStream *song_stream_ = nullptr;
   std::string song_id_;
   uint64_t song_frames_ = 0;
   unsigned song_rate_ = 0;

   SDL_AudioStream *game_stream_ = nullptr;
   int game_rate_ = 0;
};
