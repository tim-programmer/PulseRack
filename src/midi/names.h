// Device names (docs/midi.md, *Device names*): a device is chosen by name, and identical devices
// are told apart by their order: "nanoKONTROL2", "nanoKONTROL2 #2". No libremidi here.
#pragma once

#include <span>
#include <string>
#include <vector>

namespace rack::midi
{

// The names as the system lists them, made unique: the second of a name gets " #2", the third
// " #3". An empty name becomes "MIDI input".
std::vector<std::string> unique_names(std::span<const std::string> raw);

} // namespace rack::midi
