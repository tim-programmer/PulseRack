#include "audio/audio_out.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

namespace rack::audio
{

std::vector<OutputDevice> AudioOut::outputs(std::string *error)
{
    std::vector<OutputDevice> out;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        *error = std::string("audio could not start: ") + SDL_GetError();
        return out;
    }
    int                n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; ids && i < n; i++)
    {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        out.push_back(OutputDevice { ids[i], name ? name : "?" });
    }
    SDL_free(ids);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return out;
}

bool AudioOut::start(std::string *error, const std::string &device)
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
    SDL_AudioSpec     dev { };
    int               frames = 0;
    uint32_t          rate = 48000;
    SDL_AudioDeviceID which = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!device.empty())
    { // the output of that name, in this session of SDL's audio
        int                n = 0;
        SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
        which = 0;
        for (int i = 0; ids && i < n && !which; i++)
            if (const char *name = SDL_GetAudioDeviceName(ids[i]); name && device == name)
                which = ids[i];
        SDL_free(ids);
        if (!which)
        {
            *error = "no output called \"" + device + "\" now";
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            subsystem_ = false;
            return false;
        }
    }
    if (SDL_GetAudioDeviceFormat(which, &dev, &frames) && dev.freq > 0)
        rate = (uint32_t)dev.freq;
    if (engine_.sample_rate() != rate)
        engine_.set_sample_rate(rate);

    engine_.attach_audio(true); // from here on only the callback renders
    const SDL_AudioSpec spec { SDL_AUDIO_F32, 2, (int)rate };
    stream_ = SDL_OpenAudioDeviceStream(which, &spec, &AudioOut::callback, this);
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
    const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
    info_.name = name ? name : (device.empty() ? "default output" : device);
    meter_.reset(info_.frames > 0 ? (double)info_.frames / rate : 256.0 / rate);
    dropouts_.store(0);
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void AudioOut::stop()
{
    if (stream_)
    {
        // Setting the callback takes the stream's lock, which SDL holds while the callback runs:
        // any callback still running has finished when this returns, and none starts after
        // (SDL_audio.h, SDL_SetAudioStreamGetCallback). Only then is the engine detached.
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
        const double freq = (double)SDL_GetPerformanceFrequency();
        const double spent = (double)(SDL_GetPerformanceCounter() - t0) / freq;
        const double span = (double)total / (double)a->engine_.sample_rate();
        a->meter_.on_callback((double)t0 / freq, span, spent); // dropouts, from the timing
        a->dropouts_.store(a->meter_.count, std::memory_order_relaxed);
        auto &load = a->engine_.telemetry().load;
        load.store((float)(0.9 * load.load(std::memory_order_relaxed) + 0.1 * spent / span),
            std::memory_order_relaxed);
    }
}

} // namespace rack::audio
