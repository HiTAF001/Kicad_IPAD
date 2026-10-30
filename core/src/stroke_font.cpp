/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "stroke_font.h"
#include "newstroke_font.h"

#include <cmath>

namespace kp
{

static constexpr double STROKE_FONT_SCALE = 1.0 / 21.0;
static constexpr int    FONT_OFFSET = -8;
static constexpr double ITALIC_TILT = 1.0 / 8.0;
static constexpr double INTERLINE_PITCH = 1.68;
static constexpr double LEGACY_FACTOR = 0.9583;
static constexpr double OVERBAR_HEIGHT = 1.23;
static constexpr double SUPER_SUB_SIZE = 0.8;
static constexpr double SUBSCRIPT_OFFSET = 0.15;
static constexpr double SUPERSCRIPT_OFFSET = -0.35;


std::u32string DecodeUtf8( const std::string& s )
{
    std::u32string out;
    out.reserve( s.size() );

    for( size_t i = 0; i < s.size(); )
    {
        unsigned char c = (unsigned char) s[i];
        uint32_t      cp;
        int           extra;

        if( c < 0x80 )
        {
            cp = c;
            extra = 0;
        }
        else if( ( c & 0xE0 ) == 0xC0 )
        {
            cp = c & 0x1F;
            extra = 1;
        }
        else if( ( c & 0xF0 ) == 0xE0 )
        {
            cp = c & 0x0F;
            extra = 2;
        }
        else if( ( c & 0xF8 ) == 0xF0 )
        {
            cp = c & 0x07;
            extra = 3;
        }
        else
        {
            out.push_back( 0xFFFD );
            ++i;
            continue;
        }

        if( extra > 0 && i + (size_t) extra >= s.size() )
        {
            out.push_back( 0xFFFD );
            break;
        }

        bool ok = true;

        for( int k = 1; k <= extra; ++k )
        {
            unsigned char cc = (unsigned char) s[i + (size_t) k];

            if( ( cc & 0xC0 ) != 0x80 )
            {
                ok = false;
                break;
            }

            cp = ( cp << 6 ) | ( cc & 0x3F );
        }

        if( !ok )
        {
            out.push_back( 0xFFFD );
            ++i;
            continue;
        }

        out.push_back( cp );
        i += (size_t) extra + 1;
    }

    return out;
}


StrokeFont::StrokeFont() = default;


const StrokeFont& StrokeFont::Get()
{
    static StrokeFont font;
    return font;
}


const StrokeGlyph& StrokeFont::glyph( uint32_t aCodePoint ) const
{
    int idx = (int) aCodePoint - 32;

    if( idx < 0 || idx >= newstroke_font_bufsize )
        idx = '?' - 32;

    std::lock_guard<std::mutex> lock( m_mutex );

    auto it = m_cache.find( (uint32_t) idx );

    if( it != m_cache.end() )
        return *it->second;

    auto        g = std::make_unique<StrokeGlyph>();
    const char* s = newstroke_font[idx];

    if( s && s[0] && s[1] )
    {
        double startX = ( s[0] - 'R' ) * STROKE_FONT_SCALE;
        double endX = ( s[1] - 'R' ) * STROKE_FONT_SCALE;
        g->width = endX - startX;

        Path current;

        for( int i = 2; s[i] && s[i + 1]; i += 2 )
        {
            if( s[i] == ' ' && s[i + 1] == 'R' )
            {
                if( current.size() > 1 )
                    g->strokes.push_back( current );

                current.clear();
                continue;
            }

            double x = ( s[i] - 'R' ) * STROKE_FONT_SCALE - startX;
            double y = ( s[i + 1] - 'R' + FONT_OFFSET ) * STROKE_FONT_SCALE;
            current.push_back( Pt( x, y ) );
        }

        if( current.size() > 1 )
            g->strokes.push_back( current );
        else if( current.size() == 1 )
            g->strokes.push_back( { current[0], current[0] } ); // dot
    }

    const StrokeGlyph& ref = *g;
    m_cache.emplace( (uint32_t) idx, std::move( g ) );
    return ref;
}


double StrokeFont::spaceWidth() const
{
    return glyph( ' ' ).width;
}


void StrokeFont::parseMarkup( const std::u32string& aLine, std::vector<Run>& aRuns ) const
{
    // KiCad markup: ~{overbar} _{subscript} ^{superscript}; nesting supported shallowly.
    std::vector<int> styleStack;
    int              style = 0;
    Run              cur;

    auto flush = [&]()
    {
        if( !cur.text.empty() )
        {
            cur.style = style;
            aRuns.push_back( cur );
            cur.text.clear();
        }
    };

    for( size_t i = 0; i < aLine.size(); ++i )
    {
        char32_t c = aLine[i];

        if( ( c == U'~' || c == U'_' || c == U'^' ) && i + 1 < aLine.size() && aLine[i + 1] == U'{' )
        {
            flush();
            styleStack.push_back( style );

            if( c == U'~' )
                style |= 1;
            else if( c == U'_' )
                style = ( style & 1 ) | 2;
            else
                style = ( style & 1 ) | 4;

            ++i;
            continue;
        }

        if( c == U'}' && !styleStack.empty() )
        {
            flush();
            style = styleStack.back();
            styleStack.pop_back();
            continue;
        }

        cur.text.push_back( c );
    }

    flush();
}


double StrokeFont::drawRuns( const std::vector<Run>& aRuns, const Pt& aStart, const TextLayout& L,
                             std::vector<Path>* aOut ) const
{
    // Returns the advance (cursor.x - aStart.x)
    const double tilt = L.italic ? ITALIC_TILT : 0.0;
    const double space = spaceWidth();
    double       cursorX = aStart.x;
    int          charCount = 0;

    auto emit = [&]( Path p )
    {
        for( Pt& q : p )
        {
            if( L.mirror )
                q.x = L.pos.x - ( q.x - L.pos.x );

            if( L.angle != 0.0 )
                q = RotatePt( q, L.pos, L.angle );
        }

        aOut->push_back( std::move( p ) );
    };

    for( const Run& run : aRuns )
    {
        double sx = L.sizeX, sy = L.sizeY;
        double baseY = aStart.y;

        if( run.style & ( 2 | 4 ) )
        {
            sx *= SUPER_SUB_SIZE;
            sy *= SUPER_SUB_SIZE;
            baseY += sy * ( ( run.style & 2 ) ? SUBSCRIPT_OFFSET : SUPERSCRIPT_OFFSET );
        }

        double runStart = cursorX;

        for( char32_t c : run.text )
        {
            if( c == U'\t' )
            {
                const int TAB_WIDTH = 4;
                charCount = ( charCount / TAB_WIDTH + 1 ) * TAB_WIDTH - 1;
                double nc = aStart.x + L.sizeX * charCount + L.sizeX * space;

                while( nc <= cursorX )
                {
                    charCount += TAB_WIDTH;
                    nc += L.sizeX * TAB_WIDTH;
                }

                cursorX = nc;
            }
            else if( c == U' ' )
            {
                cursorX += sx * space;
            }
            else
            {
                const StrokeGlyph& g = glyph( (uint32_t) c );

                if( aOut )
                {
                    for( const Path& stroke : g.strokes )
                    {
                        Path p;
                        p.reserve( stroke.size() );

                        for( const Pt& q : stroke )
                        {
                            double x = q.x * sx;
                            double y = q.y * sy;

                            if( tilt != 0.0 )
                                x -= y * tilt;

                            p.push_back( Pt( x + cursorX, y + baseY ) );
                        }

                        emit( std::move( p ) );
                    }
                }

                cursorX += g.width * sx;
            }

            ++charCount;
        }

        if( ( run.style & 1 ) && aOut && cursorX > runStart )
        {
            double trim = L.sizeX * 0.1;
            double off = OVERBAR_HEIGHT * L.sizeY;
            emit( { Pt( runStart + trim, aStart.y - off ), Pt( cursorX - trim, aStart.y - off ) } );
        }
    }

    return cursorX - aStart.x;
}


void StrokeFont::Layout( const std::string& aUtf8, const TextLayout& L,
                         std::vector<Path>& aStrokes ) const
{
    if( aUtf8.empty() )
        return;

    std::u32string all = DecodeUtf8( aUtf8 );

    // split lines
    std::vector<std::u32string> lines;
    std::u32string              cur;

    for( char32_t c : all )
    {
        if( c == U'\n' )
        {
            lines.push_back( cur );
            cur.clear();
        }
        else if( c != U'\r' )
        {
            cur.push_back( c );
        }
    }

    lines.push_back( cur );

    const double interline = L.sizeY * INTERLINE_PITCH * LEGACY_FACTOR * L.lineSpacing;

    std::vector<std::vector<Run>> runs( lines.size() );
    std::vector<double>           extents( lines.size() );
    double                        height = 0.0;

    for( size_t i = 0; i < lines.size(); ++i )
    {
        parseMarkup( lines[i], runs[i] );
        extents[i] = drawRuns( runs[i], Pt( 0, 0 ), L, nullptr );

        if( i == 0 )
            height += L.sizeY * 1.17;
        else
            height += interline;
    }

    Pt offset( 0, L.sizeY );
    offset.x += L.thickness / 1.52;
    offset.y -= L.thickness * 0.052;

    if( L.vAlign == VAlign::CENTER )
        offset.y -= height / 2;
    else if( L.vAlign == VAlign::BOTTOM )
        offset.y -= height;

    for( size_t i = 0; i < lines.size(); ++i )
    {
        Pt lineOffset = offset;
        lineOffset.y += (double) i * interline;

        if( L.hAlign == HAlign::CENTER )
            lineOffset.x = -extents[i] / 2;
        else if( L.hAlign == HAlign::RIGHT )
            lineOffset.x = -( extents[i] + offset.x );

        drawRuns( runs[i], L.pos + lineOffset, L, &aStrokes );
    }
}

} // namespace kp
