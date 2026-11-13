// The ladder filter, unison oscillator, delay and reverb in real patches: each does what its panel
// says, and the reverb's settings that make it again (size, algorithm) do so without a glitch in
// the engine.
#include "testutil.h"

#include "engine/model/patchfile.h"
#include "engine/model/wav.h"

#include <filesystem>
#include <fstream>

using namespace rt_util;

namespace
{
// A gate from a scripted key: 0, then 1 from `on` to `off` seconds.
uint32_t gate(Rig &r, double on, double off)
{
    const uint32_t kb = r.add("keyboard"), g = r.add("note_to_gate");
    CHECK(r.cable(kb, "notes", g, "notes") == Err::Ok);
    r.key(kb, on, true, 60);
    r.key(kb, off, false, 60);
    return g;
}
} // namespace

TEST(delay_repeats_its_input_after_the_time)
{
    Rig            r;
    const uint32_t g = gate(r, 0.1, 10), d = r.add("delay"), out = r.add("out");
    r.param(d, "time", 100);
    r.param(d, "feedback", 0);
    r.param(d, "mix", 1); // the echo alone
    r.param(d, "glide", 0);
    r.param(d, "tone", 20000);
    r.param(out, "level", 1);
    CHECK(r.cable(g, "gate", d, "in") == Err::Ok && r.cable(d, "out", out, "l") == Err::Ok
        && r.compile());
    r.render(kRate / 2);
    CHECK(peak(r.L, 0, (size_t)(0.195 * kRate)) < 0.01f);               // not before 0.1 + 0.1 s
    CHECK(std::fabs(r.L[(size_t)(0.25 * kRate)] - 1.f) < 0.02f);       // the gate, 100 ms late
    CHECK(r.e.apply_failures() == 0 && r.e.patch().nodes().size() == 4);
}

TEST(delay_feedback_makes_repeats_that_fade)
{
    Rig            r;
    const uint32_t g = gate(r, 0.05, 0.06), d = r.add("delay"), out = r.add("out");
    r.param(d, "time", 100);
    r.param(d, "feedback", 0.5f);
    r.param(d, "mix", 1);
    r.param(d, "glide", 0);
    r.param(out, "level", 1);
    CHECK(r.cable(g, "gate", d, "in") == Err::Ok && r.cable(d, "out", out, "l") == Err::Ok
        && r.compile());
    r.render(kRate / 2);
    const float first = peak(r.L, (size_t)(0.14 * kRate), (size_t)(0.18 * kRate)),
                second = peak(r.L, (size_t)(0.24 * kRate), (size_t)(0.28 * kRate)),
                third = peak(r.L, (size_t)(0.34 * kRate), (size_t)(0.38 * kRate));
    CHECK(first > 0.8f && second > 0.35f && second < 0.65f && third > 0.15f && third < 0.35f);
}

TEST(reverb_tail_outlasts_its_input_in_stereo)
{
    Rig            r;
    const uint32_t g = gate(r, 0.05, 0.15), osc = r.add("osc"), vca = r.add("vca"),
                   rv = r.add("reverb"), out = r.add("out");
    r.param(rv, "mix", 1);
    r.param(rv, "decay", 2);
    r.param(rv, "predelay", 0);
    r.param(out, "level", 1);
    CHECK(r.cable(osc, "out", vca, "in") == Err::Ok && r.cable(g, "gate", vca, "gain") == Err::Ok
        && r.cable(vca, "out", rv, "in") == Err::Ok && r.cable(rv, "l", out, "l") == Err::Ok
        && r.cable(rv, "r", out, "r") == Err::Ok && r.compile());
    r.render(kRate);
    const size_t from = (size_t)(0.4 * kRate), to = (size_t)(0.6 * kRate);
    CHECK(rms(r.L, from, to) > 0.005 && rms(r.R, from, to) > 0.005); // still sounding
    bool differ = false;
    for (size_t i = from; i < to && !differ; i++)
        differ = std::fabs(r.L[i] - r.R[i]) > 1e-3f;
    CHECK(differ); // two channels, not one copied
}

TEST(reverb_size_makes_it_again_and_its_tail_starts_over)
{
    Rig            r;
    const uint32_t g = gate(r, 0.05, 0.15), osc = r.add("osc"), vca = r.add("vca"),
                   rv = r.add("reverb"), out = r.add("out");
    r.param(rv, "mix", 1);
    r.param(rv, "decay", 10);
    r.param(rv, "predelay", 0);
    r.param(out, "level", 1);
    CHECK(r.cable(osc, "out", vca, "in") == Err::Ok && r.cable(g, "gate", vca, "gain") == Err::Ok
        && r.cable(vca, "out", rv, "in") == Err::Ok && r.cable(rv, "l", out, "l") == Err::Ok
        && r.compile());
    r.render(kRate / 2);
    CHECK(rms(r.L, (size_t)(0.4 * kRate), kRate / 2) > 0.005); // a long tail
    for (float size : { 0.3f, 1.f })
    {
        const size_t at = r.L.size();
        r.param(rv, "size", size);
        r.param(rv, "algorithm", size < 0.5f ? 0.f : 1.f);
        r.render(kRate / 4);
        CHECK(peak(r.L, at + 64) < 1e-4f); // a new, empty reverb (the input is silent now)
    }
    CHECK(r.e.apply_failures() == 0);
}

TEST(ladder_low_pass_takes_the_top_off_a_saw)
{
    float level[2] = { };
    int   k = 0;
    for (float cutoff : { 150.f, 15000.f })
    {
        Rig            r;
        const uint32_t osc = r.add("osc"), ld = r.add("ladder"), out = r.add("out");
        r.param(osc, "frequency", 440);
        r.param(ld, "cutoff", cutoff);
        r.param(ld, "resonance", 0);
        r.param(out, "level", 1);
        CHECK(r.cable(osc, "out", ld, "in") == Err::Ok && r.cable(ld, "out", out, "l") == Err::Ok
            && r.compile());
        r.render(kRate / 4);
        level[k++] = (float)rms(r.L, kRate / 10);
    }
    CHECK(level[1] > 0.3f && level[0] < level[1] * 0.05f);
}

TEST(unison_plays_notes_and_spreads_its_voices_in_stereo)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), u = r.add("unison"), out = r.add("out");
    r.param(u, "waveform", 0);
    r.param(u, "voices", 1);
    r.param(u, "detune", 0);
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", u, "note") == Err::Ok && r.cable(u, "l", out, "l") == Err::Ok
        && r.cable(u, "r", out, "r") == Err::Ok);
    r.key(kb, 0.1, true, 69); // A4
    CHECK(r.compile());
    r.render(kRate / 2);
    const double f = frequency(r.L, (size_t)(0.2 * kRate), kRate / 2);
    CHECK(f > 435 && f < 445);

    r.param(u, "voices", 5);
    r.param(u, "detune", 30);
    r.param(u, "width", 1);
    const size_t at = r.L.size();
    r.render(kRate / 2);
    bool differ = false;
    for (size_t i = at + kRate / 10; i < r.L.size() && !differ; i++)
        differ = std::fabs(r.L[i] - r.R[i]) > 1e-3f;
    CHECK(differ && rms(r.L, at + kRate / 10) > 0.1);
}

// ---- the Sampler

namespace
{
namespace fs = std::filesystem;

// A sine of `hz` for `seconds` at 48 kHz, written as a 16-bit WAV file in a temporary folder.
std::string sine_wav(const char *name, double hz, double seconds)
{
    std::vector<float> f((size_t)(seconds * kRate));
    for (size_t i = 0; i < f.size(); i++)
        f[i] = 0.5f * (float)std::sin(2 * 3.14159265358979 * hz * (double)i / kRate);
    const fs::path dir = fs::temp_directory_path() / "pulserack_test_samples";
    fs::create_directories(dir);
    const std::string path = (dir / name).string();
    std::ofstream(path, std::ios::binary) << make_wav(f, kRate);
    return path;
}
} // namespace

TEST(wav_reads_pcm_and_float_and_mixes_channels)
{
    SampleData  d;
    std::string err;
    // the writer's own 16-bit mono
    CHECK(parse_wav(make_wav({ 0.5f, -0.25f, 1.f }, 44100), &d, &err));
    CHECK(d.rate == 44100 && d.frames.size() == 3 && std::fabs(d.frames[0] - 0.5f) < 1e-4f
        && std::fabs(d.frames[1] + 0.25f) < 1e-4f);
    // 24-bit stereo, then 32-bit float stereo: both channels averaged
    auto wav = [](uint16_t format, uint16_t bits, const std::string &data)
    {
        std::string s = "RIFF____WAVEfmt ";
        auto        put = [&](uint32_t v, int n)
        {
            for (int i = 0; i < n; i++)
                s += (char)(v >> (8 * i) & 0xff);
        };
        put(16, 4);
        put(format, 2);
        put(2, 2);
        put(48000, 4);
        put(48000 * 2 * bits / 8, 4);
        put(2 * bits / 8, 2);
        put(bits, 2);
        s += "data";
        put((uint32_t)data.size(), 4);
        return s + data;
    };
    const std::string s24 = std::string("\x00\x00\x40", 3) + std::string("\x00\x00\xc0", 3); // +0.5, -0.5
    CHECK(parse_wav(wav(1, 24, s24), &d, &err) && d.frames.size() == 1
        && std::fabs(d.frames[0]) < 1e-6f);
    float       pair[2] = { 0.75f, 0.25f };
    std::string f32(reinterpret_cast<const char *>(pair), 8);
    CHECK(parse_wav(wav(3, 32, f32), &d, &err) && std::fabs(d.frames[0] - 0.5f) < 1e-6f);
    // refused, with a reason
    CHECK(!parse_wav("RIFF", &d, &err) && err == "not a WAV file");
    CHECK(!parse_wav(wav(2, 16, "abcd"), &d, &err) && err.find("compressed") != std::string::npos);
    CHECK(!parse_wav(wav(1, 12, "abcd"), &d, &err) && err.find("12-bit") != std::string::npos);
}

TEST(sampler_plays_its_file_at_the_pitch_of_each_note)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), s = r.add("sampler"), out = r.add("out");
    r.param(s, "root", 69);    // the file is an A4
    r.param(s, "loop", 1);     // forward: it keeps going while the key is held
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", s, "note") == Err::Ok && r.cable(s, "out", out, "l") == Err::Ok);
    std::string err;
    CHECK(r.e.set_sample(s, sine_wav("a440.wav", 440, 0.5), &err) == Err::Ok);
    r.key(kb, 0.1, true, 81); // an octave up
    r.key(kb, 1.5, false, 81);
    CHECK(r.compile());
    r.render(2 * kRate);
    CHECK(peak(r.L, 0, (size_t)(0.09 * kRate)) == 0.f); // nothing before the note
    const double f = frequency(r.L, (size_t)(0.2 * kRate), (size_t)(1.4 * kRate));
    CHECK(f > 875 && f < 885);
    CHECK(rms(r.L, (size_t)(1.2 * kRate), (size_t)(1.4 * kRate)) > 0.2); // looped past its 0.5 s
    CHECK(peak(r.L, (size_t)(1.6 * kRate)) < 1e-3f);                     // and stopped on release
    CHECK(r.e.sample_error(s).empty() && r.e.apply_failures() == 0);
}

TEST(sampler_one_shot_plays_once_and_a_missing_file_is_silent_but_opens)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), s = r.add("sampler"), out = r.add("out");
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", s, "note") == Err::Ok && r.cable(s, "out", out, "l") == Err::Ok);
    std::string err;
    CHECK(r.e.set_sample(s, sine_wav("short.wav", 440, 0.2), &err) == Err::Ok);
    r.key(kb, 0.1, true, 60); // the root: as recorded
    CHECK(r.compile());
    r.render(kRate);
    CHECK(rms(r.L, (size_t)(0.12 * kRate), (size_t)(0.28 * kRate)) > 0.2);
    CHECK(peak(r.L, (size_t)(0.32 * kRate)) < 1e-3f); // its 0.2 s, then nothing, though held

    // a file that cannot be read: refused, and the module keeps the one it has
    CHECK(r.e.set_sample(s, "/no/such/file.wav", &err) == Err::InitFailed && !err.empty());
    CHECK(r.e.patch().find(s)->sample.find("short.wav") != std::string::npos);

    // a patch naming a missing file still opens; the module says why it is silent
    Patch      p;
    LoadResult lr = load_patch("pulserack 1\nnode 1 sampler x=0 y=0\nsample 1 /no/such/take #3.wav\n", p);
    CHECK(lr.ok && p.find(1)->sample == "/no/such/take #3.wav"); // ('#' is part of the name)
    Rig r2;
    r2.e.patch() = std::move(p);
    CHECK(r2.compile() && r2.e.sample_error(1) == "cannot be read");
}

TEST(sampler_file_survives_a_patch_file_round_trip)
{
    Patch          p;
    const uint32_t s = p.add_node(*find_module("sampler"));
    const uint32_t o = p.add_node(*find_module("osc"));
    p.find(s)->sample = "/samples/my kick.wav";
    Patch back;
    CHECK(load_patch(save_patch(p), back).ok && back.find(s)->sample == "/samples/my kick.wav");
    // on a module that plays no sample: skipped with a warning
    LoadResult lr = load_patch("pulserack 1\nnode 1 osc x=0 y=0\nsample 1 x.wav\n", back);
    CHECK(lr.ok && lr.warnings.size() == 1 && back.find(1)->sample.empty());
    (void)o;
}
