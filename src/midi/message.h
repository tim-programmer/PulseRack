// MIDI messages as the rack uses them (docs/midi.md, *Messages*): one small, trivially copyable
// struct per message, so it can travel through an SpscRing. No libremidi here: this is the part the
// engine and the tests see.
#pragma once

#include <cstdint>
#include <span>

namespace rack::midi
{

enum class Type : uint8_t
{
    NoteOn,
    NoteOff,         // also a note on with velocity 0
    Control,         // data: controller number, value: 0..127
    PitchBend,       // value: 0..16383, 8192 is the middle
    ChannelPressure, // value: 0..127
    KeyPressure,     // data: note, value: 0..127
    Program,         // data: program number
    Clock,           // the realtime messages: no channel
    Start,
    Continue,
    Stop,
    DeviceLost, // not MIDI: the device went away; release every note it holds
};

struct Msg
{
    int64_t  time_ns = 0; // monotonic time it arrived (libremidi's), 0 if unknown
    Type     type = Type::NoteOn;
    uint8_t  slot = 0;    // the device's slot (docs/midi.md, *Threads*)
    uint8_t  channel = 0; // 0..15; 0 for the realtime messages and DeviceLost
    uint8_t  data = 0;    // note, controller or program number
    uint16_t value = 0;   // velocity, controller value, pressure, or pitch bend
};

// One whole MIDI 1.0 message (libremidi delivers them whole, running status already expanded).
// False for a message the rack does not use (SysEx, time code, song position...) or a malformed
// one; `out` is then untouched. `slot` and `time_ns` are the caller's to fill.
bool decode(std::span<const uint8_t> bytes, Msg *out);

} // namespace rack::midi
