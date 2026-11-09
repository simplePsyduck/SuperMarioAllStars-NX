// Sound on Android: see audio_sdl.h.
#include "platform/android/audio_sdl.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>

bool Audio::init(int latencyMs) {
  shutdown();
  latencyMs_ = latencyMs;
  if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
  SDL_AudioSpec want = {}, have = {};
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
#ifdef __ANDROID__
  want.samples = 512;
#else
  // (Linux desktops and the Steam Deck: PipeWire / PulseAudio take bigger
  // chunks; with 512 the queue ran dry now and then, a crackle)
  want.samples = 1024;
#endif
#ifdef __ANDROID__
  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
#else
  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
#endif
  if (!dev_) return false;
  rate_ = have.freq > 0 ? have.freq : 48000;
  deviceFrames_ = have.samples > 0 ? have.samples : want.samples;
  SDL_PauseAudioDevice(dev_, 0);
  return true;
}

void Audio::shutdown() {
  if (dev_) SDL_CloseAudioDevice(dev_);
  dev_ = 0;
}

void Audio::setPaused(bool paused) {
  if (dev_) SDL_PauseAudioDevice(dev_, paused ? 1 : 0);
}

int Audio::queuedFrames() const { return dev_ ? (int)(SDL_GetQueuedAudioSize(dev_) / 4) : 0; }

void Audio::flush() {
  if (dev_) SDL_ClearQueuedAudio(dev_);
  pos_ = 0;
}

void Audio::push(const int16_t* in, size_t frames, double inputRate) {
  if (!dev_ || frames == 0) return;
  int queued = queuedFrames();
  // (at least two of the device's own chunks queued: it takes a whole one at once)
  int target = std::max(rate_ * latencyMs_ / 1000, deviceFrames_ * 2);
  minQueued_ = std::min(minQueued_, queued);
  if (queued > target * 3) return;  // overrun: drop
  // Dynamic rate control: nudge the resampling ratio by up to 0.5%.
  double err = std::clamp((double)(queued - target) / (double)std::max(target, 1), -1.0, 1.0);
  double ratio = inputRate / rate_ * (1.0 + err * 0.005);
  if (queued == 0) {
    underruns_++;
    // Start or underrun: prime the queue with silence so the rate control
    // starts near its latency target instead of at zero.
    std::vector<int16_t> pad((size_t)(target / 2) * 2, 0);
    SDL_QueueAudio(dev_, pad.data(), (Uint32)(pad.size() * sizeof(int16_t)));
    ratio = inputRate / rate_ * 0.995;
  }
  out_.clear();
  out_.reserve((size_t)((double)frames * 2.0 * (rate_ / inputRate) * 1.1) + 64);  // (no growth inside the loop)
  float peak = 0;
  // Hermite interpolation across the input stream; hist_ holds the 4 most
  // recent input frames, pos_ is the fraction between hist_[1] and hist_[2].
  size_t i = 0;
  for (;;) {
    bool more = true;
    while (pos_ >= 1.0) {
      if (i >= frames) {
        more = false;
        break;
      }
      for (int k = 0; k < 3; k++) {
        hist_[k][0] = hist_[k + 1][0];
        hist_[k][1] = hist_[k + 1][1];
      }
      hist_[3][0] = in[i * 2];
      hist_[3][1] = in[i * 2 + 1];
      i++;
      pos_ -= 1.0;
    }
    if (!more) break;
    float t = (float)pos_;
    for (int c = 0; c < 2; c++) {
      float y0 = hist_[0][c], y1 = hist_[1][c], y2 = hist_[2][c], y3 = hist_[3][c];
      float c1 = 0.5f * (y2 - y0);
      float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
      float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
      float v = (((c3 * t + c2) * t + c1) * t + y1) * volume_;
      v = std::clamp(v, -32768.0f, 32767.0f);
      peak = std::max(peak, std::fabs(v) / 32768.0f);
      out_.push_back((int16_t)v);
    }
    pos_ += ratio;
  }
  level_ = level_ * 0.8f + peak * 0.2f;
  if (!out_.empty()) SDL_QueueAudio(dev_, out_.data(), (Uint32)(out_.size() * sizeof(int16_t)));
}
