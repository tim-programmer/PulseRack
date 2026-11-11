#include "audio/audio_out.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

namespace rack::audio
{

bool AudioOut::start(std::string *error)
{
    if (stream_)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        *error = std::string("audio could not start: ") + SDL_GetError();
        return false;
    }
    subsystem_ = true;
    // a short device buffer (measured in the spike: 256 frames is 5.3 ms at 48 kHz)
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(kDeviceFrames).c_str());

    // the engine runs at the device's own rate, so nothing resamples
    SDL_AudioSpec dev { };
    int           frames = 0;
    uint32_t      rate = 48000;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, &frames) && dev.freq > 0)
        rate = (uint32_t)dev.freq;
    if (engine_.sample_rate() != rate)
        engine_.set_sample_rate(rate);

    engine_.attach_audio(true); // from here on only the callback renders
    const SDL_AudioSpec spec { SDL_AUDIO_F32, 2, (int)rate };
    stream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioOut::callback, this);
    if (!stream_)
    {
        *error = std::string("no audio output: ") + SDL_GetError();
        engine_.attach_audio(false);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        subsystem_ = false;
        return false;
    }
    info_ = DeviceInfo { };
    const char *drv = SDL_GetCurrentAudioDriver();
    info_.driver = drv ? drv : "?";
    info_.rate = rate;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &dev, &frames))
        info_.frames = frames;
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void AudioOut::stop()
{
    if (stream_)
    {
        // Setting the callback takes the stream's lock, which SDL holds while the callback runs: any
        // callback still running has finished when this returns, and none starts after (SDL_audio.h,
        // SDL_SetAudioStreamGetCallback). Only then is the engine detached.
        SDL_SetAudioStreamGetCallback(stream_, nullptr, nullptr);
        SDL_DestroyAudioStream(stream_); // and the device it opened
        stream_ = nullptr;
        engine_.attach_audio(false); // what the callback left behind is applied now
        engine_.telemetry().load.store(0);
    }
    if (subsystem_)
    {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        subsystem_ = false;
    }
}

// The audio thread. No allocation, no lock, no logging: the engine and these fixed buffers only.
void AudioOut::callback(void *self, SDL_AudioStream *stream, int additional, int)
{
    auto      *a = static_cast<AudioOut *>(self);
    int        frames = additional / (int)(2 * sizeof(float));
    const auto t0 = SDL_GetPerformanceCounter();
    const int  total = frames;
    while (frames > 0)
    {
        const size_t n = std::min<size_t>((size_t)frames, kChunk);
        a->engine_.render(n, a->l_, a->r_);
        for (size_t i = 0; i < n; i++)
        {
            a->inter_[2 * i] = a->l_[i];
            a->inter_[2 * i + 1] = a->r_[i];
        }
        SDL_PutAudioStreamData(stream, a->inter_, (int)(n * 2 * sizeof(float)));
        frames -= (int)n;
    }
    if (total > 0)
    { // the share of the time these frames cover that rendering them took, smoothed
        const double spent
            = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
        const double span = (double)total / (double)a->engine_.sample_rate();
        auto        &load = a->engine_.telemetry().load;
        load.store((float)(0.9 * load.load(std::memory_order_relaxed) + 0.1 * spent / span),
            std::memory_order_relaxed);
    }
}

} // namespace rack::audio
