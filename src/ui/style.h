// The look: colours and the fixed sizes of the window's chrome (docs/gui.md: a restrained dark
// theme, flat category colour with a subtle gradient). The smoke test uses the sizes to find
// things.
#pragma once

#include <gpuic/gpuic.hpp>

#include <string_view>

namespace rack::ui::style
{

using gpuic::Color;
using gpuic::rgb;

// ---- chrome
inline constexpr float kWindowW = 1400, kWindowH = 900;
inline constexpr float kToolbarH = 36;
inline constexpr float kHintH = 26;
inline constexpr float kBrowserW = 280;
inline constexpr float kBrowserPad = 10;
inline constexpr float kSearchH = 32; // the search box
inline constexpr float kChipsH = 56;  // the category chips (two rows) and the gap under them
inline constexpr float kCardH = 74;   // one module card
inline constexpr float kCardGap = 6;
inline constexpr float kAddW = 46, kAddH = 24; // a card's Add button (top right of the card)

// ---- colours
inline const Color kBase = rgb(0x15161b);
inline const Color kRack = rgb(0x1b1c22);
inline const Color kRackLine = rgb(0x22232b);
inline const Color kSurface = rgb(0x23252e);
inline const Color kSurfaceHi = rgb(0x2c2f3a);
inline const Color kBorder = rgb(0x30333f);
inline const Color kText = rgb(0xe4e5ee);
inline const Color kSubtext = rgb(0x9aa0b4);
inline const Color kDim = rgb(0x6b7085);
inline const Color kAccent = rgb(0x89b4fa);
inline const Color kAccentHi = rgb(0xa6c4fb);
inline const Color kLedOn = rgb(0xa6e3a1);
inline const Color kLedOff = rgb(0x2a2c35);
inline const Color kKnobFace = rgb(0x24262f);
inline const Color kKnobEdge = rgb(0x0f1014);
inline const Color kKnobPointer = rgb(0xf2f2f7);
inline const Color kWell = rgb(0x111216); // a slider's track, a selector's box, a jack's hole
inline const Color kDanger = rgb(0xf38ba8);

struct CategoryColors
{
    Color top, bottom, title, chip;
};
// Sources green, modulators red, filters blue, event modules teal, utilities grey.
inline CategoryColors category(std::string_view c)
{
    if (c == "Source")
        return { rgb(0x3d6e49), rgb(0x2e5638), rgb(0x264a30), rgb(0xa6e3a1) };
    if (c == "Modulator")
        return { rgb(0x784245), rgb(0x5e3335), rgb(0x4f2a2c), rgb(0xf38ba8) };
    if (c == "Filter")
        return { rgb(0x3d5a86), rgb(0x2f476b), rgb(0x283c5b), rgb(0x89b4fa) };
    if (c == "Event")
        return { rgb(0x346e70), rgb(0x285658), rgb(0x22494b), rgb(0x94e2d5) };
    return { rgb(0x535764), rgb(0x40434e), rgb(0x373a43), rgb(0xbac2de) };
}

} // namespace rack::ui::style
