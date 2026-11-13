// The MIDI output devices (docs/midi.md, step 9): lists them, opens the ones the patch's MIDI Out
// modules name (when they are present), follows hot-plugging, and sends what waits in the Outbox
// at the time each message is due, from a thread of its own. outputs.cpp is, with inputs.cpp, the
// only file that includes libremidi.
//
// The calls are from one thread (the UI's). The sender thread only reads the outbox and sends.
#pragma once

#include "midi/outbox.h"

#include <memory>
#include <string>
#include <vector>

namespace rack::midi
{

class Outputs
{
public:
    explicit Outputs(Outbox &outbox);
    ~Outputs(); // stops the sender, sends "all notes off" to every open output, closes them
    Outputs(const Outputs &) = delete;
    Outputs &operator=(const Outputs &) = delete;

    // Connects to the system's MIDI and starts the sender. False if there is none (`error`).
    bool start(std::string *error);
    bool started() const;

    // The outputs present now, by unique name (as the inputs are named).
    std::vector<std::string> available() const;
    // A name's slot, given a free one if it has none (-1: all kMaxOutputs taken, or no name). A
    // name keeps its slot for the run; its device is opened now if it is present, else when it
    // appears.
    int         slot_for(const std::string &name);
    int         slot_of(const std::string &name) const;
    std::string name_of(int slot) const;
    bool        is_open(const std::string &name) const;

    // Follows hot-plugging; call it often. True if an output opened or closed.
    bool poll(std::vector<std::string> *errors);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rack::midi
