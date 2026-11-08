// The patch file: a text file, one record per line, no third-party parser (docs/patch-engine.md).
//
//   pulserack 1
//   node 1 clock x=12 y=0
//   param 1 bpm 120                  (a number; an enumerated choice by its name, a space written
//   as _) step 2 0 note=48 vel=100 gate=0 tie=0 key 5 0.5 on 60 100              (a scripted key
//   press: node, seconds, on|off, note, velocity) cable 1.clock -> 2.in            (output ->
//   input, by the jacks' ids)
//
// A file with a syntax error, an unknown version, or a cable the rules forbid is rejected whole. An
// unknown module type, jack or control is skipped with a warning, so a patch from a newer rack
// still opens.
#pragma once

#include "engine/model/patch.h"

#include <string>
#include <string_view>
#include <vector>

namespace rack
{

inline constexpr int    kPatchFileVersion = 1;
inline constexpr size_t kMaxNodes = 1024, kMaxCables = 4096, kMaxKeys = 100000, kMaxLine = 512;

struct LoadResult
{
    bool                     ok = false;
    std::string              error; // "line 7: ..." when !ok
    std::vector<std::string> warnings;
};

LoadResult load_patch(
    std::string_view text, Patch &out); // `out` is replaced only if the file loads
std::string save_patch(const Patch &patch);

} // namespace rack
