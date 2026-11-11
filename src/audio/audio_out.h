// The sound card: an SDL3 audio stream whose callback runs the engine (docs/architecture.md,
// *Audio*). The default output is opened at its own rate (the engine is made again at that rate if
// it differs) with a 256-frame device buffer; the engine is attached before the stream starts
// calling it and detached after the stream is gone. Owned by something that is destroyed before
// gpuic's SDL_Quit.
#pragma once

#include "engine/engine.h"

#include <cstdint>
#include <string>

struct SDL_AudioStream;

namespace rack::audio
{

struct DeviceInfo
{
    std::string driver;     // "pipewire", "pulseaudio", "alsa", "dummy"...
    uint32_t    rate = 0;   // Hz
    int         frames = 0; // the device buffer
    double      latency_ms() const { return rate ? 1000.0 * frames / rate : 0; }
};

class AudioOut
{
public:
    explicit AudioOut(Engine &engine)
        : engine_(engine)
    {
    }
    ~AudioOut() { stop(); }
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;

    bool              start(std::string *error); // false: no device (the reason in `error`)
    void              stop();
    bool              running() const { return stream_ != nullptr; }
    const DeviceInfo &info() const { return info_; }

    static constexpr int kDeviceFrames = 256; // asked for; the device has the last word

private:
    static void callback(void *self, SDL_AudioStream *stream, int additional, int total);

    Engine          &engine_;
    SDL_AudioStream *stream_ = nullptr;
    DeviceInfo       info_;
    bool             subsystem_ = false;

    // the audio thread's own buffers
    static constexpr size_t kChunk = 512;
    float                   l_[kChunk] { }, r_[kChunk] { }, inter_[2 * kChunk] { };
};

} // namespace rack::audio
