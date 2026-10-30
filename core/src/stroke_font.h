/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * KiCad stroke font: glyph decoding and text layout following KiCad's KIFONT::STROKE_FONT /
 * KIFONT::FONT behaviour, so text lands where KiCad draws it.
 */
#ifndef KIPAD_STROKE_FONT_H
#define KIPAD_STROKE_FONT_H

#include "board.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kp
{

struct StrokeGlyph
{
    double            width = 0.0; ///< advance in font units (multiply by glyph width)
    std::vector<Path> strokes;     ///< polylines in font units, y down, baseline at 0
};

struct TextLayout
{
    Pt     pos;              ///< anchor
    double sizeX = 1.0;
    double sizeY = 1.0;
    double thickness = 0.15;
    double angle = 0.0;      ///< degrees, KiCad sense
    double lineSpacing = 1.0;
    bool   mirror = false;
    bool   italic = false;
    HAlign hAlign = HAlign::CENTER;
    VAlign vAlign = VAlign::CENTER;
};

class StrokeFont
{
public:
    static const StrokeFont& Get();

    /// Lay out (possibly multi-line, KiCad-markup) UTF-8 text into stroke polylines in board
    /// coordinates. Each polyline should be drawn with the layout thickness.
    void Layout( const std::string& aUtf8, const TextLayout& aLayout,
                 std::vector<Path>& aStrokes ) const;

private:
    StrokeFont();

    const StrokeGlyph& glyph( uint32_t aCodePoint ) const;
    double             spaceWidth() const;

    struct Run
    {
        std::u32string text;
        int            style = 0; ///< bit0 overbar, bit1 subscript, bit2 superscript
    };

    void   parseMarkup( const std::u32string& aLine, std::vector<Run>& aRuns ) const;
    double drawRuns( const std::vector<Run>& aRuns, const Pt& aStart, const TextLayout& aL,
                     std::vector<Path>* aOut ) const;

    mutable std::mutex                              m_mutex;
    mutable std::unordered_map<uint32_t, std::unique_ptr<StrokeGlyph>> m_cache;
};

/// Decode UTF-8 into UTF-32 (invalid bytes become U+FFFD).
std::u32string DecodeUtf8( const std::string& aText );

} // namespace kp

#endif
