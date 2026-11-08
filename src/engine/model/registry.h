// The table of module types. A type is found by the id the patch file uses; ids never change.
#pragma once

#include "engine/model/descriptor.h"

namespace rack
{

std::span<const ModuleDesc *const> all_modules();
const ModuleDesc                  *find_module(std::string_view id); // nullptr if unknown
int find_port(const ModuleDesc &d, std::string_view id);             // index into d.ports, or -1
int find_param(const ModuleDesc &d, std::string_view id);            // index into d.params, or -1

} // namespace rack
