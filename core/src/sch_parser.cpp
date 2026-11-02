/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Reader for KiCad 6 .. 10 schematic files (.kicad_sch) and the sheet hierarchy.
 */
#include "schematic.h"
#include "i18n.h"
#include "sexpr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace kp
{

namespace
{

constexpr double SCH_PI = 3.14159265358979323846;

std::string S( std::string_view v )
{
    return std::string( v );
}

Pt xy( const N& n, bool invertY = false )
{
    return Pt( n.num( 0 ), invertY ? -n.num( 1 ) : n.num( 1 ) );
}

Color parseColor( const N& n )
{
    if( !n )
        return { 0, 0, 0, 0 };

    return { (float) ( n.num( 0 ) / 255.0 ), (float) ( n.num( 1 ) / 255.0 ), (float) ( n.num( 2 ) / 255.0 ),
             (float) n.num( 3, 1.0 ) };
}

SchStroke parseStroke( const N& n )
{
    SchStroke s;

    if( !n )
        return s;

    if( N w = n.find( "width" ) )
        s.width = w.num( 0 );

    if( N t = n.find( "type" ) )
    {
        std::string_view v = t.str( 0 );
        s.style = v == "solid"          ? LineStyle::SOLID
                  : v == "dash"         ? LineStyle::DASH
                  : v == "dot"          ? LineStyle::DOT
                  : v == "dash_dot"     ? LineStyle::DASH_DOT
                  : v == "dash_dot_dot" ? LineStyle::DASH_DOT_DOT
                                        : LineStyle::DEFAULT;
    }

    if( N c = n.find( "color" ) )
        s.color = parseColor( c );

    return s;
}

void parseFill( const N& n, SchFill& aFill, Color& aColor )
{
    aFill = SchFill::NONE;

    if( !n )
        return;

    if( N t = n.find( "type" ) )
    {
        std::string_view v = t.str( 0 );
        aFill = v == "outline"      ? SchFill::OUTLINE
                : v == "background" ? SchFill::BACKGROUND
                : v == "color"      ? SchFill::COLOR
                : v == "hatch" || v == "reverse_hatch" || v == "cross_hatch" ? SchFill::HATCH
                                    : SchFill::NONE;
    }

    if( N c = n.find( "color" ) )
        aColor = parseColor( c );
}

/// "(effects (font (size h w) (thickness t) bold italic (color ..)) (justify left top mirror) hide)"
void parseEffects( const N& e, SchTextAttr& a, bool aDefaultCenter = true )
{
    if( !e )
        return;

    if( N f = e.find( "font" ) )
    {
        if( N sz = f.find( "size" ) )
        {
            a.sizeY = sz.num( 0, 1.27 );
            a.sizeX = sz.num( 1, a.sizeY );
        }

        if( N t = f.find( "thickness" ) )
            a.thickness = t.num( 0 );

        a.bold = f.flag( "bold" );
        a.italic = f.flag( "italic" );

        if( N c = f.find( "color" ) )
            a.color = parseColor( c );

        if( N ls = f.find( "line_spacing" ) )
            a.lineSpacing = ls.num( 0, 1.0 );
    }

    if( N j = e.find( "justify" ) )
    {
        if( aDefaultCenter )
        {
            a.hAlign = HAlign::CENTER;
            a.vAlign = VAlign::CENTER;
        }

        for( N c = j.first().next(); c; c = c.next() )
        {
            std::string_view v = c.text();

            if( v == "left" )
                a.hAlign = HAlign::LEFT;
            else if( v == "right" )
                a.hAlign = HAlign::RIGHT;
            else if( v == "top" )
                a.vAlign = VAlign::TOP;
            else if( v == "bottom" )
                a.vAlign = VAlign::BOTTOM;
            else if( v == "mirror" )
                a.mirror = true;
        }
    }

    if( e.flag( "hide" ) )
        a.hidden = true;
}

SchField parseField( const N& p, bool invertY = false )
{
    SchField f;
    f.name = S( p.str( 0 ) );
    f.value = S( p.str( 1 ) );

    if( N at = p.find( "at" ) )
    {
        f.pos = xy( at, invertY );
        f.angle = at.num( 2 );
    }

    parseEffects( p.find( "effects" ), f.attr );

    // newer files: (hide yes) and (show_name yes) next to the effects
    if( p.find( "hide" ) && p.flag( "hide" ) )
        f.attr.hidden = true;

    f.showName = p.flag( "show_name" );
    return f;
}

PinType pinType( std::string_view v )
{
    if( v == "input" ) return PinType::INPUT;
    if( v == "output" ) return PinType::OUTPUT;
    if( v == "bidirectional" ) return PinType::BIDI;
    if( v == "tri_state" ) return PinType::TRISTATE;
    if( v == "passive" ) return PinType::PASSIVE;
    if( v == "power_in" ) return PinType::POWER_IN;
    if( v == "power_out" ) return PinType::POWER_OUT;
    if( v == "open_collector" ) return PinType::OPEN_COLLECTOR;
    if( v == "open_emitter" ) return PinType::OPEN_EMITTER;
    if( v == "no_connect" || v == "unconnected" ) return PinType::NO_CONNECT;
    if( v == "free" ) return PinType::FREE;
    return PinType::UNSPECIFIED;
}

PinShape pinShape( std::string_view v )
{
    if( v == "inverted" ) return PinShape::INVERTED;
    if( v == "clock" ) return PinShape::CLOCK;
    if( v == "inverted_clock" ) return PinShape::INVERTED_CLOCK;
    if( v == "input_low" ) return PinShape::INPUT_LOW;
    if( v == "clock_low" ) return PinShape::CLOCK_LOW;
    if( v == "output_low" ) return PinShape::OUTPUT_LOW;
    if( v == "edge_clock_high" ) return PinShape::FALLING_EDGE_CLOCK;
    if( v == "non_logic" ) return PinShape::NONLOGIC;
    return PinShape::LINE;
}

LabelShape labelShape( std::string_view v )
{
    if( v == "input" ) return LabelShape::INPUT;
    if( v == "output" ) return LabelShape::OUTPUT;
    if( v == "bidirectional" ) return LabelShape::BIDI;
    if( v == "tri_state" ) return LabelShape::TRISTATE;
    if( v == "dot" ) return LabelShape::DOT;
    if( v == "round" ) return LabelShape::ROUND;
    if( v == "diamond" ) return LabelShape::DIAMOND;
    if( v == "rectangle" ) return LabelShape::RECTANGLE;
    return LabelShape::PASSIVE;
}

/// Centre of the circle through three points (false if they are collinear).
bool circleCenter( const Pt& a, const Pt& b, const Pt& c, Pt& out )
{
    double d = 2 * ( a.x * ( b.y - c.y ) + b.x * ( c.y - a.y ) + c.x * ( a.y - b.y ) );

    if( std::fabs( d ) < 1e-12 )
        return false;

    double a2 = a.x * a.x + a.y * a.y, b2 = b.x * b.x + b.y * b.y, c2 = c.x * c.x + c.y * c.y;
    out = Pt( ( a2 * ( b.y - c.y ) + b2 * ( c.y - a.y ) + c2 * ( a.y - b.y ) ) / d,
              ( a2 * ( c.x - b.x ) + b2 * ( a.x - c.x ) + c2 * ( b.x - a.x ) ) / d );
    return true;
}

/// Swept angle (radians, signed) of the arc start -> mid -> end around its centre.
double arcSweep( const Pt& s, const Pt& m, const Pt& e, const Pt& c )
{
    double a0 = std::atan2( s.y - c.y, s.x - c.x );
    double am = std::atan2( m.y - c.y, m.x - c.x );
    double a1 = std::atan2( e.y - c.y, e.x - c.x );
    auto   norm = []( double a )
    {
        while( a < 0 )
            a += 2 * SCH_PI;

        while( a >= 2 * SCH_PI )
            a -= 2 * SCH_PI;

        return a;
    };
    double ccw = norm( a1 - a0 ), toMid = norm( am - a0 );
    return toMid <= ccw ? ccw : ccw - 2 * SCH_PI;
}

/// Mid point of the arc from s to e around c going the short way (or the long way).
Pt arcMid( const Pt& s, const Pt& e, const Pt& c, bool aShort )
{
    double r = std::hypot( s.x - c.x, s.y - c.y );
    double a0 = std::atan2( s.y - c.y, s.x - c.x ), a1 = std::atan2( e.y - c.y, e.x - c.x );
    double d = a1 - a0;

    while( d <= -SCH_PI )
        d += 2 * SCH_PI;

    while( d > SCH_PI )
        d -= 2 * SCH_PI;

    if( !aShort )
        d = d > 0 ? d - 2 * SCH_PI : d + 2 * SCH_PI;

    return Pt( c.x + r * std::cos( a0 + d / 2 ), c.y + r * std::sin( a0 + d / 2 ) );
}

class Parser
{
public:
    explicit Parser( SchScreen& aScreen ) :
            m_s( aScreen )
    {
    }

    void Parse( const char* aData, size_t aLen )
    {
        SReader          r( aData, aLen );
        std::string_view root = r.OpenRoot();

        if( root != "kicad_sch" )
            throw ParseError( Tr( "Not a KiCad schematic file" ), 0 );

        SPool pool;

        while( N item = r.NextTopLevel( pool ) )
        {
            if( !item.isList() )
                continue;

            topLevel( item );
        }
    }

private:
    void topLevel( const N& n )
    {
        std::string_view k = n.head();

        if( k == "version" )
            m_s.version = (int) n.num( 0 );
        else if( k == "generator" )
            m_s.generator = S( n.str( 0 ) );
        else if( k == "generator_version" )
            m_s.generatorVersion = S( n.str( 0 ) );
        else if( k == "uuid" )
            m_s.uuid = S( n.str( 0 ) );
        else if( k == "paper" || ( k == "page" && !n.arg( 0 ).isNumber() ) )
            paper( n ); // (page "A4") in early KiCad 6 files; (page 1 2) is a page number
        else if( k == "title_block" )
            titleBlock( n );
        else if( k == "lib_symbols" )
        {
            n.each( "symbol", [&]( const N& s ) { libSymbol( s ); } );
        }
        else if( k == "symbol" )
            symbol( n );
        else if( k == "wire" || k == "bus" )
            wire( n, k == "wire" ? SchLine::WIRE : SchLine::BUS );
        else if( k == "polyline" )
            schPolyline( n );
        else if( k == "junction" )
        {
            SchJunction j;
            j.pos = xy( n.find( "at" ) );

            if( N d = n.find( "diameter" ) )
                j.diameter = d.num( 0 );

            if( N c = n.find( "color" ) )
                j.color = parseColor( c );

            m_s.junctions.push_back( j );
        }
        else if( k == "no_connect" )
            m_s.noConnects.push_back( { xy( n.find( "at" ) ) } );
        else if( k == "bus_entry" )
        {
            SchBusEntry b;
            b.pos = xy( n.find( "at" ) );

            if( N sz = n.find( "size" ) )
                b.size = xy( sz );

            b.stroke = parseStroke( n.find( "stroke" ) );
            m_s.busEntries.push_back( b );
        }
        else if( k == "label" || k == "global_label" || k == "hierarchical_label" || k == "netclass_flag"
                 || k == "directive_label" )
            label( n );
        else if( k == "text" )
            text( n );
        else if( k == "text_box" )
            m_s.graphics.push_back( textBox( n, false ) );
        else if( k == "rectangle" || k == "circle" || k == "arc" || k == "bezier" || k == "rule_area" )
            schShape( n );
        else if( k == "table" )
            table( n );
        else if( k == "image" )
            image( n );
        else if( k == "sheet" )
            sheet( n );
        else if( k == "sheet_instances" )
        {
            n.each( "path", [&]( const N& p )
                    {
                        if( N pg = p.find( "page" ) )
                            m_s.legacySheetPages[S( p.str( 0 ) )] = S( pg.str( 0 ) );
                    } );
        }
        else if( k == "symbol_instances" )
        {
            n.each( "path", [&]( const N& p )
                    {
                        SymbolInstance in;
                        in.path = S( p.str( 0 ) );

                        if( N r = p.find( "reference" ) )
                            in.reference = S( r.str( 0 ) );

                        if( N u = p.find( "unit" ) )
                            in.unit = (int) u.num( 0, 1 );

                        if( N v = p.find( "value" ) )
                            in.value = S( v.str( 0 ) );

                        if( N f = p.find( "footprint" ) )
                            in.footprint = S( f.str( 0 ) );

                        m_s.legacySymbolInstances[in.path] = in;
                    } );
        }
        // bus_alias, group, embedded_fonts, embedded_files, net chains: nothing to draw
    }

    void paper( const N& n )
    {
        std::string name = S( n.str( 0 ) );
        double      w = 297, h = 210;

        if( name == "User" )
        {
            w = n.num( 1, 297 );
            h = n.num( 2, 210 );
        }
        else if( !PaperSize( name, w, h ) )
        {
            m_s.warnings.push_back( TrF( "Unknown paper size {0}", { name } ) );
        }

        if( n.hasAtom( "portrait" ) && w > h )
            std::swap( w, h );

        m_s.paper = name;
        m_s.paperW = w;
        m_s.paperH = h;
    }

    void titleBlock( const N& n )
    {
        TitleBlock& t = m_s.titleBlock;

        for( N c = n.first().next(); c; c = c.next() )
        {
            std::string_view k = c.head();

            if( k == "title" )
                t.title = S( c.str( 0 ) );
            else if( k == "date" )
                t.date = S( c.str( 0 ) );
            else if( k == "rev" )
                t.rev = S( c.str( 0 ) );
            else if( k == "company" )
                t.company = S( c.str( 0 ) );
            else if( k == "comment" )
            {
                int i = (int) c.num( 0 );

                if( i >= 1 && i <= 9 )
                    t.comments[(size_t) i - 1] = S( c.str( 1 ) );
            }
        }
    }

    // ---- library symbols ---------------------------------------------------------------

    void libSymbol( const N& n )
    {
        LibSymbol L;
        L.id = S( n.str( 0 ) );

        size_t esc;

        while( ( esc = L.id.find( "{slash}" ) ) != std::string::npos )
            L.id.replace( esc, 7, "/" );

        // unit names "Name_unit_style" use the name without the library prefix
        std::string base = L.id.substr( L.id.find( ':' ) == std::string::npos ? 0 : L.id.find( ':' ) + 1 );

        for( N c = n.first().next(); c; c = c.next() )
        {
            if( !c.isList() )
                continue;

            std::string_view k = c.head();

            if( k == "power" )
            {
                L.power = true;
                L.localPower = c.str( 0 ) == "local";
            }
            else if( k == "pin_names" )
            {
                if( N o = c.find( "offset" ) )
                    L.pinNameOffset = o.num( 0 );

                if( c.hasAtom( "hide" ) || ( c.find( "hide" ) && c.flag( "hide" ) ) )
                    L.showPinNames = false;
            }
            else if( k == "pin_numbers" )
            {
                if( c.hasAtom( "hide" ) || ( c.find( "hide" ) && c.flag( "hide" ) ) )
                    L.showPinNumbers = false;
            }
            else if( k == "extends" )
                L.extends = S( c.str( 0 ) );
            else if( k == "property" )
                L.fields.push_back( parseField( c, true ) );
            else if( k == "symbol" )
            {
                std::string name = S( c.str( 0 ) );
                int         unit = 1, style = 1;
                size_t      u = name.rfind( '_' );

                if( u != std::string::npos && u > 0 )
                {
                    size_t v = name.rfind( '_', u - 1 );

                    if( v != std::string::npos )
                    {
                        unit = std::atoi( name.c_str() + v + 1 );
                        style = std::atoi( name.c_str() + u + 1 );
                    }
                }

                L.unitCount = std::max( L.unitCount, unit );
                L.bodyStyleCount = std::max( L.bodyStyleCount, style );

                for( N d = c.first().next(); d; d = d.next() )
                {
                    if( !d.isList() )
                        continue;

                    if( d.head() == "unit_name" )
                        L.unitNames[unit] = S( d.str( 0 ) );
                    else
                        libItem( L, d, unit, style );
                }
            }
            else
            {
                libItem( L, c, 0, 0 );
            }
        }

        m_s.libSymbols[L.id] = std::move( L );
    }

    void libItem( LibSymbol& L, const N& d, int unit, int style )
    {
        std::string_view k = d.head();

        if( k == "pin" )
        {
            LibPin p;
            p.unit = unit;
            p.bodyStyle = style;
            p.type = pinType( d.str( 0 ) );
            p.shape = pinShape( d.str( 1 ) );

            if( N at = d.find( "at" ) )
            {
                p.pos = xy( at, true );
                p.orient = ( (int) std::lround( at.num( 2 ) ) % 360 + 360 ) % 360;
            }

            if( N len = d.find( "length" ) )
                p.length = len.num( 0 );

            if( d.hasAtom( "hide" ) || ( d.find( "hide" ) && d.flag( "hide" ) ) )
                p.visible = false;

            if( N nm = d.find( "name" ) )
            {
                p.name = S( nm.str( 0 ) );

                if( p.name == "~" && m_s.version < 20250318 )
                    p.name.clear();

                SchTextAttr a;
                parseEffects( nm.find( "effects" ), a );
                p.nameSize = a.sizeY;
            }

            if( N nb = d.find( "number" ) )
            {
                p.number = S( nb.str( 0 ) );

                if( p.number == "~" && m_s.version < 20250318 )
                    p.number.clear();

                SchTextAttr a;
                parseEffects( nb.find( "effects" ), a );
                p.numberSize = a.sizeY;
            }

            L.pins.push_back( p );
            return;
        }

        SchGraphic g;
        g.unit = unit;
        g.bodyStyle = style;

        if( !graphic( g, d, true ) )
            return;

        L.graphics.push_back( std::move( g ) );
    }

    /// Shapes and text common to library symbols (invertY) and sheets.
    bool graphic( SchGraphic& g, const N& d, bool invertY )
    {
        std::string_view k = d.head();
        g.stroke = parseStroke( d.find( "stroke" ) );
        parseFill( d.find( "fill" ), g.fill, g.fillColor );
        g.isPrivate = d.hasAtom( "private" ) || ( d.find( "private" ) && d.flag( "private" ) );

        if( k == "polyline" || k == "bezier" )
        {
            g.kind = k == "polyline" ? SchGraphic::POLY : SchGraphic::BEZIER;

            if( N pts = d.find( "pts" ) )
                pts.each( "xy", [&]( const N& p ) { g.pts.push_back( xy( p, invertY ) ); } );

            return g.pts.size() >= 2;
        }

        if( k == "rectangle" )
        {
            g.kind = SchGraphic::RECT;
            g.pts = { xy( d.find( "start" ), invertY ), xy( d.find( "end" ), invertY ) };

            if( N r = d.find( "radius" ) )
                g.cornerRadius = r.num( 0 );

            return true;
        }

        if( k == "circle" )
        {
            g.kind = SchGraphic::CIRCLE;
            g.center = xy( d.find( "center" ), invertY );
            g.radius = d.find( "radius" ).num( 0 );
            return g.radius > 0;
        }

        if( k == "arc" )
        {
            g.kind = SchGraphic::ARC;
            Pt s = xy( d.find( "start" ), invertY ), e = xy( d.find( "end" ), invertY );

            if( N m = d.find( "mid" ) )
            {
                Pt mid = xy( m, invertY ), c;
                g.start = s;
                g.mid = mid;
                g.end = e;

                // before 7.0, arcs of more than 180 degrees were stored by mistake: KiCad draws
                // the other part of the circle
                if( m_s.version <= 20230121 && circleCenter( s, mid, e, c )
                    && std::fabs( arcSweep( s, mid, e, c ) ) > SCH_PI + 1e-9 )
                {
                    g.mid = arcMid( s, e, c, true );
                }
            }
            else if( N r = d.find( "radius" ) )
            {
                // KiCad 6 development format: centre and angles, at most 180 degrees
                Pt c = xy( r.find( "at" ), invertY );
                g.start = s;
                g.end = e;
                g.mid = arcMid( s, e, c, true );
            }
            else
            {
                return false;
            }

            return true;
        }

        if( k == "text" )
        {
            g.kind = SchGraphic::TEXT;
            g.text = S( d.str( 0 ) );

            if( N at = d.find( "at" ) )
            {
                g.textPos = xy( at, invertY );
                // library text angles are in tenths of a degree
                g.textAngle = invertY ? at.num( 2 ) / 10.0 : at.num( 2 );
            }

            parseEffects( d.find( "effects" ), g.attr );
            return true;
        }

        if( k == "text_box" )
        {
            g = textBox( d, invertY );
            return true;
        }

        return false;
    }

    SchGraphic textBox( const N& d, bool invertY )
    {
        SchGraphic g;
        g.kind = SchGraphic::TEXTBOX;
        g.text = S( d.str( 0 ) );
        g.stroke = parseStroke( d.find( "stroke" ) );
        parseFill( d.find( "fill" ), g.fill, g.fillColor );
        g.attr.hAlign = HAlign::LEFT;
        g.attr.vAlign = VAlign::TOP;
        parseEffects( d.find( "effects" ), g.attr, false );

        Pt pos, end;

        if( N at = d.find( "at" ) )
        {
            pos = xy( at, invertY );
            g.textAngle = at.num( 2 );
        }

        if( N st = d.find( "start" ) )
            pos = xy( st, invertY );

        if( N en = d.find( "end" ) )
            end = xy( en, invertY );
        else if( N sz = d.find( "size" ) )
            end = pos + xy( sz, invertY );

        g.pts = { pos, end };

        double margin = 0.0;

        if( N m = d.find( "margins" ) )
        {
            for( int i = 0; i < 4; ++i )
                g.margins[i] = m.num( (uint32_t) i );
        }
        else
        {
            // KiCad: legacy margin of half the text size plus the border width
            margin = g.attr.sizeY * 0.75 + std::max( 0.0, g.stroke.width ) / 2;

            for( double& v : g.margins )
                v = margin;
        }

        return g;
    }

    // ---- sheet items -------------------------------------------------------------------

    void symbol( const N& n )
    {
        SchSymbol s;
        std::string libName;

        for( N c = n.first().next(); c; c = c.next() )
        {
            if( !c.isList() )
                continue;

            std::string_view k = c.head();

            if( k == "lib_name" )
                libName = S( c.str( 0 ) );
            else if( k == "lib_id" )
                s.libId = S( c.str( 0 ) );
            else if( k == "at" )
            {
                s.pos = xy( c );

                switch( ( (int) std::lround( c.num( 2 ) ) % 360 + 360 ) % 360 )
                {
                case 90: s.transform = { 0, 1, -1, 0 }; break;
                case 180: s.transform = { -1, 0, 0, -1 }; break;
                case 270: s.transform = { 0, -1, 1, 0 }; break;
                default: s.transform = { 1, 0, 0, 1 }; break;
                }
            }
            else if( k == "mirror" )
            {
                // composed after the rotation (KiCad SetOrientation: new = temp * old)
                for( N m = c.first().next(); m; m = m.next() )
                {
                    int tx1 = 1, ty2 = 1;

                    if( m.text() == "x" )
                        ty2 = -1;
                    else if( m.text() == "y" )
                        tx1 = -1;
                    else
                        continue;

                    SymTransform& t = s.transform;
                    t = { tx1 * t.x1, tx1 * t.y1, ty2 * t.x2, ty2 * t.y2 };
                }
            }
            else if( k == "unit" )
                s.unit = std::max( 1, (int) c.num( 0, 1 ) );
            else if( k == "convert" || k == "body_style" )
                s.bodyStyle = std::max( 1, (int) c.num( 0, 1 ) );
            else if( k == "in_bom" )
                s.inBom = n.flag( "in_bom", true );
            else if( k == "on_board" )
                s.onBoard = n.flag( "on_board", true );
            else if( k == "dnp" )
                s.dnp = n.flag( "dnp" );
            else if( k == "exclude_from_sim" )
                s.excludeFromSim = n.flag( "exclude_from_sim" );
            else if( k == "fields_autoplaced" )
                s.fieldsAutoplaced = true;
            else if( k == "uuid" )
                s.uuid = S( c.str( 0 ) );
            else if( k == "property" )
                s.fields.push_back( parseField( c ) );
            else if( k == "pin" )
            {
                if( N alt = c.find( "alternate" ) )
                    s.pinAlternates[S( c.str( 0 ) )] = S( alt.str( 0 ) );
            }
            else if( k == "instances" )
            {
                c.each( "project", [&]( const N& proj )
                        {
                            proj.each( "path", [&]( const N& p )
                                       {
                                           SymbolInstance in;
                                           in.path = S( p.str( 0 ) );

                                           if( N r = p.find( "reference" ) )
                                               in.reference = S( r.str( 0 ) );

                                           if( N u = p.find( "unit" ) )
                                               in.unit = (int) u.num( 0, 1 );

                                           s.instances.push_back( in );
                                       } );
                        } );
            }
        }

        if( !libName.empty() )
            s.libId = libName;

        m_s.symbols.push_back( std::move( s ) );
    }

    void wire( const N& n, SchLine::Kind kind )
    {
        Path pts;

        if( N p = n.find( "pts" ) )
            p.each( "xy", [&]( const N& q ) { pts.push_back( xy( q ) ); } );

        SchStroke st = parseStroke( n.find( "stroke" ) );

        for( size_t i = 0; i + 1 < pts.size(); ++i )
            m_s.lines.push_back( { kind, pts[i], pts[i + 1], st } );
    }

    void schPolyline( const N& n )
    {
        SchGraphic g;

        if( !graphic( g, n, false ) )
            return;

        // two points without a fill: a graphic line (notes layer)
        if( g.pts.size() == 2 && g.fill == SchFill::NONE )
        {
            m_s.lines.push_back( { SchLine::NOTES, g.pts[0], g.pts[1], g.stroke } );
            return;
        }

        m_s.graphics.push_back( std::move( g ) );
    }

    void schShape( const N& n )
    {
        if( n.head() == "rule_area" )
        {
            // drawn as its outline polyline
            if( N poly = n.find( "polyline" ) )
            {
                SchGraphic g;

                if( graphic( g, poly, false ) )
                {
                    g.stroke.style = LineStyle::DASH;
                    m_s.graphics.push_back( std::move( g ) );
                }
            }

            return;
        }

        SchGraphic g;

        if( graphic( g, n, false ) )
            m_s.graphics.push_back( std::move( g ) );
    }

    void label( const N& n )
    {
        SchLabel L;
        std::string_view k = n.head();
        L.kind = k == "label"                ? SchLabel::LOCAL
                 : k == "global_label"       ? SchLabel::GLOBAL
                 : k == "hierarchical_label" ? SchLabel::HIERARCHICAL
                                             : SchLabel::DIRECTIVE;
        L.text = S( n.str( 0 ) );

        if( L.kind == SchLabel::GLOBAL || L.kind == SchLabel::HIERARCHICAL )
            L.shape = LabelShape::INPUT;

        if( L.kind == SchLabel::DIRECTIVE )
            L.shape = LabelShape::ROUND;

        double angle = 0;

        if( N at = n.find( "at" ) )
        {
            L.pos = xy( at );
            angle = at.num( 2 );
        }

        if( N sh = n.find( "shape" ) )
            L.shape = labelShape( sh.str( 0 ) );

        if( N len = n.find( "length" ) )
            L.pinLength = len.num( 0 );

        if( N u = n.find( "uuid" ) )
            L.uuid = S( u.str( 0 ) );

        // spin from the angle (kept upright), then the justification decides the side
        int a = ( (int) std::lround( angle ) % 360 + 360 ) % 360;
        bool vertical = a == 90 || a == 270;
        L.attr.hAlign = a == 180 || a == 270 ? HAlign::RIGHT : HAlign::LEFT;
        L.attr.vAlign = VAlign::BOTTOM;
        parseEffects( n.find( "effects" ), L.attr, false );

        if( vertical )
            L.spin = L.attr.hAlign == HAlign::RIGHT ? Spin::BOTTOM : Spin::UP;
        else
            L.spin = L.attr.hAlign == HAlign::RIGHT ? Spin::LEFT : Spin::RIGHT;

        n.each( "property", [&]( const N& p ) { L.fields.push_back( parseField( p ) ); } );
        m_s.labels.push_back( std::move( L ) );
    }

    void text( const N& n )
    {
        SchText t;
        t.text = S( n.str( 0 ) );

        if( N at = n.find( "at" ) )
        {
            t.pos = xy( at );
            t.angle = at.num( 2 );
        }

        parseEffects( n.find( "effects" ), t.attr );
        t.attr.hidden = false; // hidden schematic text is not supported by KiCad either
        m_s.texts.push_back( std::move( t ) );
    }

    void table( const N& n )
    {
        // each cell is a text box; the borders come from the table's stroke
        SchStroke border;

        if( N b = n.find( "border" ) )
            border = parseStroke( b.find( "stroke" ) );

        if( N cells = n.find( "cells" ) )
        {
            cells.each( "table_cell", [&]( const N& c )
                        {
                            SchGraphic g = textBox( c, false );

                            if( g.stroke.width <= 0 )
                                g.stroke = border;

                            m_s.graphics.push_back( std::move( g ) );
                        } );
        }
    }

    void image( const N& n )
    {
        SchImage im;
        im.pos = xy( n.find( "at" ) );

        if( N sc = n.find( "scale" ) )
            im.scale = sc.num( 0, 1.0 );

        // size from the PNG header inside the base64 data (IHDR width/height, pHYs density)
        std::string b64;

        if( N data = n.find( "data" ) )
        {
            for( N c = data.first().next(); c && b64.size() < 400; c = c.next() )
                b64 += S( c.text() );
        }

        std::string bin = decodeBase64( b64.substr( 0, 400 ) );

        if( bin.size() >= 24 && bin.compare( 1, 3, "PNG" ) == 0 )
        {
            auto be32 = [&]( size_t o ) -> uint32_t
            {
                return ( (uint32_t) (uint8_t) bin[o] << 24 ) | ( (uint32_t) (uint8_t) bin[o + 1] << 16 )
                       | ( (uint32_t) (uint8_t) bin[o + 2] << 8 ) | (uint32_t) (uint8_t) bin[o + 3];
            };
            double ppi = 300.0; // KiCad's default when the image has no density
            size_t phys = bin.find( "pHYs" );

            if( phys != std::string::npos && phys + 13 <= bin.size() && (uint8_t) bin[phys + 12] == 1 )
                ppi = be32( phys + 4 ) * 0.0254;

            if( ppi < 10 )
                ppi = 300.0;

            im.width = be32( 16 ) / ppi * 25.4 * im.scale;
            im.height = be32( 20 ) / ppi * 25.4 * im.scale;
        }

        m_s.images.push_back( im );
    }

    static std::string decodeBase64( const std::string& in )
    {
        std::string out;
        int         val = 0, bits = -8;

        for( char ch : in )
        {
            int d;

            if( ch >= 'A' && ch <= 'Z' )
                d = ch - 'A';
            else if( ch >= 'a' && ch <= 'z' )
                d = ch - 'a' + 26;
            else if( ch >= '0' && ch <= '9' )
                d = ch - '0' + 52;
            else if( ch == '+' )
                d = 62;
            else if( ch == '/' )
                d = 63;
            else
                continue;

            val = ( val << 6 ) | d;
            bits += 6;

            if( bits >= 0 )
            {
                out.push_back( (char) ( ( val >> bits ) & 0xFF ) );
                bits -= 8;
            }
        }

        return out;
    }

    void sheet( const N& n )
    {
        SchSheet sh;
        sh.pos = xy( n.find( "at" ) );
        sh.size = xy( n.find( "size" ) );
        sh.stroke = parseStroke( n.find( "stroke" ) );

        if( N f = n.find( "fill" ) )
        {
            if( N c = f.find( "color" ) )
                sh.fill = parseColor( c );
        }

        if( N u = n.find( "uuid" ) )
            sh.uuid = S( u.str( 0 ) );

        n.each( "property", [&]( const N& p )
                {
                    SchField f = parseField( p );

                    if( f.name == "Sheetname" || f.name == "Sheet name" )
                    {
                        f.name = "Sheetname";
                        sh.name = f.value;
                    }
                    else if( f.name == "Sheetfile" || f.name == "Sheet file" )
                    {
                        f.name = "Sheetfile";
                        sh.file = f.value;
                    }

                    sh.fields.push_back( f );
                } );

        n.each( "pin", [&]( const N& p )
                {
                    SchSheetPin pin;
                    pin.name = S( p.str( 0 ) );
                    pin.shape = labelShape( p.str( 1 ) );

                    if( N at = p.find( "at" ) )
                    {
                        pin.pos = xy( at );
                        int a = ( (int) std::lround( at.num( 2 ) ) % 360 + 360 ) % 360;
                        // side of the sheet -> label spin (text inside the sheet)
                        pin.spin = a == 0 ? Spin::LEFT : a == 90 ? Spin::BOTTOM : a == 180 ? Spin::RIGHT : Spin::UP;
                    }

                    pin.attr.hAlign = HAlign::LEFT;
                    parseEffects( p.find( "effects" ), pin.attr, false );

                    if( N u = p.find( "uuid" ) )
                        pin.uuid = S( u.str( 0 ) );

                    sh.pins.push_back( pin );
                } );

        if( N inst = n.find( "instances" ) )
        {
            inst.each( "project", [&]( const N& proj )
                       {
                           proj.each( "path", [&]( const N& p )
                                      {
                                          if( N pg = p.find( "page" ) )
                                              sh.pages[S( p.str( 0 ) )] = S( pg.str( 0 ) );
                                      } );
                       } );
        }

        m_s.sheets.push_back( std::move( sh ) );
    }

    SchScreen& m_s;
};

std::string baseName( const std::string& aPath )
{
    size_t slash = aPath.find_last_of( "/\\" );
    return slash == std::string::npos ? aPath : aPath.substr( slash + 1 );
}

std::string dirName( const std::string& aPath )
{
    size_t slash = aPath.find_last_of( "/\\" );
    return slash == std::string::npos ? std::string() : aPath.substr( 0, slash + 1 );
}

/// "a/b/../c.kicad_sch" -> "a/c.kicad_sch", backslashes -> slashes.
std::string normalizePath( std::string p )
{
    std::replace( p.begin(), p.end(), '\\', '/' );
    std::vector<std::string> parts;
    size_t                   s = 0;

    while( s <= p.size() )
    {
        size_t      e = p.find( '/', s );
        std::string part = p.substr( s, e == std::string::npos ? std::string::npos : e - s );

        if( part == ".." && !parts.empty() && parts.back() != ".." )
            parts.pop_back();
        else if( !part.empty() && part != "." )
            parts.push_back( part );

        if( e == std::string::npos )
            break;

        s = e + 1;
    }

    std::string out;

    for( size_t i = 0; i < parts.size(); ++i )
        out += ( i ? "/" : "" ) + parts[i];

    return out;
}

std::string lowerAscii( std::string s )
{
    for( char& c : s )
        c = (char) std::tolower( (unsigned char) c );

    return s;
}

} // anonymous namespace


bool PaperSize( const std::string& aName, double& w, double& h )
{
    static const struct
    {
        const char* name;
        double      w, h;
    } SIZES[] = {
        { "A5", 210, 148 },        { "A4", 297, 210 },      { "A3", 420, 297 },       { "A2", 594, 420 },
        { "A1", 841, 594 },        { "A0", 1189, 841 },     { "A", 279.4, 215.9 },    { "B", 431.8, 279.4 },
        { "C", 558.8, 431.8 },     { "D", 863.6, 558.8 },   { "E", 1117.6, 863.6 },   { "GERBER", 812.8, 812.8 },
        { "USLetter", 279.4, 215.9 }, { "USLegal", 355.6, 215.9 }, { "USLedger", 431.8, 279.4 }
    };

    for( const auto& s : SIZES )
    {
        if( aName == s.name )
        {
            w = s.w;
            h = s.h;
            return true;
        }
    }

    return false;
}


bool LooksLikeSchematic( const char* aData, size_t aLen )
{
    size_t i = 0;

    while( i < aLen && i < 64 && std::isspace( (unsigned char) aData[i] ) )
        ++i;

    return aLen - i >= 10 && std::strncmp( aData + i, "(kicad_sch", 10 ) == 0;
}


void ParseSchematicFile( SchScreen& aScreen, const char* aData, size_t aLen )
{
    Parser p( aScreen );
    p.Parse( aData, aLen );
}


std::vector<std::string> SheetFileNames( const SchScreen& aScreen )
{
    std::vector<std::string> out;

    for( const SchSheet& s : aScreen.sheets )
    {
        if( !s.file.empty() && std::find( out.begin(), out.end(), s.file ) == out.end() )
            out.push_back( s.file );
    }

    return out;
}


std::string Schematic::Reference( const SheetNode& aSheet, const SchSymbol& aSymbol ) const
{
    const std::string rootUuid = screens.empty() ? std::string() : screens[0].uuid;
    // instance paths: KiCad 7+ "/<root uuid>/<sheet uuid>...", KiCad 6 "/<sheet uuid>..."
    std::string p6 = aSheet.path == "/" ? std::string() : aSheet.path;
    std::string p7 = "/" + rootUuid + p6;

    for( const SymbolInstance& in : aSymbol.instances )
    {
        if( ( in.path == p7 || in.path == p6 || ( p6.empty() && in.path == "/" ) ) && !in.reference.empty() )
            return in.reference;
    }

    if( !screens.empty() )
    {
        auto it = screens[0].legacySymbolInstances.find( p6 + "/" + aSymbol.uuid );

        if( it != screens[0].legacySymbolInstances.end() && !it->second.reference.empty() )
            return it->second.reference;
    }

    // a symbol used on one sheet only: its single instance
    if( aSymbol.instances.size() == 1 && !aSymbol.instances[0].reference.empty() )
        return aSymbol.instances[0].reference;

    for( const SchField& f : aSymbol.fields )
    {
        if( f.name == "Reference" )
            return f.value;
    }

    return std::string();
}


int Schematic::Unit( const SheetNode& aSheet, const SchSymbol& aSymbol ) const
{
    const std::string rootUuid = screens.empty() ? std::string() : screens[0].uuid;
    std::string       p6 = aSheet.path == "/" ? std::string() : aSheet.path;
    std::string       p7 = "/" + rootUuid + p6;

    for( const SymbolInstance& in : aSymbol.instances )
    {
        if( in.path == p7 || in.path == p6 )
            return std::max( 1, in.unit );
    }

    if( !screens.empty() )
    {
        auto it = screens[0].legacySymbolInstances.find( p6 + "/" + aSymbol.uuid );

        if( it != screens[0].legacySymbolInstances.end() )
            return std::max( 1, it->second.unit );
    }

    return aSymbol.unit;
}


std::string Schematic::FieldValue( const SheetNode& aSheet, const SchSymbol& aSymbol, const SchField& aField ) const
{
    if( ( aField.name == "Value" || aField.name == "Footprint" ) && !screens.empty() )
    {
        std::string p6 = aSheet.path == "/" ? std::string() : aSheet.path;
        auto        it = screens[0].legacySymbolInstances.find( p6 + "/" + aSymbol.uuid );

        if( it != screens[0].legacySymbolInstances.end() )
        {
            const std::string& v = aField.name == "Value" ? it->second.value : it->second.footprint;

            if( !v.empty() )
                return v;
        }
    }

    return aField.value;
}


const LibSymbol* Schematic::Lib( const SchScreen& aScreen, const SchSymbol& aSymbol ) const
{
    auto it = aScreen.libSymbols.find( aSymbol.libId );
    return it == aScreen.libSymbols.end() ? nullptr : &it->second;
}


void LoadSchematic( Schematic& aSch, const std::vector<SchFile>& aFiles, size_t aRoot )
{
    aSch = Schematic();

    if( aRoot >= aFiles.size() )
        throw ParseError( Tr( "No schematic file" ), 0 );

    // files by normalised relative path and by lower-case file name
    std::map<std::string, size_t> byPath, byName;

    for( size_t i = 0; i < aFiles.size(); ++i )
    {
        byPath[normalizePath( aFiles[i].name )] = i;
        byName.emplace( lowerAscii( baseName( aFiles[i].name ) ), i );
    }

    std::map<size_t, int> screenOf; // file index -> screen index

    auto screenFor = [&]( size_t file ) -> int
    {
        auto it = screenOf.find( file );

        if( it != screenOf.end() )
            return it->second;

        SchScreen sc;
        sc.fileName = aFiles[file].name;
        ParseSchematicFile( sc, aFiles[file].data.data(), aFiles[file].data.size() );

        // derived symbols ("extends") take their parent's drawing
        for( auto& [id, L] : sc.libSymbols )
        {
            std::set<std::string> seen;
            std::string           parent = L.extends;

            while( !parent.empty() && L.graphics.empty() && L.pins.empty() && seen.insert( parent ).second )
            {
                std::string full = id.find( ':' ) != std::string::npos ? id.substr( 0, id.find( ':' ) + 1 ) + parent : parent;
                auto        p = sc.libSymbols.find( full );

                if( p == sc.libSymbols.end() )
                    p = sc.libSymbols.find( parent );

                if( p == sc.libSymbols.end() )
                    break;

                L.graphics = p->second.graphics;
                L.pins = p->second.pins;
                L.unitCount = p->second.unitCount;
                L.bodyStyleCount = p->second.bodyStyleCount;
                L.pinNameOffset = p->second.pinNameOffset;
                L.showPinNames = p->second.showPinNames;
                L.showPinNumbers = p->second.showPinNumbers;
                L.power = L.power || p->second.power;
                parent = p->second.extends;
            }
        }

        aSch.screens.push_back( std::move( sc ) );
        screenOf[file] = (int) aSch.screens.size() - 1;
        return (int) aSch.screens.size() - 1;
    };

    SheetNode root;
    root.screen = screenFor( aRoot );
    root.path = "/";
    root.page = "1";

    {
        const SchScreen& sc = aSch.screens[(size_t) root.screen];
        auto             pg = sc.legacySheetPages.find( "/" );

        if( pg != sc.legacySheetPages.end() )
            root.page = pg->second;
    }

    aSch.sheets.push_back( root );

    // breadth first, so page order follows the hierarchy; guard against recursion
    for( size_t ni = 0; ni < aSch.sheets.size() && aSch.sheets.size() < 2000; ++ni )
    {
        int         screen = aSch.sheets[ni].screen;
        std::string here = dirName( aFiles[screenOf.empty() ? aRoot : 0].name );

        for( const auto& [file, idx] : screenOf )
        {
            if( idx == screen )
                here = dirName( aFiles[file].name );
        }

        const size_t sheetCount = aSch.screens[(size_t) screen].sheets.size();

        for( size_t si = 0; si < sheetCount; ++si )
        {
            const SchSheet sh = aSch.screens[(size_t) screen].sheets[si];
            SheetNode      child;
            child.parent = (int) ni;
            child.sheetItem = (int) si;
            child.path = ( aSch.sheets[ni].path == "/" ? std::string() : aSch.sheets[ni].path ) + "/" + sh.uuid;
            child.name = sh.name;

            if( child.path.size() > 4000 )
                continue;

            // page number from the sheet's instances (KiCad 7+) or the root's list (KiCad 6)
            const std::string rootUuid = aSch.screens[0].uuid;

            for( const auto& [path, page] : sh.pages )
            {
                std::string parentP6 = aSch.sheets[ni].path == "/" ? std::string() : aSch.sheets[ni].path;

                if( path == "/" + rootUuid + parentP6 || path == parentP6 || ( parentP6.empty() && path == "/" ) )
                    child.page = page;
            }

            if( child.page.empty() && sh.pages.size() == 1 )
                child.page = sh.pages.begin()->second;

            if( child.page.empty() )
            {
                auto pg = aSch.screens[0].legacySheetPages.find( child.path );

                if( pg != aSch.screens[0].legacySheetPages.end() )
                    child.page = pg->second;
            }

            if( child.page.empty() )
                child.page = std::to_string( aSch.sheets.size() + 1 );

            // the file: relative to the parent's folder, then by name anywhere
            std::string want = normalizePath( here + sh.file );
            auto        f = byPath.find( want );

            if( f == byPath.end() )
                f = byPath.find( normalizePath( sh.file ) );

            if( f == byPath.end() )
            {
                auto g = byName.find( lowerAscii( baseName( sh.file ) ) );

                if( g != byName.end() )
                    f = byPath.find( normalizePath( aFiles[g->second].name ) );
            }

            if( f == byPath.end() || sh.file.empty() )
            {
                if( std::find( aSch.missingFiles.begin(), aSch.missingFiles.end(), sh.file ) == aSch.missingFiles.end() )
                    aSch.missingFiles.push_back( sh.file );

                continue;
            }

            // a sheet that (indirectly) contains itself
            auto known = screenOf.find( f->second );
            bool loop = false;

            if( known != screenOf.end() )
            {
                for( int a = (int) ni; a >= 0; a = aSch.sheets[(size_t) a].parent )
                    loop |= aSch.sheets[(size_t) a].screen == known->second;
            }

            if( loop )
            {
                aSch.warnings.push_back( TrF( "Sheet {0} includes itself", { sh.name } ) );
                continue;
            }

            try
            {
                child.screen = screenFor( f->second );
            }
            catch( const ParseError& e )
            {
                aSch.warnings.push_back( TrF( "{0} could not be read: {1}", { sh.file, e.what() } ) );
                continue;
            }

            aSch.sheets[ni].children.push_back( (int) aSch.sheets.size() );
            aSch.sheets.push_back( child );
        }
    }

    for( const SchScreen& sc : aSch.screens )
    {
        for( const std::string& w : sc.warnings )
            aSch.warnings.push_back( w );
    }

    for( const std::string& m : aSch.missingFiles )
        aSch.warnings.push_back( TrF( "Sub-sheet file not available: {0}", { m } ) );
}

} // namespace kp
