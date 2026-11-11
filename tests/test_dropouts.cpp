// The dropout meter (src/audio/dropout_meter.h) with made-up callback timings.
#include "testutil.h"

#include "audio/dropout_meter.h"

using rack::audio::DropoutMeter;

namespace
{
constexpr double kBuf = 256.0 / 48000.0; // 5.3 ms
}

TEST(dropouts_steady_callbacks_are_none)
{
    DropoutMeter m;
    m.reset(kBuf, 0);
    double t = 0;
    for (int i = 0; i < 10000; i++, t += kBuf)
        m.on_callback(t, kBuf, kBuf * 0.1);
    CHECK(m.count == 0);
}

TEST(dropouts_jitter_within_a_buffer_is_none)
{
    DropoutMeter m;
    m.reset(kBuf, 0);
    double t = 0;
    for (int i = 0; i < 10000; i++)
    {
        const double late = (i % 7 == 3) ? 0.6 * kBuf : (i % 7 == 4 ? -0.6 * kBuf : 0);
        m.on_callback(t + late, kBuf, kBuf * 0.2);
        t += kBuf;
    }
    CHECK(m.count == 0);
}

TEST(dropouts_a_late_callback_is_one)
{
    DropoutMeter m;
    m.reset(kBuf, 0);
    double t = 0;
    for (int i = 0; i < 100; i++, t += kBuf)
        m.on_callback(t, kBuf, 0.0005);
    t += 3 * kBuf; // the system stalled for three buffers
    for (int i = 0; i < 100; i++, t += kBuf)
        m.on_callback(t, kBuf, 0.0005);
    CHECK(m.count == 1);
}

TEST(dropouts_rendering_slower_than_real_time_counts)
{
    DropoutMeter m;
    m.reset(kBuf, 0);
    double t = 0;
    for (int i = 0; i < 10; i++, t += kBuf)
        m.on_callback(t, kBuf, i == 5 ? kBuf * 1.5 : kBuf * 0.1);
    CHECK(m.count == 1);
}

TEST(dropouts_bigger_requests_that_keep_pace_are_none)
{
    // the device asks for two buffers every two buffer times, or one now and three later: in step
    DropoutMeter m;
    m.reset(kBuf, 0);
    double t = 0;
    for (int i = 0; i < 1000; i++)
    {
        const double audio = (i % 2) ? 3 * kBuf : kBuf;
        m.on_callback(t, audio, 0.0005);
        t += audio;
    }
    CHECK(m.count == 0);
    m.reset(kBuf);
    CHECK(m.count == 0 && m.last < 0);
}

TEST(dropouts_the_first_quarter_second_is_not_counted)
{
    DropoutMeter m;
    m.reset(kBuf); // 0.25 s of warm-up
    double t = 0;
    for (int i = 0; i < 10; i++, t += kBuf)
        m.on_callback(t, kBuf, 0.0005);
    t += 5 * kBuf;                    // a stall while the device settles: not counted
    m.on_callback(t, kBuf, kBuf * 2); // nor a slow first render
    t += kBuf;
    for (int i = 0; i < 100; i++, t += kBuf) // past the warm-up, steady
        m.on_callback(t, kBuf, 0.0005);
    CHECK(m.count == 0 && m.played > 0.25);
    t += 5 * kBuf; // the same stall now counts
    m.on_callback(t, kBuf, 0.0005);
    CHECK(m.count == 1);
}
