// The machine's settings (docs/midi.md, *Settings file*): what belongs to this computer, not to a
// patch. $XDG_CONFIG_HOME/pulserack/settings, line-based like a patch file:
//
//   pulserack-settings 1
//   output <name>         the sound card (missing: the system's default)
//   midi-input <name>     a MIDI input that is switched on, one line each
//   learn <channel> <controller> <control> <device name>
//                         MIDI learn: this controller (channel 1..16, number 0..119) of this device
//                         turns control number <control> (from 1) of the selected module
//
// No window here, so the headless tests use it.
#pragma once

#include <string>
#include <vector>

namespace rack::ui
{

// One MIDI learn mapping (docs/midi.md, *MIDI learn*).
struct Learn
{
    int channel = 0;    // 0..15
    int cc = 0;         // the controller number
    int control = 0;    // which control of the selected module, from 0, in the descriptor's order
    std::string device; // the input's name (empty: one with no name, as in the tests)

    bool operator==(const Learn &) const = default;
};

struct Settings
{
    std::string              output;      // empty: the system's default
    std::vector<std::string> midi_inputs; // switched on, by name
    std::vector<Learn>       learn;       // the MIDI learn mappings

    bool operator==(const Settings &) const = default;
};

std::string settings_text(const Settings &s);
// False (and `out` untouched) if `text` is not a settings file at all. Lines it does not know (a
// newer PulseRack's) are skipped with a warning each.
bool parse_settings(const std::string &text, Settings *out, std::vector<std::string> *warnings);

// $XDG_CONFIG_HOME/pulserack, or ~/.config/pulserack (%APPDATA%/PulseRack on Windows); empty if neither can be found.
std::string default_config_dir();
std::string settings_path(const std::string &config_dir); // empty for an empty dir

// A missing file is not an error: the defaults. `error` is set only for a file that exists but
// cannot be read or is not a settings file.
Settings load_settings(
    const std::string &config_dir, std::vector<std::string> *warnings, std::string *error);
// Creates the directory if needed. With an empty config_dir, does nothing and succeeds.
bool save_settings(const std::string &config_dir, const Settings &s, std::string *error);

} // namespace rack::ui
