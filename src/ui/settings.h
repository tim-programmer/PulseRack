// The machine's settings (docs/midi.md, *Settings file*): what belongs to this computer, not to a
// patch. $XDG_CONFIG_HOME/pulserack/settings, line-based like a patch file:
//
//   pulserack-settings 1
//   output <name>         the sound card (missing: the system's default)
//   midi-input <name>     a MIDI input that is switched on, one line each
//
// No window here, so the headless tests use it.
#pragma once

#include <string>
#include <vector>

namespace rack::ui
{

struct Settings
{
    std::string              output;      // empty: the system's default
    std::vector<std::string> midi_inputs; // switched on, by name

    bool operator==(const Settings &) const = default;
};

std::string settings_text(const Settings &s);
// False (and `out` untouched) if `text` is not a settings file at all. Lines it does not know (a
// newer PulseRack's) are skipped with a warning each.
bool parse_settings(const std::string &text, Settings *out, std::vector<std::string> *warnings);

// $XDG_CONFIG_HOME/pulserack, or ~/.config/pulserack; empty if neither can be found.
std::string default_config_dir();
std::string settings_path(const std::string &config_dir); // empty for an empty dir

// A missing file is not an error: the defaults. `error` is set only for a file that exists but
// cannot be read or is not a settings file.
Settings load_settings(
    const std::string &config_dir, std::vector<std::string> *warnings, std::string *error);
// Creates the directory if needed. With an empty config_dir, does nothing and succeeds.
bool save_settings(const std::string &config_dir, const Settings &s, std::string *error);

} // namespace rack::ui
