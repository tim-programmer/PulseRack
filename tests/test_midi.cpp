// MIDI without a device (docs/midi.md): decoding messages, unique device names, the inboxes, and
// the settings file.
#include "testutil.h"

#include "midi/inbox.h"
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
