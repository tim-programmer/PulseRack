// The oscilloscope module: what it captures, its trigger, and the triple buffer it hands frames
// over with (the threaded case is the one to run under ThreadSanitizer).
#include "testutil.h"

#include "engine/model/scope.h"

#include <atomic>
#include <thread>

using namespace rt_util;

namespace
{
// an oscillator (sine, `hz`) into the scope's A; the scope's settings as given
struct ScopeRig
{
    Rig      r;
    uint32_t osc, scope;
    ScopeRig(float hz, float trigger, float time_ms = 20, float scale_a = 1)
    {
        osc = r.add("osc");
        scope = r.add("scope");
        r.param(osc, "waveform", 0); // sine
        r.param(osc, "frequency", hz);
        r.param(scope, "trigger", trigger);
        r.param(scope, "time", time_ms);
        r.param(scope, "scale_a", scale_a);
        CHECK(r.cable(osc, "out", scope, "in_a") == Err::Ok);
        CHECK(r.compile());
    }
    ScopeFrame run(size_t frames)
    {
        r.render(frames);
        ScopeFrame f;
        CHECK(r.e.scope_frame(scope, f));
        return f;
    }
};
float top(const ScopeFrame &f, int ch)
{
    float m = -1e9f;
    for (int c = 0; c < kScopeColumns; c++)
        m = std::max(m, f.hi[ch][c]);
    return m;
}
float bottom(const ScopeFrame &f, int ch)
{
    float m = 1e9f;
    for (int c = 0; c < kScopeColumns; c++)
        m = std::min(m, f.lo[ch][c]);
    return m;
}
float mid(const ScopeFrame &f, int ch, int c) { return (f.lo[ch][c] + f.hi[ch][c]) / 2; }
} // namespace

TEST(scope_a_sine_on_a_rising_edge)
{
    ScopeRig   s(1000, 1); // 1 kHz, rise; 20 ms is 20 periods
    ScopeFrame f = s.run(9600);
    CHECK(f.seq >= 1 && f.triggered && f.has[0] && !f.has[1]);
    CHECK(std::fabs(f.window_ms - 20) < 0.2f);
    CHECK(top(f, 0) > 0.9f && top(f, 0) <= 1.01f && bottom(f, 0) < -0.9f); // its full swing
    // it starts at the first sample at or above the level (0), at most one sample's step past it (a
    // 1 kHz sine at 48 kHz moves 0.13 a sample: float hits 0.000 exactly at the crossing, Q31 is a
    // hair below and fires on the next sample, 0.131), and the wave goes up
    CHECK(f.lo[0][0] >= -0.001f && f.lo[0][0] < 0.14f && mid(f, 0, 2) > mid(f, 0, 0));
    for (int c = 0; c < kScopeColumns;
        c++) // every column has its samples, and B (not connected) is 0
        CHECK(f.lo[0][c] <= f.hi[0][c] && f.lo[1][c] == 0 && f.hi[1][c] == 0);
    // a falling edge starts going down
    ScopeRig   d(1000, 2);
    ScopeFrame g = d.run(9600);
    CHECK(
        g.triggered && g.hi[0][0] <= 0.001f && g.hi[0][0] > -0.14f && mid(g, 0, 2) < mid(g, 0, 0));
}

TEST(scope_scale_and_window)
{
    ScopeRig   s(200, 1, 100, 2); // 100 ms, A scaled x2
    ScopeFrame f = s.run(48000);
    CHECK(top(f, 0) > 1.8f && bottom(f, 0) < -1.8f); // scaled (the window draws +-1 at its edges)
    CHECK(std::fabs(f.window_ms - 100) < 1);
    ScopeRig   t(1000, 0, 5); // the shortest window: 240 samples, one a column
    ScopeFrame g = t.run(4800);
    CHECK(g.seq >= 1 && std::fabs(g.window_ms - 5) < 0.1f && g.triggered); // (free: always)
}

TEST(scope_auto_trigger_and_free_run)
{
    // a signal that never crosses the level: drawn anyway after a while, marked as not triggered
    ScopeRig s(1000, 1);
    s.r.param(s.scope, "level",
        1.0f); // the top of the range: a sine of amplitude ~1 never rises through it
    ScopeFrame f = s.run(48000);
    CHECK(f.seq >= 1 && !f.triggered);
    // free: one frame after another, all marked as triggered (nothing was awaited)
    ScopeRig   r(1000, 0);
    ScopeFrame g = r.run(48000);
    CHECK(g.triggered && g.seq >= 40); // ~50 windows of 20 ms in a second
}

TEST(scope_the_reader_gets_the_newest_frame_once)
{
    ScopeRig       s(1000, 0);
    ScopeFrame     f = s.run(48000);
    const uint32_t first = f.seq;
    CHECK(!s.r.e.scope_frame(s.scope, f)); // nothing new since
    s.r.render(48000);
    CHECK(s.r.e.scope_frame(s.scope, f)
        && f.seq >= first + 40);                  // the newest, frames in between skipped
    CHECK(!s.r.e.scope_frame(s.r.add("vca"), f)); // not a scope (and not compiled yet)
}

TEST(scope_threaded_reader_and_writer)
{
    // the audio thread renders while this thread takes frames; every frame is whole (TSan: no race)
    ScopeRig s(440, 0, 5);
    s.r.e.attach_audio(true);
    std::atomic<bool> stop { false };
    std::thread       audio(
        [&]
        {
            std::vector<float> l(256), r(256);
            while (!stop.load())
                s.r.e.render(256, l.data(), r.data());
        });
    int      frames = 0;
    uint32_t last = 0;
    for (int i = 0; i < 20000 && frames < 300; i++)
    {
        ScopeFrame f;
        if (s.r.e.scope_frame(s.scope, f))
        {
            frames++;
            CHECK(f.seq > last && f.has[0]);
            last = f.seq;
            bool whole = true;
            for (int c = 0; c < kScopeColumns; c++)
                whole &= f.lo[0][c] <= f.hi[0][c];
            CHECK(whole);
        }
        std::this_thread::yield();
    }
    stop.store(true);
    audio.join();
    s.r.e.attach_audio(false);
    CHECK(frames > 20);
}
