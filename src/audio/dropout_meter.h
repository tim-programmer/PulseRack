// Counts dropouts (underruns) from the audio callback's timing, with no help from the driver: the
// audio delivered must keep pace with the clock. Each callback adds the audio it rendered and takes
// away the time since the one before; when that balance falls more than one device buffer behind,
// the device had nothing to play for a while: a dropout, and the balance starts again. A callback
// that takes longer to render than the audio it renders is one too. The first quarter of a second
// is not counted: the device and the stream settle then, and the callbacks come unevenly (seen once
// in four starts here, with nothing to hear). No SDL, so it is tested on its own.
#pragma once

#include <algorithm>
#include <cstdint>

namespace rack::audio
{

struct DropoutMeter
{
    double   buffer = 0;                // seconds of audio in one device buffer
    double   last = -1;                 // when the last callback started (seconds, any clock)
    double   balance = 0;               // audio delivered minus time gone by
    double   warmup = 0.25, played = 0; // seconds of audio before anything counts, and so far
    uint32_t count = 0;

    void reset(double buffer_seconds, double warmup_seconds = 0.25)
    {
        buffer = buffer_seconds;
        warmup = warmup_seconds;
        played = 0;
        last = -1;
        balance = 0;
        count = 0;
    }
    // A callback that started at `now`, rendered `audio` seconds of sound and took `spent` seconds.
    void on_callback(double now, double audio, double spent)
    {
        played += audio;
        if (played <= warmup)
        { // settling: follow the clock, count nothing
            last = now;
            balance = 0;
            return;
        }
        if (audio > 0 && spent > audio)
            count++; // rendering slower than real time: the device ran dry while it waited
        else if (last >= 0)
        {
            balance += audio - (now - last);
            if (balance < -buffer)
            {
                count++;
                balance = 0;
            }
            balance = std::min(balance, 4 * buffer); // what the stream can hold ahead, at most
        }
        last = now;
    }
};

} // namespace rack::audio
