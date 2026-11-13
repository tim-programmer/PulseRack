// MIDI without a device (docs/midi.md): decoding messages, unique device names, the inboxes, and
// the settings file.
#include "portable_env.h"
#include "testutil.h"

#include "midi/inbox.h"
#include "midi/outbox.h"
#include "midi/message.h"
#include "midi/names.h"
#include "engine/rt/modules_common.h"
#include "ui/settings.h"

#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <thread>
#include <vector>

using namespace rt_util;
using namespace rack::midi;
using rack::ui::Settings;
namespace fs = std::filesystem;

namespace
{
bool dec(std::initializer_list<uint8_t> bytes, Msg *m)
{
    const std::vector<uint8_t> v(bytes);
    return decode(v, m);
}
} // namespace

TEST(midi_decode_channel_messages)
{
    Msg m;
    CHECK(dec({ 0x93, 60, 100 }, &m));
    CHECK(m.type == Type::NoteOn && m.channel == 3 && m.data == 60 && m.value == 100);
    CHECK(dec({ 0x90, 60, 0 }, &m)); // velocity 0 is a note off
    CHECK(m.type == Type::NoteOff && m.data == 60 && m.value == 0);
    CHECK(dec({ 0x8F, 61, 40 }, &m));
    CHECK(m.type == Type::NoteOff && m.channel == 15 && m.data == 61 && m.value == 40);
    CHECK(dec({ 0xB0, 1, 127 }, &m));
    CHECK(m.type == Type::Control && m.data == 1 && m.value == 127);
    CHECK(dec({ 0xA2, 64, 33 }, &m));
    CHECK(m.type == Type::KeyPressure && m.channel == 2 && m.data == 64 && m.value == 33);
    CHECK(dec({ 0xC5, 12 }, &m));
    CHECK(m.type == Type::Program && m.channel == 5 && m.data == 12);
    CHECK(dec({ 0xD0, 90 }, &m));
    CHECK(m.type == Type::ChannelPressure && m.value == 90);
    // pitch bend: 14 bits, LSB first; the middle is 8192
    CHECK(dec({ 0xE0, 0x00, 0x40 }, &m) && m.type == Type::PitchBend && m.value == 8192);
    CHECK(dec({ 0xE0, 0x7F, 0x7F }, &m) && m.value == 16383);
    CHECK(dec({ 0xE0, 0x00, 0x00 }, &m) && m.value == 0);
}

TEST(midi_decode_realtime_and_refusals)
{
    Msg m;
    CHECK(dec({ 0xF8 }, &m) && m.type == Type::Clock && m.channel == 0);
    CHECK(dec({ 0xFA }, &m) && m.type == Type::Start);
    CHECK(dec({ 0xFB }, &m) && m.type == Type::Continue);
    CHECK(dec({ 0xFC }, &m) && m.type == Type::Stop);

    m.type = Type::Program;
    m.data = 99;
    CHECK(!dec({ }, &m));
    CHECK(!dec({ 0xF0, 0x7E, 0x7F, 0xF7 }, &m));    // SysEx
    CHECK(!dec({ 0xFE }, &m));                      // active sensing
    CHECK(!dec({ 0xF2, 0, 0 }, &m));                // song position
    CHECK(!dec({ 60, 100 }, &m));                   // no status byte
    CHECK(!dec({ 0x90, 60 }, &m));                  // too short
    CHECK(!dec({ 0x90, 60, 100, 1 }, &m));          // too long
    CHECK(!dec({ 0x90, 0x80, 100 }, &m));           // a data byte with the top bit set
    CHECK(!dec({ 0xC0, 1, 2 }, &m));                // program change has one data byte
    CHECK(m.type == Type::Program && m.data == 99); // untouched by every refusal
}

TEST(midi_unique_names)
{
    const std::vector<std::string> raw { "nanoKONTROL2", "KeyStep", "nanoKONTROL2", "",
        "nanoKONTROL2" };
    const std::vector<std::string> u = unique_names(raw);
    CHECK((u
        == std::vector<std::string> {
            "nanoKONTROL2", "KeyStep", "nanoKONTROL2 #2", "MIDI input", "nanoKONTROL2 #3" }));
    // a device really named "X #2" does not collide with the second "X"
    const std::vector<std::string> raw2 { "X #2", "X", "X" };
    CHECK((unique_names(raw2) == std::vector<std::string> { "X #2", "X", "X #3" }));
    CHECK(unique_names(std::vector<std::string> { }).empty());
}

TEST(midi_inbox_counts_what_it_loses_and_keeps_order)
{
    static Inbox box; // large
    Msg          m;
    for (size_t i = 0; i < kInboxSize + 5; i++)
    {
        m.data = uint8_t(i & 0x7F);
        box.push(m);
    }
    CHECK(box.lost.load() == 5);
    size_t n = 0;
    while (box.ring.pop(m))
        CHECK(m.data == uint8_t(n++ & 0x7F));
    CHECK(n == kInboxSize);
}

TEST(midi_inbox_one_producer_thread_one_consumer)
{
    static Inbox  box;
    constexpr int kCount = 100000;
    std::thread   producer(
        [&]
        {
            for (int i = 0; i < kCount; i++)
            {
                Msg m;
                m.time_ns = i;
                while (!box.ring.push(m))
                    std::this_thread::yield();
            }
        });
    int64_t next = 0;
    while (next < kCount)
    {
        Msg m;
        if (box.ring.pop(m))
            CHECK(m.time_ns == next++);
        else
            std::this_thread::yield();
    }
    producer.join();
    CHECK(box.lost.load() == 0);
}

// ---- the engine and the MIDI In module: messages put in an inbox, as a device's thread would
namespace
{
Msg msg(Type t, uint8_t data = 0, uint16_t value = 0, uint8_t channel = 0, uint8_t slot = 0)
{
    Msg m;
    m.type = t;
    m.data = data;
    m.value = value;
    m.channel = channel;
    m.slot = slot;
    return m;
}
// MIDI In -> (a port) -> the output at level 1, with its own inboxes
struct MidiRig : Rig
{
    std::unique_ptr<Inboxes> boxes = std::make_unique<Inboxes>();
    uint32_t                 in = 0, gate = 0, out = 0;
    explicit MidiRig(const char *signal = nullptr) // null: the notes, through Note to Gate
    {
        e.set_midi(boxes.get());
        in = add("midi_in");
        out = add("out");
        param(out, "level", 1);
        if (signal)
            CHECK(cable(in, signal, out, "l") == Err::Ok);
        else
        {
            gate = add("note_to_gate");
            CHECK(cable(in, "notes", gate, "notes") == Err::Ok);
            CHECK(cable(gate, "gate", out, "l") == Err::Ok);
        }
        CHECK(compile());
    }
    void  send(const Msg &m) { (*boxes)[m.slot].push(m); }
    float now(size_t frames = 256) // the output's last sample after `frames` more
    {
        render(frames);
        return L.back();
    }
};
} // namespace

TEST(midi_in_notes_open_and_close_a_gate)
{
    MidiRig r;
    CHECK(r.now() == 0.0f);
    r.send(msg(Type::NoteOn, 60, 100));
    CHECK(r.now() > 0.99f);
    CHECK(r.L[r.L.size() - 256] > 0.99f); // from the first sample of the block it arrived before
    r.send(msg(Type::NoteOff, 60));
    CHECK(r.now() == 0.0f);
    // every inbox is emptied, whichever device it belongs to
    r.send(msg(Type::NoteOn, 62, 90, 0, 5));
    CHECK(r.now() > 0.99f);
    r.send(msg(Type::NoteOff, 62, 0, 0, 5));
    CHECK(r.now() == 0.0f);
    Msg left;
    for (Inbox &b : *r.boxes)
        CHECK(!b.ring.pop(left) && b.lost.load() == 0);
}

TEST(midi_in_channel_filters_and_a_change_releases_held_notes)
{
    MidiRig r;
    r.param(r.in, "channel", 2); // channel 2 only (a message's channel counts from 0)
    r.send(msg(Type::NoteOn, 60, 100, 0));
    CHECK(r.now() == 0.0f);
    r.send(msg(Type::NoteOn, 60, 100, 1));
    CHECK(r.now() > 0.99f);
    r.send(msg(Type::NoteOff, 60, 0, 0)); // another channel's note off: not ours
    CHECK(r.now() > 0.99f);
    // changing the channel lets go of what is held: its note off would never be heard
    r.param(r.in, "channel", 0);
    CHECK(r.now() == 0.0f);
    r.send(msg(Type::NoteOn, 64, 100, 9)); // omni: any channel
    CHECK(r.now() > 0.99f);
}

TEST(midi_in_a_lost_device_releases_its_notes_only)
{
    MidiRig r;
    r.send(msg(Type::NoteOn, 60, 100, 0, 0));
    CHECK(r.now() > 0.99f);
    r.send(msg(Type::DeviceLost, 0, 0, 0, 1)); // another device, holding nothing
    CHECK(r.now() > 0.99f);
    r.send(msg(Type::DeviceLost, 0, 0, 0, 0));
    CHECK(r.now() == 0.0f);
    // all notes off (CC 123) does the same for the device that sends it
    r.send(msg(Type::NoteOn, 60, 100, 0, 2));
    CHECK(r.now() > 0.99f);
    r.send(msg(Type::Control, 123, 0, 0, 2));
    CHECK(r.now() == 0.0f);
    // reset() forgets held notes too
    r.send(msg(Type::NoteOn, 60, 100));
    CHECK(r.now() > 0.99f);
    r.e.reset();
    CHECK(r.now() == 0.0f);
}

TEST(midi_in_signals)
{
    { // pitch bend: -1 .. 1, smoothed (not there at once, there after a few milliseconds)
        MidiRig r("bend");
        r.send(msg(Type::PitchBend, 0, 16383));
        r.render(16);
        CHECK(r.L.back() > 0.0f && r.L.back() < 0.5f);
        CHECK(std::fabs(r.now(4800) - 1.0f) < 0.01f);
        r.send(msg(Type::PitchBend, 0, 8192));
        CHECK(std::fabs(r.now(4800)) < 0.01f);
        r.send(msg(Type::PitchBend, 0, 0));
        CHECK(std::fabs(r.now(4800) + 1.0f) < 0.01f);
    }
    { // the mod wheel is CC 1; other controllers are not
        MidiRig r("mod");
        r.send(msg(Type::Control, 7, 127));
        CHECK(std::fabs(r.now(4800)) < 0.01f);
        r.send(msg(Type::Control, 1, 127));
        CHECK(std::fabs(r.now(4800) - 1.0f) < 0.01f);
        r.send(msg(Type::Control, 1, 64));
        CHECK(std::fabs(r.now(4800) - 64.0f / 127) < 0.01f);
    }
    { // pressure: the channel's, or the key's for the note pressed last
        MidiRig r("pressure");
        r.send(msg(Type::ChannelPressure, 0, 127));
        CHECK(std::fabs(r.now(4800) - 1.0f) < 0.01f);
        r.send(msg(Type::NoteOn, 60, 100));
        r.send(msg(Type::NoteOn, 64, 100));
        r.send(msg(Type::KeyPressure, 60, 10)); // not the newest note
        CHECK(std::fabs(r.now(4800) - 1.0f) < 0.01f);
        r.send(msg(Type::KeyPressure, 64, 0));
        CHECK(std::fabs(r.now(4800)) < 0.01f);
    }
    { // velocity: of the newest note, at once, and it stays after the note ends
        MidiRig r("velocity");
        r.send(msg(Type::NoteOn, 60, 64));
        CHECK(std::fabs(r.now() - 64.0f / 127) < 0.01f);
        r.send(msg(Type::NoteOff, 60));
        CHECK(std::fabs(r.now() - 64.0f / 127) < 0.01f);
    }
}

TEST(midi_cc_follows_its_controller_on_its_channel)
{
    Rig            r;
    auto           boxes = std::make_unique<Inboxes>();
    const uint32_t cc = r.add("midi_cc"), out = r.add("out");
    r.e.set_midi(boxes.get());
    r.param(out, "level", 1);
    CHECK(r.cable(cc, "out", out, "l") == Err::Ok);
    CHECK(r.compile());
    auto send = [&](const Msg &m) { (*boxes)[m.slot].push(m); };
    auto now = [&](size_t frames = 4800)
    {
        r.render(frames);
        return r.L.back();
    };
    CHECK(now() == 0.0f); // nothing yet
    // the default controller is 1 (the mod wheel), on any channel, from any device
    send(msg(Type::Control, 1, 127, 5, 3));
    r.render(16); // smoothed: not there at once
    CHECK(r.L.back() > 0.0f && r.L.back() < 0.5f);
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    send(msg(Type::Control, 74, 0)); // another controller: not ours
    send(msg(Type::NoteOn, 1, 0));   // nor a note with the same number
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    // another controller number: the value stays until that controller moves
    r.param(cc, "cc", 74);
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    send(msg(Type::Control, 1, 0));
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    send(msg(Type::Control, 74, 32));
    CHECK(std::fabs(now() - 32.0f / 127) < 0.01f);
    // one channel only
    r.param(cc, "channel", 3);
    send(msg(Type::Control, 74, 127, 0));
    CHECK(std::fabs(now() - 32.0f / 127) < 0.01f);
    send(msg(Type::Control, 74, 127, 2));
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    // a MIDI In beside it gets the same messages: both are fed
    const uint32_t in = r.add("midi_in");
    CHECK(r.e.patch().disconnect({ out, (uint16_t)r.port(out, "l") }) == Err::Ok);
    CHECK(r.cable(in, "mod", out, "l") == Err::Ok && r.compile());
    send(msg(Type::Control, 1, 127));
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    r.e.reset();
    CHECK(now() == 0.0f);
}

TEST(midi_messages_are_taken_with_no_midi_in_module_and_can_be_discarded)
{
    Rig  r;
    auto boxes = std::make_unique<Inboxes>();
    r.e.set_midi(boxes.get());
    const uint32_t out = r.add("out");
    (void)out;
    CHECK(r.compile());
    (*boxes)[0].push(msg(Type::NoteOn, 60, 100));
    r.render(64);
    Msg m;
    CHECK(!(*boxes)[0].ring.pop(m)); // taken, though nothing wanted it
    (*boxes)[3].push(msg(Type::NoteOn, 60, 100, 0, 3));
    r.e.discard_midi();
    CHECK(!(*boxes)[3].ring.pop(m));
    r.e.set_midi(nullptr); // and none at all is fine
    r.render(64);
}

TEST(midi_in_drops_notes_it_has_no_room_for_and_counts_them)
{
    MidiRig r;
    for (int i = 0; i < 200; i++) // more than a block's worth of notes
        r.send(msg(Type::NoteOn, (uint8_t)(i % 100), 100));
    r.render(64);
    CHECK(midi_in_dropped(r.e.state(r.in)) == 200 - 128);
    r.render(64); // and it goes on working
    r.send(msg(Type::DeviceLost));
    CHECK(r.now() == 0.0f);
}

// ---- when a message is heard (docs/midi.md, step 8): its age on a clock the test owns
namespace
{
int64_t           g_clock_ns = 0;
int64_t           fake_clock() { return g_clock_ns; }
constexpr int64_t kStart = 1'000'000'000; // any time but 0 (a time of 0 is "no timestamp")
int64_t           samples_ns(double samples) { return (int64_t)(samples * 1e9 / kRate); }
// a message that arrived `samples` before the clock's now
Msg aged(Msg m, double samples)
{
    m.time_ns = g_clock_ns - samples_ns(samples);
    return m;
}
// the first sample at or after `from` that is over a half, or -1
long first_high(const std::vector<float> &v, size_t from = 0)
{
    for (size_t i = from; i < v.size(); i++)
        if (v[i] > 0.5f)
            return (long)i;
    return -1;
}
struct TimedRig : MidiRig
{
    explicit TimedRig(const char *signal = nullptr)
        : MidiRig(signal)
    {
        g_clock_ns = kStart;
        e.set_midi_clock(fake_clock);
    }
    // a device call: the clock moves on by its length afterwards, as the real one does
    void call(size_t frames = 256)
    {
        render(frames);
        g_clock_ns += samples_ns((double)frames);
    }
};
} // namespace

TEST(midi_timing_a_note_starts_at_its_own_sample)
{
    TimedRig r;
    r.send(aged(msg(Type::NoteOn, 60, 100), 100)); // 100 samples old in a call of 256
    r.call();
    CHECK(first_high(r.L) == 156);
    CHECK(r.L[155] == 0.0f && r.L[255] > 0.99f);
    r.send(aged(msg(Type::NoteOff, 60), 100));
    r.call();
    CHECK(r.L[256 + 155] > 0.99f && r.L[256 + 156] == 0.0f);
}

TEST(midi_timing_old_new_and_unstamped_messages)
{
    { // older than the call: its first sample
        TimedRig r;
        r.send(aged(msg(Type::NoteOn, 60, 100), 1000));
        r.call();
        CHECK(first_high(r.L) == 0);
    }
    { // exactly a call old: its first sample too
        TimedRig r;
        r.send(aged(msg(Type::NoteOn, 60, 100), 256));
        r.call();
        CHECK(first_high(r.L) == 0);
    }
    { // just arrived: the last sample
        TimedRig r;
        r.send(aged(msg(Type::NoteOn, 60, 100), 0));
        r.call();
        CHECK(first_high(r.L) == 255);
    }
    { // stamped after the clock was read (the device's thread ran on): the last sample
        TimedRig r;
        r.send(aged(msg(Type::NoteOn, 60, 100), -50));
        r.call();
        CHECK(first_high(r.L) == 255);
    }
    { // no timestamp: the first sample
        TimedRig r;
        r.send(msg(Type::NoteOn, 60, 100));
        r.call();
        CHECK(first_high(r.L) == 0);
    }
    { // any length of call: 30 samples old in one of 64
        TimedRig r;
        r.send(aged(msg(Type::NoteOn, 60, 100), 30));
        r.call(64);
        CHECK(first_high(r.L) == 34);
    }
}

TEST(midi_timing_messages_keep_their_spacing_and_order)
{
    // on and off 40 samples apart in one call: a gate of 40 samples
    TimedRig r;
    r.send(aged(msg(Type::NoteOn, 60, 100), 200));
    r.send(aged(msg(Type::NoteOff, 60), 160));
    r.call();
    CHECK(first_high(r.L) == 56);
    CHECK(r.L[95] > 0.99f && r.L[96] == 0.0f && r.L[255] == 0.0f);
    // two devices: the later inbox's earlier message still comes first
    r.send(aged(msg(Type::NoteOn, 62, 100, 0, 0), 50));
    r.send(aged(msg(Type::NoteOn, 64, 100, 0, 3), 150));
    r.send(aged(msg(Type::DeviceLost, 0, 0, 0, 3), 100));
    r.call();
    CHECK(first_high(r.L, 256) == 256 + 106);
    CHECK(r.L[256 + 155] > 0.99f); // (the other device's note goes on at 206: no gap is seen)
    CHECK(r.L[511] > 0.99f);
    r.send(aged(msg(Type::NoteOff, 62), 10));
    r.call();
    CHECK(r.L[512 + 245] > 0.99f && r.L[512 + 246] == 0.0f);
}

TEST(midi_timing_a_signal_changes_inside_the_block)
{
    { // pitch bend: nothing before its sample, moving from it on
        TimedRig r("bend");
        r.send(aged(msg(Type::PitchBend, 0, 16383), 100));
        r.call();
        for (size_t i = 0; i < 156; i++)
            CHECK(r.L[i] == 0.0f);
        CHECK(r.L[156] > 0.0f && r.L[157] > r.L[156] && r.L[255] > r.L[157]);
    }
    { // the mod wheel
        TimedRig r("mod");
        r.send(aged(msg(Type::Control, 1, 127), 56));
        r.call();
        CHECK(r.L[199] == 0.0f && r.L[200] > 0.0f);
    }
    { // velocity is not smoothed: a step at the note's sample
        TimedRig r("velocity");
        r.send(aged(msg(Type::NoteOn, 60, 127), 100));
        r.call();
        CHECK(r.L[155] == 0.0f && std::fabs(r.L[156] - 1.0f) < 0.01f);
    }
    { // MIDI CC
        Rig            r;
        auto           boxes = std::make_unique<Inboxes>();
        const uint32_t cc = r.add("midi_cc"), out = r.add("out");
        r.e.set_midi(boxes.get());
        g_clock_ns = kStart;
        r.e.set_midi_clock(fake_clock);
        r.param(out, "level", 1);
        CHECK(r.cable(cc, "out", out, "l") == Err::Ok && r.compile());
        (*boxes)[0].push(aged(msg(Type::Control, 1, 127), 100));
        r.render(256);
        CHECK(r.L[155] == 0.0f && r.L[156] > 0.0f && r.L[255] > r.L[156]);
    }
}

TEST(midi_timing_a_call_longer_than_a_block)
{
    // 1000 frames are made in blocks of PCL_MAX_BLOCK; the message lands in a later one
    TimedRig r;
    r.send(aged(msg(Type::NoteOn, 60, 100), 300));
    r.send(aged(msg(Type::NoteOff, 60), 10));
    r.call(1000);
    CHECK(first_high(r.L) == 700);
    CHECK(r.L[989] > 0.99f && r.L[990] == 0.0f);
    // and the next call starts clean
    r.send(aged(msg(Type::NoteOn, 60, 100), 1));
    r.call(1000);
    CHECK(first_high(r.L, 1000) == 1999);
}

TEST(midi_timing_reset_forgets_what_waits)
{
    TimedRig r;
    r.send(aged(msg(Type::NoteOn, 60, 100), 300));
    r.call(1000); // heard at 700
    r.send(aged(msg(Type::NoteOn, 62, 100), 0));
    r.e.reset(); // nothing held, nothing waiting from before
    r.call();
    CHECK(r.L[1000 + 254] == 0.0f && r.L[1000 + 255] > 0.99f);
}

// ---- a module's own device (docs/midi.md): by name, which the application turns into a slot
TEST(midi_in_listens_to_its_own_device_only)
{
    MidiRig r;
    r.e.midi_slot_of = [](const std::string &name)
    {
        return name == "KeyStep" ? 2 : name == "Pads" ? 5 : -1;
    };
    CHECK(r.e.set_device(r.in, "KeyStep") == Err::Ok);
    CHECK(r.e.patch().find(r.in)->device == "KeyStep");
    r.send(msg(Type::NoteOn, 60, 100, 0, 5)); // another device
    CHECK(r.now() == 0.0f);
    r.send(msg(Type::NoteOn, 60, 100, 0, 2));
    CHECK(r.now() > 0.99f);
    r.send(msg(Type::NoteOff, 60, 0, 0, 5)); // the other device's note off is not ours
    CHECK(r.now() > 0.99f);
    // changing the device lets go of what is held: its note off would never be heard
    CHECK(r.e.set_device(r.in, "Pads") == Err::Ok);
    CHECK(r.now() == 0.0f);
    r.send(msg(Type::NoteOn, 62, 100, 0, 5));
    CHECK(r.now() > 0.99f);
    // a name with no slot on this machine: nothing is heard (and what was held is let go)
    CHECK(r.e.set_device(r.in, "Not here") == Err::Ok);
    CHECK(r.now() == 0.0f);
    for (uint8_t slot = 0; slot < kMaxInputs; slot++)
        r.send(msg(Type::NoteOn, 64, 100, 0, slot));
    CHECK(r.now() == 0.0f);
    // no name: every device again
    CHECK(r.e.set_device(r.in, "") == Err::Ok);
    r.send(msg(Type::NoteOn, 65, 100, 0, 7));
    CHECK(r.now() > 0.99f);
    CHECK(r.e.set_device(999, "KeyStep") == Err::NoSuchNode);
}

TEST(midi_a_device_set_in_the_patch_is_used_from_the_start_and_looked_up_again)
{
    // the patch names a device before the engine is compiled (a file being loaded)
    Rig            r;
    auto           boxes = std::make_unique<Inboxes>();
    const uint32_t cc = r.add("midi_cc"), out = r.add("out");
    r.e.set_midi(boxes.get());
    r.e.midi_slot_of = [](const std::string &name) { return name == "Knobs" ? 1 : -1; };
    r.e.patch().find(cc)->device = "Knobs";
    r.param(out, "level", 1);
    CHECK(r.cable(cc, "out", out, "l") == Err::Ok && r.compile());
    auto now = [&]
    {
        r.render(4800);
        return r.L.back();
    };
    (*boxes)[0].push(msg(Type::Control, 1, 127, 0, 0)); // not its device
    CHECK(now() == 0.0f);
    (*boxes)[1].push(msg(Type::Control, 1, 127, 0, 1));
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    // the names' slots change (the application's device manager started): looked up again; the
    // value stays
    r.e.midi_slot_of = [](const std::string &name) { return name == "Knobs" ? 4 : -1; };
    r.e.refresh_devices();
    (*boxes)[1].push(msg(Type::Control, 1, 0, 0, 1));
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    (*boxes)[4].push(msg(Type::Control, 1, 0, 0, 4));
    CHECK(std::fabs(now()) < 0.01f);
    // with nothing to turn names into slots, a named device is not heard; a module with none is
    r.e.midi_slot_of = nullptr;
    r.e.refresh_devices();
    (*boxes)[4].push(msg(Type::Control, 1, 127, 0, 4));
    CHECK(std::fabs(now()) < 0.01f);
    CHECK(r.e.set_device(cc, "") == Err::Ok);
    (*boxes)[4].push(msg(Type::Control, 1, 127, 0, 4));
    CHECK(std::fabs(now() - 1.0f) < 0.01f);
    // a module that takes no MIDI keeps no device
    CHECK(r.e.set_device(out, "Knobs") == Err::NoSuchParam);
    CHECK(r.e.patch().find(out)->device.empty());
}

TEST(settings_round_trip_and_unknown_lines)
{
    Settings s;
    s.output = "Built-in Audio Analog Stereo";
    s.midi_inputs = { "Arturia KeyStep 37", "nanoKONTROL2 #2" };
    const std::string t = rack::ui::settings_text(s);
    CHECK(t
        == "pulserack-settings 1\noutput Built-in Audio Analog Stereo\nmidi-input Arturia KeyStep "
           "37\nmidi-input nanoKONTROL2 #2\n");
    Settings                 back;
    std::vector<std::string> warnings;
    CHECK(rack::ui::parse_settings(t, &back, &warnings) && back == s && warnings.empty());

    // a newer file: unknown lines are skipped with a warning; CRLF and comments are fine
    CHECK(rack::ui::parse_settings(
        "pulserack-settings 2\r\n# hello\r\ncolour blue\r\nmidi-input A\r\n", &back, &warnings));
    CHECK(back.output.empty() && back.midi_inputs == std::vector<std::string> { "A" });
    CHECK(warnings.size() == 1 && warnings[0].find("'colour'") != std::string::npos);

    // not a settings file: refused, the result untouched
    back.output = "kept";
    CHECK(!rack::ui::parse_settings("pulserack 1\nnode 1 osc\n", &back, nullptr));
    CHECK(!rack::ui::parse_settings("", &back, nullptr));
    CHECK(back.output == "kept");

    // a name with a newline would break the file: it is not written
    Settings bad;
    bad.output = "a\nmidi-input b";
    CHECK(rack::ui::settings_text(bad) == "pulserack-settings 1\n");
}

TEST(settings_learn_lines)
{
    Settings s;
    s.learn = { { 0, 74, 2, "Arturia KeyStep 37 MIDI 1" }, { 15, 7, 0, "" } };
    const std::string t = rack::ui::settings_text(s);
    // channels and controls are written from 1; a device name keeps its spaces
    CHECK(t == "pulserack-settings 1\nlearn 1 74 3 Arturia KeyStep 37 MIDI 1\nlearn 16 7 1\n");
    Settings                 back;
    std::vector<std::string> warnings;
    CHECK(rack::ui::parse_settings(t, &back, &warnings) && back == s && warnings.empty());
    // lines that are not mappings are skipped with a warning
    CHECK(rack::ui::parse_settings("pulserack-settings 1\nlearn 0 74 1 x\nlearn 1 200 1 x\nlearn 1 "
                                   "74 0 x\nlearn a b c\nlearn 2 "
                                   "3 4 ok\n",
        &back, &warnings));
    CHECK(warnings.size() == 4);
    CHECK((back.learn == std::vector<rack::ui::Learn> { { 1, 3, 3, "ok" } }));
}

TEST(midi_control_changes_are_passed_on_for_midi_learn)
{
    MidiRig r("mod");
    Msg     m;
    CHECK(!r.e.take_control(m));
    r.send(msg(Type::NoteOn, 60, 100)); // not a control
    r.send(msg(Type::Control, 74, 99, 2, 4));
    r.send(msg(Type::Control, 1, 127));
    // with no audio thread, taking them also feeds the modules: the mod wheel has moved
    CHECK(r.e.take_control(m) && m.type == Type::Control && m.data == 1 && m.value == 127);
    CHECK(r.e.take_control(m) && m.data == 74 && m.value == 99 && m.channel == 2 && m.slot == 4);
    CHECK(!r.e.take_control(m));
    CHECK(std::fabs(r.now(4800) - 1.0f) < 0.01f);
    // rendering passes them on too, and more than the ring holds are dropped, not blocked on
    for (int i = 0; i < 1500; i++)
    {
        r.send(msg(Type::Control, 20, (uint16_t)(i % 128)));
        if (i % 100 == 99)
            r.render(64);
    }
    r.render(64);
    int n = 0;
    while (r.e.take_control(m))
        n++;
    CHECK(n == 1024);
}

TEST(settings_on_disk)
{
    const fs::path d = fs::temp_directory_path() / "pulserack_test_settings";
    fs::remove_all(d);
    std::string              err;
    std::vector<std::string> warnings;
    // missing: the defaults, no error
    CHECK(rack::ui::load_settings(d.string(), &warnings, &err) == Settings { } && err.empty());
    Settings s;
    s.output = "USB Audio";
    s.midi_inputs = { "KeyStep" };
    CHECK(rack::ui::save_settings(d.string(), s, &err)); // makes the directory
    CHECK(rack::ui::load_settings(d.string(), &warnings, &err) == s && err.empty());
    // an empty dir: nothing is read or written
    CHECK(rack::ui::save_settings("", s, &err));
    CHECK(rack::ui::load_settings("", &warnings, &err) == Settings { });
    CHECK(rack::ui::settings_path("").empty());
    fs::remove_all(d);
}

TEST(settings_default_config_dir_follows_xdg)
{
    const char       *old = std::getenv("XDG_CONFIG_HOME");
    const std::string keep = old ? old : "";
    setenv("XDG_CONFIG_HOME", "/tmp/xdg-cfg", 1);
    CHECK(rack::ui::default_config_dir() == "/tmp/xdg-cfg/pulserack");
    if (old)
        setenv("XDG_CONFIG_HOME", keep.c_str(), 1);
    else
        unsetenv("XDG_CONFIG_HOME");
}

// ---- MIDI Clock (step 9): the module alone, its messages pushed at chosen samples, its events read
// back with their engine time
namespace
{
struct ClockRig
{
    const ModuleDesc &d = *find_module("midi_clock");
    std::vector<unsigned char> mem;
    std::vector<pcl_event_t>   buf = std::vector<pcl_event_t>(64);
    EventBuf                   out;
    uint64_t                   time = 0;
    std::vector<std::pair<uint64_t, uint8_t>> got; // (sample, type)
    explicit ClockRig(float ppqn = 24, float follow = 1)
        : mem(d.state_size + d.state_align)
    {
        const float p[2] = { ppqn, follow };
        CHECK(d.init(state(), NodeContext { 48000 }, p));
    }
    void *state()
    {
        void  *s = mem.data();
        size_t n = mem.size();
        return std::align(d.state_align, d.state_size, s, n);
    }
    void push(Type t, uint64_t at) { midi_sink(d)(state(), msg(t), at); }
    void run(uint64_t frames, size_t block = 256)
    {
        static const pcl_sample_t zeros[PCL_MAX_BLOCK] = { };
        static pcl_sample_t       scratch[PCL_MAX_BLOCK];
        EventBuf                 *outs[1] = { &out };
        for (uint64_t done = 0; done < frames;)
        {
            const size_t n = (size_t)std::min<uint64_t>(block, frames - done);
            out = EventBuf { buf.data(), buf.size(), 0 };
            NodeIo io { { }, { }, { }, outs, scratch, zeros, { nullptr, nullptr }, time, n };
            d.process(state(), io);
            for (size_t i = 0; i < out.count; i++)
            {
                CHECK(out.data[i].offset < n && (i == 0 || out.data[i].offset >= out.data[i - 1].offset));
                got.push_back({ time + out.data[i].offset, out.data[i].type });
            }
            time += n;
            done += n;
        }
    }
    static std::pair<uint64_t, uint8_t> ev(uint64_t at, uint8_t type) { return { at, type }; }
    std::vector<uint64_t> pulses() const
    {
        std::vector<uint64_t> v;
        for (auto [at, type] : got)
            if (type == PCL_EVT_CLOCK)
                v.push_back(at);
        return v;
    }
};
} // namespace

TEST(midi_clock_passes_ticks_on_between_start_and_stop)
{
    ClockRig c; // 24 ppqn, following the transport
    c.push(Type::Clock, 100); // before a start: not passed on
    c.push(Type::Start, 1000);
    for (uint64_t t = 1000; t < 3000; t += 250) // a tick every 250 samples: 192 bpm at 48 kHz
        c.push(Type::Clock, t);
    c.push(Type::Stop, 3100);
    c.push(Type::Clock, 3250); // after the stop: not passed on
    c.run(4000);
    CHECK(c.got.size() == 10 && c.got.front() == ClockRig::ev(1000, PCL_EVT_START));
    CHECK(c.got[1] == ClockRig::ev(1000, PCL_EVT_CLOCK)); // START before CLOCK
    CHECK(c.got.back() == ClockRig::ev(3100, PCL_EVT_STOP));
    const auto p = c.pulses();
    CHECK(p.size() == 8 && p[0] == 1000 && p[7] == 2750); // each on its own sample, across blocks
    // continue: pulses again, without a start
    c.got.clear();
    c.push(Type::Continue, 4100);
    c.push(Type::Clock, 4200);
    c.run(500);
    CHECK(c.got.size() == 1 && c.got[0] == ClockRig::ev(4200, PCL_EVT_CLOCK));
}

TEST(midi_clock_at_other_pulses_per_beat)
{
    { // 12: every other tick, the first one after a start included
        ClockRig c(12);
        c.push(Type::Start, 0);
        for (uint64_t t = 0; t < 2400; t += 200)
            c.push(Type::Clock, t);
        c.run(3000);
        CHECK((c.pulses() == std::vector<uint64_t> { 0, 400, 800, 1200, 1600, 2000 }));
    }
    { // 96: four a tick, spread over the last tick's time (the first tick has none: all at once)
        ClockRig c(96);
        c.push(Type::Start, 0);
        for (uint64_t t = 0; t < 1200; t += 400)
            c.push(Type::Clock, t);
        c.run(2000, 128); // (blocks shorter than a tick: the spread pulses wait across them)
        CHECK((c.pulses()
            == std::vector<uint64_t> { 0, 0, 0, 0, 400, 500, 600, 700, 800, 900, 1000, 1100 }));
    }
    { // 96, and the tempo jumps up: the pulses still owed go out at the next tick, none lost
        ClockRig c(96);
        c.push(Type::Start, 0);
        c.push(Type::Clock, 0);
        c.push(Type::Clock, 400);
        c.push(Type::Clock, 500); // much sooner
        c.run(2000);
        const auto p = c.pulses();
        CHECK(p.size() == 12 && p[4] == 400 && p[5] == 500 && p[6] == 500 && p[7] == 500);
        CHECK(std::is_sorted(p.begin(), p.end()));
    }
}

TEST(midi_clock_without_start_stop_and_a_lost_device)
{
    { // Start/stop off: every tick is a pulse, the transport is not passed on
        ClockRig c(24, 0);
        c.push(Type::Clock, 10);
        c.push(Type::Start, 20);
        c.push(Type::Clock, 30);
        c.push(Type::Stop, 40);
        c.push(Type::Clock, 50);
        c.run(256);
        CHECK(c.got.size() == 3 && c.pulses() == (std::vector<uint64_t> { 10, 30, 50 }));
    }
    { // following: its device going away stops it, as a stop would
        ClockRig c;
        c.push(Type::Start, 0);
        c.push(Type::Clock, 0);
        c.push(Type::DeviceLost, 100);
        c.push(Type::Clock, 200);
        c.run(256);
        CHECK(c.got.size() == 3 && c.got[2] == ClockRig::ev(100, PCL_EVT_STOP));
    }
    { // notes and controls are not for it; another device's ticks neither
        ClockRig c(24, 0);
        CHECK(midi_set_device(c.d, c.state(), 3));
        c.push(Type::NoteOn, 10);
        c.push(Type::Control, 20);
        c.push(Type::Clock, 30); // slot 0
        Msg m = msg(Type::Clock, 0, 0, 0, 3);
        midi_sink(c.d)(c.state(), m, 40);
        c.run(256);
        CHECK(c.pulses() == std::vector<uint64_t> { 40 });
    }
}

TEST(midi_clock_drives_a_sequencer_in_a_patch)
{
    // MIDI Clock -> Sequencer (a step every 6 pulses) -> Note to Gate: a start and 24 ticks (a beat)
    // are four steps, through the engine's inboxes
    Rig                      r;
    std::unique_ptr<Inboxes> boxes = std::make_unique<Inboxes>();
    r.e.set_midi(boxes.get());
    const uint32_t mc = r.add("midi_clock"), seq = r.add("seq"), g = r.add("note_to_gate"),
                   out = r.add("out");
    r.param(seq, "division", 6);
    r.param(seq, "gate", 3);
    r.param(out, "level", 1);
    CHECK(r.cable(mc, "clock", seq, "in") == Err::Ok && r.cable(seq, "out", g, "notes") == Err::Ok
        && r.cable(g, "gate", out, "l") == Err::Ok);
    for (size_t i = 0; i < 8; i++)
        CHECK(r.e.set_step(seq, i, StepData { 60, 100, 0, 0 }) == Err::Ok);
    CHECK(r.compile());
    (*boxes)[0].push(msg(Type::Start));
    int rises = 0;
    float prev = 0;
    for (int tick = 0; tick < 24; tick++)
    {
        (*boxes)[0].push(msg(Type::Clock));
        r.render(256);
        for (size_t i = r.L.size() - 256; i < r.L.size(); i++)
        {
            rises += prev < 0.5f && r.L[i] >= 0.5f;
            prev = r.L[i];
        }
    }
    CHECK(rises == 4);
}

// ---- MIDI Out (step 9): what the module sends reaches the outbox, as bytes, on its output's slot
namespace
{
struct OutRig : Rig
{
    std::unique_ptr<midi::Outbox> box = std::make_unique<midi::Outbox>();
    uint32_t                      kb = 0, mo = 0;
    OutRig()
    {
        e.set_midi_out(box.get());
        e.midi_out_slot_of = [](const std::string &n) { return n == "Synth" ? 2 : n == "Drums" ? 5 : -1; };
        kb = add("keyboard");
        mo = add("midi_out");
        CHECK(cable(kb, "notes", mo, "notes") == Err::Ok);
    }
    std::vector<midi::OutMsg> take()
    {
        std::vector<midi::OutMsg> v;
        midi::OutMsg              m;
        while (box->ring.pop(m))
            v.push_back(m);
        return v;
    }
    static bool is(const midi::OutMsg &m, uint8_t slot, std::initializer_list<uint8_t> bytes)
    {
        if (m.slot != slot || m.len != bytes.size())
            return false;
        size_t i = 0;
        for (uint8_t b : bytes)
            if (m.bytes[i++] != b)
                return false;
        return true;
    }
};
} // namespace

TEST(midi_out_sends_notes_on_its_channel_to_its_output)
{
    OutRig r;
    r.param(r.mo, "channel", 9); // (the 10th: drums)
    CHECK(r.compile() && r.e.set_device(r.mo, "Synth") == Err::Ok);
    CHECK(r.e.key(r.kb, true, 60, 100) == Err::Ok);
    r.render(256);
    CHECK(r.e.key(r.kb, false, 60) == Err::Ok);
    r.render(256);
    const auto v = r.take();
    CHECK(v.size() == 2 && OutRig::is(v[0], 2, { 0x99, 60, 100 }) && OutRig::is(v[1], 2, { 0x89, 60, 0 }));
    CHECK(v[0].due_ns == 0); // no audio thread: at once
    // a note off for a note it did not send is not passed on
    CHECK(r.e.key(r.kb, false, 61) == Err::Ok);
    r.render(256);
    CHECK(r.take().empty());
}

TEST(midi_out_lets_go_of_its_notes_on_a_change_of_channel_or_output)
{
    OutRig r;
    CHECK(r.compile() && r.e.set_device(r.mo, "Synth") == Err::Ok);
    CHECK(r.e.key(r.kb, true, 64, 90) == Err::Ok);
    r.render(256);
    r.take();
    r.param(r.mo, "channel", 3); // the held note: off on channel 1, where it was sent
    r.render(256);
    auto v = r.take();
    CHECK(v.size() == 1 && OutRig::is(v[0], 2, { 0x80, 64, 0 }));
    CHECK(r.e.key(r.kb, true, 65, 90) == Err::Ok);
    r.render(256);
    r.take();
    CHECK(r.e.set_device(r.mo, "Drums") == Err::Ok); // off to the output it was sent to
    r.render(256);
    v = r.take();
    CHECK(v.size() == 1 && OutRig::is(v[0], 2, { 0x83, 65, 0 }));
    CHECK(r.e.key(r.kb, true, 66, 90) == Err::Ok);
    r.render(256);
    v = r.take();
    CHECK(v.size() == 1 && OutRig::is(v[0], 5, { 0x93, 66, 90 }));
    // no output: nothing is sent
    CHECK(r.e.set_device(r.mo, "") == Err::Ok);
    r.render(256);
    r.take(); // (the note off for 66, to Drums)
    CHECK(r.e.key(r.kb, true, 67, 90) == Err::Ok);
    r.render(256);
    CHECK(r.take().empty());
    CHECK(r.e.set_device(r.mo, "Nowhere") == Err::Ok); // a name with no slot: none either
    CHECK(r.e.key(r.kb, true, 68, 90) == Err::Ok);
    r.render(256);
    CHECK(r.take().empty());
}

TEST(midi_out_sends_the_clock_start_and_stop)
{
    OutRig         r;
    const uint32_t clk = r.add("clock");
    r.param(clk, "bpm", 120);
    r.param(clk, "ppqn", 24);
    CHECK(r.cable(clk, "clock", r.mo, "clock") == Err::Ok && r.compile()
        && r.e.set_device(r.mo, "Synth") == Err::Ok);
    CHECK(r.e.key(r.kb, true, 60, 100) == Err::Ok);
    r.render(kRate / 2); // half a second at 120 bpm: one beat, 24 pulses (the 25th is at 0.5 s)
    auto   v = r.take();
    size_t ticks = 0, starts = 0;
    for (const auto &m : v)
    {
        ticks += OutRig::is(m, 2, { 0xF8 });
        starts += OutRig::is(m, 2, { 0xFA });
    }
    CHECK(starts == 1 && OutRig::is(v[0], 2, { 0xFA }) && ticks == 24);
    r.param(clk, "run", 0); // a stop: FC, and the held note let go
    r.render(256);
    v = r.take();
    CHECK(v.size() == 2 && OutRig::is(v[0], 2, { 0xFC }) && OutRig::is(v[1], 2, { 0x80, 60, 0 }));
}

TEST(midi_out_messages_are_due_a_device_call_after_their_sample)
{
    OutRig r;
    CHECK(r.compile() && r.e.set_device(r.mo, "Synth") == Err::Ok);
    g_clock_ns = kStart;
    r.e.set_midi_clock(fake_clock);
    r.e.attach_audio(true); // this thread is the audio thread now
    r.e.key(r.kb, true, 60, 100, 300); // at sample 300: the second block of a call of 512
    r.render(512);
    r.e.attach_audio(false);
    const auto v = r.take();
    CHECK(v.size() == 1);
    if (v.size() == 1)
    {
        const double want = (double)kStart + (300.0 + 512.0) * 1e9 / kRate;
        CHECK(std::fabs((double)v[0].due_ns - want) < 1000); // within a microsecond
    }
}
