// MIDI without a device (docs/midi.md): decoding messages, unique device names, the inboxes, and
// the settings file.
#include "testutil.h"

#include "midi/inbox.h"
#include "midi/message.h"
#include "midi/names.h"
#include "ui/settings.h"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
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
        "pulserack-settings 2\r\n# hello\r\nlearn 1 2 3\r\nmidi-input A\r\n", &back, &warnings));
    CHECK(back.output.empty() && back.midi_inputs == std::vector<std::string> { "A" });
    CHECK(warnings.size() == 1 && warnings[0].find("'learn'") != std::string::npos);

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
