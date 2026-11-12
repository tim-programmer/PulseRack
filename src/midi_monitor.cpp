// pulserack-midi: lists the MIDI inputs, or opens some and prints what they send, through the same
// code the rack uses (src/midi). For checking a controller without the window.
//
//   pulserack-midi --list       the inputs, by the names the settings file uses
//   pulserack-midi [name...]    print what these inputs send (none: every input); Ctrl-C stops
#include "midi/inputs.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
std::atomic<bool> g_stop { false };

const char *type_name(rack::midi::Type t)
{
    using rack::midi::Type;
    switch (t)
    {
    case Type::NoteOn:
        return "note on";
    case Type::NoteOff:
        return "note off";
    case Type::Control:
        return "control";
    case Type::PitchBend:
        return "pitch bend";
    case Type::ChannelPressure:
        return "pressure";
    case Type::KeyPressure:
        return "key pressure";
    case Type::Program:
        return "program";
    case Type::Clock:
        return "clock";
    case Type::Start:
        return "start";
    case Type::Continue:
        return "continue";
    case Type::Stop:
        return "stop";
    case Type::DeviceLost:
        return "device lost";
    }
    return "?";
}
} // namespace

int main(int argc, char **argv)
{
    using namespace rack::midi;
    bool                     list = false;
    std::vector<std::string> names;
    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help"))
        {
            std::printf("usage: pulserack-midi --list | pulserack-midi [input name...]\n");
            return 0;
        }
        if (!std::strcmp(argv[i], "--list"))
            list = true;
        else
            names.push_back(argv[i]);
    }

    static Inboxes inboxes; // large, and the devices' threads push into it until `in` is gone
    Inputs         in(inboxes);
    std::string    err;
    if (!in.start(&err))
    {
        std::fprintf(stderr, "pulserack-midi: %s\n", err.c_str());
        return 1;
    }
    const std::vector<std::string> avail = in.available();
    if (list)
    {
        std::printf("MIDI inputs (%s):\n", in.api_name().c_str());
        for (const std::string &n : avail)
            std::printf("  %s\n", n.c_str());
        if (avail.empty())
            std::printf("  (none)\n");
        return 0;
    }
    if (names.empty())
        names = avail;
    if (names.size() > kMaxInputs)
        names.resize(kMaxInputs);
    std::vector<std::string> errors;
    in.set_enabled(names, &errors);
    for (const std::string &e : errors)
        std::fprintf(stderr, "pulserack-midi: %s\n", e.c_str());
    for (const std::string &n : names)
        std::printf("%s %s\n", in.is_open(n) ? "listening to" : "waiting for", n.c_str());
    std::fflush(stdout);

    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    uint32_t lost_seen[kMaxInputs] = { };
    while (!g_stop)
    {
        errors.clear();
        if (in.poll(&errors))
            for (const std::string &n : names)
                std::printf("-- %s: %s\n", n.c_str(), in.is_open(n) ? "open" : "closed");
        for (const std::string &e : errors)
            std::fprintf(stderr, "pulserack-midi: %s\n", e.c_str());
        for (size_t s = 0; s < kMaxInputs; s++)
        {
            Msg m;
            while (inboxes[s].ring.pop(m))
            {
                std::printf("[%zu] %-12s", s, type_name(m.type));
                if (m.type < Type::Clock)
                    std::printf(" ch %2d  %3d  %5d", m.channel + 1, m.data, m.value);
                std::printf("\n");
            }
            const uint32_t lost = inboxes[s].lost.load();
            if (lost != lost_seen[s])
                std::printf("[%zu] %u messages lost\n", s, lost - lost_seen[s]),
                    lost_seen[s] = lost;
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}
