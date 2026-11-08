// rack-render: renders a patch file to a WAV file, with the same engine the application uses.
//   rack-render patch.rack out.wav [seconds] [--rate 48000]
#include "engine/engine.h"
#include "engine/model/patchfile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{

void put32(std::vector<unsigned char> &b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b.push_back((unsigned char)(v >> (8 * i)));
}
void put16(std::vector<unsigned char> &b, uint16_t v)
{
    for (int i = 0; i < 2; i++)
        b.push_back((unsigned char)(v >> (8 * i)));
}

bool write_wav(
    const char *path, const std::vector<float> &l, const std::vector<float> &r, uint32_t rate)
{
    const uint32_t             frames = (uint32_t)l.size(), bytes = frames * 4;
    std::vector<unsigned char> b;
    b.insert(b.end(), { 'R', 'I', 'F', 'F' });
    put32(b, 36 + bytes);
    b.insert(b.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    put32(b, 16);
    put16(b, 1);
    put16(b, 2);
    put32(b, rate);
    put32(b, rate * 4);
    put16(b, 4);
    put16(b, 16);
    b.insert(b.end(), { 'd', 'a', 't', 'a' });
    put32(b, bytes);
    for (uint32_t i = 0; i < frames; i++)
    {
        put16(b, (uint16_t)(int16_t)std::lround(l[i] * 32767.0f));
        put16(b, (uint16_t)(int16_t)std::lround(r[i] * 32767.0f));
    }
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace

int main(int argc, char **argv)
{
    uint32_t                  rate = 48000;
    std::vector<const char *> args;
    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "--rate") && i + 1 < argc)
            rate = (uint32_t)std::atoi(argv[++i]);
        else
            args.push_back(argv[i]);
    }
    if (args.size() < 2 || args.size() > 3 || rate < 8000)
    {
        std::fprintf(stderr, "usage: rack-render patch.rack out.wav [seconds] [--rate Hz]\n");
        return 2;
    }
    const double seconds = args.size() == 3 ? std::atof(args[2]) : 5.0;

    std::ifstream in(args[0], std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "%s: cannot open\n", args[0]);
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    rack::Engine           engine(rate);
    const rack::LoadResult lr = rack::load_patch(ss.str(), engine.patch());
    for (const std::string &w : lr.warnings)
        std::fprintf(stderr, "%s: warning: %s\n", args[0], w.c_str());
    if (!lr.ok)
    {
        std::fprintf(stderr, "%s: %s\n", args[0], lr.error.c_str());
        return 1;
    }
    if (const rack::Err e = engine.compile(); e != rack::Err::Ok)
    {
        std::fprintf(stderr, "%s: %s\n", args[0], rack::describe(e));
        return 1;
    }

    const size_t       frames = (size_t)std::llround(seconds * rate);
    std::vector<float> l(frames), r(frames);
    engine.render(frames, l.data(), r.data());

    double peak = 0, sum = 0;
    for (size_t i = 0; i < frames; i++)
    {
        peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
        sum += (double)l[i] * l[i] + (double)r[i] * r[i];
    }
    if (!write_wav(args[1], l, r, rate))
    {
        std::perror(args[1]);
        return 1;
    }
    std::printf("%s: %.2f s at %u Hz, peak %.3f, rms %.4f\n", args[1], seconds, rate, peak,
        frames ? std::sqrt(sum / (2.0 * (double)frames)) : 0.0);
    return 0;
}
