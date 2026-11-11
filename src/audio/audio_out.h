// The sound card: an SDL3 audio stream whose callback runs the engine (docs/architecture.md,
// *Audio*). The default output is opened at its own rate (the engine is made again at that rate if
// it differs) with a 256-frame device buffer; the engine is attached before the stream starts
// calling it and detached after the stream is gone. Owned by something that is destroyed before
// gpuic's SDL_Quit.
#pragma once

#include "audio/dropout_meter.h"
#include "engine/engine.h"

#include <atomic>
#include <vector>

#include <cstdint>
#include <string>

struct SDL_AudioStream;

namespace rack::audio
{

struct OutputDevice
{
    uint32_t    id = 0; // SDL's; 0 is "the system's default"
    std::string name;
};

struct DeviceInfo
{
    std::string name;       // the device's name
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

    // false: no device (the reason in `error`). `device` is an output's name (SDL's ids last only
    // as long as its audio subsystem, so a choice is kept by name); empty: the system's default.
    bool start(std::string *error, const std::string &device = { });
    // The outputs SDL knows of now (empty if audio cannot start; `error` says why).
    static std::vector<OutputDevice> outputs(std::string *error);
    uint32_t          dropouts() const { return dropouts_.load(std::memory_order_relaxed); }
    void              stop();
    bool              running() const { return stream_ != nullptr; }
    const DeviceInfo &info() const { return info_; }

    static constexpr int kDeviceFrames = 256; // asked for; the device has the last word

private:
    static void callback(void *self, SDL_AudioStream *stream, int additional, int total);

    Engine               &engine_;
    SDL_AudioStream      *stream_ = nullptr;
    DeviceInfo            info_;
    bool                  subsystem_ = false;
    DropoutMeter          meter_;          // the audio thread's
    std::atomic<uint32_t> dropouts_ { 0 }; // what it counted, for the window

    // the audio thread's own buffers
    static constexpr size_t kChunk = 512;
    float                   l_[kChunk] { }, r_[kChunk] { }, inter_[2 * kChunk] { };
};

} // namespace rack::audio
