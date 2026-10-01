#pragma once

#include <cstdint>

// Visual fallback stand-in for a codepoint that the active font does not have a glyph for.
// Returns `cp` unchanged when there is no sensible stand-in.
//
// Applies ONLY when the font has been probed and confirmed to lack a glyph for `cp`.
// Used for IPA phonetics, Spacing Modifier Letters, polytonic Greek, and dictionary symbols.
uint32_t fallbackGlyphCodepoint(uint32_t cp);
