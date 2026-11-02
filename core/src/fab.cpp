/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Gerber files follow the Ucamco Gerber Layer Format Specification (2023.08): RS-274X with
 * X2 file and aperture attributes, millimetres, 4.6 coordinates. Drill files are Excellon
 * with decimal millimetre coordinates. Y is flipped (board Y points down, Gerber Y up) as
 * KiCad does, so the files line up with KiCad's own.
 */
#include "fab.h"
#include "geom.h"
#include "i18n.h"
#include "pick.h"
#include "stroke_font.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace kp
{

namespace
{

constexpr double PI_ = 3.14159265358979323846;

std::string num( double v, int decimals = 6 )
{
    char buf[48];
    snprintf( buf, sizeof( buf ), "%.*f", decimals, v );
    std::string s( buf );

    if( s.find( '.' ) != std::string::npos )
    {
        while( !s.empty() && s.back() == '0' )
            s.pop_back();

        if( !s.empty() && s.back() == '.' )
            s.pop_back();
    }

    if( s == "-0" )
        s = "0";

    return s;
}

bool isLayerKind( const Board& b, int l, LayerKind k )
{
    return l >= 0 && l < (int) b.layers.size() && b.layers[(size_t) l].kind == k;
}

bool padOnLayer( const Pad& p, int l )
{
    return std::find( p.layers.begin(), p.layers.end(), l ) != p.layers.end();
}

/// Outward offset of a convex polygon (mitred corners).
Path offsetConvex( const Path& poly, double m )
{
    if( std::fabs( m ) < 1e-9 || poly.size() < 3 )
        return poly;

    // orientation
    double area = 0;

    for( size_t i = 0; i < poly.size(); ++i )
    {
        const Pt& a = poly[i];
        const Pt& b = poly[( i + 1 ) % poly.size()];
        area += a.x * b.y - b.x * a.y;
    }

    double sgn = area > 0 ? -1.0 : 1.0; // outward normal side (Y down)
    Path   out;
    size_t n = poly.size();

    for( size_t i = 0; i < n; ++i )
    {
        const Pt& p0 = poly[( i + n - 1 ) % n];
        const Pt& p1 = poly[i];
        const Pt& p2 = poly[( i + 1 ) % n];
        Pt        e1 = p1 - p0, e2 = p2 - p1;
        double    l1 = std::hypot( e1.x, e1.y ), l2 = std::hypot( e2.x, e2.y );

        if( l1 < 1e-12 || l2 < 1e-12 )
        {
            out.push_back( p1 );
            continue;
        }

        Pt     n1( sgn * e1.y / l1, -sgn * e1.x / l1 ), n2( sgn * e2.y / l2, -sgn * e2.x / l2 );
        Pt     bis = n1 + n2;
        double bl = std::hypot( bis.x, bis.y );

        if( bl < 1e-9 )
        {
            out.push_back( p1 + n1 * m );
            continue;
        }

        bis = bis * ( 1.0 / bl );
        double c = bis.x * n1.x + bis.y * n1.y;
        out.push_back( p1 + bis * ( m / std::max( 0.2, c ) ) );
    }

    return out;
}

Path convexHull( std::vector<Pt> pts )
{
    std::sort( pts.begin(), pts.end(), []( const Pt& a, const Pt& b ) { return a.x < b.x || ( a.x == b.x && a.y < b.y ); } );

    if( pts.size() < 3 )
        return pts;

    auto cross = []( const Pt& o, const Pt& a, const Pt& b ) { return ( a.x - o.x ) * ( b.y - o.y ) - ( a.y - o.y ) * ( b.x - o.x ); };
    Path h( 2 * pts.size() );
    size_t k = 0;

    for( size_t i = 0; i < pts.size(); ++i )
    {
        while( k >= 2 && cross( h[k - 2], h[k - 1], pts[i] ) <= 0 )
            --k;

        h[k++] = pts[i];
    }

    for( size_t i = pts.size() - 1, t = k + 1; i-- > 0; )
    {
        while( k >= t && cross( h[k - 2], h[k - 1], pts[i] ) <= 0 )
            --k;

        h[k++] = pts[i];
    }

    h.resize( k - 1 );
    return h;
}

/// Points on the outline of primitives grown by their radius (for hulls).
void outlinePoints( const std::vector<Prim>& prims, std::vector<Pt>& out )
{
    for( const Prim& p : prims )
    {
        std::vector<Pt> core;

        if( p.kind == Prim::POINT )
            core = { p.a };
        else if( p.kind == Prim::SEGMENT )
            core = { p.a, p.b };
        else
            core = p.poly;

        for( const Pt& c : core )
        {
            if( p.r <= 1e-9 )
            {
                out.push_back( c );
                continue;
            }

            for( int k = 0; k < 16; ++k )
                out.push_back( Pt( c.x + p.r * std::cos( 2 * PI_ * k / 16 ), c.y + p.r * std::sin( 2 * PI_ * k / 16 ) ) );
        }
    }
}

double polyDistance( const Path& a, const Path& b )
{
    double d = 1e300;

    for( size_t i = 0; i < a.size(); ++i )
    {
        for( size_t j = 0; j < b.size(); ++j )
            d = std::min( d, SegSegDistance( a[i], a[( i + 1 ) % a.size()], b[j], b[( j + 1 ) % b.size()] ) );
    }

    return d;
}

/// Join holes to their outline with zero-width cuts, so a polygon with holes becomes one
/// outline a Gerber region can take.
Path fracture( const std::vector<Path>& outlineAndHoles )
{
    if( outlineAndHoles.empty() )
        return {};

    Path out = outlineAndHoles[0];

    for( size_t h = 1; h < outlineAndHoles.size(); ++h )
    {
        const Path& hole = outlineAndHoles[h];

        if( hole.size() < 3 || out.size() < 3 )
            continue;

        // leftmost hole vertex, joined to the nearest outline vertex
        size_t hi = 0;

        for( size_t i = 1; i < hole.size(); ++i )
        {
            if( hole[i].x < hole[hi].x )
                hi = i;
        }

        size_t oi = 0;
        double best = 1e300;

        for( size_t i = 0; i < out.size(); ++i )
        {
            double d = std::hypot( out[i].x - hole[hi].x, out[i].y - hole[hi].y );

            if( d < best )
            {
                best = d;
                oi = i;
            }
        }

        Path merged( out.begin(), out.begin() + (long) oi + 1 );

        for( size_t k = 0; k <= hole.size(); ++k )
            merged.push_back( hole[( hi + k ) % hole.size()] );

        merged.insert( merged.end(), out.begin() + (long) oi, out.end() );
        out.swap( merged );
    }

    return out;
}


class Gerber
{
public:
    Gerber( const Pt& aOrigin ) :
            m_origin( aOrigin )
    {
    }

    std::string X( double x ) const { return std::to_string( std::llround( ( x - m_origin.x ) * 1e6 ) ); }
    std::string Y( double y ) const { return std::to_string( std::llround( -( y - m_origin.y ) * 1e6 ) ); }
    std::string XY( const Pt& p ) const { return "X" + X( p.x ) + "Y" + Y( p.y ); }

    /// Relative macro coordinates (mm, Gerber frame) of a board point around a flash point.
    std::string rel( const Pt& p, const Pt& centre ) const { return num( p.x - centre.x ) + "," + num( -( p.y - centre.y ) ); }

    std::string macro( const std::string& body )
    {
        auto it = m_macros.find( body );

        if( it != m_macros.end() )
            return it->second;

        std::string name = "NL" + std::to_string( m_macros.size() );
        m_macros[body] = name;
        m_macroText += "%AM" + name + "*\n" + body + "%\n";
        return name;
    }

    int aperture( const std::string& def, const char* func )
    {
        std::string key = def + "|" + ( func ? func : "" );
        auto        it = m_apertures.find( key );

        if( it != m_apertures.end() )
            return it->second;

        int d = 10 + (int) m_apertures.size();
        m_apertures[key] = d;

        if( func && m_headerFunc != func )
        {
            m_apertureText += std::string( "%TA.AperFunction," ) + func + "*%\n";
            m_headerFunc = func;
        }
        else if( !func && !m_headerFunc.empty() )
        {
            m_apertureText += "%TD.AperFunction*%\n";
            m_headerFunc.clear();
        }

        m_apertureText += "%ADD" + std::to_string( d ) + def + "*%\n";
        return d;
    }

    /// Dark (normal) or clear polarity for what follows.
    void polarity( bool aDark )
    {
        if( aDark != m_dark )
        {
            m_body += aDark ? "%LPD*%\n" : "%LPC*%\n";
            m_dark = aDark;
        }
    }

    void select( int d )
    {
        if( d != m_current )
        {
            m_body += "D" + std::to_string( d ) + "*\n";
            m_current = d;
        }
    }

    void flash( const Pt& p, int d )
    {
        select( d );
        m_body += XY( p ) + "D03*\n";
    }

    int circle( double dia, const char* func ) { return aperture( "C," + num( std::max( 0.0, dia ) ), func ); }

    void line( const Pt& a, const Pt& b, double width, const char* func )
    {
        select( circle( width, func ) );

        if( !m_penValid || std::hypot( m_pen.x - a.x, m_pen.y - a.y ) > 1e-9 )
            m_body += XY( a ) + "D02*\n";

        m_body += XY( b ) + "D01*\n";
        m_pen = b;
        m_penValid = true;
    }

    void polyline( const Path& pts, double width, bool closed, const char* func )
    {
        if( pts.size() == 1 )
        {
            flash( pts[0], circle( width, func ) );
            return;
        }

        for( size_t i = 0; i + 1 < pts.size(); ++i )
            line( pts[i], pts[i + 1], width, func );

        if( closed && pts.size() > 2 )
            line( pts.back(), pts.front(), width, func );
    }

    void arc( const Pt& s, const Pt& mid, const Pt& e, double width, const char* func )
    {
        // circle through three points
        double ax = s.x, ay = s.y, bx = mid.x, by = mid.y, cx = e.x, cy = e.y;
        double d = 2 * ( ax * ( by - cy ) + bx * ( cy - ay ) + cx * ( ay - by ) );

        if( std::fabs( d ) < 1e-12 )
        {
            line( s, e, width, func );
            return;
        }

        double ux = ( ( ax * ax + ay * ay ) * ( by - cy ) + ( bx * bx + by * by ) * ( cy - ay ) + ( cx * cx + cy * cy ) * ( ay - by ) ) / d;
        double uy = ( ( ax * ax + ay * ay ) * ( cx - bx ) + ( bx * bx + by * by ) * ( ax - cx ) + ( cx * cx + cy * cy ) * ( bx - ax ) ) / d;
        Pt     c( ux, uy );

        // Turning direction of start -> mid -> end. In board coordinates (Y down) a positive
        // cross product is clockwise as seen on screen; flipping Y for Gerber keeps the picture,
        // so that is clockwise (G02) in the Gerber file too.
        double orient = ( mid.x - s.x ) * ( e.y - s.y ) - ( mid.y - s.y ) * ( e.x - s.x );
        bool   cwOnScreen = orient > 0;
        select( circle( width, func ) );

        if( !m_penValid || std::hypot( m_pen.x - s.x, m_pen.y - s.y ) > 1e-9 )
            m_body += XY( s ) + "D02*\n";

        m_body += cwOnScreen ? "G02*\n" : "G03*\n";
        m_body += XY( e ) + "I" + std::to_string( std::llround( ( c.x - s.x ) * 1e6 ) ) + "J"
                  + std::to_string( std::llround( -( c.y - s.y ) * 1e6 ) ) + "D01*\nG01*\n";
        m_pen = e;
        m_penValid = true;
    }

    void fullCircle( const Pt& c, double r, double width, const char* func )
    {
        Pt s( c.x + r, c.y );
        select( circle( width, func ) );
        m_body += XY( s ) + "D02*\nG02*\n" + XY( s ) + "I" + std::to_string( std::llround( -r * 1e6 ) ) + "J0D01*\nG01*\n";
        m_pen = s;
        m_penValid = true;
    }

    void region( const Path& poly, const char* func )
    {
        if( poly.size() < 3 )
            return;

        if( ( func ? std::string( func ) : std::string() ) != m_bodyFunc )
        {
            if( func )
                m_body += std::string( "%TA.AperFunction," ) + func + "*%\n";
            else
                m_body += "%TD.AperFunction*%\n";

            m_bodyFunc = func ? func : "";
        }

        m_body += "G36*\n" + XY( poly[0] ) + "D02*\n";

        for( size_t i = 1; i < poly.size(); ++i )
            m_body += XY( poly[i] ) + "D01*\n";

        m_body += XY( poly[0] ) + "D01*\nG37*\n";
        m_penValid = false;
    }

    std::string finish( const std::vector<std::string>& headerLines ) const
    {
        std::string out;

        for( const std::string& h : headerLines )
            out += h + "\n";

        out += "%FSLAX46Y46*%\n";
        out += "G04 Gerber Fmt 4.6, Leading zero omitted, Abs format (unit mm)*\n";
        out += "%MOMM*%\n%LPD*%\nG01*\nG75*\n";
        out += m_macroText;
        out += m_apertureText;

        if( !m_headerFunc.empty() )
            out += "%TD*%\n";

        out += m_body;
        out += "M02*\n";
        return out;
    }

private:
    Pt                                 m_origin;
    std::map<std::string, std::string> m_macros;
    std::map<std::string, int>         m_apertures;
    std::string                        m_macroText, m_apertureText, m_body;
    std::string                        m_headerFunc, m_bodyFunc;
    int                                m_current = -1;
    bool                               m_dark = true;
    Pt                                 m_pen;
    bool                               m_penValid = false;
};


class Plotter
{
public:
    Plotter( const Board& b, const FabOptions& o ) :
            m_b( b ),
            m_o( o )
    {
        m_origin = o.useAuxOrigin && b.hasAuxOrigin ? b.auxOrigin : Pt();
        m_copper.Build( b, false, false, 0.002, true );
    }

    std::string PlotLayer( int layer )
    {
        Gerber g( m_origin );
        const Layer& L = m_b.layers[(size_t) layer];

        switch( L.kind )
        {
        case LayerKind::COPPER: copper( g, layer ); break;
        case LayerKind::MASK: mask( g, layer ); break;
        case LayerKind::PASTE: paste( g, layer ); break;
        default: graphicsOnly( g, layer ); break;
        }

        return g.finish( header( layer ) );
    }

    std::string FileFunction( int layer ) const
    {
        const Layer& L = m_b.layers[(size_t) layer];
        int          n = (int) m_b.copperStack.size();
        const char*  side = L.side > 0 ? "Top" : "Bot";

        switch( L.kind )
        {
        case LayerKind::COPPER:
        {
            int         o = L.copperOrder + 1;
            const char* pos = o == 1 ? "Top" : ( o == n ? "Bot" : "Inr" );
            return "Copper,L" + std::to_string( o ) + "," + pos;
        }
        case LayerKind::MASK: return std::string( "Soldermask," ) + side;
        case LayerKind::PASTE: return std::string( "Paste," ) + side;
        case LayerKind::SILK: return std::string( "Legend," ) + side;
        case LayerKind::EDGE: return "Profile,NP";
        default: return "Other,User";
        }
    }

private:
    std::vector<std::string> header( int layer ) const
    {
        const Layer& L = m_b.layers[(size_t) layer];
        std::vector<std::string> h;
        h.push_back( "%TF.GenerationSoftware,Seyhan Labs,Netloom," + m_o.version + "*%" );

        if( !m_o.date.empty() )
            h.push_back( "%TF.CreationDate," + m_o.date + "*%" );

        h.push_back( "%TF.SameCoordinates,Original*%" );
        h.push_back( "%TF.FileFunction," + FileFunction( layer ) + "*%" );
        h.push_back( std::string( "%TF.FilePolarity," ) + ( L.kind == LayerKind::MASK ? "Negative" : "Positive" ) + "*%" );
        h.push_back( "G04 Netloom " + m_o.version + " (Seyhan Labs) layer " + L.canonical + "*" );
        return h;
    }

    static const char* padFunc( const Pad& p )
    {
        switch( p.attr )
        {
        case PadAttr::SMD: return "SMDPad,CuDef";
        case PadAttr::CONN: return "ConnectorPad";
        case PadAttr::NPTH: return "WasherPad";
        default: return "ComponentPad";
        }
    }

    double maskMargin( const Pad& p ) const
    {
        if( !std::isnan( p.maskMargin ) )
            return p.maskMargin;

        if( p.footprint >= 0 && !std::isnan( m_b.footprints[(size_t) p.footprint].maskMargin ) )
            return m_b.footprints[(size_t) p.footprint].maskMargin;

        return m_b.padToMask;
    }

    Pt pasteMargin( const Pad& p, const PadGeom& g ) const
    {
        double m = m_b.padToPaste, r = m_b.padToPasteRatio;

        if( p.footprint >= 0 )
        {
            const Footprint& f = m_b.footprints[(size_t) p.footprint];

            if( !std::isnan( f.pasteMargin ) )
                m = f.pasteMargin;

            if( !std::isnan( f.pasteRatio ) )
                r = f.pasteRatio;
        }

        if( !std::isnan( p.pasteMargin ) )
            m = p.pasteMargin;

        if( !std::isnan( p.pasteRatio ) )
            r = p.pasteRatio;

        return Pt( m + r * std::fabs( g.size.x ), m + r * std::fabs( g.size.y ) );
    }

    /// Plot a pad shape grown by (mx, my); flashes where the shape allows.
    void pad( Gerber& g, const Pad& p, const PadGeom& geom, double mx, double my, const char* func )
    {
        double sx = std::fabs( geom.size.x ) + 2 * mx, sy = std::fabs( geom.size.y ) + 2 * my;

        if( sx <= 1e-6 || sy <= 1e-6 )
            return;

        Pt     c = p.pos + RotatePt( geom.offset, p.angle );
        double a = std::fmod( std::fmod( p.angle, 360.0 ) + 360.0, 360.0 );
        bool   ortho = std::fabs( std::fmod( a, 90.0 ) ) < 1e-6 || std::fabs( std::fmod( a, 90.0 ) - 90.0 ) < 1e-6;
        bool   swap = ortho && ( std::fabs( a - 90 ) < 1e-6 || std::fabs( a - 270 ) < 1e-6 );
        auto   corner = [&]( double lx, double ly ) { return p.pos + RotatePt( Pt( lx, ly ) + geom.offset, p.angle ); };

        switch( geom.shape )
        {
        case PadShape::CIRCLE:
            g.flash( c, g.circle( sx, func ) );
            return;

        case PadShape::RECT:
            if( ortho )
            {
                g.flash( c, g.aperture( "R," + num( swap ? sy : sx ) + "X" + num( swap ? sx : sy ), func ) );
            }
            else
            {
                std::string body = "4,1,4," + g.rel( corner( -sx / 2, -sy / 2 ), c ) + "," + g.rel( corner( sx / 2, -sy / 2 ), c )
                                   + "," + g.rel( corner( sx / 2, sy / 2 ), c ) + "," + g.rel( corner( -sx / 2, sy / 2 ), c )
                                   + "," + g.rel( corner( -sx / 2, -sy / 2 ), c ) + ",0*\n";
                g.flash( c, g.aperture( g.macro( body ), func ) );
            }

            return;

        case PadShape::OVAL:
            if( ortho )
            {
                g.flash( c, g.aperture( "O," + num( swap ? sy : sx ) + "X" + num( swap ? sx : sy ), func ) );
            }
            else
            {
                double d = std::min( sx, sy ), half = ( std::max( sx, sy ) - d ) / 2;
                Pt     e1 = sx > sy ? corner( -half, 0 ) : corner( 0, -half );
                Pt     e2 = sx > sy ? corner( half, 0 ) : corner( 0, half );
                std::string body = "1,1," + num( d ) + "," + g.rel( e1, c ) + "*\n1,1," + num( d ) + "," + g.rel( e2, c )
                                   + "*\n20,1," + num( d ) + "," + g.rel( e1, c ) + "," + g.rel( e2, c ) + ",0*\n";
                g.flash( c, g.aperture( g.macro( body ), func ) );
            }

            return;

        case PadShape::ROUNDRECT:
        {
            double base = std::min( std::fabs( geom.size.x ), std::fabs( geom.size.y ) );
            double r = std::min( geom.rratio * base + std::min( mx, my ), std::min( sx, sy ) / 2 );

            if( r <= 1e-6 )
            {
                PadGeom rg = geom;
                rg.shape = PadShape::RECT;
                pad( g, p, rg, mx, my, func );
                return;
            }

            double hx = sx / 2 - r, hy = sy / 2 - r;
            Pt     k[4] = { corner( -hx, -hy ), corner( hx, -hy ), corner( hx, hy ), corner( -hx, hy ) };
            std::string body = "4,1,4";

            for( int i = 0; i <= 4; ++i )
                body += "," + g.rel( k[i % 4], c );

            body += ",0*\n";

            for( int i = 0; i < 4; ++i )
                body += "1,1," + num( 2 * r ) + "," + g.rel( k[i], c ) + "*\n";

            for( int i = 0; i < 4; ++i )
                body += "20,1," + num( 2 * r ) + "," + g.rel( k[i], c ) + "," + g.rel( k[( i + 1 ) % 4], c ) + ",0*\n";

            g.flash( c, g.aperture( g.macro( body ), func ) );
            return;
        }

        case PadShape::CHAMFERED_RECT:
        case PadShape::TRAPEZOID:
        {
            std::vector<Prim> prims;
            PadGeom           gg = geom;

            if( geom.shape == PadShape::CHAMFERED_RECT )
            {
                gg.size = Pt( sx, sy );
            }

            PadPrims( p, gg, 0.0, prims, 0.001 );

            for( const Prim& pr : prims )
            {
                if( pr.kind == Prim::POLYGON )
                {
                    Path poly = geom.shape == PadShape::TRAPEZOID ? offsetConvex( pr.poly, ( mx + my ) / 2 ) : pr.poly;
                    std::string body = "4,1," + std::to_string( poly.size() );

                    for( size_t i = 0; i <= poly.size(); ++i )
                        body += "," + g.rel( poly[i % poly.size()], c );

                    body += ",0*\n";
                    g.flash( c, g.aperture( g.macro( body ), func ) );
                }
                else
                {
                    primitive( g, pr, 0.0, func );
                }
            }

            return;
        }

        case PadShape::CUSTOM:
        {
            std::vector<Prim> prims;
            PadPrims( p, geom, 0.0, prims, 0.001 );
            double m = ( mx + my ) / 2;

            for( const Prim& pr : prims )
                primitive( g, pr, m, func );

            return;
        }
        }
    }

    /// A primitive grown by m: segments/points drawn, polygons as regions (plus an outline).
    void primitive( Gerber& g, const Prim& pr, double m, const char* func )
    {
        switch( pr.kind )
        {
        case Prim::POINT: g.flash( pr.a, g.circle( 2 * ( pr.r + m ), func ) ); break;
        case Prim::SEGMENT: g.line( pr.a, pr.b, 2 * ( pr.r + m ), func ); break;
        case Prim::POLYGON:
        case Prim::CHAIN:
            if( pr.kind == Prim::POLYGON )
                g.region( pr.poly, func );

            if( pr.r + m > 1e-6 )
                g.polyline( pr.poly, 2 * ( pr.r + m ), pr.kind == Prim::POLYGON, func );

            break;
        }
    }

    void shape( Gerber& g, const Shape& s, const char* func )
    {
        g.polarity( !s.clear );
        plotShape( g, s, func );
        g.polarity( true );
    }

    void plotShape( Gerber& g, const Shape& s, const char* func )
    {
        double w = s.width;

        switch( s.type )
        {
        case ShapeType::SEGMENT: g.line( s.start, s.end, std::max( w, 0.0 ), func ); break;
        case ShapeType::ARC: g.arc( s.start, s.mid, s.end, std::max( w, 0.0 ), func ); break;
        case ShapeType::CIRCLE:
        {
            double r = std::hypot( s.end.x - s.start.x, s.end.y - s.start.y );

            if( s.filled )
                g.flash( s.start, g.circle( 2 * r + w, func ) );
            else
                g.fullCircle( s.start, r, w, func );

            break;
        }
        case ShapeType::RECT:
        {
            Path box = { s.start, Pt( s.end.x, s.start.y ), s.end, Pt( s.start.x, s.end.y ) };

            if( s.radius > 0 )
            {
                std::vector<Prim> prims;
                Shape             rs = s;
                rs.width = 0;
                ShapePrims( rs, prims, 0.001 );

                if( !prims.empty() && prims[0].kind == Prim::POLYGON )
                    box = prims[0].poly;
                else if( !prims.empty() )
                {
                    box.clear();

                    for( const Prim& pr : prims )
                        box.push_back( pr.a );
                }
            }

            if( s.filled )
                g.region( box, func );

            if( w > 0 )
                g.polyline( box, w, true, func );

            break;
        }
        case ShapeType::POLY:
            if( s.filled && s.poly.size() >= 3 )
                g.region( s.poly, func );

            if( w > 0 || !s.filled )
                g.polyline( s.poly, std::max( w, 0.0 ), true, func );

            break;
        case ShapeType::BEZIER: g.polyline( s.poly, std::max( w, 0.0 ), false, func ); break;
        }
    }

    void texts( Gerber& g, int layer, const char* func )
    {
        for( const Text& t : m_b.texts )
        {
            if( t.layer != layer || !t.visible || t.knockout )
                continue;

            if( !t.renderCache.empty() )
            {
                for( const std::vector<Path>& glyph : t.renderCache )
                    g.region( fracture( glyph ), func );

                continue;
            }

            std::string text = m_b.ResolveText( t.text, t.footprint, t.layer );

            if( text.empty() )
                continue;

            TextLayout L;
            L.pos = t.pos;
            L.sizeX = t.sizeX;
            L.sizeY = t.sizeY;
            L.thickness = t.thickness;
            L.angle = t.angle;
            L.lineSpacing = t.lineSpacing;
            L.mirror = t.mirror;
            L.italic = t.italic;
            L.hAlign = t.hAlign;
            L.vAlign = t.vAlign;
            std::vector<Path> strokes;
            StrokeFont::Get().Layout( text, L, strokes );

            for( const Path& s : strokes )
                g.polyline( s, t.thickness, false, func );
        }
    }

    void zones( Gerber& g, int layer, const char* func )
    {
        for( const Zone& z : m_b.zones )
        {
            if( z.keepout )
                continue;

            for( const ZoneFill& f : z.fills )
            {
                if( f.layer != layer )
                    continue;

                for( const Path& poly : f.polys )
                {
                    g.region( poly, func );

                    if( z.strokeFill && z.minThickness > 0 )
                        g.polyline( poly, z.minThickness, true, func );
                }
            }
        }
    }

    void graphicsOnly( Gerber& g, int layer )
    {
        bool        edge = m_b.layers[(size_t) layer].kind == LayerKind::EDGE;
        const char* func = edge ? "Profile" : nullptr;

        for( const Shape& s : m_b.shapes )
        {
            if( s.layer == layer )
            {
                if( edge && s.width <= 0 )
                {
                    Shape t = s;
                    t.width = 0.05;
                    shape( g, t, func );
                }
                else
                {
                    shape( g, s, func );
                }
            }
        }

        zones( g, layer, func );
        texts( g, layer, func );
    }

    void copper( Gerber& g, int layer )
    {
        // zones first, then everything drawn over them
        zones( g, layer, "Conductor" );

        for( size_t i = 0; i < m_b.tracks.size(); ++i )
        {
            const Track& t = m_b.tracks[i];

            if( t.layer == layer )
                g.line( t.start, t.end, t.width, "Conductor" );
        }

        for( const ArcTrack& a : m_b.arcs )
        {
            if( a.layer == layer )
                g.arc( a.start, a.mid, a.end, a.width, "Conductor" );
        }

        for( size_t i = 0; i < m_b.vias.size(); ++i )
        {
            const Via& v = m_b.vias[i];
            double     size = 0;

            if( !ViaOnLayer( m_b, v, layer, &size ) || m_copper.HoleOnly( ItemType::VIA, (int) i, layer ) )
                continue;

            g.flash( v.pos, g.circle( size, "ViaPad" ) );
        }

        for( size_t i = 0; i < m_b.pads.size(); ++i )
        {
            const Pad& p = m_b.pads[i];

            if( !padOnLayer( p, layer ) || m_copper.HoleOnly( ItemType::PAD, (int) i, layer ) )
                continue;

            const PadGeom& geom = PadGeomOnLayer( m_b, p, layer );

            if( p.attr == PadAttr::NPTH )
            {
                double hole = std::max( p.drill.x, p.drill.y );

                if( std::max( std::fabs( geom.size.x ), std::fabs( geom.size.y ) ) <= hole + 1e-6 )
                    continue; // a bare mechanical hole has no copper
            }

            pad( g, p, geom, 0, 0, padFunc( p ) );
        }

        for( const Shape& s : m_b.shapes )
        {
            if( s.layer == layer )
                shape( g, s, "Conductor" );
        }

        texts( g, layer, "NonConductor" );
    }

    void mask( Gerber& g, int layer )
    {
        int               side = m_b.layers[(size_t) layer].side;
        std::vector<Path> openings;

        for( size_t i = 0; i < m_b.pads.size(); ++i )
        {
            const Pad& p = m_b.pads[i];

            if( !padOnLayer( p, layer ) )
                continue;

            // the copper the opening is made for: the pad on this side's outer copper layer
            int cu = -1;

            for( int l : m_b.copperStack )
            {
                if( m_b.layers[(size_t) l].side == side )
                    cu = l;
            }

            const PadGeom& geom = PadGeomOnLayer( m_b, p, cu >= 0 ? cu : layer );
            double         m = maskMargin( p );
            pad( g, p, geom, m, m, padFunc( p ) );

            if( m_b.maskMinWidth > 0 )
            {
                std::vector<Prim> prims;
                PadPrims( p, geom, m, prims, 0.01 );
                std::vector<Pt> pts;
                outlinePoints( prims, pts );
                openings.push_back( convexHull( pts ) );
            }
        }

        // Webs of mask between openings narrower than the minimum width can't be made: the
        // openings merge there (the area between such neighbours is opened too).
        if( m_b.maskMinWidth > 0 && openings.size() > 1 )
        {
            std::vector<BBox> boxes;

            for( const Path& o : openings )
            {
                BBox b;

                for( const Pt& q : o )
                    b.add( q );

                boxes.push_back( b );
            }

            double w = m_b.maskMinWidth;
            std::vector<size_t> order( openings.size() );

            for( size_t i = 0; i < order.size(); ++i )
                order[i] = i;

            std::sort( order.begin(), order.end(), [&]( size_t a, size_t b ) { return boxes[a].minX < boxes[b].minX; } );

            for( size_t ii = 0; ii < order.size(); ++ii )
            {
                size_t i = order[ii];

                for( size_t jj = ii + 1; jj < order.size(); ++jj )
                {
                    size_t j = order[jj];

                    if( boxes[j].minX > boxes[i].maxX + w )
                        break;

                    if( boxes[j].minY > boxes[i].maxY + w || boxes[i].minY > boxes[j].maxY + w )
                        continue;

                    double d = polyDistance( openings[i], openings[j] );

                    if( d > 1e-6 && d < w - 1e-6 )
                    {
                        // fill the neck: hull of the parts of both outlines that face each
                        // other closer than the minimum width (close to a morphological closing)
                        std::vector<Pt> near;

                        for( int side2 = 0; side2 < 2; ++side2 )
                        {
                            const Path& a = side2 == 0 ? openings[i] : openings[j];
                            const Path& b = side2 == 0 ? openings[j] : openings[i];

                            for( size_t k = 0; k < a.size(); ++k )
                            {
                                const Pt& p0 = a[k];
                                const Pt& p1 = a[( k + 1 ) % a.size()];
                                int       steps = std::max( 1, (int) std::ceil( std::hypot( p1.x - p0.x, p1.y - p0.y ) / 0.01 ) );

                                for( int t = 0; t < steps; ++t )
                                {
                                    Pt     q = p0 + ( p1 - p0 ) * ( (double) t / steps );
                                    double dq = 1e300;

                                    for( size_t e = 0; e < b.size(); ++e )
                                        dq = std::min( dq, PointSegmentDistance( q, b[e], b[( e + 1 ) % b.size()] ) );

                                    if( dq < w - 1e-6 )
                                        near.push_back( q );
                                }
                            }
                        }

                        if( near.size() >= 3 )
                            g.region( convexHull( near ), nullptr );
                    }
                }
            }
        }

        for( const Via& v : m_b.vias )
        {
            bool tented = side > 0 ? ( v.tentFront >= 0 ? v.tentFront == 1 : m_b.viasTentedFront )
                                   : ( v.tentBack >= 0 ? v.tentBack == 1 : m_b.viasTentedBack );

            if( tented )
                continue;

            int outer = side > 0 ? m_b.copperStack.front() : m_b.copperStack.back();
            double size = 0;

            if( ViaOnLayer( m_b, v, outer, &size ) )
                g.flash( v.pos, g.circle( size + 2 * m_b.padToMask, "ViaPad" ) );
        }

        for( const Shape& s : m_b.shapes )
        {
            if( s.layer == layer )
                shape( g, s, nullptr );
        }

        zones( g, layer, nullptr );
        texts( g, layer, nullptr );
    }

    void paste( Gerber& g, int layer )
    {
        int side = m_b.layers[(size_t) layer].side;

        for( size_t i = 0; i < m_b.pads.size(); ++i )
        {
            const Pad& p = m_b.pads[i];

            if( !padOnLayer( p, layer ) || p.attr == PadAttr::NPTH )
                continue;

            int cu = -1;

            for( int l : m_b.copperStack )
            {
                if( m_b.layers[(size_t) l].side == side )
                    cu = l;
            }

            const PadGeom& geom = PadGeomOnLayer( m_b, p, cu >= 0 ? cu : layer );
            Pt             m = pasteMargin( p, geom );
            pad( g, p, geom, m.x, m.y, padFunc( p ) );
        }

        for( const Shape& s : m_b.shapes )
        {
            if( s.layer == layer )
                shape( g, s, nullptr );
        }

        zones( g, layer, nullptr );
        texts( g, layer, nullptr );
    }

    const Board&      m_b;
    const FabOptions& m_o;
    Pt                m_origin;
    CopperIndex       m_copper;
};


// ---- drill ------------------------------------------------------------------------------

struct DrillHit
{
    Pt     a, b; ///< a == b for round holes
    double dia;
    bool   via;
};

/// Excellon decimal number: always with a decimal point (a bare integer would be read with the
/// zero suppression rules and land somewhere else).
std::string drillNum( double v )
{
    std::string s = num( v, 4 );

    if( s.find( '.' ) == std::string::npos )
        s += ".0";

    return s;
}


std::string drillFile( const Board& b, const FabOptions& o, const std::vector<DrillHit>& hits, bool plated )
{
    Pt origin = o.useAuxOrigin && b.hasAuxOrigin ? b.auxOrigin : Pt();
    auto X = [&]( double x ) { return drillNum( x - origin.x ); };
    auto Y = [&]( double y ) { return drillNum( -( y - origin.y ) ); };

    // tools by diameter (and via/pad function for the attribute)
    std::map<std::pair<long, int>, int> tools;

    for( const DrillHit& h : hits )
        tools[{ std::lround( h.dia * 1000 ), h.via ? 0 : 1 }] = 0;

    int t = 1;

    for( auto& [k, v] : tools )
        v = t++;

    int n = (int) b.copperStack.size();
    std::string s = "M48\n";
    s += "; DRILL file {Netloom " + o.version + " (Seyhan Labs)} date " + o.date + "\n";
    s += "; FORMAT={-:-/ absolute / metric / decimal}\n";

    if( !o.date.empty() )
        s += "; #@! TF.CreationDate," + o.date + "\n";

    s += "; #@! TF.GenerationSoftware,Seyhan Labs,Netloom," + o.version + "\n";
    s += std::string( "; #@! TF.FileFunction," ) + ( plated ? "Plated,1," : "NonPlated,1," ) + std::to_string( n )
         + ( plated ? ",PTH\n" : ",NPTH\n" );
    s += "FMAT,2\nMETRIC\n";

    for( const auto& [k, v] : tools )
    {
        const char* func = !plated ? "NonPlated,NPTH,ComponentDrill" : ( k.second == 0 ? "Plated,PTH,ViaDrill" : "Plated,PTH,ComponentDrill" );
        s += std::string( "; #@! TA.AperFunction," ) + func + "\n";
        char size[32];
        snprintf( size, sizeof( size ), "%.3f", k.first / 1000.0 );
        s += "T" + std::to_string( v ) + "C" + size + "\n";
    }

    s += "%\nG90\nG05\n";

    for( const auto& [k, v] : tools )
    {
        s += "T" + std::to_string( v ) + "\n";

        for( const DrillHit& h : hits )
        {
            if( std::lround( h.dia * 1000 ) != k.first || ( h.via ? 0 : 1 ) != k.second )
                continue;

            if( std::hypot( h.b.x - h.a.x, h.b.y - h.a.y ) < 1e-6 )
                s += "X" + X( h.a.x ) + "Y" + Y( h.a.y ) + "\n";
            else
                s += "X" + X( h.a.x ) + "Y" + Y( h.a.y ) + "G85X" + X( h.b.x ) + "Y" + Y( h.b.y ) + "\n";
        }
    }

    s += "M30\n";
    return s;
}


// ---- BOM and positions ------------------------------------------------------------------

bool hasAttr( const Footprint& f, const char* a )
{
    return std::find( f.attrs.begin(), f.attrs.end(), a ) != f.attrs.end();
}

std::string csv( const std::string& v )
{
    std::string out = "\"";

    for( char c : v )
    {
        if( c == '"' )
            out += "\"\"";
        else if( c == '\n' || c == '\r' )
            out += ' ';
        else
            out += c;
    }

    return out + "\"";
}

/// R2 before R10: compare runs of digits by value.
bool naturalLess( const std::string& a, const std::string& b )
{
    size_t i = 0, j = 0;

    while( i < a.size() && j < b.size() )
    {
        if( isdigit( (unsigned char) a[i] ) && isdigit( (unsigned char) b[j] ) )
        {
            size_t i2 = i, j2 = j;

            while( i2 < a.size() && isdigit( (unsigned char) a[i2] ) )
                ++i2;

            while( j2 < b.size() && isdigit( (unsigned char) b[j2] ) )
                ++j2;

            long long va = std::atoll( a.substr( i, i2 - i ).c_str() ), vb = std::atoll( b.substr( j, j2 - j ).c_str() );

            if( va != vb )
                return va < vb;

            i = i2;
            j = j2;
        }
        else
        {
            if( a[i] != b[j] )
                return a[i] < b[j];

            ++i;
            ++j;
        }
    }

    return a.size() - i < b.size() - j;
}

std::string packageName( const Footprint& f )
{
    size_t c = f.libId.find( ':' );
    return c == std::string::npos ? f.libId : f.libId.substr( c + 1 );
}

std::string bomCsv( const Board& b )
{
    static const char* SKIP[] = { "Reference", "Value", "Footprint", "Datasheet", "Description", "Sheetfile",
                                  "Sheetname", "ki_description", "ki_keywords", "ki_fp_filters" };
    std::set<std::string> extra;

    for( const Footprint& f : b.footprints )
    {
        for( const auto& [k, v] : f.fields )
        {
            bool skip = v.empty() || k.rfind( "ki_", 0 ) == 0;

            for( const char* s : SKIP )
                skip |= k == s;

            if( !skip )
                extra.insert( k );
        }
    }

    struct Group
    {
        std::vector<std::string> refs;
        std::string              value, package;
        bool                     dnp = false;
        std::map<std::string, std::string> fields;
    };

    std::map<std::string, Group> groups;

    for( const Footprint& f : b.footprints )
    {
        if( hasAttr( f, "exclude_from_bom" ) || hasAttr( f, "board_only" ) || f.reference.empty() )
            continue;

        bool        dnp = hasAttr( f, "dnp" );
        std::string key = f.value + "\x1f" + packageName( f ) + "\x1f" + ( dnp ? "1" : "0" );

        for( const std::string& e : extra )
        {
            auto it = f.fields.find( e );
            key += "\x1f" + ( it == f.fields.end() ? std::string() : it->second );
        }

        Group& g = groups[key];
        g.refs.push_back( f.reference );
        g.value = f.value;
        g.package = packageName( f );
        g.dnp = dnp;

        for( const std::string& e : extra )
        {
            auto it = f.fields.find( e );

            if( it != f.fields.end() )
                g.fields[e] = it->second;
        }
    }

    std::vector<Group*> order;

    for( auto& [k, g] : groups )
    {
        std::sort( g.refs.begin(), g.refs.end(), naturalLess );
        order.push_back( &g );
    }

    std::sort( order.begin(), order.end(), []( const Group* a, const Group* b ) { return naturalLess( a->refs[0], b->refs[0] ); } );

    std::string out = "\"Reference\",\"Value\",\"Footprint\",\"Qty\",\"DNP\"";

    for( const std::string& e : extra )
        out += "," + csv( e );

    out += "\n";

    for( const Group* g : order )
    {
        std::string refs;

        for( size_t i = 0; i < g->refs.size(); ++i )
            refs += ( i ? "," : "" ) + g->refs[i];

        out += csv( refs ) + "," + csv( g->value ) + "," + csv( g->package ) + "," + std::to_string( g->refs.size() )
               + "," + csv( g->dnp ? "DNP" : "" );

        for( const std::string& e : extra )
        {
            auto it = g->fields.find( e );
            out += "," + csv( it == g->fields.end() ? std::string() : it->second );
        }

        out += "\n";
    }

    return out;
}

std::string positionsCsv( const Board& b, const FabOptions& o, int& aCount )
{
    Pt origin = o.useAuxOrigin && b.hasAuxOrigin ? b.auxOrigin : Pt();
    std::vector<const Footprint*> list;

    for( const Footprint& f : b.footprints )
    {
        if( hasAttr( f, "exclude_from_pos_files" ) || hasAttr( f, "board_only" ) || f.reference.empty() )
            continue;

        if( o.excludeDnpFromPositions && hasAttr( f, "dnp" ) )
            continue;

        list.push_back( &f );
    }

    std::sort( list.begin(), list.end(), []( const Footprint* a, const Footprint* b ) { return naturalLess( a->reference, b->reference ); } );

    std::string out = "Ref,Val,Package,PosX,PosY,Rot,Side\n";

    for( const Footprint* f : list )
    {
        double rot = std::fmod( std::fmod( f->angle, 360.0 ) + 360.0, 360.0 );
        out += csv( f->reference ) + "," + csv( f->value ) + "," + csv( packageName( *f ) ) + "," + num( f->pos.x - origin.x, 4 )
               + "," + num( -( f->pos.y - origin.y ), 4 ) + "," + num( rot, 4 ) + "," + ( f->back ? "bottom" : "top" ) + "\n";
    }

    aCount = (int) list.size();
    return out;
}

std::string layerFileName( const Layer& L )
{
    std::string n = L.canonical;

    if( n == "F.SilkS" )
        n = "F.Silkscreen";
    else if( n == "B.SilkS" )
        n = "B.Silkscreen";

    std::replace( n.begin(), n.end(), '.', '_' );
    return n;
}

} // anonymous namespace


std::string GerberForLayer( const Board& aBoard, int aLayer, const FabOptions& aOptions )
{
    if( aLayer < 0 || aLayer >= (int) aBoard.layers.size() )
        return std::string();

    Plotter p( aBoard, aOptions );
    return p.PlotLayer( aLayer );
}


bool WriteFabFiles( const Board& b, const FabOptions& o, std::vector<FabFile>& out, std::vector<std::string>& warnings )
{
    out.clear();

    if( b.copperStack.empty() )
    {
        warnings.push_back( Tr( "The board has no copper layers." ) );
        return false;
    }

    bool hasOutline = false;

    for( const Shape& s : b.shapes )
        hasOutline |= isLayerKind( b, s.layer, LayerKind::EDGE );

    if( !hasOutline )
        warnings.push_back( Tr( "The board has no outline on Edge.Cuts; the fab will not know where to cut." ) );

    if( o.gerbers )
    {
        Plotter p( b, o );
        std::vector<int> order;

        for( int l : b.copperStack )
            order.push_back( l );

        // mask, paste, silk (front then back), then the outline
        for( LayerKind k : { LayerKind::MASK, LayerKind::PASTE, LayerKind::SILK } )
        {
            for( int side : { 1, -1 } )
            {
                for( size_t l = 0; l < b.layers.size(); ++l )
                {
                    if( b.layers[l].kind == k && b.layers[l].side == side )
                        order.push_back( (int) l );
                }
            }
        }

        for( size_t l = 0; l < b.layers.size(); ++l )
        {
            if( b.layers[l].kind == LayerKind::EDGE )
                order.push_back( (int) l );
        }

        for( int l : order )
        {
            FabFile f;
            f.name = o.baseName + "-" + layerFileName( b.layers[(size_t) l] ) + ".gbr";
            f.data = p.PlotLayer( l );
            f.kind = "gerber";
            f.layer = b.layers[(size_t) l].canonical;
            out.push_back( std::move( f ) );
        }
    }

    if( o.drill )
    {
        std::vector<DrillHit> pth, npth;

        for( const Via& v : b.vias )
        {
            if( v.drill > 0 )
                pth.push_back( { v.pos, v.pos, v.drill, true } );
        }

        bool blind = false;

        for( const Via& v : b.vias )
            blind |= v.type == ViaType::BLIND || v.type == ViaType::BURIED || v.type == ViaType::MICRO;

        if( blind )
            warnings.push_back( Tr( "Blind, buried and micro vias are listed in the plated drill file; check the drill spans with your fab." ) );

        for( const Pad& p : b.pads )
        {
            if( p.drill.x <= 0 || ( p.attr != PadAttr::PTH && p.attr != PadAttr::NPTH ) )
                continue;

            double dx = std::fabs( p.drill.x ), dy = std::fabs( p.drill.y > 0 ? p.drill.y : p.drill.x );
            DrillHit h { p.pos, p.pos, std::min( dx, dy ), false };

            if( p.drillOval && std::fabs( dx - dy ) > 1e-6 )
            {
                Pt half = dx > dy ? Pt( ( dx - dy ) / 2, 0 ) : Pt( 0, ( dy - dx ) / 2 );
                h.a = p.pos - RotatePt( half, p.angle );
                h.b = p.pos + RotatePt( half, p.angle );
            }

            ( p.attr == PadAttr::PTH ? pth : npth ).push_back( h );
        }

        out.push_back( { o.baseName + "-PTH.drl", drillFile( b, o, pth, true ), "drill", "" } );
        out.push_back( { o.baseName + "-NPTH.drl", drillFile( b, o, npth, false ), "drill", "" } );
    }

    if( o.bom )
        out.push_back( { o.baseName + "-bom.csv", bomCsv( b ), "bom", "" } );

    if( o.positions )
    {
        int count = 0;
        out.push_back( { o.baseName + "-pos.csv", positionsCsv( b, o, count ), "positions", "" } );
    }

    return true;
}


uint32_t Crc32( const void* aData, size_t aLen, uint32_t crc )
{
    static uint32_t table[256];
    static bool     init = false;

    if( !init )
    {
        for( uint32_t i = 0; i < 256; ++i )
        {
            uint32_t c = i;

            for( int k = 0; k < 8; ++k )
                c = c & 1 ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1;

            table[i] = c;
        }

        init = true;
    }

    const uint8_t* p = (const uint8_t*) aData;
    crc = ~crc;

    for( size_t i = 0; i < aLen; ++i )
        crc = table[( crc ^ p[i] ) & 0xFF] ^ ( crc >> 8 );

    return ~crc;
}


std::string MakeZip( const std::vector<FabFile>& files, const std::string& date )
{
    // DOS date/time from "YYYY-MM-DDThh:mm:ss"
    int Y = 2026, M = 1, D = 1, h = 0, m = 0, s = 0;

    if( date.size() >= 19 )
        sscanf( date.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s );

    uint16_t dosTime = (uint16_t) ( ( h << 11 ) | ( m << 5 ) | ( s / 2 ) );
    uint16_t dosDate = (uint16_t) ( ( ( Y - 1980 ) << 9 ) | ( M << 5 ) | D );

    std::string zip, central;
    auto        u16 = []( std::string& o, uint16_t v ) { o += (char) ( v & 0xFF ); o += (char) ( v >> 8 ); };
    auto        u32 = [&]( std::string& o, uint32_t v ) { u16( o, (uint16_t) ( v & 0xFFFF ) ); u16( o, (uint16_t) ( v >> 16 ) ); };

    for( const FabFile& f : files )
    {
        uint32_t crc = Crc32( f.data.data(), f.data.size() );
        uint32_t offset = (uint32_t) zip.size();

        u32( zip, 0x04034b50 );
        u16( zip, 10 );      // version needed
        u16( zip, 0x0800 );  // UTF-8 names
        u16( zip, 0 );       // stored
        u16( zip, dosTime );
        u16( zip, dosDate );
        u32( zip, crc );
        u32( zip, (uint32_t) f.data.size() );
        u32( zip, (uint32_t) f.data.size() );
        u16( zip, (uint16_t) f.name.size() );
        u16( zip, 0 );
        zip += f.name;
        zip += f.data;

        u32( central, 0x02014b50 );
        u16( central, 0x031E ); // made by: unix, 3.0
        u16( central, 10 );
        u16( central, 0x0800 );
        u16( central, 0 );
        u16( central, dosTime );
        u16( central, dosDate );
        u32( central, crc );
        u32( central, (uint32_t) f.data.size() );
        u32( central, (uint32_t) f.data.size() );
        u16( central, (uint16_t) f.name.size() );
        u16( central, 0 );
        u16( central, 0 );
        u16( central, 0 );
        u16( central, 0 );
        u32( central, 0100644u << 16 ); // -rw-r--r--
        u32( central, offset );
        central += f.name;
    }

    uint32_t cdOffset = (uint32_t) zip.size();
    zip += central;
    u32( zip, 0x06054b50 );
    u16( zip, 0 );
    u16( zip, 0 );
    u16( zip, (uint16_t) files.size() );
    u16( zip, (uint16_t) files.size() );
    u32( zip, (uint32_t) central.size() );
    u32( zip, cdOffset );
    u16( zip, 0 );
    return zip;
}

} // namespace kp
