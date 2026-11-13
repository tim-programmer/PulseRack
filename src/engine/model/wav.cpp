#include "engine/model/wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace rack
{

namespace
{
    uint32_t le32(const unsigned char *p)
    {
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

    // One sample of `bits` at `p` as -1 .. 1.
    float pcm(const unsigned char *p, uint16_t bits, bool is_float)
    {
        if (is_float && bits == 32)
        {
            const uint32_t u = le32(p);
            float          f;
            std::memcpy(&f, &u, 4);
            return std::isfinite(f) ? f : 0.f;
        }
        if (is_float) // 64
        {
            const uint64_t u = (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
            double         d;
            std::memcpy(&d, &u, 8);
            return std::isfinite(d) ? (float)d : 0.f;
        }
        switch (bits)
        {
        case 8:
            return ((float)p[0] - 128.f) / 128.f; // unsigned
        case 16:
            return (float)(int16_t)le16(p) / 32768.f;
        case 24:
        {
            const int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16
                                  | (uint32_t)p[2] << 24)
                >> 8;
            return (float)v / 8388608.f;
        }
        default: // 32
            return (float)((double)(int32_t)le32(p) / 2147483648.0);
        }
    }
} // namespace

bool parse_wav(std::string_view bytes, SampleData *out, std::string *error)
{
    const auto *b = reinterpret_cast<const unsigned char *>(bytes.data());
    const size_t n = bytes.size();
    if (n < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0)
    {
        *error = "not a WAV file";
        return false;
    }
    uint16_t             format = 0, channels = 0, bits = 0, align = 0;
    uint32_t             rate = 0;
    const unsigned char *data = nullptr;
    size_t               data_len = 0;
    for (size_t at = 12; at + 8 <= n;)
    {
        const uint32_t len = le32(b + at + 4);
        const size_t   body = at + 8, avail = std::min<size_t>(len, n - body); // (a cut file)
        if (!std::memcmp(b + at, "fmt ", 4) && avail >= 16)
        {
            format = le16(b + body);
            channels = le16(b + body + 2);
            rate = le32(b + body + 4);
            align = le16(b + body + 12);
            bits = le16(b + body + 14);
            if (format == 0xFFFE && avail >= 26) // WAVE_FORMAT_EXTENSIBLE: the sub-format's first 2 bytes
                format = le16(b + body + 24);
        }
        else if (!std::memcmp(b + at, "data", 4))
        {
            data = b + body;
            data_len = avail;
        }
        at = body + len + (len & 1); // chunks are padded to an even length
    }
    if (!format || !data)
    {
        *error = !format ? "no format chunk" : "no audio data";
        return false;
    }
    const bool is_float = format == 3;
    if (format != 1 && format != 3)
    {
        *error = "compressed WAV (format " + std::to_string(format) + ") is not supported";
        return false;
    }
    if ((is_float && bits != 32 && bits != 64)
        || (!is_float && bits != 8 && bits != 16 && bits != 24 && bits != 32))
    {
        *error = std::to_string(bits) + "-bit " + (is_float ? "float" : "PCM") + " is not supported";
        return false;
    }
    if (channels == 0 || align != channels * (bits / 8) || rate < 8000 || rate > 768000)
    {
        *error = "a format this reader does not know (" + std::to_string(channels) + " channels, "
            + std::to_string(rate) + " Hz)";
        return false;
    }
    const size_t frames = data_len / align;
    if (frames == 0)
    {
        *error = "no audio in it";
        return false;
    }
    if (frames > kMaxSampleFrames)
    {
        *error = "too long (at most " + std::to_string(kMaxSampleFrames / rate) + " s)";
        return false;
    }
    out->rate = rate;
    out->frames.assign(frames, 0.f);
    const size_t step = bits / 8;
    for (size_t i = 0; i < frames; i++)
    {
        float s = 0;
        for (size_t c = 0; c < channels; c++)
            s += pcm(data + i * align + c * step, bits, is_float);
        out->frames[i] = std::clamp(s / (float)channels, -1.f, 1.f);
    }
    return true;
}

bool read_wav(const std::string &path, SampleData *out, std::string *error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        *error = "cannot be read";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_wav(ss.str(), out, error);
}

std::string make_wav(const std::vector<float> &frames, uint32_t rate)
{
    std::string s;
    auto        put32 = [&](uint32_t v)
    {
        for (int i = 0; i < 4; i++)
            s += (char)(v >> (8 * i) & 0xff);
    };
    auto put16 = [&](uint16_t v)
    {
        s += (char)(v & 0xff);
        s += (char)(v >> 8);
    };
    const uint32_t bytes = (uint32_t)frames.size() * 2;
    s += "RIFF";
    put32(36 + bytes);
    s += "WAVEfmt ";
    put32(16);
    put16(1); // PCM
    put16(1); // mono
    put32(rate);
    put32(rate * 2);
    put16(2);
    put16(16);
    s += "data";
    put32(bytes);
    for (float f : frames)
        put16((uint16_t)(int16_t)std::lround(std::clamp(f, -1.f, 1.f) * 32767.f));
    return s;
}

} // namespace rack
