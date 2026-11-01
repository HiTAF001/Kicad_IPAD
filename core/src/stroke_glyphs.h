/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Stroke font glyph table (generated into stroke_glyphs.cpp by tools/gen_stroke_font.py).
 */
#ifndef KIPAD_STROKE_GLYPHS_H
#define KIPAD_STROKE_GLYPHS_H

#include <cstdint>

namespace kp
{

struct StrokeGlyphData
{
    uint32_t    codePoint;
    const char* data; ///< Hershey .jhf encoding: bearings, then 'R'-relative pairs, " R" = pen up
};

/// Sorted by code point.
extern const StrokeGlyphData g_strokeGlyphs[];
extern const int             g_strokeGlyphCount;

} // namespace kp

#endif
