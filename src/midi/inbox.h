// Where MIDI messages wait for the audio thread (docs/midi.md, *Threads*): one ring per device
// slot, because each device's messages arrive on its own thread and a ring has exactly one
// producer. The consumer (the engine's render(), step 4) pops from every slot. No libremidi here.
#pragma once

#include "engine/rt/spsc.h"
#include "midi/message.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rack::midi
{

constexpr size_t kMaxInputs = 8;   // devices open at once
constexpr size_t kInboxSize = 512; // messages waiting per device

struct Inbox
{
    // Producer: the device's thread while it is open; the UI thread after closing it (closing stops
    // the device's thread, so the two never push at once).
    SpscRing<Msg, kInboxSize> ring;
    std::atomic<uint32_t>     lost { 0 }; // messages dropped because the ring was full

    void push(const Msg &m)
    {
        if (!ring.push(m))
            lost.fetch_add(1, std::memory_order_relaxed);
    }
};

using Inboxes = std::array<Inbox, kMaxInputs>;

} // namespace rack::midi
