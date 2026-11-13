// Where MIDI messages wait to be sent (docs/midi.md, step 9, MIDI out): the engine pushes what the
// MIDI Out modules made, each stamped with the time it is due, and midi::Outputs' sender thread
// sends it then. One ring: its one producer is whichever thread renders (the audio thread, or the
// UI's while none is attached). No libremidi here.
#pragma once

#include "engine/rt/spsc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rack::midi
{

constexpr size_t kMaxOutputs = 8;     // output devices named in one run
constexpr size_t kOutboxSize = 2048;  // messages waiting to be sent

struct OutMsg
{
    int64_t due_ns = 0; // steady_clock time to send it at; 0: at once
    uint8_t slot = 0;   // the output's slot
    uint8_t len = 0;    // 1..3
    uint8_t bytes[3] = { };
};

struct Outbox
{
    SpscRing<OutMsg, kOutboxSize> ring;
    std::atomic<uint32_t>         lost { 0 }; // dropped because the ring was full

    void push(const OutMsg &m)
    {
        if (!ring.push(m))
            lost.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace rack::midi
