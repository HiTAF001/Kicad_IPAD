/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Reader for KiCad .kicad_pcb files (s-expression format, file versions 4 .. 2026xxxx).
 * Written from the public file format and the behaviour of KiCad's own reader
 * (pcbnew/pcb_io/kicad_sexpr/pcb_io_kicad_sexpr_parser.cpp).
 */
#include "board.h"
#include "sexpr.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kp
{

static constexpr double PI = 3.14159265358979323846;
static constexpr double DEG2RAD = PI / 180.0;

// File format versions that change interpretation
static constexpr int LEGACY_ARC_FORMATTING = 20210925;     // <=: arcs are (start=center)(end=start)(angle)
static constexpr int NEW_LAYER_IDS = 20240800;             // >=: KiCad 9 layer numbering
static constexpr int FIRST_FP_AFFINE_TRANSFORM = 20260616; // >=: footprint text angles are lib-relative
static constexpr int BOLD_IS_MULTIPLIER = 20260826;        // >=: bold multiplies the stroke width
static constexpr double DEFAULT_LINE_WIDTH = 0.10;
static constexpr double PARSE_ARC_ERROR = 0.002; // mm, flattening error used while parsing


Pt RotatePt( const Pt& p, double aDeg )
{
    double a = std::fmod( aDeg, 360.0 );

    if( a < 0 )
        a += 360.0;

    if( a == 0.0 )
        return p;
    if( a == 90.0 )
        return Pt( p.y, -p.x );
    if( a == 180.0 )
        return Pt( -p.x, -p.y );
    if( a == 270.0 )
        return Pt( -p.y, p.x );

    double s = std::sin( a * DEG2RAD );
    double c = std::cos( a * DEG2RAD );
    return Pt( p.y * s + p.x * c, p.y * c - p.x * s );
}


Pt RotatePt( const Pt& p, const Pt& aCenter, double aDeg )
{
    return RotatePt( p - aCenter, aDeg ) + aCenter;
}


int CircleSegments( double aRadius, double aMaxErr )
{
    if( aRadius <= aMaxErr * 2 )
        return 8;

    double step = 2.0 * std::acos( 1.0 - aMaxErr / aRadius );
    int    n = (int) std::ceil( 2.0 * PI / step );
    return std::clamp( n, 8, 360 );
}


void ArcToPointsCenter( Path& aOut, const Pt& aCenter, const Pt& aStart, double aSweepDeg,
                        double aMaxErr )
{
    double r = std::hypot( aStart.x - aCenter.x, aStart.y - aCenter.y );
    int    full = CircleSegments( r, aMaxErr );
    int    n = std::max( 1, (int) std::ceil( full * std::fabs( aSweepDeg ) / 360.0 ) );

    for( int i = 0; i <= n; ++i )
        aOut.push_back( RotatePt( aStart, aCenter, aSweepDeg * i / n ) );
}


static bool circumCenter( const Pt& a, const Pt& b, const Pt& c, Pt& aCenter )
{
    double d = 2.0 * ( a.x * ( b.y - c.y ) + b.x * ( c.y - a.y ) + c.x * ( a.y - b.y ) );

    if( std::fabs( d ) < 1e-12 )
        return false;

    double a2 = a.x * a.x + a.y * a.y;
    double b2 = b.x * b.x + b.y * b.y;
    double c2 = c.x * c.x + c.y * c.y;
    aCenter.x = ( a2 * ( b.y - c.y ) + b2 * ( c.y - a.y ) + c2 * ( a.y - b.y ) ) / d;
    aCenter.y = ( a2 * ( c.x - b.x ) + b2 * ( a.x - c.x ) + c2 * ( b.x - a.x ) ) / d;
    return true;
}


void ArcToPoints( Path& aOut, const Pt& aStart, const Pt& aMid, const Pt& aEnd, double aMaxErr )
{
    Pt c;

    if( !circumCenter( aStart, aMid, aEnd, c ) )
    {
        aOut.push_back( aStart );
        aOut.push_back( aEnd );
        return;
    }

    // Math angles in a Y-down frame; we just need consistent sweep through mid.
    double a0 = std::atan2( aStart.y - c.y, aStart.x - c.x );
    double am = std::atan2( aMid.y - c.y, aMid.x - c.x );
    double a1 = std::atan2( aEnd.y - c.y, aEnd.x - c.x );

    auto norm = []( double v )
    {
        while( v < 0 )
            v += 2 * PI;
        while( v >= 2 * PI )
            v -= 2 * PI;
        return v;
    };

    double dm = norm( am - a0 );
    double d1 = norm( a1 - a0 );
    double sweep = ( dm <= d1 ) ? d1 : d1 - 2 * PI; // radians, math sense

    if( std::fabs( sweep ) < 1e-12 )
        sweep = 2 * PI; // full circle (start == end)

    double r = std::hypot( aStart.x - c.x, aStart.y - c.y );
    int    full = CircleSegments( r, aMaxErr );
    int    n = std::max( 1, (int) std::ceil( full * std::fabs( sweep ) / ( 2 * PI ) ) );

    for( int i = 0; i <= n; ++i )
    {
        double a = a0 + sweep * i / n;
        aOut.push_back( Pt( c.x + r * std::cos( a ), c.y + r * std::sin( a ) ) );
    }

    aOut.back() = aEnd;
}


// ------------------------------------------------------------------------------------------
// Layer tables
// ------------------------------------------------------------------------------------------

namespace
{

struct RGBA
{
    float r, g, b, a;
};

RGBA css( int r, int g, int b, float a = 1.0f )
{
    return { r / 255.0f, g / 255.0f, b / 255.0f, a };
}

// KiCad default colour theme (common/settings/builtin_color_themes.h)
const RGBA INNER_COLORS[] = {
    css( 127, 200, 127 ), css( 206, 125, 44 ),  css( 79, 203, 203 ),  css( 219, 98, 139 ),
    css( 167, 165, 198 ), css( 40, 204, 217 ),  css( 232, 178, 167 ), css( 242, 237, 161 ),
    css( 141, 203, 129 ), css( 237, 124, 51 ),  css( 91, 195, 235 ),  css( 247, 111, 142 ),
    css( 167, 165, 198 ), css( 40, 204, 217 ),  css( 232, 178, 167 ), css( 242, 237, 161 ),
    css( 237, 124, 51 ),  css( 91, 195, 235 ),  css( 247, 111, 142 ), css( 167, 165, 198 ),
    css( 40, 204, 217 ),  css( 232, 178, 167 ), css( 242, 237, 161 ), css( 237, 124, 51 ),
    css( 91, 195, 235 ),  css( 247, 111, 142 ), css( 167, 165, 198 ), css( 40, 204, 217 ),
    css( 232, 178, 167 ), css( 242, 237, 161 )
};

const RGBA USER_COLORS[] = { css( 194, 194, 194 ), css( 89, 148, 220 ), css( 180, 219, 210 ),
                             css( 216, 200, 82 ) };

const char* LEGACY_IDS[] = { "F.Cu",      "In1.Cu",    "In2.Cu",    "In3.Cu",    "In4.Cu",
                             "In5.Cu",    "In6.Cu",    "In7.Cu",    "In8.Cu",    "In9.Cu",
                             "In10.Cu",   "In11.Cu",   "In12.Cu",   "In13.Cu",   "In14.Cu",
                             "In15.Cu",   "In16.Cu",   "In17.Cu",   "In18.Cu",   "In19.Cu",
                             "In20.Cu",   "In21.Cu",   "In22.Cu",   "In23.Cu",   "In24.Cu",
                             "In25.Cu",   "In26.Cu",   "In27.Cu",   "In28.Cu",   "In29.Cu",
                             "In30.Cu",   "B.Cu",      "B.Adhes",   "F.Adhes",   "B.Paste",
                             "F.Paste",   "B.SilkS",   "F.SilkS",   "B.Mask",    "F.Mask",
                             "Dwgs.User", "Cmts.User", "Eco1.User", "Eco2.User", "Edge.Cuts",
                             "Margin",    "B.CrtYd",   "F.CrtYd",   "B.Fab",     "F.Fab",
                             "User.1",    "User.2",    "User.3",    "User.4",    "User.5",
                             "User.6",    "User.7",    "User.8",    "User.9" };

std::string newIdToCanonical( int id )
{
    switch( id )
    {
    case 0: return "F.Cu";
    case 2: return "B.Cu";
    case 1: return "F.Mask";
    case 3: return "B.Mask";
    case 5: return "F.SilkS";
    case 7: return "B.SilkS";
    case 9: return "F.Adhes";
    case 11: return "B.Adhes";
    case 13: return "F.Paste";
    case 15: return "B.Paste";
    case 17: return "Dwgs.User";
    case 19: return "Cmts.User";
    case 21: return "Eco1.User";
    case 23: return "Eco2.User";
    case 25: return "Edge.Cuts";
    case 27: return "Margin";
    case 29: return "B.CrtYd";
    case 31: return "F.CrtYd";
    case 33: return "B.Fab";
    case 35: return "F.Fab";
    default: break;
    }

    if( id >= 4 && id <= 62 && id % 2 == 0 )
        return "In" + std::to_string( ( id - 2 ) / 2 ) + ".Cu";

    if( id >= 39 && id % 2 == 1 )
        return "User." + std::to_string( ( id - 37 ) / 2 );

    return std::string();
}

int innerNumber( const std::string& aName )
{
    // "In12.Cu" -> 12
    if( aName.size() > 5 && aName.compare( 0, 2, "In" ) == 0
        && aName.compare( aName.size() - 3, 3, ".Cu" ) == 0 )
    {
        int n = 0;

        for( size_t i = 2; i < aName.size() - 3; ++i )
        {
            if( !isdigit( (unsigned char) aName[i] ) )
                return -1;

            n = n * 10 + ( aName[i] - '0' );
        }

        return n;
    }

    return -1;
}

int userNumber( const std::string& aName )
{
    if( aName.size() > 5 && aName.compare( 0, 5, "User." ) == 0 )
    {
        int n = 0;

        for( size_t i = 5; i < aName.size(); ++i )
        {
            if( !isdigit( (unsigned char) aName[i] ) )
                return -1;

            n = n * 10 + ( aName[i] - '0' );
        }

        return n;
    }

    return -1;
}

/// Fill kind/side/colour/sort for a canonical layer name. Returns false if unknown.
bool describeLayer( const std::string& n, Layer& L )
{
    L.canonical = n;
    int inner = innerNumber( n );
    int user = userNumber( n );

    struct Info
    {
        const char* name;
        LayerKind   kind;
        int         side;
        RGBA        color;
        int         sort;
    };

    static const Info table[] = {
        { "F.Cu", LayerKind::COPPER, 1, css( 200, 52, 52 ), 200 },
        { "B.Cu", LayerKind::COPPER, -1, css( 77, 127, 196 ), 60 },
        { "B.Fab", LayerKind::FAB, -1, css( 88, 93, 132 ), 10 },
        { "B.CrtYd", LayerKind::COURTYARD, -1, css( 38, 233, 255 ), 11 },
        { "B.Adhes", LayerKind::ADHES, -1, css( 0, 0, 132 ), 12 },
        { "B.SilkS", LayerKind::SILK, -1, css( 232, 178, 167 ), 13 },
        { "B.Paste", LayerKind::PASTE, -1, css( 0, 194, 194, 0.9f ), 14 },
        { "B.Mask", LayerKind::MASK, -1, css( 2, 255, 238, 0.4f ), 15 },
        { "F.Mask", LayerKind::MASK, 1, css( 216, 100, 255, 0.4f ), 210 },
        { "F.Paste", LayerKind::PASTE, 1, css( 180, 160, 154, 0.9f ), 211 },
        { "F.SilkS", LayerKind::SILK, 1, css( 242, 237, 161 ), 212 },
        { "F.Adhes", LayerKind::ADHES, 1, css( 132, 0, 132 ), 213 },
        { "F.CrtYd", LayerKind::COURTYARD, 1, css( 255, 38, 226 ), 214 },
        { "F.Fab", LayerKind::FAB, 1, css( 175, 175, 175 ), 215 },
        { "Dwgs.User", LayerKind::USER, 0, css( 194, 194, 194 ), 300 },
        { "Cmts.User", LayerKind::USER, 0, css( 89, 148, 220 ), 301 },
        { "Eco1.User", LayerKind::USER, 0, css( 180, 219, 210 ), 302 },
        { "Eco2.User", LayerKind::USER, 0, css( 216, 200, 82 ), 303 },
        { "Margin", LayerKind::MARGIN, 0, css( 255, 38, 226 ), 900 },
        { "Edge.Cuts", LayerKind::EDGE, 0, css( 208, 210, 205 ), 910 },
    };

    for( const Info& i : table )
    {
        if( n == i.name )
        {
            L.kind = i.kind;
            L.side = i.side;
            L.color = { i.color.r, i.color.g, i.color.b, i.color.a };
            L.sortKey = i.sort;
            L.copperOrder = ( n == "F.Cu" ) ? 0 : ( n == "B.Cu" ? 1000 : -1 );
            return true;
        }
    }

    if( inner >= 1 && inner <= 30 )
    {
        RGBA c = INNER_COLORS[( inner - 1 ) % 30];
        L.kind = LayerKind::COPPER;
        L.side = 0;
        L.color = { c.r, c.g, c.b, c.a };
        L.sortKey = 100 + ( 31 - inner ); // deeper layers first
        L.copperOrder = inner;
        return true;
    }

    if( user >= 1 )
    {
        RGBA c = USER_COLORS[( user - 1 ) % 4];
        L.kind = LayerKind::USER;
        L.side = 0;
        L.color = { c.r, c.g, c.b, c.a };
        L.sortKey = 400 + user;
        return true;
    }

    return false;
}

struct FpXform
{
    Pt     pos;
    double angle = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    bool   active = false;

    Pt apply( const Pt& p ) const
    {
        if( !active )
            return p;

        return RotatePt( Pt( p.x * scaleX, p.y * scaleY ), angle ) + pos;
    }

    double len( double v ) const
    {
        if( !active )
            return v;

        return v * std::sqrt( std::fabs( scaleX * scaleY ) );
    }
};

double normalizeDeg( double a )
{
    a = std::fmod( a, 360.0 );

    if( a < 0 )
        a += 360.0;

    return a;
}

} // anonymous namespace


// ------------------------------------------------------------------------------------------
// Parser
// ------------------------------------------------------------------------------------------

class BoardParser
{
public:
    BoardParser( Board& aBoard ) :
            m_board( aBoard )
    {
    }

    void Parse( const char* aData, size_t aLen );

private:
    void parseLayers( N aNode );
    void parseNetDecl( N aNode );
    void parseSetup( N aNode );
    void parseTitleBlock( N aNode );
    void parseFootprint( N aNode );
    void parseShape( N aNode, int aFootprint, const FpXform& aXf, std::vector<Shape>& aOut,
                     bool aPrimitive = false );
    void parseText( N aNode, int aFootprint, const FpXform& aXf, bool aFpField = false );
    void parseTextBox( N aNode, int aFootprint, const FpXform& aXf );
    void parseTextEffects( N aEffects, Text& aText );
    void parsePad( N aNode, int aFootprint, const FpXform& aXf );
    void parsePadGeom( N aNode, PadGeom& aGeom, bool aPadstackLayer );
    void parseTrack( N aNode );
    void parseArc( N aNode );
    void parseVia( N aNode );
    void parseZone( N aNode, int aFootprint, const FpXform& aXf );
    void parseDimension( N aNode, int aFootprint, const FpXform& aXf );
    void parseTarget( N aNode );

    int  layerRef( std::string_view aName, bool aWarn = true );
    void layerList( N aNode, std::vector<int>& aOut ); ///< (layers ...) incl. wildcards
    int  netRef( N aNetNode );
    Pt   xy( N aNode, uint32_t aFirstArg = 0 ) const
    {
        return Pt( aNode.num( aFirstArg ), aNode.num( aFirstArg + 1 ) );
    }
    void points( N aPts, Path& aOut ) const; ///< (pts (xy..) (arc ..))
    void warn( const std::string& aMsg );

    Board& m_board;
    int    m_version = 0;
    double m_defaultViaDrill = 0.0;
    int    m_warnCount = 0;
};


void BoardParser::warn( const std::string& aMsg )
{
    if( ++m_warnCount <= 20 )
        m_board.warnings.push_back( aMsg );
}


int BoardParser::layerRef( std::string_view aName, bool aWarn )
{
    std::string name( aName );
    auto        it = m_board.layerByName.find( name );

    if( it != m_board.layerByName.end() )
        return it->second;

    // Known canonical layer not declared in the table (old files): create on demand.
    Layer L;

    if( describeLayer( name, L ) )
    {
        L.display = name;
        m_board.layers.push_back( L );
        int idx = (int) m_board.layers.size() - 1;
        m_board.layerByName[name] = idx;
        return idx;
    }

    if( aWarn )
        warn( "Bilinmeyen katman: " + name );

    return -1;
}


void BoardParser::layerList( N aNode, std::vector<int>& aOut )
{
    auto addUnique = [&]( int idx )
    {
        if( idx >= 0 && std::find( aOut.begin(), aOut.end(), idx ) == aOut.end() )
            aOut.push_back( idx );
    };

    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isAtom() )
            continue;

        std::string n( c.text() );

        if( n == "*.Cu" )
        {
            for( size_t i = 0; i < m_board.layers.size(); ++i )
            {
                if( m_board.layers[i].kind == LayerKind::COPPER )
                    addUnique( (int) i );
            }
        }
        else if( n == "F&B.Cu" )
        {
            addUnique( layerRef( "F.Cu" ) );
            addUnique( layerRef( "B.Cu" ) );
        }
        else if( n.size() > 2 && ( n.compare( 0, 2, "*." ) == 0 || n.compare( 0, 4, "F&B." ) == 0 ) )
        {
            std::string suffix = n.substr( n[0] == '*' ? 2 : 4 );
            addUnique( layerRef( "F." + suffix, false ) );
            addUnique( layerRef( "B." + suffix, false ) );
        }
        else
        {
            addUnique( layerRef( n ) );
        }
    }
}


int BoardParser::netRef( N aNetNode )
{
    N a0 = aNetNode.arg( 0 );

    if( !a0 )
        return 0;

    if( a0.isString() || !a0.isNumber() )
    {
        std::string name( a0.text() );

        if( name.empty() )
            return 0;

        auto it = m_board.netByName.find( name );

        if( it != m_board.netByName.end() )
            return it->second;

        m_board.nets.push_back( name );
        int idx = (int) m_board.nets.size() - 1;
        m_board.netByName[name] = idx;
        return idx;
    }

    int  number = (int) a0.text().empty() ? 0 : (int) ParseNumber( a0.text() );
    auto it = m_board.netByNumber.find( number );

    if( it != m_board.netByNumber.end() )
        return it->second;

    if( number == 0 )
        return 0;

    N           a1 = aNetNode.arg( 1 );
    std::string name = a1 ? std::string( a1.text() ) : ( "Net-" + std::to_string( number ) );

    auto byName = m_board.netByName.find( name );

    if( byName != m_board.netByName.end() )
    {
        m_board.netByNumber[number] = byName->second;
        return byName->second;
    }

    m_board.nets.push_back( name );
    int idx = (int) m_board.nets.size() - 1;
    m_board.netByNumber[number] = idx;
    m_board.netByName[name] = idx;
    return idx;
}


void BoardParser::points( N aPts, Path& aOut ) const
{
    for( N c = aPts.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view h = c.head();

        if( h == "xy" )
        {
            aOut.push_back( xy( c ) );
        }
        else if( h == "arc" )
        {
            N s = c.find( "start" );
            N m = c.find( "mid" );
            N e = c.find( "end" );

            if( s && m && e )
            {
                Path arc;
                ArcToPoints( arc, xy( s ), xy( m ), xy( e ), PARSE_ARC_ERROR );

                // avoid duplicating the joint with the previous point
                size_t first = ( !aOut.empty() && !arc.empty()
                                 && std::hypot( aOut.back().x - arc[0].x, aOut.back().y - arc[0].y ) < 1e-9 )
                                       ? 1
                                       : 0;

                aOut.insert( aOut.end(), arc.begin() + (long) first, arc.end() );
            }
        }
    }
}


void BoardParser::Parse( const char* aData, size_t aLen )
{
    SReader          reader( aData, aLen );
    std::string_view root = reader.OpenRoot();

    if( root != "kicad_pcb" )
    {
        if( root == "footprint" || root == "module" )
            throw ParseError( "Bu bir footprint dosyası (.kicad_mod), kart dosyası değil", 0 );

        if( root == "kicad_sch" || root == "kicad_symbol_lib" )
            throw ParseError( "Bu bir şema/sembol dosyası. KiPad şimdilik yalnızca kart "
                              "(.kicad_pcb) dosyalarını açıyor.", 0 );

        throw ParseError( "Bu bir KiCad kart dosyası değil (kök: '" + std::string( root ) + "')", 0 );
    }

    SPool pool;
    FpXform noXf;

    while( N item = reader.NextTopLevel( pool ) )
    {
        std::string_view h = item.head();

        try
        {
            if( h == "version" )
            {
                m_version = (int) item.num( 0 );
                m_board.version = m_version;
            }
            else if( h == "generator" )
                m_board.generator = std::string( item.str( 0 ) );
            else if( h == "generator_version" )
                m_board.generatorVersion = std::string( item.str( 0 ) );
            else if( h == "general" )
            {
                if( N t = item.find( "thickness" ) )
                    m_board.thickness = t.num( 0, 1.6 );
            }
            else if( h == "layers" )
                parseLayers( item );
            else if( h == "setup" )
                parseSetup( item );
            else if( h == "title_block" )
                parseTitleBlock( item );
            else if( h == "net" )
                parseNetDecl( item );
            else if( h == "footprint" || h == "module" )
                parseFootprint( item );
            else if( h == "segment" )
                parseTrack( item );
            else if( h == "arc" )
                parseArc( item );
            else if( h == "via" )
                parseVia( item );
            else if( h == "zone" )
                parseZone( item, -1, noXf );
            else if( h == "gr_line" || h == "gr_arc" || h == "gr_circle" || h == "gr_rect"
                     || h == "gr_poly" || h == "gr_curve" || h == "gr_bbox" || h == "gr_vector"
                     || h == "gr_ellipse" || h == "gr_ellipse_arc" )
                parseShape( item, -1, noXf, m_board.shapes );
            else if( h == "gr_text" )
                parseText( item, -1, noXf );
            else if( h == "gr_text_box" )
                parseTextBox( item, -1, noXf );
            else if( h == "dimension" )
                parseDimension( item, -1, noXf );
            else if( h == "target" )
                parseTarget( item );
            // ignored: page/paper, net_class, group, generated, image, table, barcode,
            // embedded_fonts, embedded_files, net_chains, variants, point, grid_item ...
        }
        catch( const std::exception& e )
        {
            warn( std::string( "Öğe atlandı (" ) + std::string( h ) + "): " + e.what() );
        }
    }

    // Copper stack order
    std::vector<int> copper;

    for( size_t i = 0; i < m_board.layers.size(); ++i )
    {
        if( m_board.layers[i].kind == LayerKind::COPPER )
            copper.push_back( (int) i );
    }

    std::sort( copper.begin(), copper.end(),
               [&]( int a, int b )
               { return m_board.layers[a].copperOrder < m_board.layers[b].copperOrder; } );

    for( size_t k = 0; k < copper.size(); ++k )
        m_board.layers[copper[k]].copperOrder = (int) k;

    m_board.copperStack = copper;
}


void BoardParser::parseLayers( N aNode )
{
    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() || c.size() < 2 )
            continue;

        int         id = (int) ParseNumber( c.child( 0 ).text(), -1 );
        std::string name( c.child( 1 ).text() );
        std::string user = c.size() >= 4 ? std::string( c.child( 3 ).text() ) : std::string();

        Layer L;
        bool  known = describeLayer( name, L );

        if( !known )
        {
            std::string canonical;

            if( m_version >= NEW_LAYER_IDS )
                canonical = newIdToCanonical( id );
            else if( id >= 0 && id < (int) ( sizeof( LEGACY_IDS ) / sizeof( LEGACY_IDS[0] ) ) )
                canonical = LEGACY_IDS[id];

            if( canonical.empty() || !describeLayer( canonical, L ) )
            {
                warn( "Tanınmayan katman tanımı: " + name );
                continue;
            }

            if( user.empty() )
                user = name; // the file's own name for it (e.g. "Dessus")
        }

        L.display = user.empty() ? L.canonical : user;

        if( m_board.layerByName.count( L.canonical ) )
        {
            // already present (created on demand): just map the alias
            m_board.layerByName[name] = m_board.layerByName[L.canonical];
            continue;
        }

        m_board.layers.push_back( L );
        int idx = (int) m_board.layers.size() - 1;
        m_board.layerByName[L.canonical] = idx;
        m_board.layerByName[name] = idx;
    }
}


void BoardParser::parseNetDecl( N aNode )
{
    // (net N "name")
    int         number = (int) aNode.num( 0 );
    std::string name( aNode.str( 1 ) );

    if( number == 0 && name.empty() )
        return;

    auto it = m_board.netByName.find( name );
    int  idx;

    if( it != m_board.netByName.end() )
    {
        idx = it->second;
    }
    else
    {
        m_board.nets.push_back( name );
        idx = (int) m_board.nets.size() - 1;
        m_board.netByName[name] = idx;
    }

    m_board.netByNumber[number] = idx;
}


void BoardParser::parseSetup( N aNode )
{
    if( N d = aNode.find( "via_drill" ) )
        m_defaultViaDrill = d.num( 0 );
}


void BoardParser::parseTitleBlock( N aNode )
{
    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view h = c.head();

        if( h == "title" )
            m_board.titleBlock["TITLE"] = std::string( c.str( 0 ) );
        else if( h == "date" )
            m_board.titleBlock["ISSUE_DATE"] = std::string( c.str( 0 ) );
        else if( h == "rev" )
            m_board.titleBlock["REVISION"] = std::string( c.str( 0 ) );
        else if( h == "company" )
            m_board.titleBlock["COMPANY"] = std::string( c.str( 0 ) );
        else if( h == "comment" )
            m_board.titleBlock["COMMENT" + std::string( c.str( 0 ) )] = std::string( c.str( 1 ) );
    }
}


// ---------------------------------------------------------------------------- shapes

static void rectPoints( Path& aOut, const Pt& a, const Pt& b, double aRadius )
{
    double x0 = std::min( a.x, b.x ), x1 = std::max( a.x, b.x );
    double y0 = std::min( a.y, b.y ), y1 = std::max( a.y, b.y );
    double r = std::min( { aRadius, ( x1 - x0 ) / 2, ( y1 - y0 ) / 2 } );

    if( r <= 0 )
    {
        aOut = { Pt( x0, y0 ), Pt( x1, y0 ), Pt( x1, y1 ), Pt( x0, y1 ) };
        return;
    }

    aOut.clear();
    auto corner = [&]( Pt c, double startDeg )
    {
        Path arc;
        ArcToPointsCenter( arc, c, c + RotatePt( Pt( r, 0 ), startDeg ), 90.0, PARSE_ARC_ERROR );
        aOut.insert( aOut.end(), arc.begin(), arc.end() );
    };
    // KiCad rotation is CCW on screen (Y down) -> going around: top-right corner etc.
    corner( Pt( x1 - r, y0 + r ), 0 );    // from right edge up to top edge
    corner( Pt( x0 + r, y0 + r ), 90 );   // top-left
    corner( Pt( x0 + r, y1 - r ), 180 );  // bottom-left
    corner( Pt( x1 - r, y1 - r ), 270 );  // bottom-right
}


static void bezierPoints( Path& aOut, const Pt& p0, const Pt& p1, const Pt& p2, const Pt& p3 )
{
    double len = std::hypot( p1.x - p0.x, p1.y - p0.y ) + std::hypot( p2.x - p1.x, p2.y - p1.y )
                 + std::hypot( p3.x - p2.x, p3.y - p2.y );
    int n = std::clamp( (int) std::ceil( len / 0.1 ), 8, 200 );

    for( int i = 0; i <= n; ++i )
    {
        double t = (double) i / n;
        double u = 1 - t;
        double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        aOut.push_back( Pt( b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x,
                            b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y ) );
    }
}


void BoardParser::parseShape( N aNode, int aFootprint, const FpXform& aXf,
                              std::vector<Shape>& aOut, bool aPrimitive )
{
    std::string_view h = aNode.head();
    std::string_view t = h.substr( 3 ); // strip "gr_" / "fp_"

    Shape s;
    s.footprint = aFootprint;
    bool   foundFill = false;
    double width = 0.0;
    bool   isEllipse = ( t == "ellipse" || t == "ellipse_arc" );
    bool   closed = true;
    Pt     eCenter;
    double eMajor = 0, eMinor = 0, eRot = 0, eStart = 0, eEnd = 360;
    std::vector<int> layers;

    if( t == "line" || t == "vector" )
    {
        s.type = ShapeType::SEGMENT;
        s.start = xy( aNode.find( "start" ) );
        s.end = xy( aNode.find( "end" ) );
    }
    else if( t == "rect" || t == "bbox" )
    {
        s.type = ShapeType::RECT;
        s.start = xy( aNode.find( "start" ) );
        s.end = xy( aNode.find( "end" ) );
    }
    else if( t == "circle" )
    {
        s.type = ShapeType::CIRCLE;
        s.start = xy( aNode.find( "center" ) );
        s.end = xy( aNode.find( "end" ) );
    }
    else if( t == "arc" )
    {
        s.type = ShapeType::ARC;
        N st = aNode.find( "start" );
        N mid = aNode.find( "mid" );
        N en = aNode.find( "end" );

        if( !mid && m_version <= LEGACY_ARC_FORMATTING )
        {
            // legacy: start = centre, end = arc start, angle = sweep
            Pt     c = xy( st );
            Pt     p0 = xy( en );
            double sweep = 0;

            if( N a = aNode.find( "angle" ) )
                sweep = a.num( 0 );

            // KiCad: end = RotatePoint( start, centre, -angle )
            s.start = p0;
            s.end = RotatePt( p0, c, -sweep );
            s.mid = RotatePt( p0, c, -sweep / 2 );
        }
        else
        {
            s.start = xy( st );
            s.mid = xy( mid );
            s.end = xy( en );
        }
    }
    else if( t == "poly" )
    {
        s.type = ShapeType::POLY;

        if( N pts = aNode.find( "pts" ) )
            points( pts, s.poly );
    }
    else if( t == "curve" )
    {
        s.type = ShapeType::POLY;
        closed = false;
        Path cp;

        if( N pts = aNode.find( "pts" ) )
            points( pts, cp );

        if( cp.size() == 4 )
            bezierPoints( s.poly, cp[0], cp[1], cp[2], cp[3] );
        else
            s.poly = cp;
    }
    else if( isEllipse )
    {
        s.type = ShapeType::POLY;
        closed = ( t == "ellipse" );
    }
    else
    {
        return;
    }

    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view k = c.head();

        if( k == "layer" )
            layers.push_back( layerRef( c.str( 0 ) ) );
        else if( k == "layers" )
            layerList( c, layers );
        else if( k == "width" )
            width = c.num( 0 );
        else if( k == "stroke" )
        {
            if( N w = c.find( "width" ) )
                width = w.num( 0 );
        }
        else if( k == "radius" )
            s.radius = c.num( 0 );
        else if( k == "fill" )
        {
            foundFill = true;
            std::string_view v = c.str( 0 );
            s.filled = ( v == "yes" || v == "solid" || v == "hatch" || v == "reverse_hatch"
                         || v == "cross_hatch" || v.empty() );
        }
        else if( k == "center" && isEllipse )
            eCenter = xy( c );
        else if( k == "major_radius" )
            eMajor = c.num( 0 );
        else if( k == "minor_radius" )
            eMinor = c.num( 0 );
        else if( k == "rotation_angle" )
            eRot = c.num( 0 );
        else if( k == "start_angle" )
            eStart = c.num( 0 );
        else if( k == "end_angle" )
            eEnd = c.num( 0 );
    }

    if( isEllipse && eMajor > 0 )
    {
        double sweep = eEnd - eStart;

        if( closed || sweep == 0 )
            sweep = 360;

        int n = std::clamp( (int) std::ceil( std::fabs( sweep ) / 3.0 ), 8, 180 );

        for( int i = 0; i <= n; ++i )
        {
            double a = ( eStart + sweep * i / n ) * DEG2RAD;
            Pt     p( eMajor * std::cos( a ), -eMinor * std::sin( a ) );
            s.poly.push_back( RotatePt( p, eRot ) + eCenter );
        }
    }

    bool edge = false;

    for( int l : layers )
    {
        if( l >= 0 && m_board.layers[l].kind == LayerKind::EDGE )
            edge = true;
    }

    if( !foundFill )
    {
        if( width == 0 && ( s.type == ShapeType::RECT || s.type == ShapeType::CIRCLE ) )
            s.filled = true;
        else if( s.type == ShapeType::POLY && closed && !edge )
            s.filled = true;
    }

    if( !closed )
        s.filled = false;

    if( width <= 0 && !s.filled )
        width = DEFAULT_LINE_WIDTH;

    s.width = aXf.len( width );

    // Open polylines (bezier, ellipse arcs): represent as a POLY with stroke only.
    // We mark them by duplicating nothing; the mesh builder strokes POLY outlines closed, so
    // store open ones as a chain of segments instead.
    auto transformShape = [&]( Shape& sh )
    {
        if( !aXf.active )
            return;

        if( sh.type == ShapeType::RECT )
        {
            // a rotated rectangle is no longer axis aligned: make it a polygon
            Path p;
            rectPoints( p, sh.start, sh.end, sh.radius );
            sh.type = ShapeType::POLY;
            sh.poly = p;
            sh.radius = 0;
        }

        sh.start = aXf.apply( sh.start );
        sh.mid = aXf.apply( sh.mid );
        sh.end = aXf.apply( sh.end );

        for( Pt& p : sh.poly )
            p = aXf.apply( p );
    };

    if( aPrimitive )
    {
        // custom pad primitives stay in pad-local coordinates
        if( !closed && s.poly.size() >= 2 )
        {
            for( size_t i = 0; i + 1 < s.poly.size(); ++i )
            {
                Shape seg = s;
                seg.type = ShapeType::SEGMENT;
                seg.start = s.poly[i];
                seg.end = s.poly[i + 1];
                seg.poly.clear();
                aOut.push_back( seg );
            }
        }
        else
        {
            aOut.push_back( s );
        }

        return;
    }

    if( layers.empty() )
        return;

    transformShape( s );

    for( int l : layers )
    {
        if( l < 0 )
            continue;

        if( !closed && s.poly.size() >= 2 )
        {
            for( size_t i = 0; i + 1 < s.poly.size(); ++i )
            {
                Shape seg = s;
                seg.type = ShapeType::SEGMENT;
                seg.start = s.poly[i];
                seg.end = s.poly[i + 1];
                seg.poly.clear();
                seg.layer = l;
                aOut.push_back( seg );
            }
        }
        else
        {
            Shape copy = s;
            copy.layer = l;
            aOut.push_back( std::move( copy ) );
        }
    }
}


// ---------------------------------------------------------------------------- text

void BoardParser::parseTextEffects( N aEffects, Text& aText )
{
    if( !aEffects )
        return;

    if( N font = aEffects.find( "font" ) )
    {
        if( N sz = font.find( "size" ) )
        {
            aText.sizeY = sz.num( 0, 1.0 );
            aText.sizeX = sz.num( 1, aText.sizeY );
        }

        if( N th = font.find( "thickness" ) )
            aText.thickness = th.num( 0 );

        if( N ls = font.find( "line_spacing" ) )
            aText.lineSpacing = ls.num( 0, 1.0 );

        if( font.find( "face" ) )
            aText.hasFace = true;

        aText.bold = font.flag( "bold" );
        aText.italic = font.flag( "italic" );
    }

    if( N j = aEffects.find( "justify" ) )
    {
        for( N c = j.arg( 0 ); c; c = c.next() )
        {
            std::string_view v = c.text();

            if( v == "left" )
                aText.hAlign = HAlign::LEFT;
            else if( v == "right" )
                aText.hAlign = HAlign::RIGHT;
            else if( v == "top" )
                aText.vAlign = VAlign::TOP;
            else if( v == "bottom" )
                aText.vAlign = VAlign::BOTTOM;
            else if( v == "mirror" )
                aText.mirror = true;
        }
    }

    // very old files: (effects ... hide)
    if( aEffects.hasAtom( "hide" ) )
        aText.visible = false;
    else if( N hide = aEffects.find( "hide" ) )
        aText.visible = !( hide.str( 0 ) != "no" );
}


static void parseRenderCache( N aCache, Text& aText )
{
    for( N poly = aCache.arg( 0 ); poly; poly = poly.next() )
    {
        if( !poly.isList() || poly.head() != "polygon" )
            continue;

        std::vector<Path> glyph;

        for( N pts = poly.arg( 0 ); pts; pts = pts.next() )
        {
            if( !pts.isList() || pts.head() != "pts" )
                continue;

            Path p;

            for( N c = pts.arg( 0 ); c; c = c.next() )
            {
                if( c.isList() && c.head() == "xy" )
                    p.push_back( Pt( c.num( 0 ), c.num( 1 ) ) );
            }

            if( p.size() >= 3 )
                glyph.push_back( std::move( p ) );
        }

        if( !glyph.empty() )
            aText.renderCache.push_back( std::move( glyph ) );
    }
}


static void finishTextGeometry( Text& t, int aVersion )
{
    // effective pen width (EDA_TEXT::GetEffectiveTextPenWidth)
    double pen = t.thickness;

    if( pen <= 1e-6 )
        pen = t.bold ? t.sizeX / 5.0 : t.sizeX / 8.0;
    else if( t.bold && aVersion >= BOLD_IS_MULTIPLIER )
        pen *= 1.6;

    double maxPen = std::min( std::fabs( t.sizeX ), std::fabs( t.sizeY ) ) * 0.25;
    t.thickness = std::min( pen, maxPen );

    // draw rotation (PCB_TEXT::GetDrawRotation)
    double a = t.angle;

    if( t.footprint >= 0 && t.keepUpright )
    {
        while( a > 90.0 )
            a -= 180.0;
        while( a <= -90.0 )
            a += 180.0;
    }
    else
    {
        a = normalizeDeg( a );
    }

    t.angle = a;
}


void BoardParser::parseText( N aNode, int aFootprint, const FpXform& aXf, bool aFpField )
{
    // gr_text "txt" ... | fp_text reference|value|user "txt" ... | property "Name" "Value" ...
    Text        t;
    std::string fieldName;
    uint32_t    textArg = 0;
    std::string_view h = aNode.head();

    t.footprint = aFootprint;

    if( h == "fp_text" )
    {
        fieldName = std::string( aNode.str( 0 ) ); // reference / value / user
        textArg = 1;
    }
    else if( h == "property" )
    {
        fieldName = std::string( aNode.str( 0 ) );
        textArg = 1;
    }

    // skip a legacy bare "locked" token before the text
    N txt = aNode.arg( textArg );

    if( txt && !txt.isString() && txt.text() == "locked" )
        txt = txt.next();

    t.text = txt ? std::string( txt.text() ) : std::string();

    // legacy %R / %V
    auto replaceAll = []( std::string& s, const std::string& a, const std::string& b )
    {
        size_t p = 0;

        while( ( p = s.find( a, p ) ) != std::string::npos )
        {
            s.replace( p, a.size(), b );
            p += b.size();
        }
    };

    replaceAll( t.text, "%R", "${REFERENCE}" );
    replaceAll( t.text, "%V", "${VALUE}" );

    if( aFootprint >= 0 )
    {
        Footprint& fp = m_board.footprints[aFootprint];

        if( fieldName == "reference" || fieldName == "Reference" )
            fp.reference = t.text;
        else if( fieldName == "value" || fieldName == "Value" )
            fp.value = t.text;

        if( h == "property" )
            fp.fields[fieldName] = t.text;
        else if( fieldName == "reference" )
            fp.fields["Reference"] = t.text;
        else if( fieldName == "value" )
            fp.fields["Value"] = t.text;

        t.keepUpright = true;
    }

    bool hasAngle = false;
    bool hasPos = false;
    bool hasLayer = false;
    Pt   pos;

    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( c.isAtom() )
        {
            if( !c.isString() && c.text() == "hide" )
                t.visible = false;

            continue;
        }

        std::string_view k = c.head();

        if( k == "at" )
        {
            hasPos = true;
            pos = xy( c );
            N a = c.arg( 2 );

            if( a && a.isNumber() )
            {
                t.angle = a.isNumber() ? ParseNumber( a.text() ) : 0.0;
                hasAngle = true;
            }

            if( c.hasAtom( "unlocked" ) )
                t.keepUpright = false;
        }
        else if( k == "layer" )
        {
            t.layer = layerRef( c.str( 0 ) );
            hasLayer = true;

            if( c.hasAtom( "knockout" ) )
                t.knockout = true;
        }
        else if( k == "hide" )
        {
            std::string_view v = c.str( 0 );
            t.visible = ( v == "no" || v == "false" );

            if( v.empty() )
                t.visible = false;
        }
        else if( k == "unlocked" )
        {
            std::string_view v = c.str( 0 );
            t.keepUpright = ( v == "no" );
        }
        else if( k == "effects" )
        {
            parseTextEffects( c, t );
        }
        else if( k == "render_cache" )
        {
            parseRenderCache( c, t );
        }
    }

    if( !hasLayer || t.layer < 0 )
        return; // e.g. (property ki_fp_filters "...") or unknown layer

    (void) hasAngle;

    if( aFootprint >= 0 )
    {
        const Footprint& fp = m_board.footprints[aFootprint];

        if( m_version >= FIRST_FP_AFFINE_TRANSFORM )
            t.angle += fp.angle;

        t.pos = hasPos ? aXf.apply( pos ) : fp.pos;
        t.sizeX = aXf.len( t.sizeX );
        t.sizeY = aXf.len( t.sizeY );
        t.thickness = aXf.len( t.thickness );
    }
    else
    {
        t.pos = pos;
        t.keepUpright = false;
    }

    (void) aFpField;

    if( !t.visible )
        return;

    finishTextGeometry( t, m_version );
    m_board.texts.push_back( std::move( t ) );
}


void BoardParser::parseTextBox( N aNode, int aFootprint, const FpXform& aXf )
{
    // (gr_text_box "text" (start x y) (end x y) | (pts ...) (angle a) (margins l t r b)
    //    (layer L) (effects ...) (border yes) (stroke ...) (render_cache ...))
    Text t;
    t.footprint = aFootprint;
    N txt = aNode.arg( 0 );

    if( txt && !txt.isString() && txt.text() == "locked" )
        txt = txt.next();

    t.text = txt ? std::string( txt.text() ) : std::string();
    t.hAlign = HAlign::LEFT;
    t.vAlign = VAlign::TOP;

    Path   box;
    double angle = 0;
    double margins[4] = { -1, -1, -1, -1 };
    bool   border = true;
    double strokeW = DEFAULT_LINE_WIDTH;

    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view k = c.head();

        if( k == "start" )
            box.insert( box.begin(), xy( c ) );
        else if( k == "end" )
            box.push_back( xy( c ) );
        else if( k == "pts" )
        {
            box.clear();
            points( c, box );
        }
        else if( k == "angle" )
            angle = c.num( 0 );
        else if( k == "margins" )
        {
            for( int i = 0; i < 4; ++i )
                margins[i] = c.num( (uint32_t) i, -1 );
        }
        else if( k == "layer" )
            t.layer = layerRef( c.str( 0 ) );
        else if( k == "effects" )
            parseTextEffects( c, t );
        else if( k == "border" )
            border = c.str( 0 ) != "no";
        else if( k == "stroke" )
        {
            if( N w = c.find( "width" ) )
                strokeW = w.num( 0 );
        }
        else if( k == "render_cache" )
            parseRenderCache( c, t );
    }

    if( t.layer < 0 || box.size() < 2 )
        return;

    Path corners;

    if( box.size() == 2 )
        corners = { box[0], Pt( box[1].x, box[0].y ), box[1], Pt( box[0].x, box[1].y ) };
    else
        corners = box;

    for( Pt& p : corners )
        p = aXf.apply( p );

    if( border && strokeW > 0 )
    {
        Shape s;
        s.type = ShapeType::POLY;
        s.poly = corners;
        s.filled = false;
        s.width = aXf.len( strokeW );
        s.layer = t.layer;
        s.footprint = aFootprint;
        m_board.shapes.push_back( s );
    }

    double margin = t.sizeY * 0.75 + t.thickness; // KiCad default margin approximation

    if( margins[0] >= 0 )
        margin = margins[0];

    // anchor at the first corner (top-left in box frame) inset by the margins
    Pt     p0 = corners[0];
    double a = angle + ( aFootprint >= 0 && m_version >= FIRST_FP_AFFINE_TRANSFORM
                                 ? m_board.footprints[aFootprint].angle
                                 : 0.0 );
    Pt     inset = RotatePt( Pt( margin, margins[1] >= 0 ? margins[1] : margin ), a );

    t.pos = p0 + inset;
    t.angle = a;
    t.sizeX = aXf.len( t.sizeX );
    t.sizeY = aXf.len( t.sizeY );
    t.thickness = aXf.len( t.thickness );
    t.keepUpright = false;
    finishTextGeometry( t, m_version );
    m_board.texts.push_back( std::move( t ) );
}


// ---------------------------------------------------------------------------- footprints

void BoardParser::parseFootprint( N aNode )
{
    Footprint fp;
    N         lib = aNode.arg( 0 );

    if( lib && lib.text() == "locked" )
        lib = lib.next();

    fp.libId = lib ? std::string( lib.text() ) : std::string();

    FpXform xf;
    xf.active = true;

    // pass 1: placement
    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view k = c.head();

        if( k == "at" )
        {
            fp.pos = xy( c );
            fp.angle = c.num( 2, 0.0 );
        }
        else if( k == "layer" )
        {
            fp.layer = layerRef( c.str( 0 ) );
            fp.back = ( c.str( 0 ) == "B.Cu" )
                      || ( fp.layer >= 0 && m_board.layers[fp.layer].canonical == "B.Cu" );
        }
        else if( k == "transform" )
        {
            if( N tr = c.find( "translate" ) )
                fp.pos = xy( tr );

            if( N ro = c.find( "rotate" ) )
                fp.angle = ro.num( 0 );

            if( N sc = c.find( "scale" ) )
            {
                double sx = sc.num( 0, 1.0 ), sy = sc.num( 1, 1.0 );
                xf.scaleX = ( std::isfinite( sx ) && sx > 0 ) ? sx : 1.0;
                xf.scaleY = ( std::isfinite( sy ) && sy > 0 ) ? sy : 1.0;
            }
        }
        else if( k == "attr" )
        {
            for( N a = c.arg( 0 ); a; a = a.next() )
                fp.attrs.push_back( std::string( a.text() ) );
        }
    }

    xf.pos = fp.pos;
    xf.angle = fp.angle;

    m_board.footprints.push_back( fp );
    int idx = (int) m_board.footprints.size() - 1;

    // pass 2: children
    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view k = c.head();

        try
        {
            if( k == "fp_line" || k == "fp_arc" || k == "fp_circle" || k == "fp_rect"
                || k == "fp_poly" || k == "fp_curve" || k == "fp_ellipse" || k == "fp_ellipse_arc" )
                parseShape( c, idx, xf, m_board.shapes );
            else if( k == "fp_text" || k == "property" )
                parseText( c, idx, xf, true );
            else if( k == "fp_text_box" )
                parseTextBox( c, idx, xf );
            else if( k == "pad" )
                parsePad( c, idx, xf );
            else if( k == "zone" )
                parseZone( c, idx, xf );
            else if( k == "dimension" )
                parseDimension( c, idx, xf );
        }
        catch( const std::exception& e )
        {
            warn( std::string( "Footprint öğesi atlandı: " ) + e.what() );
        }
    }
}


void BoardParser::parsePadGeom( N aNode, PadGeom& g, bool aPadstackLayer )
{
    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view k = c.head();

        if( k == "shape" && aPadstackLayer )
        {
            std::string_view v = c.str( 0 );

            if( v == "circle" )
                g.shape = PadShape::CIRCLE;
            else if( v == "rect" )
                g.shape = PadShape::RECT;
            else if( v == "oval" )
                g.shape = PadShape::OVAL;
            else if( v == "trapezoid" )
                g.shape = PadShape::TRAPEZOID;
            else if( v == "roundrect" )
                g.shape = PadShape::ROUNDRECT;
            else if( v == "custom" )
                g.shape = PadShape::CUSTOM;
        }
        else if( k == "size" )
        {
            g.size.x = c.num( 0, g.size.x );
            g.size.y = c.num( 1, g.size.x );
        }
        else if( k == "offset" )
        {
            g.offset = xy( c );
        }
        else if( k == "rect_delta" )
        {
            g.delta = xy( c );
        }
        else if( k == "roundrect_rratio" )
        {
            g.rratio = c.num( 0, 0.25 );
        }
        else if( k == "chamfer_ratio" )
        {
            g.chamferRatio = c.num( 0 );
        }
        else if( k == "chamfer" )
        {
            for( N a = c.arg( 0 ); a; a = a.next() )
            {
                std::string_view v = a.text();

                if( v == "top_left" )
                    g.chamferCorners |= CHAMFER_TOP_LEFT;
                else if( v == "top_right" )
                    g.chamferCorners |= CHAMFER_TOP_RIGHT;
                else if( v == "bottom_left" )
                    g.chamferCorners |= CHAMFER_BOTTOM_LEFT;
                else if( v == "bottom_right" )
                    g.chamferCorners |= CHAMFER_BOTTOM_RIGHT;
            }
        }
        else if( k == "options" )
        {
            if( N an = c.find( "anchor" ) )
                g.anchor = ( an.str( 0 ) == "rect" ) ? PadShape::RECT : PadShape::CIRCLE;
        }
        else if( k == "primitives" )
        {
            FpXform none;

            for( N p = c.arg( 0 ); p; p = p.next() )
            {
                if( !p.isList() )
                    continue;

                std::string_view ph = p.head();

                if( ph.size() > 3 && ph.compare( 0, 3, "gr_" ) == 0 )
                    parseShape( p, -1, none, g.primitives, true );
            }
        }
    }

    if( g.shape == PadShape::ROUNDRECT && g.chamferRatio > 0 && g.chamferCorners )
        g.shape = PadShape::CHAMFERED_RECT;
}


void BoardParser::parsePad( N aNode, int aFootprint, const FpXform& aXf )
{
    Pad p;
    p.footprint = aFootprint;
    p.number = std::string( aNode.str( 0 ) );

    std::string_view type = aNode.str( 1 );
    std::string_view shape = aNode.str( 2 );

    if( type == "thru_hole" )
        p.attr = PadAttr::PTH;
    else if( type == "np_thru_hole" )
        p.attr = PadAttr::NPTH;
    else if( type == "connect" )
        p.attr = PadAttr::CONN;
    else
        p.attr = PadAttr::SMD;

    if( shape == "circle" )
        p.geom.shape = PadShape::CIRCLE;
    else if( shape == "rect" )
        p.geom.shape = PadShape::RECT;
    else if( shape == "oval" )
        p.geom.shape = PadShape::OVAL;
    else if( shape == "trapezoid" )
        p.geom.shape = PadShape::TRAPEZOID;
    else if( shape == "roundrect" )
        p.geom.shape = PadShape::ROUNDRECT;
    else if( shape == "custom" )
        p.geom.shape = PadShape::CUSTOM;

    Pt   local;
    bool hasAngle = false;

    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view k = c.head();

        if( k == "at" )
        {
            local = xy( c );
            N a = c.arg( 2 );

            if( a && a.isNumber() )
            {
                p.angle = ParseNumber( a.text() );
                hasAngle = true;
            }
        }
        else if( k == "drill" )
        {
            N a = c.arg( 0 );

            if( a && a.text() == "oval" )
            {
                p.drillOval = true;
                a = a.next();
            }

            double dx = 0, dy = 0;

            if( a && a.isNumber() )
            {
                dx = ParseNumber( a.text() );
                a = a.next();
            }

            if( a && a.isNumber() )
                dy = ParseNumber( a.text() );
            else
                dy = dx;

            p.drill = Pt( dx, dy );

            // (offset x y) inside drill: shape offset relative to the hole
            if( N off = c.find( "offset" ) )
                p.geom.offset = xy( off );
        }
        else if( k == "layers" )
        {
            layerList( c, p.layers );
        }
        else if( k == "net" )
        {
            p.net = netRef( c );
        }
        else if( k == "pinfunction" )
        {
            p.pinFunction = std::string( c.str( 0 ) );
        }
        else if( k == "pintype" )
        {
            p.pinType = std::string( c.str( 0 ) );
        }
    }

    parsePadGeom( aNode, p.geom, false );

    // padstack overrides
    if( N ps = aNode.find( "padstack" ) )
    {
        std::string_view mode = ps.find( "mode" ) ? ps.find( "mode" ).str( 0 ) : "normal";

        for( N L = ps.arg( 0 ); L; L = L.next() )
        {
            if( !L.isList() || L.head() != "layer" )
                continue;

            PadGeom g = p.geom; // start from the default definition
            g.primitives.clear();
            g.chamferCorners = 0;
            g.chamferRatio = 0;
            parsePadGeom( L, g, true );

            if( g.primitives.empty() && g.shape == PadShape::CUSTOM )
                g.primitives = p.geom.primitives;

            std::string lname( L.str( 0 ) );

            if( lname == "Inner" )
            {
                p.layerGeom.emplace_back( -1, g );
                p.innerGeom = (int) p.layerGeom.size() - 1;
            }
            else
            {
                int li = layerRef( lname, false );

                if( li >= 0 )
                    p.layerGeom.emplace_back( li, g );
            }
        }

        (void) mode;
    }

    if( !hasAngle )
        p.angle = 0.0;

    p.pos = aXf.apply( local );

    if( p.attr == PadAttr::SMD || p.attr == PadAttr::CONN )
        p.drill = Pt();
    else if( p.drill.x <= 0 && p.drill.y <= 0 && p.attr == PadAttr::NPTH )
        p.drill = Pt( std::min( p.geom.size.x, p.geom.size.y ),
                      std::min( p.geom.size.x, p.geom.size.y ) );

    if( aFootprint >= 0 )
        m_board.footprints[aFootprint].padCount++;

    m_board.pads.push_back( std::move( p ) );
}


// ---------------------------------------------------------------------------- tracks & vias

void BoardParser::parseTrack( N aNode )
{
    Track t;
    t.start = xy( aNode.find( "start" ) );
    t.end = xy( aNode.find( "end" ) );

    if( N w = aNode.find( "width" ) )
        t.width = w.num( 0, 0.2 );

    if( N l = aNode.find( "layer" ) )
        t.layer = layerRef( l.str( 0 ) );

    if( N n = aNode.find( "net" ) )
        t.net = netRef( n );

    if( t.layer >= 0 )
        m_board.tracks.push_back( t );
}


void BoardParser::parseArc( N aNode )
{
    ArcTrack a;
    a.start = xy( aNode.find( "start" ) );
    a.mid = xy( aNode.find( "mid" ) );
    a.end = xy( aNode.find( "end" ) );

    if( N w = aNode.find( "width" ) )
        a.width = w.num( 0, 0.2 );

    if( N l = aNode.find( "layer" ) )
        a.layer = layerRef( l.str( 0 ) );

    if( N n = aNode.find( "net" ) )
        a.net = netRef( n );

    if( a.layer >= 0 )
        m_board.arcs.push_back( a );
}


void BoardParser::parseVia( N aNode )
{
    Via v;

    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( c.isAtom() )
        {
            std::string_view t = c.text();

            if( t == "blind" )
                v.type = ViaType::BLIND;
            else if( t == "micro" )
                v.type = ViaType::MICRO;
            else if( t == "buried" )
                v.type = ViaType::BURIED;

            continue;
        }

        std::string_view k = c.head();

        if( k == "at" )
            v.pos = xy( c );
        else if( k == "size" )
            v.size = c.num( 0, 0.6 );
        else if( k == "drill" )
            v.drill = c.num( 0, 0.3 );
        else if( k == "layers" )
        {
            std::vector<int> ls;

            for( N a = c.arg( 0 ); a; a = a.next() )
                ls.push_back( layerRef( a.text() ) );

            if( ls.size() >= 2 )
            {
                v.layerA = ls[0];
                v.layerB = ls[1];
            }
            else if( ls.size() == 1 )
            {
                v.layerA = v.layerB = ls[0];
            }
        }
        else if( k == "net" )
            v.net = netRef( c );
        else if( k == "padstack" )
        {
            for( N L = c.arg( 0 ); L; L = L.next() )
            {
                if( !L.isList() || L.head() != "layer" )
                    continue;

                double s = L.find( "size" ) ? L.find( "size" ).num( 0, v.size ) : v.size;
                std::string lname( L.str( 0 ) );

                if( lname == "Inner" )
                    v.innerSize = s;
                else
                {
                    int li = layerRef( lname, false );

                    if( li >= 0 )
                        v.sizeOverride.emplace_back( li, s );
                }
            }
        }
    }

    if( !aNode.find( "drill" ) && m_defaultViaDrill > 0 )
        v.drill = m_defaultViaDrill;

    m_board.vias.push_back( v );
}


// ---------------------------------------------------------------------------- zones

void BoardParser::parseZone( N aNode, int aFootprint, const FpXform& aXf )
{
    Zone z;
    z.footprint = aFootprint;
    bool libFrame = ( aFootprint >= 0 && m_version >= FIRST_FP_AFFINE_TRANSFORM );
    bool foundThicknessFlag = false;

    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view k = c.head();

        if( k == "min_thickness" )
            z.minThickness = c.num( 0 );
        else if( k == "filled_areas_thickness" )
        {
            foundThicknessFlag = true;
            z.strokeFill = c.str( 0 ) != "no";
        }
        else if( k == "net" )
            z.net = netRef( c );
        else if( k == "net_name" )
        {
            z.netName = std::string( c.str( 0 ) );

            if( z.net == 0 && !z.netName.empty() )
            {
                auto it = m_board.netByName.find( z.netName );

                if( it != m_board.netByName.end() )
                    z.net = it->second;
            }
        }
        else if( k == "layer" )
            z.layers.push_back( layerRef( c.str( 0 ) ) );
        else if( k == "layers" )
            layerList( c, z.layers );
        else if( k == "name" )
            z.name = std::string( c.str( 0 ) );
        else if( k == "keepout" )
            z.keepout = true;
        else if( k == "polygon" )
        {
            if( N pts = c.find( "pts" ) )
            {
                z.outline.clear();
                points( pts, z.outline );
            }
        }
        else if( k == "filled_polygon" )
        {
            int layer = -1;

            if( N l = c.find( "layer" ) )
                layer = layerRef( l.str( 0 ) );

            Path poly;

            if( N pts = c.find( "pts" ) )
                points( pts, poly );

            if( poly.size() < 3 )
                continue;

            if( libFrame )
            {
                for( Pt& p : poly )
                    p = aXf.apply( p );
            }

            if( layer < 0 )
                layer = z.layers.empty() ? -1 : z.layers[0];

            if( layer < 0 )
                continue;

            auto it = std::find_if( z.fills.begin(), z.fills.end(),
                                    [&]( const ZoneFill& f ) { return f.layer == layer; } );

            if( it == z.fills.end() )
            {
                z.fills.push_back( ZoneFill{ layer, {} } );
                it = z.fills.end() - 1;
            }

            it->polys.push_back( std::move( poly ) );
        }
    }

    if( libFrame )
    {
        for( Pt& p : z.outline )
            p = aXf.apply( p );
    }

    // KiCad 5 and older drew the stored fill outlines with a min_thickness pen
    if( !foundThicknessFlag && m_version <= 20200000 )
        z.strokeFill = true;

    if( z.minThickness <= 0 )
        z.strokeFill = false;

    z.layers.erase( std::remove( z.layers.begin(), z.layers.end(), -1 ), z.layers.end() );
    m_board.zones.push_back( std::move( z ) );
}


// ---------------------------------------------------------------------------- dimensions

void BoardParser::parseDimension( N aNode, int aFootprint, const FpXform& aXf )
{
    int    layer = -1;
    double width = 0.15;
    double arrowLen = 1.27;
    double extOffset = 0.0;
    double extHeight = 0.58;
    double height = 0.0;
    int    orientation = 0;
    std::string type = "aligned";
    Path   pts;

    if( N l = aNode.find( "layer" ) )
        layer = layerRef( l.str( 0 ) );

    if( layer < 0 )
        return;

    if( N t = aNode.find( "type" ) )
        type = std::string( t.str( 0 ) );

    if( N p = aNode.find( "pts" ) )
        points( p, pts );

    if( N h = aNode.find( "height" ) )
        height = h.num( 0 );

    if( N o = aNode.find( "orientation" ) )
        orientation = (int) o.num( 0 );

    if( N w = aNode.find( "width" ) )
        width = w.num( 0, width );

    if( N st = aNode.find( "style" ) )
    {
        if( N t = st.find( "thickness" ) )
            width = t.num( 0, width );
        if( N a = st.find( "arrow_length" ) )
            arrowLen = a.num( 0, arrowLen );
        if( N e = st.find( "extension_offset" ) )
            extOffset = e.num( 0 );
        if( N e = st.find( "extension_height" ) )
            extHeight = e.num( 0, extHeight );
    }

    // dimension text
    if( N gt = aNode.find( "gr_text" ) )
        parseText( gt, -1, FpXform() );

    auto line = [&]( Pt a, Pt b )
    {
        Shape s;
        s.type = ShapeType::SEGMENT;
        s.start = aXf.apply( a );
        s.end = aXf.apply( b );
        s.width = width;
        s.layer = layer;
        s.footprint = aFootprint;
        m_board.shapes.push_back( s );
    };

    auto arrow = [&]( Pt tip, Pt dir )
    {
        double l = std::hypot( dir.x, dir.y );

        if( l <= 0 )
            return;

        Pt u( dir.x / l * arrowLen, dir.y / l * arrowLen );
        line( tip, tip + RotatePt( u, 27.5 ) );
        line( tip, tip + RotatePt( u, -27.5 ) );
    };

    // Legacy (KiCad 5) dimensions carry their geometry explicitly
    bool legacy = false;

    for( const char* key : { "feature1", "feature2", "crossbar", "arrow1a", "arrow1b", "arrow2a", "arrow2b" } )
    {
        if( N f = aNode.find( key ) )
        {
            legacy = true;
            Path p;

            if( N fp = f.find( "pts" ) )
                points( fp, p );

            if( p.size() >= 2 )
                line( p[0], p[1] );
        }
    }

    if( legacy || pts.size() < 2 )
        return;

    Pt p0 = pts[0], p1 = pts[1];

    if( type == "aligned" || type == "orthogonal" )
    {
        Pt a = p0, b = p1;

        if( type == "orthogonal" )
        {
            // orientation 0 = horizontal measurement, 1 = vertical
            if( orientation == 0 )
                b = Pt( p1.x, p0.y );
            else
                b = Pt( p0.x, p1.y );
        }

        Pt     d = b - a;
        double len = std::hypot( d.x, d.y );

        if( len <= 0 )
            return;

        Pt     ext( d.y / len, -d.x / len ); // perpendicular
        Pt     extVec = ext * height;
        Pt     ca = a + extVec;
        Pt     cb = b + extVec;
        double sgn = height >= 0 ? 1.0 : -1.0;

        if( type == "orthogonal" )
        {
            // extension lines from the real feature points
            Pt fa = p0, fb = p1;
            line( fa + ( ca - fa ) * ( extOffset / std::max( 1e-9, std::hypot( ca.x - fa.x, ca.y - fa.y ) ) ),
                  ca + ext * ( sgn * extHeight ) );
            line( fb + ( cb - fb ) * ( extOffset / std::max( 1e-9, std::hypot( cb.x - fb.x, cb.y - fb.y ) ) ),
                  cb + ext * ( sgn * extHeight ) );
        }
        else
        {
            line( a + ext * ( sgn * extOffset ), ca + ext * ( sgn * extHeight ) );
            line( b + ext * ( sgn * extOffset ), cb + ext * ( sgn * extHeight ) );
        }

        line( ca, cb );
        arrow( ca, cb - ca );
        arrow( cb, ca - cb );
    }
    else if( type == "leader" )
    {
        line( p0, p1 );
        arrow( p0, p1 - p0 );
    }
    else if( type == "center" )
    {
        Pt d = p1 - p0;
        line( p0 - d, p0 + d );
        Pt r = RotatePt( d, 90 );
        line( p0 - r, p0 + r );
    }
    else if( type == "radial" )
    {
        line( p0, p1 );
        arrow( p1, p0 - p1 );
    }
}


void BoardParser::parseTarget( N aNode )
{
    int layer = -1;

    if( N l = aNode.find( "layer" ) )
        layer = layerRef( l.str( 0 ) );

    if( layer < 0 )
        return;

    Pt     at = aNode.find( "at" ) ? xy( aNode.find( "at" ) ) : Pt();
    double size = aNode.find( "size" ) ? aNode.find( "size" ).num( 0, 5 ) : 5;
    double width = aNode.find( "width" ) ? aNode.find( "width" ).num( 0, 0.15 ) : 0.15;
    bool   xShape = aNode.hasAtom( "x" );
    double r = size / 3.0;

    Shape c;
    c.type = ShapeType::CIRCLE;
    c.start = at;
    c.end = at + Pt( r, 0 );
    c.width = width;
    c.layer = layer;
    m_board.shapes.push_back( c );

    double h = size / 2.0;

    for( int i = 0; i < 2; ++i )
    {
        Shape s;
        s.type = ShapeType::SEGMENT;
        Pt d = i == 0 ? Pt( h, 0 ) : Pt( 0, h );

        if( xShape )
            d = RotatePt( d, 45 );

        s.start = at - d;
        s.end = at + d;
        s.width = width;
        s.layer = layer;
        m_board.shapes.push_back( s );
    }
}


// ------------------------------------------------------------------------------------------
// Board methods
// ------------------------------------------------------------------------------------------

int Board::LayerIndex( const std::string& aName ) const
{
    auto it = layerByName.find( aName );
    return it == layerByName.end() ? -1 : it->second;
}


static void shapeBBox( const Shape& s, BBox& b )
{
    double hw = s.width / 2;

    switch( s.type )
    {
    case ShapeType::SEGMENT:
    case ShapeType::RECT:
        b.add( s.start, hw );
        b.add( s.end, hw );
        break;

    case ShapeType::ARC:
    {
        Path p;
        ArcToPoints( p, s.start, s.mid, s.end, 0.01 );

        for( const Pt& q : p )
            b.add( q, hw );

        break;
    }

    case ShapeType::CIRCLE:
    {
        double r = std::hypot( s.end.x - s.start.x, s.end.y - s.start.y );
        b.add( s.start, r + hw );
        break;
    }

    case ShapeType::POLY:
    case ShapeType::BEZIER:
        for( const Pt& q : s.poly )
            b.add( q, hw );

        break;
    }
}


BBox Board::OutlineBBox() const
{
    BBox b;

    for( const Shape& s : shapes )
    {
        if( s.layer >= 0 && layers[s.layer].kind == LayerKind::EDGE )
        {
            BBox sb;
            shapeBBox( s, sb );

            // outline extents are measured on the centre line
            if( sb.valid() )
            {
                double hw = s.width / 2;
                sb.minX += hw;
                sb.minY += hw;
                sb.maxX -= hw;
                sb.maxY -= hw;
                b.add( sb );
            }
        }
    }

    return b;
}


BBox Board::ContentBBox() const
{
    BBox b;

    for( const Shape& s : shapes )
        shapeBBox( s, b );

    for( const Track& t : tracks )
    {
        b.add( t.start, t.width / 2 );
        b.add( t.end, t.width / 2 );
    }

    for( const ArcTrack& a : arcs )
    {
        b.add( a.start, a.width / 2 );
        b.add( a.mid, a.width / 2 );
        b.add( a.end, a.width / 2 );
    }

    for( const Via& v : vias )
        b.add( v.pos, v.size / 2 );

    for( const Pad& p : pads )
        b.add( p.pos, std::max( p.geom.size.x, p.geom.size.y ) / 2 );

    for( const Zone& z : zones )
    {
        for( const ZoneFill& f : z.fills )
        {
            for( const Path& poly : f.polys )
            {
                for( const Pt& q : poly )
                    b.add( q );
            }
        }
    }

    for( const Text& t : texts )
        b.add( t.pos, std::max( t.sizeX, t.sizeY ) );

    return b;
}


BoardStats Board::Stats() const
{
    BoardStats s;
    s.footprints = (int) footprints.size();
    s.pads = (int) pads.size();
    s.tracks = (int) tracks.size();
    s.arcs = (int) arcs.size();
    s.vias = (int) vias.size();

    for( const Zone& z : zones )
    {
        if( z.keepout )
            s.keepouts++;
        else
            s.zones++;
    }

    for( size_t i = 1; i < nets.size(); ++i )
    {
        if( !nets[i].empty() )
            s.nets++;
    }

    s.copperLayers = (int) copperStack.size();

    for( const Shape& sh : shapes )
    {
        if( sh.footprint < 0 )
            s.graphics++;
    }

    for( const Text& t : texts )
    {
        if( t.footprint < 0 )
            s.texts++;
    }

    for( const Pad& p : pads )
    {
        if( p.drill.x > 0 || p.drill.y > 0 )
            s.holes++;
    }

    s.holes += s.vias;

    BBox b = OutlineBBox();

    if( b.valid() )
    {
        s.widthMM = b.maxX - b.minX;
        s.heightMM = b.maxY - b.minY;
    }

    return s;
}


std::string Board::ResolveText( const std::string& aText, int aFootprint, int aLayer ) const
{
    if( aText.find( "${" ) == std::string::npos )
        return aText;

    std::string out;
    out.reserve( aText.size() );

    for( size_t i = 0; i < aText.size(); )
    {
        if( aText.compare( i, 2, "${" ) == 0 )
        {
            size_t close = aText.find( '}', i + 2 );

            if( close == std::string::npos )
            {
                out.append( aText, i, std::string::npos );
                break;
            }

            std::string var = aText.substr( i + 2, close - i - 2 );
            std::string upper = var;

            for( char& ch : upper )
                ch = (char) toupper( (unsigned char) ch );

            bool        found = false;
            std::string value;

            if( aFootprint >= 0 && aFootprint < (int) footprints.size() )
            {
                const Footprint& fp = footprints[aFootprint];

                if( upper == "REFERENCE" )
                {
                    value = fp.reference;
                    found = true;
                }
                else if( upper == "VALUE" )
                {
                    value = fp.value;
                    found = true;
                }
                else if( upper == "FOOTPRINT_LIBRARY" || upper == "FOOTPRINT_NAME" )
                {
                    size_t colon = fp.libId.find( ':' );

                    if( upper == "FOOTPRINT_LIBRARY" )
                        value = colon == std::string::npos ? std::string() : fp.libId.substr( 0, colon );
                    else
                        value = colon == std::string::npos ? fp.libId : fp.libId.substr( colon + 1 );

                    found = true;
                }
                else
                {
                    for( const auto& [k, v] : fp.fields )
                    {
                        std::string ku = k;

                        for( char& ch : ku )
                            ch = (char) toupper( (unsigned char) ch );

                        if( ku == upper )
                        {
                            value = v;
                            found = true;
                            break;
                        }
                    }
                }
            }

            if( !found && upper == "LAYER" && aLayer >= 0 && aLayer < (int) layers.size() )
            {
                value = layers[aLayer].display;
                found = true;
            }

            if( !found )
            {
                auto it = titleBlock.find( upper );

                if( it != titleBlock.end() )
                {
                    value = it->second;
                    found = true;
                }
            }

            if( found )
                out += value;
            else
                out.append( aText, i, close - i + 1 );

            i = close + 1;
        }
        else
        {
            out.push_back( aText[i++] );
        }
    }

    return out;
}


void Board::Finalize()
{
    // Footprint outlines in the footprint frame: courtyard if present, otherwise pads and
    // fab/silk drawings. Used for picking and zoom-to.
    std::vector<BBox> court( footprints.size() ), other( footprints.size() );

    auto toLocal = [&]( int fp, const Pt& p )
    {
        const Footprint& f = footprints[(size_t) fp];
        return RotatePt( p - f.pos, -f.angle );
    };

    for( const Shape& s : shapes )
    {
        if( s.footprint < 0 || s.layer < 0 )
            continue;

        LayerKind k = layers[(size_t) s.layer].kind;
        BBox*     box = nullptr;

        if( k == LayerKind::COURTYARD )
            box = &court[(size_t) s.footprint];
        else if( k == LayerKind::FAB || k == LayerKind::SILK )
            box = &other[(size_t) s.footprint];

        if( !box )
            continue;

        auto addPt = [&]( const Pt& q ) { box->add( toLocal( s.footprint, q ), s.width / 2 ); };

        switch( s.type )
        {
        case ShapeType::CIRCLE:
        {
            double r = std::hypot( s.end.x - s.start.x, s.end.y - s.start.y );
            box->add( toLocal( s.footprint, s.start ), r + s.width / 2 );
            break;
        }
        case ShapeType::ARC:
            addPt( s.start );
            addPt( s.mid );
            addPt( s.end );
            break;
        case ShapeType::POLY:
        case ShapeType::BEZIER:
            for( const Pt& q : s.poly )
                addPt( q );
            break;
        default:
            addPt( s.start );
            addPt( s.end );
        }
    }

    for( const Pad& p : pads )
    {
        if( p.footprint < 0 )
            continue;

        double r = std::max( p.geom.size.x, p.geom.size.y ) / 2;
        Pt     c = p.pos + RotatePt( p.geom.offset, p.angle );
        other[(size_t) p.footprint].add( toLocal( p.footprint, c ), r );
    }

    for( size_t i = 0; i < footprints.size(); ++i )
    {
        footprints[i].localBox = court[i].valid() ? court[i] : other[i];

        if( !footprints[i].localBox.valid() )
        {
            footprints[i].localBox.add( Pt( -0.5, -0.5 ) );
            footprints[i].localBox.add( Pt( 0.5, 0.5 ) );
        }
    }
}


void LoadBoard( Board& aBoard, const char* aData, size_t aLen )
{
    aBoard = Board();
    aBoard.nets.push_back( std::string() ); // net 0 = no net
    BoardParser parser( aBoard );
    parser.Parse( aData, aLen );
    aBoard.Finalize();
}

} // namespace kp
