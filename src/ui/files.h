// Patch files on disk, without any window: reading, writing safely, the factory patches that are
// built into the program, and where an unsaved patch is kept when the window closes.
#pragma once

#include <span>
#include <string>
#include <string_view>

namespace rack::ui
{

// The patches in examples/, compiled in (so they need no path at run time). `name` is the file's
// ("sequence"), `title` is for a menu ("Sequence"), `about` is the file's first comment line.
struct FactoryPatch
{
    std::string_view name, title, about, text;
};
std::span<const FactoryPatch> factory_patches();

bool read_file(const std::string &path, std::string *out, std::string *error);
// Writes `text` to `path` through a temporary file in the same directory and a rename, so that a
// failure (a full disk, a crash) never leaves a half-written patch where the old one was.
bool write_file_atomic(const std::string &path, const std::string &text, std::string *error);

// $XDG_STATE_HOME/pulserack, or ~/.local/state/pulserack; empty if neither can be found.
std::string default_state_dir();
// The patch kept when the window closed with unsaved changes (in a state directory).
std::string recovery_path(const std::string &state_dir);

std::string file_name(const std::string &path); // the last part of a path

} // namespace rack::ui
