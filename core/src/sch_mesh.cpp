/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Triangle meshes for one schematic sheet, following KiCad's schematic painter: pin
 * decorations and text placement, label shapes, field placement through the symbol
 * transform, the default drawing sheet, the default colour theme.
 *
 * The renderer paints each pixel once per slot (stencil), so inside a slot the groups are
 * emitted top-most first: outlines are then drawn over fills, as in KiCad.
 */
#include "schematic.h"
#include "i18n.h"
#include "mesh.h"
#include "mesh_acc.h"
#include "pick.h"
#include "stroke_font.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace kp
{

namespace
{

constexpr double MIL = 0.0254;
constexpr double DEFAULT_LINE = 6 * MIL;    // schematic default pen
constexpr double WIRE_WIDTH = 6 * MIL;
constexpr double BUS_WIDTH = 12 * MIL;
constexpr double JUNCTION_DIAM = 36 * MIL;
constexpr double NOCONNECT_SIZE = 48 * MIL;
constexpr double PIN_DECO = 25 * MIL;        // m_PinSymbolSize
constexpr double TARGET_PIN_RADIUS = 15 * MIL;
constexpr double PIN_TEXT_MARGIN = 4 * MIL;
constexpr double TEXT_OFFSET_RATIO = 0.15;
constexpr double LABEL_SIZE_RATIO = 0.375;

Color css( int r, int g, int b, float a = 1.0f )
{
    return { r / 255.0f, g / 255.0f, b / 255.0f, a };
}

// KiCad "default" schematic theme
const Color C_WIRE = css( 0, 150, 0 );
const Color C_BUS = css( 0, 0, 132 );
const Color C_JUNCTION = css( 0, 150, 0 );
const Color C_NOCONNECT = css( 0, 0, 132 );
const Color C_DEVICE = css( 132, 0, 0 );
const Color C_DEVICE_BG = css( 255, 255, 194 );
const Color C_PIN = css( 132, 0, 0 );
const Color C_PINNAME = css( 0, 100, 100 );
const Color C_PINNUM = css( 169, 0, 0 );
const Color C_REFERENCE = css( 0, 100, 100 );
const Color C_VALUE = css( 0, 100, 100 );
const Color C_FIELDS = css( 132, 0, 132 );
const Color C_LOCLABEL = css( 15, 15, 15 );
const Color C_GLOBLABEL = css( 132, 0, 0 );
const Color C_HIERLABEL = css( 114, 86, 0 );
const Color C_NETCLASS = css( 72, 72, 72 );
const Color C_NOTES = css( 0, 0, 194 );
const Color C_SHEET = css( 132, 0, 0 );
const Color C_SHEETNAME = css( 0, 100, 100 );
const Color C_SHEETFILE = css( 114, 86, 0 );
const Color C_SHEETFIELDS = css( 132, 0, 132 );
const Color C_SHEETLABEL = css( 0, 100, 100 );
const Color C_DRAWINGSHEET = css( 132, 0, 0 );
const Color C_PAGELIMITS = css( 181, 181, 181 );
const Color C_DNP = css( 220, 9, 13, 0.85f );

const Color& pick( const Color& aCustom, const Color& aTheme )
{
    return aCustom[3] > 0 ? aCustom : aTheme;
}

/// KiCad RotatePoint( p, angle ) with Y down: positive angles turn counter-clockwise on screen.
Pt rotate( const Pt& p, double deg )
{
    int a = ( (int) std::lround( deg ) % 360 + 360 ) % 360;

    switch( a )
    {
    case 0: return p;
    case 90: return Pt( p.y, -p.x );
    case 180: return Pt( -p.x, -p.y );
    case 270: return Pt( -p.y, p.x );
    default:
    {
        double r = deg * PI / 180, c = std::cos( r ), s = std::sin( r );
        return Pt( p.x * c + p.y * s, -p.x * s + p.y * c );
    }
    }
}

/// Spin rotation used by label shapes (the shapes are defined for SPIN LEFT).
Pt spinRotate( const Pt& p, Spin s )
{
    switch( s )
    {
    case Spin::LEFT: return p;
    case Spin::UP: return Pt( -p.y, p.x );
    case Spin::RIGHT: return Pt( -p.x, -p.y );
    case Spin::BOTTOM: return Pt( p.y, -p.x );
    }

    return p;
}

double textPen( const SchTextAttr& a )
{
    double size = std::fabs( a.sizeX );
    double t = a.thickness > 0 ? a.thickness : ( a.bold ? size / 5 : DEFAULT_LINE );
    return std::min( t, size * ( a.bold ? 0.25 : 0.18 ) );
}

std::string unitLetter( int unit )
{
    std::string s;
    int         u = std::max( 1, unit );

    do
    {
        --u;
        s.insert( s.begin(), (char) ( 'A' + u % 26 ) );
        u /= 26;
    } while( u > 0 );

    return s;
}

/// Layers, back to front. Each is a slot the user can hide.
enum Layer : int
{
    L_DRAWINGSHEET,
    L_SHEETS,
    L_NOTES,
    L_SYMBOLS,
    L_PINS,
    L_WIRES,
    L_BUSES,
    L_PINTEXT,
    L_FIELDS,
    L_LABELS,
    L_COUNT
};

struct LayerInfo
{
    const char* name;
    const char* display;
    int         sort;
    Color       color;
};

const LayerInfo LAYERS[L_COUNT] = {
    { "Sch.DrawingSheet", N_( "Drawing sheet" ), 10, css( 132, 0, 0 ) },
    { "Sch.Sheets", N_( "Sheets" ), 20, css( 132, 0, 0 ) },
    { "Sch.Notes", N_( "Notes and graphics" ), 30, css( 0, 0, 194 ) },
    { "Sch.Symbols", N_( "Symbols" ), 40, css( 132, 0, 0 ) },
    { "Sch.Pins", N_( "Pins" ), 50, css( 132, 0, 0 ) },
    { "Sch.Wires", N_( "Wires" ), 60, css( 0, 150, 0 ) },
    { "Sch.Buses", N_( "Buses" ), 70, css( 0, 0, 132 ) },
    { "Sch.PinText", N_( "Pin names and numbers" ), 80, css( 0, 100, 100 ) },
    { "Sch.Fields", N_( "Symbol fields" ), 90, css( 0, 100, 100 ) },
    { "Sch.Labels", N_( "Labels" ), 100, css( 132, 0, 0 ) },
};

/// Z order inside a slot (higher = on top).
enum Sub : int
{
    S_FILL = 0,
    S_FILL2 = 1,
    S_LINE = 2,
    S_TEXT = 3
};

class SchBuilder
{
public:
    SchBuilder( const Schematic& aSch, int aSheet, Mesh& aMesh ) :
            m_sch( aSch ),
            m_node( aSch.sheets[(size_t) aSheet] ),
            m_screen( aSch.screens[(size_t) aSch.sheets[(size_t) aSheet].screen] ),
            m_mesh( aMesh )
    {
    }

    void Build()
    {
        m_mesh = Mesh();

        for( int l = 0; l < L_COUNT; ++l )
        {
            Slot s;
            s.name = LAYERS[l].name;
            s.display = Tr( LAYERS[l].display );
            s.layer = -1;
            s.kind = 20; // schematic layer
            s.sortFront = s.sortBack = LAYERS[l].sort;
            s.color = LAYERS[l].color;
            m_mesh.slots.push_back( s );
        }

        m_mesh.offsets.assign( (size_t) ItemType::COUNT, {} );
        m_pinBase = SchematicPinBase( m_sch, m_screen );

        drawingSheet();
        symbols();
        pins();
        wires();
        junctions();
        labels();
        sheets();
        notes();
        merge();
    }

private:
    // ---- accumulators ------------------------------------------------------------------

    struct Pending
    {
        size_t   acc;
        uint32_t start, count;
    };

    struct AccKey
    {
        int   layer, sub;
        Color color;

        bool operator<( const AccKey& o ) const
        {
            if( layer != o.layer )
                return layer < o.layer;

            if( sub != o.sub )
                return sub < o.sub;

            return color < o.color;
        }
    };

    Acc& acc( int layer, int sub, const Color& c )
    {
        AccKey key { layer, sub, c };
        auto   it = m_accIndex.find( key );

        if( it == m_accIndex.end() )
        {
            it = m_accIndex.emplace( key, m_accs.size() ).first;
            m_accs.emplace_back();
            m_keys.push_back( key );
        }

        m_lastAcc = it->second;
        return m_accs[it->second];
    }

    /// Draw into (layer, sub, colour) and remember the triangles for the current item.
    template <typename F>
    void draw( int layer, int sub, const Color& c, F&& f )
    {
        Acc&     a = acc( layer, sub, c );
        uint32_t s0 = (uint32_t) a.i.size();
        f( a );
        uint32_t n = (uint32_t) a.i.size() - s0;

        if( n && m_recording )
            m_pending.push_back( { m_lastAcc, s0, n } );
    }

    void beginItem( ItemType t )
    {
        m_recording = true;
        m_mesh.offsets[(size_t) t].push_back( (uint32_t) m_pending.size() );
    }

    void endType( ItemType t )
    {
        m_mesh.offsets[(size_t) t].push_back( (uint32_t) m_pending.size() );
        m_recording = false;
    }

    // ---- primitives --------------------------------------------------------------------

    static void strokePath( Acc& a, const Path& pts, bool closed, double w, LineStyle style )
    {
        if( pts.size() < 2 || w <= 0 )
            return;

        if( style == LineStyle::DEFAULT || style == LineStyle::SOLID )
        {
            a.polyline( pts, w, closed );
            return;
        }

        // dash patterns (KiCad: dash 12 x width, gap 3 x width, dot = width)
        double              dw = std::max( w, 0.1 );
        double              dash = 12 * dw, gap = 3 * dw, dot = 0.2 * dw;
        std::vector<double> pattern;

        switch( style )
        {
        case LineStyle::DASH: pattern = { dash, gap }; break;
        case LineStyle::DOT: pattern = { dot, gap }; break;
        case LineStyle::DASH_DOT: pattern = { dash, gap, dot, gap }; break;
        default: pattern = { dash, gap, dot, gap, dot, gap }; break;
        }

        Path path = pts;

        if( closed )
            path.push_back( pts[0] );

        size_t pi = 0;
        double left = pattern[0];
        bool   on = true;

        for( size_t i = 0; i + 1 < path.size(); ++i )
        {
            Pt     p = path[i], q = path[i + 1];
            double len = std::hypot( q.x - p.x, q.y - p.y ), t = 0;

            while( len - t > 1e-9 )
            {
                double step = std::min( left, len - t );
                Pt     a0( p.x + ( q.x - p.x ) * t / len, p.y + ( q.y - p.y ) * t / len );
                Pt     a1( p.x + ( q.x - p.x ) * ( t + step ) / len, p.y + ( q.y - p.y ) * ( t + step ) / len );

                if( on )
                    a.capsule( a0, a1, w );

                t += step;
                left -= step;

                if( left <= 1e-9 )
                {
                    pi = ( pi + 1 ) % pattern.size();
                    left = pattern[pi];
                    on = !on;
                }
            }
        }
    }

    static Path circlePath( const Pt& c, double r )
    {
        Path p;
        int  n = std::max( 24, roundSegments( r ) );

        for( int k = 0; k < n; ++k )
            p.push_back( Pt( c.x + r * std::cos( 2 * PI * k / n ), c.y + r * std::sin( 2 * PI * k / n ) ) );

        return p;
    }

    static Path arcPath( const Pt& s, const Pt& m, const Pt& e )
    {
        Path p;
        ArcToPoints( p, s, m, e, 0.005 );
        return p;
    }

    static Path bezierPath( const Path& c )
    {
        Path p;

        if( c.size() < 4 )
            return c;

        for( int k = 0; k <= 32; ++k )
        {
            double t = k / 32.0, u = 1 - t;
            p.push_back( Pt( u * u * u * c[0].x + 3 * u * u * t * c[1].x + 3 * u * t * t * c[2].x + t * t * t * c[3].x,
                             u * u * u * c[0].y + 3 * u * u * t * c[1].y + 3 * u * t * t * c[2].y + t * t * t * c[3].y ) );
        }

        return p;
    }

    /// Text drawn with its own justification at aPos.
    static void text( Acc& a, const std::string& s, const Pt& pos, double angle, const SchTextAttr& attr,
                      HAlign h, VAlign v, double pen )
    {
        if( s.empty() )
            return;

        TextLayout L;
        L.pos = pos;
        L.sizeX = attr.sizeX;
        L.sizeY = attr.sizeY;
        L.thickness = pen;
        L.angle = angle;
        L.lineSpacing = attr.lineSpacing;
        L.italic = attr.italic;
        L.hAlign = h;
        L.vAlign = v;

        std::vector<Path> strokes;
        StrokeFont::Get().Layout( s, L, strokes );

        for( const Path& st : strokes )
            a.polyline( st, pen, false );
    }

    static Pt extents( const std::string& s, const SchTextAttr& attr, double pen )
    {
        TextLayout L;
        L.sizeX = attr.sizeX;
        L.sizeY = attr.sizeY;
        L.thickness = pen;
        L.lineSpacing = attr.lineSpacing;
        L.italic = attr.italic;
        return StrokeFont::Get().Extents( s, L );
    }

    /// Centre of a justified text's box (KiCad draws fields and symbol text centred there).
    static Pt boxCentre( const std::string& s, const Pt& pos, double angle, const SchTextAttr& attr, double pen )
    {
        Pt ext = extents( s, attr, pen );
        Pt d( attr.hAlign == HAlign::LEFT ? ext.x / 2 : attr.hAlign == HAlign::RIGHT ? -ext.x / 2 : 0,
              attr.vAlign == VAlign::TOP ? ext.y / 2 : attr.vAlign == VAlign::BOTTOM ? -ext.y / 2 : 0 );
        return pos + rotate( d, angle );
    }

    void fillAndStroke( int layer, const Path& outline, bool closed, const SchStroke& stroke, SchFill fill,
                        const Color& fillColor, const Color& lineColor, const Color& bgColor, double defWidth )
    {
        if( closed && outline.size() >= 3 )
        {
            Color fc { 0, 0, 0, 0 };

            if( fill == SchFill::BACKGROUND )
                fc = bgColor;
            else if( fill == SchFill::OUTLINE )
                fc = lineColor;
            else if( fill == SchFill::COLOR || fill == SchFill::HATCH )
                fc = fillColor[3] > 0 ? fillColor : lineColor;

            if( fc[3] > 0 )
                draw( layer, fill == SchFill::BACKGROUND ? S_FILL : S_FILL2, fc, [&]( Acc& a ) { a.polygon( { outline } ); } );
        }

        double w = stroke.width > 0 ? stroke.width : stroke.width < 0 ? 0.0 : defWidth;

        if( w > 0 )
            draw( layer, S_LINE, lineColor, [&]( Acc& a ) { strokePath( a, outline, closed, w, stroke.style ); } );
    }

    /// A graphic in sheet coordinates (aMap converts symbol coordinates when drawing symbols).
    void graphic( int layer, const SchGraphic& g, const std::function<Pt( const Pt& )>& map,
                  const Color& lineTheme, const Color& bgTheme, double textAngleToggle )
    {
        Color line = pick( g.stroke.color, lineTheme );

        switch( g.kind )
        {
        case SchGraphic::POLY:
        {
            Path p;

            for( const Pt& q : g.pts )
                p.push_back( map( q ) );

            bool closed = g.fill != SchFill::NONE && p.size() >= 3;

            if( closed && std::hypot( p.front().x - p.back().x, p.front().y - p.back().y ) < 1e-9 )
                p.pop_back();

            fillAndStroke( layer, p, closed, g.stroke, g.fill, g.fillColor, line, bgTheme, DEFAULT_LINE );
            break;
        }
        case SchGraphic::BEZIER:
        {
            Path p;

            for( const Pt& q : bezierPath( g.pts ) )
                p.push_back( map( q ) );

            fillAndStroke( layer, p, g.fill != SchFill::NONE, g.stroke, g.fill, g.fillColor, line, bgTheme, DEFAULT_LINE );
            break;
        }
        case SchGraphic::RECT:
        {
            Pt   a = g.pts[0], b = g.pts[1];
            Path p;

            if( g.cornerRadius > 0 )
            {
                double hx = std::fabs( b.x - a.x ) / 2, hy = std::fabs( b.y - a.y ) / 2;
                Pt     c( ( a.x + b.x ) / 2, ( a.y + b.y ) / 2 );

                for( const Pt& q : roundRectPath( hx, hy, g.cornerRadius, 0, 0 ) )
                    p.push_back( map( q + c ) );
            }
            else
            {
                p = { map( a ), map( Pt( b.x, a.y ) ), map( b ), map( Pt( a.x, b.y ) ) };
            }

            fillAndStroke( layer, p, true, g.stroke, g.fill, g.fillColor, line, bgTheme, DEFAULT_LINE );
            break;
        }
        case SchGraphic::CIRCLE:
        {
            Path p;

            for( const Pt& q : circlePath( g.center, g.radius ) )
                p.push_back( map( q ) );

            fillAndStroke( layer, p, true, g.stroke, g.fill, g.fillColor, line, bgTheme, DEFAULT_LINE );
            break;
        }
        case SchGraphic::ARC:
        {
            Path p;

            for( const Pt& q : arcPath( g.start, g.mid, g.end ) )
                p.push_back( map( q ) );

            if( g.fill != SchFill::NONE && p.size() >= 3 )
            {
                // a filled arc is a pie: KiCad fills the area between the arc and its centre
                Pt   c;
                Path pie = p;
                double d = 2 * ( g.start.x * ( g.mid.y - g.end.y ) + g.mid.x * ( g.end.y - g.start.y )
                                 + g.end.x * ( g.start.y - g.mid.y ) );

                if( std::fabs( d ) > 1e-12 )
                {
                    double a2 = g.start.x * g.start.x + g.start.y * g.start.y, b2 = g.mid.x * g.mid.x + g.mid.y * g.mid.y,
                           c2 = g.end.x * g.end.x + g.end.y * g.end.y;
                    c = Pt( ( a2 * ( g.mid.y - g.end.y ) + b2 * ( g.end.y - g.start.y ) + c2 * ( g.start.y - g.mid.y ) ) / d,
                            ( a2 * ( g.end.x - g.mid.x ) + b2 * ( g.start.x - g.end.x ) + c2 * ( g.mid.x - g.start.x ) ) / d );
                    pie.push_back( map( c ) );
                    SchStroke none = g.stroke;
                    none.width = -1;
                    fillAndStroke( layer, pie, true, none, g.fill, g.fillColor, line, bgTheme, DEFAULT_LINE );
                }
            }

            double w = g.stroke.width > 0 ? g.stroke.width : g.stroke.width < 0 ? 0.0 : DEFAULT_LINE;

            if( w > 0 )
                draw( layer, S_LINE, line, [&]( Acc& a ) { strokePath( a, p, false, w, g.stroke.style ); } );

            break;
        }
        case SchGraphic::TEXT:
        {
            if( g.attr.hidden || g.text.empty() )
                break;

            double pen = textPen( g.attr );
            Pt     c = map( boxCentre( g.text, g.textPos, g.textAngle, g.attr, pen ) );
            double angle = g.textAngle;

            if( textAngleToggle != 0 )
                angle = std::fabs( std::remainder( angle, 180.0 ) ) < 45 ? 90 : 0;

            Color col = pick( g.attr.color, lineTheme );
            draw( layer, S_TEXT, col, [&]( Acc& a ) { text( a, g.text, c, angle, g.attr, HAlign::CENTER, VAlign::CENTER, pen ); } );
            break;
        }
        case SchGraphic::TEXTBOX:
            textBox( layer, g, map, lineTheme, bgTheme );
            break;
        }
    }

    void textBox( int layer, const SchGraphic& g, const std::function<Pt( const Pt& )>& map, const Color& lineTheme,
                  const Color& bgTheme )
    {
        Pt   a = map( g.pts[0] ), b = map( g.pts[1] );
        Pt   lo( std::min( a.x, b.x ), std::min( a.y, b.y ) ), hi( std::max( a.x, b.x ), std::max( a.y, b.y ) );
        Path box = { lo, Pt( hi.x, lo.y ), hi, Pt( lo.x, hi.y ) };
        Color line = pick( g.stroke.color, lineTheme );
        fillAndStroke( layer, box, true, g.stroke, g.fill, g.fillColor, line, bgTheme, DEFAULT_LINE );

        if( g.text.empty() )
            return;

        bool   vertical = std::fabs( std::remainder( g.textAngle, 180.0 ) ) > 45;
        double pen = textPen( g.attr );
        // area inside the margins, along the text direction
        double width = vertical ? ( hi.y - lo.y ) - g.margins[1] - g.margins[3]
                                : ( hi.x - lo.x ) - g.margins[0] - g.margins[2];
        std::string wrapped = wrap( g.text, g.attr, pen, width );
        Pt          pos;

        if( !vertical )
        {
            pos.x = g.attr.hAlign == HAlign::LEFT    ? lo.x + g.margins[0]
                    : g.attr.hAlign == HAlign::RIGHT ? hi.x - g.margins[2]
                                                     : ( lo.x + hi.x ) / 2;
            pos.y = g.attr.vAlign == VAlign::TOP      ? lo.y + g.margins[1]
                    : g.attr.vAlign == VAlign::BOTTOM ? hi.y - g.margins[3]
                                                      : ( lo.y + hi.y ) / 2;
        }
        else
        {
            pos.y = g.attr.hAlign == HAlign::LEFT    ? hi.y - g.margins[3]
                    : g.attr.hAlign == HAlign::RIGHT ? lo.y + g.margins[1]
                                                     : ( lo.y + hi.y ) / 2;
            pos.x = g.attr.vAlign == VAlign::TOP      ? lo.x + g.margins[0]
                    : g.attr.vAlign == VAlign::BOTTOM ? hi.x - g.margins[2]
                                                      : ( lo.x + hi.x ) / 2;
        }

        Color col = pick( g.attr.color, lineTheme );
        draw( layer, S_TEXT, col, [&]( Acc& acc ) { text( acc, wrapped, pos, vertical ? 90 : 0, g.attr, g.attr.hAlign, g.attr.vAlign, pen ); } );
    }

    /// Word wrap to a width (text boxes).
    static std::string wrap( const std::string& s, const SchTextAttr& attr, double pen, double width )
    {
        if( width <= 0 )
            return s;

        std::string out, line;
        size_t      i = 0;

        auto flushWord = [&]( const std::string& word )
        {
            std::string candidate = line.empty() ? word : line + " " + word;

            if( !line.empty() && extents( candidate, attr, pen ).x > width )
            {
                out += line + "\n";
                line = word;
            }
            else
            {
                line = candidate;
            }
        };

        while( i <= s.size() )
        {
            size_t nl = s.find( '\n', i );
            std::string para = s.substr( i, nl == std::string::npos ? std::string::npos : nl - i );
            line.clear();
            size_t w = 0;

            while( w < para.size() )
            {
                size_t sp = para.find( ' ', w );
                std::string word = para.substr( w, sp == std::string::npos ? std::string::npos : sp - w );

                if( !word.empty() )
                    flushWord( word );

                if( sp == std::string::npos )
                    break;

                w = sp + 1;
            }

            out += line;

            if( nl == std::string::npos )
                break;

            out += "\n";
            i = nl + 1;
        }

        return out;
    }

    // ---- text variables ----------------------------------------------------------------

    std::string resolve( const std::string& s, const SchSymbol* sym = nullptr ) const
    {
        if( s.find( "${" ) == std::string::npos )
            return s;

        std::string out;
        size_t      i = 0;

        while( i < s.size() )
        {
            size_t open = s.find( "${", i );

            if( open == std::string::npos )
            {
                out += s.substr( i );
                break;
            }

            size_t close = s.find( '}', open );

            if( close == std::string::npos )
            {
                out += s.substr( i );
                break;
            }

            out += s.substr( i, open - i );
            std::string var = s.substr( open + 2, close - open - 2 ), val;
            bool        found = variable( var, sym, val );
            out += found ? val : s.substr( open, close - open + 1 );
            i = close + 1;
        }

        return out;
    }

    bool variable( const std::string& var, const SchSymbol* sym, std::string& val ) const
    {
        const TitleBlock& tb = m_sch.screens[0].titleBlock;

        if( sym )
        {
            if( var == "REFERENCE" )
            {
                val = SchematicShownReference( m_sch, m_node, *sym );
                return true;
            }

            // field names match without case (${VALUE}, ${SIM.PARAMS})
            auto same = []( const std::string& a, const std::string& b )
            {
                if( a.size() != b.size() )
                    return false;

                for( size_t i = 0; i < a.size(); ++i )
                {
                    if( std::tolower( (unsigned char) a[i] ) != std::tolower( (unsigned char) b[i] ) )
                        return false;
                }

                return true;
            };

            for( const SchField& f : sym->fields )
            {
                if( same( f.name, var ) )
                {
                    val = m_sch.FieldValue( m_node, *sym, f );
                    return true;
                }
            }
        }

        const TitleBlock& own = m_screen.titleBlock;
        const TitleBlock& t = own.title.empty() && own.company.empty() ? tb : own;

        if( var == "TITLE" ) val = t.title;
        else if( var == "ISSUE_DATE" ) val = t.date;
        else if( var == "REVISION" ) val = t.rev;
        else if( var == "COMPANY" ) val = t.company;
        else if( var.size() == 8 && var.compare( 0, 7, "COMMENT" ) == 0 && var[7] >= '1' && var[7] <= '9' )
            val = t.comments[(size_t) ( var[7] - '1' )];
        else if( var == "PAPER" ) val = m_screen.paper;
        else if( var == "#" ) val = m_node.page;
        else if( var == "##" ) val = std::to_string( m_sch.sheets.size() );
        else if( var == "FILENAME" )
        {
            size_t slash = m_screen.fileName.find_last_of( "/\\" );
            val = slash == std::string::npos ? m_screen.fileName : m_screen.fileName.substr( slash + 1 );
        }
        else if( var == "SHEETNAME" ) val = m_node.name.empty() ? "/" : m_node.name;
        else if( var == "SHEETPATH" )
        {
            std::vector<std::string> names;

            for( int n = (int) ( &m_node - &m_sch.sheets[0] ); n > 0; n = m_sch.sheets[(size_t) n].parent )
                names.insert( names.begin(), m_sch.sheets[(size_t) n].name );

            val = "/";

            for( const std::string& nm : names )
                val += nm + "/";
        }
        else if( var == "KICAD_VERSION" )
            val = "KiCad E.D.A. " + ( m_sch.screens[0].generatorVersion.empty() ? std::string() : m_sch.screens[0].generatorVersion );
        else if( var == "INTERSHEET_REFS" )
            val = "";
        else
            return false;

        return true;
    }

    // ---- drawing sheet -----------------------------------------------------------------

    void drawingSheet()
    {
        const double W = m_screen.paperW, H = m_screen.paperH;
        const double L = 10, T = 10, R = W - 10, B = H - 10;
        const double lw = 0.15;
        const Color& c = C_DRAWINGSHEET;

        // page limits
        draw( L_DRAWINGSHEET, S_FILL, C_PAGELIMITS, [&]( Acc& a ) { a.polyline( { Pt( 0, 0 ), Pt( W, 0 ), Pt( W, H ), Pt( 0, H ) }, 0.1, true ); } );

        draw( L_DRAWINGSHEET, S_LINE, c, [&]( Acc& a )
              {
                  for( int k = 0; k < 2; ++k )
                      a.polyline( { Pt( L + 2 * k, T + 2 * k ), Pt( R - 2 * k, T + 2 * k ), Pt( R - 2 * k, B - 2 * k ),
                                    Pt( L + 2 * k, B - 2 * k ) },
                                  lw, true );

                  // zone ticks every 50 mm on the four sides
                  for( int k = 1; k < 30 && L + 50 * k < R - 2; ++k )
                  {
                      a.capsule( Pt( L + 50 * k, T ), Pt( L + 50 * k, T + 2 ), lw );
                      a.capsule( Pt( L + 50 * k, B ), Pt( L + 50 * k, B - 2 ), lw );
                  }

                  for( int k = 1; k < 30 && T + 50 * k < B - 2; ++k )
                  {
                      a.capsule( Pt( L, T + 50 * k ), Pt( L + 2, T + 50 * k ), lw );
                      a.capsule( Pt( R, T + 50 * k ), Pt( R - 2, T + 50 * k ), lw );
                  }

                  // title block frame (corner = bottom right)
                  auto rb = [&]( double x, double y ) { return Pt( R - x, B - y ); };
                  a.polyline( { rb( 110, 34 ), rb( 2, 34 ), rb( 2, 2 ), rb( 110, 2 ) }, lw, true );

                  for( double y : { 5.5, 8.5, 12.5, 18.5 } )
                      a.capsule( rb( 110, y ), rb( 2, y ), lw );

                  a.capsule( rb( 90, 8.5 ), rb( 90, 5.5 ), lw );
                  a.capsule( rb( 26, 8.5 ), rb( 26, 2 ), lw );
              } );

        SchTextAttr small;
        small.sizeX = small.sizeY = 1.3;
        SchTextAttr normal;
        normal.sizeX = normal.sizeY = 1.5;

        draw( L_DRAWINGSHEET, S_TEXT, c, [&]( Acc& a )
              {
                  for( int k = 0; k < 100 && L + 25 + 50 * k < R; ++k )
                  {
                      std::string n = std::to_string( k + 1 );
                      text( a, n, Pt( L + 25 + 50 * k, T + 1 ), 0, small, HAlign::LEFT, VAlign::CENTER, lw );
                      text( a, n, Pt( L + 25 + 50 * k, B - 1 ), 0, small, HAlign::LEFT, VAlign::CENTER, lw );
                  }

                  for( int k = 0; k < 26 && T + 25 + 50 * k < B; ++k )
                  {
                      std::string n( 1, (char) ( 'A' + k ) );
                      text( a, n, Pt( L + 1, T + 25 + 50 * k ), 0, small, HAlign::CENTER, VAlign::CENTER, lw );
                      text( a, n, Pt( R - 1, T + 25 + 50 * k ), 0, small, HAlign::CENTER, VAlign::CENTER, lw );
                  }

                  auto tb = [&]( const char* s, double x, double y, double size, bool bold, bool italic )
                  {
                      SchTextAttr at = normal;
                      at.sizeX = at.sizeY = size;
                      at.bold = bold;
                      at.italic = italic;
                      std::string shown = resolve( s );
                      text( a, shown, Pt( R - x, B - y ), 0, at, HAlign::LEFT, VAlign::CENTER, bold ? size / 5 : lw );
                  };

                  tb( "Date: ${ISSUE_DATE}", 87, 6.9, 1.5, false, false );
                  tb( "${KICAD_VERSION}", 109, 4.1, 1.5, false, false );
                  tb( "Rev: ${REVISION}", 24, 6.9, 1.5, true, false );
                  tb( "Size: ${PAPER}", 109, 6.9, 1.5, false, false );
                  tb( "Id: ${#}/${##}", 24, 4.1, 1.5, false, false );
                  tb( "Title: ${TITLE}", 109, 10.7, 2.0, true, true );
                  tb( "File: ${FILENAME}", 109, 14.3, 1.5, false, false );
                  tb( "Sheet: ${SHEETPATH}", 109, 17, 1.5, false, false );
                  tb( "${COMPANY}", 109, 20, 1.5, true, false );
                  tb( "${COMMENT1}", 109, 23, 1.5, false, false );
                  tb( "${COMMENT2}", 109, 26, 1.5, false, false );
                  tb( "${COMMENT3}", 109, 29, 1.5, false, false );
                  tb( "${COMMENT4}", 109, 32, 1.5, false, false );
              } );
    }

    // ---- symbols -----------------------------------------------------------------------

    bool shown( const LibSymbol& L, int unit, int body, int itemUnit, int itemBody ) const
    {
        (void) L;
        return ( itemUnit == 0 || itemUnit == unit ) && ( itemBody == 0 || itemBody == body );
    }

    void symbols()
    {
        for( size_t si = 0; si < m_screen.symbols.size(); ++si )
        {
            beginItem( ItemType::SCH_SYMBOL );
            const SchSymbol& sym = m_screen.symbols[si];
            const LibSymbol* lib = m_sch.Lib( m_screen, sym );
            int              unit = m_sch.Unit( m_node, sym );
            auto             map = [&]( const Pt& p ) { return sym.pos + sym.transform.apply( p ); };
            BBox             body;

            if( lib )
            {
                for( const SchGraphic& g : lib->graphics )
                {
                    if( g.isPrivate || !shown( *lib, unit, sym.bodyStyle, g.unit, g.bodyStyle ) )
                        continue;

                    graphic( L_SYMBOLS, g, map, C_DEVICE, C_DEVICE_BG, sym.transform.y1 );

                    for( const Pt& q : g.pts )
                        body.add( map( q ) );

                    if( g.kind == SchGraphic::CIRCLE )
                    {
                        body.add( map( g.center ), g.radius );
                    }
                    else if( g.kind == SchGraphic::ARC )
                    {
                        body.add( map( g.start ) );
                        body.add( map( g.mid ) );
                        body.add( map( g.end ) );
                    }
                }
            }
            else
            {
                // unknown symbol: a small box with "??" as KiCad does
                Path box = { sym.pos + Pt( -2.54, -2.54 ), sym.pos + Pt( 2.54, -2.54 ), sym.pos + Pt( 2.54, 2.54 ),
                             sym.pos + Pt( -2.54, 2.54 ) };
                draw( L_SYMBOLS, S_LINE, C_DEVICE, [&]( Acc& a ) { a.polyline( box, DEFAULT_LINE, true ); } );
                SchTextAttr at;
                draw( L_SYMBOLS, S_TEXT, C_DEVICE, [&]( Acc& a ) { text( a, "??", sym.pos, 0, at, HAlign::CENTER, VAlign::CENTER, DEFAULT_LINE ); } );
            }

            // fields
            for( const SchField& f : sym.fields )
            {
                if( f.attr.hidden )
                    continue;

                std::string value;

                if( f.name == "Reference" )
                    value = SchematicShownReference( m_sch, m_node, sym );
                else
                    value = resolve( m_sch.FieldValue( m_node, sym, f ), &sym );

                if( value.empty() )
                    continue;

                if( f.showName && f.name != "Reference" && f.name != "Value" )
                    value = f.name + ": " + value;

                // the file gives the anchor on the sheet; the justification box turns with
                // the symbol (KiCad SCH_FIELD::GetBoundingBox)
                double pen = textPen( f.attr );
                Pt     c = f.pos + sym.transform.apply( boxCentre( value, Pt( 0, 0 ), f.angle, f.attr, pen ) );
                double angle = std::fabs( std::remainder( f.angle, 180.0 ) ) > 45 ? 90 : 0;

                if( sym.transform.y1 != 0 )
                    angle = angle == 0 ? 90 : 0;

                Color theme = f.name == "Reference" ? C_REFERENCE : f.name == "Value" ? C_VALUE : C_FIELDS;
                Color col = pick( f.attr.color, theme );
                draw( L_FIELDS, S_TEXT, col, [&]( Acc& a ) { text( a, value, c, angle, f.attr, HAlign::CENTER, VAlign::CENTER, pen ); } );
            }

            // do-not-populate cross
            if( sym.dnp && body.valid() )
            {
                draw( L_SYMBOLS, S_TEXT, C_DNP, [&]( Acc& a )
                      {
                          double w = 3 * DEFAULT_LINE;
                          a.capsule( Pt( body.minX, body.minY ), Pt( body.maxX, body.maxY ), w );
                          a.capsule( Pt( body.maxX, body.minY ), Pt( body.minX, body.maxY ), w );
                      } );
            }
        }

        endType( ItemType::SCH_SYMBOL );
    }

    void pins()
    {
        for( size_t si = 0; si < m_screen.symbols.size(); ++si )
        {
            const SchSymbol& sym = m_screen.symbols[si];
            const LibSymbol* lib = m_sch.Lib( m_screen, sym );

            if( !lib )
                continue;

            int unit = m_sch.Unit( m_node, sym );

            // the pins of symbol si are items base[si] .. base[si+1]
            for( size_t pi = 0; pi < lib->pins.size(); ++pi )
            {
                beginItem( ItemType::SCH_PIN );
                const LibPin& pin = lib->pins[pi];

                if( !pin.visible || !shown( *lib, unit, sym.bodyStyle, pin.unit, pin.bodyStyle ) )
                    continue;

                pinGraphic( *lib, sym, pin );
            }
        }

        endType( ItemType::SCH_PIN );
    }

    void pinGraphic( const LibSymbol& lib, const SchSymbol& sym, const LibPin& pin )
    {
        Pt tip, root;
        SchematicPinPoints( sym, pin, tip, root );
        const Pt dir( ( tip.x > root.x + 1e-9 ) - ( tip.x < root.x - 1e-9 ), ( tip.y > root.y + 1e-9 ) - ( tip.y < root.y - 1e-9 ) );
        const double r = PIN_DECO, diam = 2 * r, clk = PIN_DECO;
        const double w = DEFAULT_LINE;

        draw( L_PINS, S_LINE, C_PIN, [&]( Acc& a )
              {
                  auto line = [&]( const Pt& p, const Pt& q ) { a.capsule( p, q, w ); };
                  auto tri = [&]( const Pt& p, const Pt& q, const Pt& s )
                  {
                      line( p, q );
                      line( q, s );
                  };
                  Pt p0 = root;

                  if( pin.type == PinType::NO_CONNECT )
                  {
                      line( p0, tip );
                      line( tip + Pt( -1, -1 ) * TARGET_PIN_RADIUS, tip + Pt( 1, 1 ) * TARGET_PIN_RADIUS );
                      line( tip + Pt( 1, -1 ) * TARGET_PIN_RADIUS, tip + Pt( -1, 1 ) * TARGET_PIN_RADIUS );
                      return;
                  }

                  switch( pin.shape )
                  {
                  case PinShape::LINE: line( p0, tip ); break;
                  case PinShape::INVERTED:
                      a.ring( p0 + dir * r, r - w / 2, r + w / 2 );
                      line( p0 + dir * diam, tip );
                      break;
                  case PinShape::INVERTED_CLOCK:
                  {
                      Pt pc = p0 - dir * clk;
                      tri( p0 + Pt( dir.y, -dir.x ) * clk, pc, p0 + Pt( -dir.y, dir.x ) * clk );
                      a.ring( p0 + dir * r, r - w / 2, r + w / 2 );
                      line( p0 + dir * diam, tip );
                      break;
                  }
                  case PinShape::CLOCK_LOW:
                  case PinShape::FALLING_EDGE_CLOCK:
                  {
                      Pt pc = p0 - dir * clk;
                      tri( p0 + Pt( dir.y, -dir.x ) * clk, pc, p0 + Pt( -dir.y, dir.x ) * clk );

                      if( dir.y == 0 )
                          tri( p0 + Pt( dir.x, 0 ) * diam, p0 + Pt( dir.x, -1 ) * diam, p0 );
                      else
                          tri( p0 + Pt( 0, dir.y ) * diam, p0 + Pt( -1, dir.y ) * diam, p0 );

                      line( p0, tip );
                      break;
                  }
                  case PinShape::CLOCK:
                      line( p0, tip );

                      if( dir.y == 0 )
                          tri( p0 + Pt( 0, clk ), p0 + Pt( -dir.x * clk, 0 ), p0 + Pt( 0, -clk ) );
                      else
                          tri( p0 + Pt( clk, 0 ), p0 + Pt( 0, -dir.y * clk ), p0 + Pt( -clk, 0 ) );

                      break;
                  case PinShape::INPUT_LOW:
                      line( p0, tip );

                      if( dir.y == 0 )
                          tri( p0 + Pt( dir.x, 0 ) * diam, p0 + Pt( dir.x, -1 ) * diam, p0 );
                      else
                          tri( p0 + Pt( 0, dir.y ) * diam, p0 + Pt( -1, dir.y ) * diam, p0 );

                      break;
                  case PinShape::OUTPUT_LOW:
                      line( p0, tip );

                      if( dir.y == 0 )
                          line( p0 - Pt( 0, diam ), p0 + Pt( dir.x, 0 ) * diam );
                      else
                          line( p0 - Pt( diam, 0 ), p0 + Pt( 0, dir.y ) * diam );

                      break;
                  case PinShape::NONLOGIC:
                      line( p0, tip );
                      line( p0 - Pt( dir.x + dir.y, dir.y - dir.x ) * r, p0 + Pt( dir.x + dir.y, dir.y - dir.x ) * r );
                      line( p0 - Pt( dir.x - dir.y, dir.x + dir.y ) * r, p0 + Pt( dir.x - dir.y, dir.x + dir.y ) * r );
                      break;
                  }
              } );

        // texts (KiCad 10 pin layout)
        const bool   horizontal = dir.y == 0;
        const int    orient = dir.x < 0 ? 0 : dir.x > 0 ? 180 : dir.y < 0 ? 270 : 90; // direction body -> tip
        // KiCad's draw orientation: PIN_RIGHT when the body is to the right of the tip
        const bool   right = horizontal && dir.x < 0, left = horizontal && dir.x > 0;
        const bool   up = !horizontal && dir.y > 0, down = !horizontal && dir.y < 0;
        (void) orient;
        (void) down;
        const double textOffset = std::round( 24 * TEXT_OFFSET_RATIO ) * MIL;
        const double halfLen = pin.length / 2;
        const bool   showNames = lib.showPinNames && !pin.name.empty();
        const bool   showNumbers = lib.showPinNumbers && !pin.number.empty();
        const bool   nameInside = lib.pinNameOffset > 0;
        SchTextAttr  nameAttr, numAttr;
        nameAttr.sizeX = nameAttr.sizeY = pin.nameSize;
        numAttr.sizeX = numAttr.sizeY = pin.numberSize;
        const double namePen = std::min( DEFAULT_LINE, pin.nameSize * 0.18 );
        const double numPen = std::min( DEFAULT_LINE, pin.numberSize * 0.18 );

        // along the pin, from the tip towards the body
        auto along = [&]( double d ) { return tip - dir * d; };

        if( showNumbers )
        {
            double perp = pin.numberSize / 2 + textOffset + PIN_TEXT_MARGIN + numPen;
            bool   both = showNames && !nameInside;
            Pt     pos = along( halfLen );

            if( horizontal )
                pos.y += both ? perp : -perp;
            else
                pos.x += both ? perp : -perp;

            draw( L_PINTEXT, S_TEXT, C_PINNUM, [&]( Acc& a )
                  { text( a, pin.number, pos, horizontal ? 0 : 90, numAttr, HAlign::CENTER, VAlign::CENTER, numPen ); } );
        }

        if( showNames )
        {
            Pt     pos;
            double angle = horizontal ? 0 : 90;
            HAlign h = HAlign::CENTER;

            if( nameInside )
            {
                pos = along( pin.length + lib.pinNameOffset );
                // text starts at the body edge and runs away from the pin
                h = right ? HAlign::LEFT : left ? HAlign::RIGHT : up ? HAlign::LEFT : HAlign::RIGHT;

                if( !horizontal )
                    h = dir.y > 0 ? HAlign::LEFT : HAlign::RIGHT;
            }
            else
            {
                double maxHalf = 0;

                for( const LibPin& p : lib.pins )
                {
                    if( !p.name.empty() )
                        maxHalf = std::max( maxHalf, p.nameSize / 2 );
                }

                double clearance = textOffset + PIN_TEXT_MARGIN;
                pos = along( halfLen );

                if( horizontal )
                    pos.y -= maxHalf + clearance + namePen;
                else
                    pos.x -= clearance + pin.nameSize / 2 + namePen;
            }

            draw( L_PINTEXT, S_TEXT, C_PINNAME, [&]( Acc& a ) { text( a, pin.name, pos, angle, nameAttr, h, VAlign::CENTER, namePen ); } );
        }
    }

    // ---- wires -------------------------------------------------------------------------

    void wires()
    {
        for( const SchLine& l : m_screen.lines )
        {
            beginItem( ItemType::SCH_WIRE );

            int    layer = l.kind == SchLine::BUS ? L_BUSES : l.kind == SchLine::WIRE ? L_WIRES : L_NOTES;
            Color  theme = l.kind == SchLine::BUS ? C_BUS : l.kind == SchLine::WIRE ? C_WIRE : C_NOTES;
            double w = l.stroke.width > 0 ? l.stroke.width : l.kind == SchLine::BUS ? BUS_WIDTH : WIRE_WIDTH;
            Color  col = pick( l.stroke.color, theme );
            draw( layer, S_LINE, col, [&]( Acc& a ) { strokePath( a, { l.a, l.b }, false, w, l.stroke.style ); } );
        }

        endType( ItemType::SCH_WIRE );
    }

    bool onBus( const Pt& p ) const
    {
        for( const SchLine& l : m_screen.lines )
        {
            if( l.kind == SchLine::BUS && PointSegmentDistance( p, l.a, l.b ) < 1e-6 )
                return true;
        }

        return false;
    }

    void junctions()
    {
        for( const SchJunction& j : m_screen.junctions )
        {
            beginItem( ItemType::SCH_JUNCTION );
            double d = j.diameter > 0 ? j.diameter : JUNCTION_DIAM;
            bool   bus = onBus( j.pos );
            Color  col = pick( j.color, bus ? C_BUS : C_JUNCTION );
            draw( bus ? L_BUSES : L_WIRES, S_TEXT, col, [&]( Acc& a ) { a.circle( j.pos, d / 2 ); } );
        }

        endType( ItemType::SCH_JUNCTION );
    }

    // ---- labels ------------------------------------------------------------------------

    void labels()
    {
        for( const SchLabel& L : m_screen.labels )
        {
            beginItem( ItemType::SCH_LABEL );
            label( L );
        }

        endType( ItemType::SCH_LABEL );
    }

    static double spinAngle( Spin s )
    {
        return s == Spin::UP || s == Spin::BOTTOM ? 90 : 0;
    }

    static HAlign spinAlign( Spin s )
    {
        return s == Spin::RIGHT || s == Spin::UP ? HAlign::LEFT : HAlign::RIGHT;
    }

    void label( const SchLabel& L )
    {
        const double pen = textPen( L.attr );
        const double h = L.attr.sizeY;
        const std::string shown = DisplayNetName( resolve( L.text ) );
        const double angle = spinAngle( L.spin );
        const HAlign align = spinAlign( L.spin );

        switch( L.kind )
        {
        case SchLabel::LOCAL:
        {
            double dist = std::round( TEXT_OFFSET_RATIO * h * 1e4 ) / 1e4 + pen;
            Pt     off = angle == 90 ? Pt( -dist, 0 ) : Pt( 0, -dist );
            Color  col = pick( L.attr.color, C_LOCLABEL );
            draw( L_LABELS, S_TEXT, col, [&]( Acc& a ) { text( a, shown, L.pos + off, angle, L.attr, align, VAlign::BOTTOM, pen ); } );
            break;
        }
        case SchLabel::GLOBAL:
        {
            double margin = LABEL_SIZE_RATIO * h;
            double halfSize = h / 2 + margin;
            double symLen = extents( shown, L.attr, pen ).x + 2 * margin;
            double x = symLen + pen, y = halfSize + pen;
            Path   pts = { Pt( 0, 0 ), Pt( 0, -y ), Pt( -x, -y ), Pt( -x, 0 ), Pt( -x, y ), Pt( 0, y ) };
            double xoff = 0;

            switch( L.shape )
            {
            case LabelShape::INPUT:
                xoff = -halfSize;
                pts[0].x += halfSize;
                break;
            case LabelShape::OUTPUT: pts[3].x -= halfSize; break;
            case LabelShape::BIDI:
            case LabelShape::TRISTATE:
                xoff = -halfSize;
                pts[0].x += halfSize;
                pts[3].x -= halfSize;
                break;
            default: break;
            }

            for( Pt& p : pts )
                p = L.pos + spinRotate( Pt( p.x + xoff, p.y ), L.spin );

            // text offset (horizontal margin, plus the arrow for input/bidi/tri-state)
            double horiz = margin;

            if( L.shape == LabelShape::INPUT || L.shape == LabelShape::BIDI || L.shape == LabelShape::TRISTATE )
                horiz += h * 3 / 4;

            double vert = h * 0.0715;
            Pt     off = L.spin == Spin::LEFT    ? Pt( -horiz, vert )
                         : L.spin == Spin::UP    ? Pt( vert, -horiz )
                         : L.spin == Spin::RIGHT ? Pt( horiz, vert )
                                                 : Pt( vert, horiz );
            Color  col = pick( L.attr.color, C_GLOBLABEL );
            draw( L_LABELS, S_LINE, col, [&]( Acc& a ) { a.polyline( pts, pen, true ); } );
            draw( L_LABELS, S_TEXT, col, [&]( Acc& a ) { text( a, shown, L.pos + off, angle, L.attr, align, VAlign::CENTER, pen ); } );
            break;
        }
        case SchLabel::HIERARCHICAL:
        {
            Color col = pick( L.attr.color, C_HIERLABEL );
            hierShape( L.pos, L.shape, L.spin, h, pen, col, L_LABELS );
            double dist = TEXT_OFFSET_RATIO * h + L.attr.sizeX;
            Pt     off = L.spin == Spin::LEFT    ? Pt( -dist, 0 )
                         : L.spin == Spin::UP    ? Pt( 0, -dist )
                         : L.spin == Spin::RIGHT ? Pt( dist, 0 )
                                                 : Pt( 0, dist );
            draw( L_LABELS, S_TEXT, col, [&]( Acc& a ) { text( a, shown, L.pos + off, angle, L.attr, align, VAlign::CENTER, pen ); } );
            break;
        }
        case SchLabel::DIRECTIVE:
        {
            double sym = 0.75 * h, len = L.pinLength;
            Color  col = pick( L.attr.color, C_NETCLASS );
            draw( L_LABELS, S_LINE, col, [&]( Acc& a )
                  {
                      auto P = [&]( double x, double y ) { return L.pos + spinRotate( Pt( x, y ), L.spin ); };

                      switch( L.shape )
                      {
                      case LabelShape::DOT:
                          a.capsule( P( 0, 0 ), P( 0, len - 0.7 * sym ), pen );
                          a.circle( P( 0, len ), 0.7 * sym );
                          break;
                      case LabelShape::DIAMOND:
                          a.capsule( P( 0, 0 ), P( 0, len - sym ), pen );
                          a.polyline( { P( 0, len - sym ), P( -2 * sym, len ), P( 0, len + sym ), P( 2 * sym, len ) }, pen, true );
                          break;
                      case LabelShape::RECTANGLE:
                      {
                          double s = 0.8 * sym;
                          a.capsule( P( 0, 0 ), P( 0, len - s ), pen );
                          a.polyline( { P( -2 * s, len - s ), P( -2 * s, len + s ), P( 2 * s, len + s ), P( 2 * s, len - s ) }, pen, true );
                          break;
                      }
                      default:
                          a.capsule( P( 0, 0 ), P( 0, len - sym ), pen );
                          a.ring( P( 0, len ), sym - pen / 2, sym + pen / 2 );
                          break;
                      }
                  } );
            break;
        }
        }

        // label fields (net class on directive labels, intersheet references when shown)
        for( const SchField& f : L.fields )
        {
            if( f.attr.hidden )
                continue;

            std::string value = resolve( f.value );

            if( value.empty() )
                continue;

            if( f.showName )
                value = f.name + ": " + value;

            double pen2 = textPen( f.attr );
            Pt     c = boxCentre( value, f.pos, f.angle, f.attr, pen2 );
            Color  col = pick( f.attr.color, C_FIELDS );
            draw( L_LABELS, S_TEXT, col, [&]( Acc& a )
                  { text( a, value, c, std::fabs( std::remainder( f.angle, 180.0 ) ) > 45 ? 90 : 0, f.attr, HAlign::CENTER, VAlign::CENTER, pen2 ); } );
        }
    }

    void hierShape( const Pt& pos, LabelShape shape, Spin spin, double h, double pen, const Color& col, int layer )
    {
        static const int IN[4][13] = { { 6, 0, 0, -1, -1, -2, -1, -2, 1, -1, 1, 0, 0 },
                                       { 6, 0, 0, 1, -1, 1, -2, -1, -2, -1, -1, 0, 0 },
                                       { 6, 0, 0, 1, 1, 2, 1, 2, -1, 1, -1, 0, 0 },
                                       { 6, 0, 0, 1, 1, 1, 2, -1, 2, -1, 1, 0, 0 } };
        static const int OUT[4][13] = { { 6, -2, 0, -1, 1, 0, 1, 0, -1, -1, -1, -2, 0 },
                                        { 6, 0, -2, 1, -1, 1, 0, -1, 0, -1, -1, 0, -2 },
                                        { 6, 2, 0, 1, -1, 0, -1, 0, 1, 1, 1, 2, 0 },
                                        { 6, 0, 2, 1, 1, 1, 0, -1, 0, -1, 1, 0, 2 } };
        static const int BIDI[4][11] = { { 5, 0, 0, -1, -1, -2, 0, -1, 1, 0, 0 },
                                         { 5, 0, 0, -1, -1, 0, -2, 1, -1, 0, 0 },
                                         { 5, 0, 0, 1, -1, 2, 0, 1, 1, 0, 0 },
                                         { 5, 0, 0, -1, 1, 0, 2, 1, 1, 0, 0 } };
        static const int UNSPC[4][11] = { { 5, 0, -1, -2, -1, -2, 1, 0, 1, 0, -1 },
                                          { 5, 1, 0, 1, -2, -1, -2, -1, 0, 1, 0 },
                                          { 5, 0, -1, 2, -1, 2, 1, 0, 1, 0, -1 },
                                          { 5, 1, 0, 1, 2, -1, 2, -1, 0, 1, 0 } };
        int         s = (int) spin;
        const int*  t = shape == LabelShape::INPUT    ? IN[s]
                        : shape == LabelShape::OUTPUT ? OUT[s]
                        : shape == LabelShape::BIDI || shape == LabelShape::TRISTATE ? BIDI[s]
                                                                                       : UNSPC[s];
        int         n = t[0];
        double      half = h / 2;
        Path        pts;

        for( int i = 0; i < n; ++i )
            pts.push_back( pos + Pt( half * t[1 + 2 * i], half * t[2 + 2 * i] ) );

        draw( layer, S_LINE, col, [&]( Acc& a ) { a.polyline( pts, pen, false ); } );
    }

    // ---- sheets ------------------------------------------------------------------------

    void sheets()
    {
        for( const SchSheet& sh : m_screen.sheets )
        {
            beginItem( ItemType::SCH_SHEET );
            Path  box = { sh.pos, sh.pos + Pt( sh.size.x, 0 ), sh.pos + sh.size, sh.pos + Pt( 0, sh.size.y ) };
            Color line = pick( sh.stroke.color, C_SHEET );

            if( sh.fill[3] > 0 )
                draw( L_SHEETS, S_FILL, sh.fill, [&]( Acc& a ) { a.convex( box ); } );

            double w = sh.stroke.width > 0 ? sh.stroke.width : DEFAULT_LINE;
            draw( L_SHEETS, S_LINE, line, [&]( Acc& a ) { strokePath( a, box, true, w, sh.stroke.style ); } );

            for( const SchField& f : sh.fields )
            {
                if( f.attr.hidden )
                    continue;

                std::string value = resolve( f.value );

                if( f.name == "Sheetfile" )
                    value = std::string( Tr( "File" ) ) + ": " + value;
                else if( f.name != "Sheetname" && f.showName )
                    value = f.name + ": " + value;

                if( value.empty() )
                    continue;

                double pen = textPen( f.attr );
                Pt     c = boxCentre( value, f.pos, f.angle, f.attr, pen );
                Color  theme = f.name == "Sheetname" ? C_SHEETNAME : f.name == "Sheetfile" ? C_SHEETFILE : C_SHEETFIELDS;
                Color  col = pick( f.attr.color, theme );
                draw( L_SHEETS, S_TEXT, col, [&]( Acc& a )
                      { text( a, value, c, std::fabs( std::remainder( f.angle, 180.0 ) ) > 45 ? 90 : 0, f.attr, HAlign::CENTER, VAlign::CENTER, pen ); } );
            }
        }

        endType( ItemType::SCH_SHEET );

        for( const SchSheet& sh : m_screen.sheets )
        {
            for( const SchSheetPin& p : sh.pins )
            {
                beginItem( ItemType::SCH_SHEET_PIN );
                double     pen = textPen( p.attr );
                double     h = p.attr.sizeY;
                Color      col = pick( p.attr.color, C_SHEETLABEL );
                // sheet pins use the hierarchical label shapes with input and output swapped
                LabelShape shape = p.shape == LabelShape::INPUT    ? LabelShape::OUTPUT
                                   : p.shape == LabelShape::OUTPUT ? LabelShape::INPUT
                                                                   : p.shape;
                hierShape( p.pos, shape, p.spin, h, pen, col, L_SHEETS );
                double dist = TEXT_OFFSET_RATIO * h + p.attr.sizeX;
                Pt     off = p.spin == Spin::LEFT    ? Pt( -dist, 0 )
                             : p.spin == Spin::UP    ? Pt( 0, -dist )
                             : p.spin == Spin::RIGHT ? Pt( dist, 0 )
                                                     : Pt( 0, dist );
                std::string name = DisplayNetName( resolve( p.name ) );
                draw( L_SHEETS, S_TEXT, col, [&]( Acc& a )
                      { text( a, name, p.pos + off, spinAngle( p.spin ), p.attr, spinAlign( p.spin ), VAlign::CENTER, pen ); } );
            }
        }

        endType( ItemType::SCH_SHEET_PIN );
    }

    // ---- notes -------------------------------------------------------------------------

    void notes()
    {
        auto identity = []( const Pt& p ) { return p; };

        for( const SchText& t : m_screen.texts )
        {
            beginItem( ItemType::SCH_NOTE );

            if( t.isPrivate )
                continue;

            double      pen = textPen( t.attr );
            Color       col = pick( t.attr.color, C_NOTES );
            std::string shown = resolve( t.text );
            draw( L_NOTES, S_TEXT, col, [&]( Acc& a ) { text( a, shown, t.pos, t.angle, t.attr, t.attr.hAlign, t.attr.vAlign, pen ); } );
        }

        for( const SchGraphic& g : m_screen.graphics )
        {
            beginItem( ItemType::SCH_NOTE );

            if( !g.isPrivate )
                graphic( L_NOTES, g, identity, C_NOTES, Color { 0, 0, 0, 0 }, 0 );
        }

        for( const SchImage& im : m_screen.images )
        {
            beginItem( ItemType::SCH_NOTE );
            double hw = im.width > 0 ? im.width / 2 : 5, hh = im.height > 0 ? im.height / 2 : 5;
            Path   box = { im.pos + Pt( -hw, -hh ), im.pos + Pt( hw, -hh ), im.pos + Pt( hw, hh ), im.pos + Pt( -hw, hh ) };
            draw( L_NOTES, S_FILL, css( 200, 200, 200, 0.35f ), [&]( Acc& a ) { a.convex( box ); } );
            draw( L_NOTES, S_LINE, css( 150, 150, 150 ), [&]( Acc& a )
                  {
                      a.polyline( box, DEFAULT_LINE, true );
                      a.capsule( box[0], box[2], DEFAULT_LINE );
                      a.capsule( box[1], box[3], DEFAULT_LINE );
                  } );
        }

        for( const SchNoConnect& nc : m_screen.noConnects )
        {
            beginItem( ItemType::SCH_NOTE );
            double s = NOCONNECT_SIZE / 2;
            draw( L_WIRES, S_LINE, C_NOCONNECT, [&]( Acc& a )
                  {
                      a.capsule( nc.pos + Pt( -s, -s ), nc.pos + Pt( s, s ), DEFAULT_LINE );
                      a.capsule( nc.pos + Pt( s, -s ), nc.pos + Pt( -s, s ), DEFAULT_LINE );
                  } );
        }

        for( const SchBusEntry& b : m_screen.busEntries )
        {
            beginItem( ItemType::SCH_NOTE );
            double w = b.stroke.width > 0 ? b.stroke.width : WIRE_WIDTH;
            Color  col = pick( b.stroke.color, C_WIRE );
            draw( L_WIRES, S_LINE, col, [&]( Acc& a ) { strokePath( a, { b.pos, b.pos + b.size }, false, w, b.stroke.style ); } );
        }

        endType( ItemType::SCH_NOTE );
    }

    // ---- output ------------------------------------------------------------------------

    void merge()
    {
        std::vector<uint32_t> groupStart( m_accs.size(), 0 );
        std::vector<size_t>   order( m_accs.size() );

        for( size_t i = 0; i < order.size(); ++i )
            order[i] = i;

        // slots back to front; inside a slot top-most first (first painter wins per slot)
        std::stable_sort( order.begin(), order.end(),
                          [&]( size_t a, size_t b )
                          {
                              const AccKey& ka = m_keys[a];
                              const AccKey& kb = m_keys[b];

                              if( ka.layer != kb.layer )
                                  return LAYERS[ka.layer].sort < LAYERS[kb.layer].sort;

                              return ka.sub > kb.sub;
                          } );

        for( size_t id : order )
        {
            Acc& a = m_accs[id];

            if( a.i.empty() )
                continue;

            DrawGroup g;
            g.slot = m_keys[id].layer;
            g.kind = ItemKind::GRAPHIC;
            g.color = m_keys[id].color;

            uint32_t base = (uint32_t) ( m_mesh.vertices.size() / 2 );
            g.indexStart = (uint32_t) m_mesh.indices.size();
            g.indexCount = (uint32_t) a.i.size();
            groupStart[id] = g.indexStart;
            m_mesh.vertices.insert( m_mesh.vertices.end(), a.v.begin(), a.v.end() );

            for( uint32_t idx : a.i )
                m_mesh.indices.push_back( base + idx );

            m_mesh.slots[(size_t) g.slot].indexCount += g.indexCount;
            m_mesh.groups.push_back( g );
        }

        m_mesh.ranges.reserve( m_pending.size() );

        for( const Pending& p : m_pending )
        {
            ItemRange r;
            r.start = groupStart[p.acc] + p.start;
            r.count = p.count;
            r.slot = m_keys[p.acc].layer;
            r.kind = (uint8_t) ItemKind::GRAPHIC;
            m_mesh.ranges.push_back( r );
        }

        m_mesh.bounds = BBox();
        m_mesh.bounds.add( Pt( 0, 0 ) );
        m_mesh.bounds.add( Pt( m_screen.paperW, m_screen.paperH ) );

        for( size_t i = 0; i + 1 < m_mesh.vertices.size(); i += 2 )
        {
            // content outside the page widens the view (rare); sample sparsely
            if( ( i / 2 ) % 7 == 0 )
                m_mesh.bounds.add( Pt( m_mesh.vertices[i], m_mesh.vertices[i + 1] ) );
        }
    }

    const Schematic& m_sch;
    const SheetNode& m_node;
    const SchScreen& m_screen;
    Mesh&            m_mesh;

    std::vector<Acc>      m_accs;
    std::vector<AccKey>   m_keys;
    std::map<AccKey, size_t> m_accIndex;
    size_t                m_lastAcc = 0;
    std::vector<Pending>  m_pending;
    bool                  m_recording = false;
    std::vector<int>      m_pinBase;
};

} // anonymous namespace


void SchematicPinPoints( const SchSymbol& aSymbol, const LibPin& aPin, Pt& aTip, Pt& aRoot )
{
    Pt root = aPin.pos;

    switch( aPin.orient )
    {
    case 90: root.y -= aPin.length; break;
    case 180: root.x -= aPin.length; break;
    case 270: root.y += aPin.length; break;
    default: root.x += aPin.length; break;
    }

    aTip = aSymbol.pos + aSymbol.transform.apply( aPin.pos );
    aRoot = aSymbol.pos + aSymbol.transform.apply( root );
}


std::vector<int> SchematicPinBase( const Schematic& aSch, const SchScreen& aScreen )
{
    std::vector<int> base( aScreen.symbols.size() + 1, 0 );

    for( size_t i = 0; i < aScreen.symbols.size(); ++i )
    {
        const LibSymbol* lib = aSch.Lib( aScreen, aScreen.symbols[i] );
        base[i + 1] = base[i] + ( lib ? (int) lib->pins.size() : 0 );
    }

    return base;
}


std::string SchematicShownReference( const Schematic& aSch, const SheetNode& aSheet, const SchSymbol& aSymbol )
{
    std::string      ref = aSch.Reference( aSheet, aSymbol );
    const SchScreen& sc = aSch.screens[(size_t) aSheet.screen];
    const LibSymbol* lib = aSch.Lib( sc, aSymbol );

    if( lib && lib->unitCount > 1 && !ref.empty() && ref.back() != '?' )
        ref += unitLetter( aSch.Unit( aSheet, aSymbol ) );
    else if( lib && lib->unitCount > 1 )
        ref += unitLetter( aSch.Unit( aSheet, aSymbol ) );

    return ref;
}


void BuildSchematicMesh( const Schematic& aSch, int aSheet, Mesh& aMesh )
{
    if( aSheet < 0 || aSheet >= (int) aSch.sheets.size() || aSch.sheets[(size_t) aSheet].screen < 0 )
    {
        aMesh = Mesh();
        return;
    }

    SchBuilder b( aSch, aSheet, aMesh );
    b.Build();
}

} // namespace kp
