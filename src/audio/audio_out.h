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

    // ---- the sound card's inputs, for the Audio In modules (the UI thread). While audio runs, up
    // to kCaptureSlots inputs are open, each an SDL recording stream at the engine's rate; the
    // callback reads what each has recorded before it renders and hands it to the engine
    // (HostCall::capture). An input is chosen by name; empty is the system's default input.
    // The inputs present now, by name (empty while audio is off).
    std::vector<std::string> inputs() const;
    // The capture slot that reads input `name`, opened if it is not open yet: -1 while audio is
    // off, if no input has that name now (one that was open and is gone is closed), or if every
    // slot is taken.
    int capture_slot(const std::string &name);
    // Closes the open inputs none of `names` asks for.
    void keep_inputs(const std::vector<std::string> &names);
    uint32_t          dropouts() const { return dropouts_.load(std::memory_order_relaxed); }
    void              stop();
    bool              running() const { return stream_ != nullptr; }
    const DeviceInfo &info() const { return info_; }

    static constexpr int kDeviceFrames = 256; // asked for; the device has the last word
    // Frames an open input may have waiting before the oldest are dropped, so that the delay from
    // the microphone does not grow (two device buffers at most, as a device's clock drifts).
    static constexpr int kMaxCaptureLag = 2 * kDeviceFrames;

private:
    static void callback(void *self, SDL_AudioStream *stream, int additional, int total);
    void        close_input(size_t slot);
    // fills cap_l_ / cap_r_ for `frames` from slot k's stream (silence for what it has not got)
    void read_input(size_t k, size_t frames);

    Engine               &engine_;
    SDL_AudioStream      *stream_ = nullptr;
    DeviceInfo            info_;
    bool                  subsystem_ = false;
    DropoutMeter          meter_;          // the audio thread's
    std::atomic<uint32_t> dropouts_ { 0 }; // what it counted, for the window

    // the audio thread's own buffers
    static constexpr size_t kChunk = 512;
    float                   l_[kChunk] { }, r_[kChunk] { }, inter_[2 * kChunk] { };

    // the open inputs: the UI thread opens and closes them; the callback reads `stream`
    struct Input
    {
        std::string                    name; // (the UI thread's)
        std::atomic<SDL_AudioStream *> stream { nullptr };
    };
    Input inputs_[kCaptureSlots];
    float cap_inter_[2 * kChunk] { }, cap_l_[kCaptureSlots][kChunk] { },
        cap_r_[kCaptureSlots][kChunk] { };
};

} // namespace rack::audio
