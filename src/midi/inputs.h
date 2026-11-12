// The MIDI input devices (docs/midi.md, *Threads*): lists them, opens the ones switched on, follows
// hot-plugging, and has each open device push its messages into its slot's Inbox. The only file in
// PulseRack that includes libremidi is inputs.cpp.
//
// Everything here is called from one thread (the UI's). Messages are pushed from the devices' own
// threads; a device that goes away gets a DeviceLost message in its inbox.
#pragma once

#include "midi/inbox.h"

#include <memory>
#include <string>
#include <vector>

namespace rack::midi
{

class Inputs
{
public:
    explicit Inputs(Inboxes &inboxes);
    ~Inputs(); // closes every device
    Inputs(const Inputs &) = delete;
    Inputs &operator=(const Inputs &) = delete;

    // Connects to the system's MIDI (ALSA on Linux) and starts watching for devices. False if there
    // is none (`error` says why); the other calls then do nothing.
    bool        start(std::string *error);
    bool        started() const;
    std::string api_name() const; // "ALSA (sequencer)"..., empty before start()

    // The inputs present now, by unique name (docs/midi.md, *Device names*).
    std::vector<std::string> available() const;

    // The inputs switched on (the settings' midi-input lines). Opens those present, closes the
    // others; one that appears later is opened by poll(). Names past kMaxInputs distinct devices
    // for this run are refused (`errors`).
    void set_enabled(const std::vector<std::string> &names, std::vector<std::string> *errors);
    const std::vector<std::string> &enabled() const;

    // Follows hot-plugging; call it often (every UI tick). True if a device opened or closed.
    bool poll(std::vector<std::string> *errors);

    bool is_open(const std::string &name) const;
    // A name's slot (its inbox), -1 if it has none yet. A name keeps its slot for the whole run.
    int slot_of(const std::string &name) const;
    // The same, but a name with no slot is given a free one (so a patch can name a device that is
    // not plugged in, and hear it when it is); -1 when all kMaxInputs are taken, or for no name.
    int slot_for(const std::string &name);
    // The name a slot belongs to, empty if it has none.
    std::string name_of(int slot) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rack::midi
