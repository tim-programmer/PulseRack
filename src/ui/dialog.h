// The native open and save dialogs, for Editor::chooser.
#pragma once

#include "ui/rack_ui.h"

namespace rack::ui
{

// Shows the dialog; the answer comes to editor->file_chosen on the main thread, later.
void native_file_dialog(
    gpuic::Ctx cx, gpuic::Entity<Editor> editor, bool save, const std::string &suggested);
// The open dialog for a Sampler's WAV file; the answer comes to editor->sample_chosen.
void native_sample_dialog(
    gpuic::Ctx cx, gpuic::Entity<Editor> editor, uint32_t node, const std::string &suggested);

} // namespace rack::ui
