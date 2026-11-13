// A WAV file as one channel of floats, for the Sampler. No third-party reader: PCM at 8, 16, 24 or
// 32 bits and IEEE float at 32 or 64 bits (WAVE_FORMAT_EXTENSIBLE too); several channels are mixed
// to one. The UI thread reads it; the module gets a copy of the frames (docs/patch-engine.md).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rack
{

struct SampleData
{
    std::vector<float> frames; // mono, -1 .. 1
    uint32_t           rate = 0;
};

inline constexpr uint32_t kMaxSampleFrames = 1u << 25; // about 11 minutes at 48 kHz

// The bytes of a WAV file; false and `error` set ("not a WAV file", "24-bit float is not
// supported", ...) if it cannot be read.
bool parse_wav(std::string_view bytes, SampleData *out, std::string *error);
bool read_wav(const std::string &path, SampleData *out, std::string *error);
// A mono 16-bit WAV file of `frames` (the tests, and later the Sampler's own recordings).
std::string make_wav(const std::vector<float> &frames, uint32_t rate);

} // namespace rack
