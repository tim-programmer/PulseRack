// Helpers shared by the tests: a rig that builds a patch by names, and a few signal measurements.
#pragma once

#include "rack_test.h"

#include "engine/engine.h"
#include "engine/model/patchfile.h"
#include "engine/model/registry.h"
#include "engine/rt/modules_common.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace rt_util
{

using namespace rack;

constexpr uint32_t kRate = 48000;

struct Rig
{
    Engine e;
    explicit Rig(uint32_t rate = kRate)
        : e(rate)
    {
    }

    uint32_t add(const char *type, float x = 0, float y = 0)
    {
        const ModuleDesc *d = find_module(type);
        CHECK(d != nullptr);
        return e.patch().add_node(*d, x, y);
    }
    int port(uint32_t node, const char *id) { return find_port(*e.patch().find(node)->desc, id); }
    int param_index(uint32_t node, const char *id)
    {
        return find_param(*e.patch().find(node)->desc, id);
    }
    Err cable(uint32_t a, const char *ap, uint32_t b, const char *bp)
    {
        const int pa = port(a, ap), pb = port(b, bp);
        CHECK(pa >= 0 && pb >= 0);
        return e.patch().connect({ a, (uint16_t)pa }, { b, (uint16_t)pb });
    }
    void param(uint32_t node, const char *id, float v)
    {
        const int i = param_index(node, id);
        CHECK(i >= 0);
        CHECK(e.set_param(node, (size_t)i, v) == Err::Ok);
    }
    void key(uint32_t node, double seconds, bool on, uint8_t note, uint8_t vel = 100)
    {
        e.patch().keys.push_back(KeyScript { node, seconds, on, note, vel });
    }
    bool compile() { return e.compile() == Err::Ok; }

    std::vector<float> L, R;
    void               render(size_t frames)
    {
        const size_t at = L.size();
        L.resize(at + frames);
        R.resize(at + frames);
        e.render(frames, L.data() + at, R.data() + at);
    }
};

inline bool same_bits(const std::vector<float> &a, const std::vector<float> &b)
{
    return a.size() == b.size()
        && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}
inline float peak(const std::vector<float> &v, size_t from = 0, size_t to = (size_t)-1)
{
    float p = 0;
    for (size_t i = from; i < std::min(to, v.size()); i++)
        p = std::max(p, std::fabs(v[i]));
    return p;
}
inline double rms(const std::vector<float> &v, size_t from = 0, size_t to = (size_t)-1)
{
    double s = 0;
    size_t n = 0;
    for (size_t i = from; i < std::min(to, v.size()); i++, n++)
        s += (double)v[i] * v[i];
    return n ? std::sqrt(s / (double)n) : 0;
}
// Frequency from rising zero crossings in [from, to).
inline double frequency(const std::vector<float> &v, size_t from, size_t to, double rate = kRate)
{
    size_t crossings = 0, first = 0, last = 0;
    for (size_t i = from + 1; i < std::min(to, v.size()); i++)
        if (v[i - 1] < 0 && v[i] >= 0)
        {
            if (!crossings)
                first = i;
            last = i;
            crossings++;
        }
    return crossings < 2 ? 0 : (double)(crossings - 1) * rate / (double)(last - first);
}

} // namespace rt_util
