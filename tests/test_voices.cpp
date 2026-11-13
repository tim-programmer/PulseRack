// The voice modules in real patches: each silent until a note, at the note's pitch (set up to sound
// plainly), let go over its release; legato glides without starting again; the string is in tune
// and decays; the grain voice plays its file at the note's pitch.
#include "testutil.h"

#include "engine/model/wav.h"

#include <filesystem>
#include <fstream>

using namespace rt_util;

namespace
{
size_t at(double seconds) { return (size_t)(seconds * kRate); }

struct VoiceRig
{
    Rig      r;
    uint32_t kb, v, out;
    explicit VoiceRig(const char *type, const char *port = "out")
    {
        kb = r.add("keyboard");
        v = r.add(type);
        out = r.add("out");
        r.param(out, "level", 1);
        CHECK(r.cable(kb, "notes", v, "note") == Err::Ok && r.cable(v, port, out, "l") == Err::Ok);
    }
    void p(const char *id, float x) { r.param(v, id, x); }
    void note(double on, double off, uint8_t n, uint8_t vel = 127)
    {
        r.key(kb, on, true, n, vel);
        r.key(kb, off, false, n);
    }
    const std::vector<float> &run(double seconds)
    {
        CHECK(r.compile());
        r.render(at(seconds));
        CHECK(r.e.nonfinite_samples() == 0 && r.e.apply_failures() == 0);
        return r.L;
    }
};
bool near_hz(double f, double want, double tol) { return std::fabs(f - want) <= want * tol; }
// The pitch of [from, to) by autocorrelation (zero crossings count a bright waveform's ripples):
// the lag between 1/2000 and 1/40 s that matches best, refined between its neighbours.
double pitch(const std::vector<float> &v, size_t from, size_t to)
{
    const size_t lo = kRate / 2000, hi = kRate / 40, n = to - from - hi;
    double       best = -1;
    size_t       lag = lo;
    std::vector<double> c(hi + 2, 0.0);
    for (size_t l = lo; l <= hi + 1; l++)
    {
        double s = 0, e0 = 0, e1 = 0;
        for (size_t i = 0; i < n; i++)
        {
            s += (double)v[from + i] * v[from + i + l];
            e0 += (double)v[from + i] * v[from + i];
            e1 += (double)v[from + i + l] * v[from + i + l];
        }
        c[l] = s / std::sqrt(e0 * e1 + 1e-30);
    }
    for (size_t l = lo + 1; l <= hi; l++)
        if (c[l] > best && c[l] >= c[l - 1] && c[l] >= c[l + 1])
        {
            if (best > 0 && c[l] < best + 0.02) // (the first strong peak, not a later multiple)
                continue;
            best = c[l];
            lag = l;
            if (best > 0.9)
                break;
        }
    const double a = c[lag - 1], b = c[lag], d = c[lag + 1];
    const double shift = (a - 2 * b + d) != 0 ? 0.5 * (a - d) / (a - 2 * b + d) : 0;
    return (double)kRate / ((double)lag + shift);
}
} // namespace

TEST(voices_are_silent_until_a_note_and_sound_at_its_pitch)
{
    struct Case
    {
        const char *type, *port;
        std::vector<std::pair<const char *, float>> set;
        double tol;
    };
    const Case cases[] = {
        { "sync_voice", "out", { { "ratio", 1.f }, { "sweep", 0.f } }, 0.01 },
        { "stack_voice", "l", { { "voices", 1.f }, { "detune", 0.f }, { "contour", 0.f }, { "cutoff", 12000.f } }, 0.01 },
        { "fm_voice", "out", { { "ratio", 1.f }, { "index", 0.f }, { "env_amount", 0.f } }, 0.005 },
        { "air_voice", "out", { { "resonance", 1.f }, { "breath", 0.f } }, 0.05 },
        { "string_voice", "out", { { "decay", 10.f } }, 0.01 },
    };
    for (const Case &c : cases)
    {
        VoiceRig r(c.type, c.port);
        for (auto [id, x] : c.set)
            r.p(id, x);
        if (std::string(c.type) != "string_voice") // (the string has no attack)
            r.p("attack", 1);
        r.note(0.1, 0.9, 57); // A3, 220 Hz
        const auto &L = r.run(1.0);
        const double f = pitch(L, at(0.3), at(0.5));
        CHECK(peak(L, 0, at(0.1)) == 0.f);
        CHECK(rms(L, at(0.3), at(0.8)) > 0.05);
        CHECK(near_hz(f, 220, c.tol));
        if (!near_hz(f, 220, c.tol) || rms(L, at(0.3), at(0.8)) <= 0.05)
            std::printf("    %s: %.2f Hz, rms %.3f\n", c.type, f, rms(L, at(0.3), at(0.8)));
    }
}

TEST(voices_let_go_over_their_release)
{
    for (const char *type : { "sync_voice", "fm_voice", "air_voice", "string_voice" })
    {
        VoiceRig r(type);
        r.p("release", 50);
        r.note(0.05, 0.3, 60);
        const auto &L = r.run(0.8);
        CHECK(rms(L, at(0.2), at(0.3)) > 0.02);
        CHECK(peak(L, at(0.5), at(0.8)) < 1e-3f); // 50 ms after the note off: long gone
    }
}

TEST(voice_legato_glides_without_starting_again_and_falls_back_to_the_held_note)
{
    VoiceRig r("fm_voice");
    r.p("ratio", 1);
    r.p("index", 0);
    r.p("env_amount", 0);
    r.p("attack", 400); // a slow attack: a restart would show as a dip
    r.r.key(r.kb, 0.05, true, 57);
    r.r.key(r.kb, 0.6, true, 69); // A4 while A3 is held
    r.r.key(r.kb, 1.0, false, 69); // back to A3, still held
    r.r.key(r.kb, 1.4, false, 57);
    const auto &L = r.run(1.6);
    CHECK(near_hz(frequency(L, at(0.65), at(0.95)), 440, 0.01));
    CHECK(near_hz(frequency(L, at(1.05), at(1.35)), 220, 0.01));
    // the level went on rising through the change (no restart from 0)
    CHECK(peak(L, at(0.6), at(0.62)) > 0.9f * peak(L, at(0.55), at(0.6)));
}

TEST(string_voice_is_in_tune_and_rings_for_its_decay)
{
    for (uint8_t n : { (uint8_t)45, (uint8_t)57, (uint8_t)69, (uint8_t)81 })
    {
        VoiceRig r("string_voice");
        r.p("decay", 10);
        r.note(0.05, 0.9, n);
        const auto  &L = r.run(1.0);
        const double want = 440.0 * std::exp2(((double)n - 69) / 12.0);
        const double f = pitch(L, at(0.2), at(0.4));
        CHECK(near_hz(f, want, 0.01));
        if (!near_hz(f, want, 0.01))
            std::printf("    string note %d: %.2f Hz, want %.2f\n", n, f, want);
    }
    float tail[2];
    int   k = 0;
    for (float decay : { 0.3f, 5.f })
    {
        VoiceRig r("string_voice");
        r.p("decay", decay);
        r.note(0.05, 1.9, 57);
        tail[k++] = (float)rms(r.run(2.0), at(1.0), at(1.2));
    }
    CHECK(tail[0] < 0.01f * tail[1]);
}

TEST(grain_voice_plays_its_file_at_the_note_and_is_silent_without_one)
{
    { // no file: nothing
        VoiceRig r("grain_voice");
        r.note(0.05, 0.5, 60);
        CHECK(peak(r.run(0.6)) == 0.f);
    }
    // a sine at 220 Hz with its root at A3: the A above plays it an octave up
    std::vector<float> f(kRate);
    for (size_t i = 0; i < f.size(); i++)
        f[i] = 0.6f * (float)std::sin(2 * 3.14159265358979 * 220.0 * (double)i / kRate);
    const auto path = (std::filesystem::temp_directory_path() / "pulserack_grain_test.wav").string();
    std::ofstream(path, std::ios::binary) << make_wav(f, kRate);
    VoiceRig r("grain_voice");
    r.p("root", 57);
    r.p("size", 120);
    r.p("density", 30);
    r.p("spread", 0);
    r.p("attack", 1);
    std::string err;
    CHECK(r.r.e.set_sample(r.v, path, &err) == Err::Ok);
    r.note(0.05, 0.9, 69);
    const auto &L = r.run(1.0);
    CHECK(rms(L, at(0.3), at(0.8)) > 0.05);
    CHECK(near_hz(frequency(L, at(0.3), at(0.8)), 440, 0.03));
}
