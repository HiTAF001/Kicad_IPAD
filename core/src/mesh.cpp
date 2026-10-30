/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "mesh.h"
#include "stroke_font.h"

#include "../third_party/earcut.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace mapbox
{
namespace util
{
template <>
struct nth<0, kp::Pt>
{
    inline static double get( const kp::Pt& t ) { return t.x; }
};

template <>
struct nth<1, kp::Pt>
{
    inline static double get( const kp::Pt& t ) { return t.y; }
};
} // namespace util
} // namespace mapbox

namespace kp
{

const std::array<float, 4> BACKGROUND_COLOR { 0.0f, 16.0f / 255.0f, 35.0f / 255.0f, 1.0f };

static constexpr double PI = 3.14159265358979323846;

namespace
{

/// Segments for a full circle used for strokes/pads: relative error with sane bounds.
int roundSegments( double r )
{
    if( r <= 0 )
        return 12;

    double err = std::clamp( r * 0.02, 0.0005, 0.005 );
    double step = 2.0 * std::acos( std::max( -1.0, 1.0 - err / r ) );
    int    n = (int) std::ceil( 2.0 * PI / step );
    return std::clamp( n, 12, 128 );
}


struct Acc
{
    std::vector<float>    v;
    std::vector<uint32_t> i;

    uint32_t base() const { return (uint32_t) ( v.size() / 2 ); }

    void vert( const Pt& p )
    {
        v.push_back( (float) p.x );
        v.push_back( (float) p.y );
    }

    void tri( uint32_t a, uint32_t b, uint32_t c )
    {
        i.push_back( a );
        i.push_back( b );
        i.push_back( c );
    }

    /// Convex polygon (fan).
    void convex( const Path& pts )
    {
        if( pts.size() < 3 )
            return;

        uint32_t b = base();

        for( const Pt& p : pts )
            vert( p );

        for( uint32_t k = 1; k + 1 < pts.size(); ++k )
            tri( b, b + k, b + k + 1 );
    }

    void circle( const Pt& c, double r )
    {
        if( r <= 0 )
            return;

        int  n = roundSegments( r );
        Path pts;
        pts.reserve( (size_t) n );

        for( int k = 0; k < n; ++k )
        {
            double a = 2 * PI * k / n;
            pts.push_back( Pt( c.x + r * std::cos( a ), c.y + r * std::sin( a ) ) );
        }

        convex( pts );
    }

    void ring( const Pt& c, double rIn, double rOut )
    {
        if( rOut <= 0 )
            return;

        if( rIn <= 0 )
        {
            circle( c, rOut );
            return;
        }

        int      n = roundSegments( rOut );
        uint32_t b = base();

        for( int k = 0; k < n; ++k )
        {
            double a = 2 * PI * k / n;
            double cs = std::cos( a ), sn = std::sin( a );
            vert( Pt( c.x + rIn * cs, c.y + rIn * sn ) );
            vert( Pt( c.x + rOut * cs, c.y + rOut * sn ) );
        }

        for( int k = 0; k < n; ++k )
        {
            uint32_t i0 = b + 2 * (uint32_t) k;
            uint32_t i1 = b + 2 * (uint32_t) ( ( k + 1 ) % n );
            tri( i0, i0 + 1, i1 + 1 );
            tri( i0, i1 + 1, i1 );
        }
    }

    /// Thick segment with round caps.
    void capsule( const Pt& a, const Pt& b, double w )
    {
        if( w <= 0 )
            return;

        double r = w / 2;
        double dx = b.x - a.x, dy = b.y - a.y;
        double len = std::hypot( dx, dy );

        if( len < 1e-9 )
        {
            circle( a, r );
            return;
        }

        int    half = std::max( 3, roundSegments( r ) / 2 );
        double th = std::atan2( dy, dx );
        Path   pts;
        pts.reserve( (size_t) ( 2 * half + 2 ) );

        for( int k = 0; k <= half; ++k )
        {
            double ang = th - PI / 2 + PI * k / half;
            pts.push_back( Pt( b.x + r * std::cos( ang ), b.y + r * std::sin( ang ) ) );
        }

        for( int k = 0; k <= half; ++k )
        {
            double ang = th + PI / 2 + PI * k / half;
            pts.push_back( Pt( a.x + r * std::cos( ang ), a.y + r * std::sin( ang ) ) );
        }

        convex( pts );
    }

    /// Polyline drawn with round joints (capsule per segment).
    void polyline( const Path& pts, double w, bool closed )
    {
        if( pts.empty() || w <= 0 )
            return;

        if( pts.size() == 1 )
        {
            circle( pts[0], w / 2 );
            return;
        }

        for( size_t k = 0; k + 1 < pts.size(); ++k )
            capsule( pts[k], pts[k + 1], w );

        if( closed && pts.size() > 2 )
            capsule( pts.back(), pts.front(), w );
    }

    /// Thick arc through 3 points: radial strip plus round end caps.
    void arcStroke( const Pt& s, const Pt& m, const Pt& e, double w )
    {
        if( w <= 0 )
            return;

        Path center;
        ArcToPoints( center, s, m, e, 0.002 );

        // find circle centre from the three points
        double d = 2.0 * ( s.x * ( m.y - e.y ) + m.x * ( e.y - s.y ) + e.x * ( s.y - m.y ) );

        if( std::fabs( d ) < 1e-12 || center.size() < 2 )
        {
            capsule( s, e, w );
            return;
        }

        double s2 = s.x * s.x + s.y * s.y, m2 = m.x * m.x + m.y * m.y, e2 = e.x * e.x + e.y * e.y;
        Pt     c( ( s2 * ( m.y - e.y ) + m2 * ( e.y - s.y ) + e2 * ( s.y - m.y ) ) / d,
                  ( s2 * ( e.x - m.x ) + m2 * ( s.x - e.x ) + e2 * ( m.x - s.x ) ) / d );
        double r = std::hypot( s.x - c.x, s.y - c.y );

        if( r > 1e5 )
        {
            capsule( s, e, w );
            return;
        }

        double   rIn = std::max( 0.0, r - w / 2 ), rOut = r + w / 2;
        uint32_t b = base();

        for( const Pt& p : center )
        {
            double ux = ( p.x - c.x ) / r, uy = ( p.y - c.y ) / r;
            vert( Pt( c.x + ux * rIn, c.y + uy * rIn ) );
            vert( Pt( c.x + ux * rOut, c.y + uy * rOut ) );
        }

        for( uint32_t k = 0; k + 1 < center.size(); ++k )
        {
            uint32_t i0 = b + 2 * k, i1 = b + 2 * ( k + 1 );
            tri( i0, i0 + 1, i1 + 1 );
            tri( i0, i1 + 1, i1 );
        }

        circle( s, w / 2 );
        circle( e, w / 2 );
    }

    /// Arbitrary polygon with optional holes (earcut).
    void polygon( const std::vector<Path>& rings )
    {
        if( rings.empty() || rings[0].size() < 3 )
            return;

        std::vector<Path> clean;
        clean.reserve( rings.size() );

        for( const Path& r : rings )
        {
            Path c;
            c.reserve( r.size() );

            for( const Pt& p : r )
            {
                if( c.empty() || std::fabs( c.back().x - p.x ) > 1e-9 || std::fabs( c.back().y - p.y ) > 1e-9 )
                    c.push_back( p );
            }

            while( c.size() > 1 && std::fabs( c.front().x - c.back().x ) < 1e-9
                   && std::fabs( c.front().y - c.back().y ) < 1e-9 )
            {
                c.pop_back();
            }

            if( c.size() >= 3 )
                clean.push_back( std::move( c ) );
            else if( clean.empty() )
                return; // degenerate outer ring
        }

        if( clean.empty() )
            return;

        std::vector<uint32_t> idx = mapbox::earcut<uint32_t>( clean );

        if( idx.empty() )
            return;

        uint32_t b = base();

        for( const Path& r : clean )
        {
            for( const Pt& p : r )
                vert( p );
        }

        for( uint32_t k : idx )
            i.push_back( b + k );
    }

    /**
     * Large single-ring polygons (zone fills can have 100k+ points) make ear clipping
     * super-linear. Split them recursively with half-plane clips (Sutherland-Hodgman) into
     * pieces small enough for earcut. The clip leaves zero-width seams along the cut lines,
     * which earcut treats like the "fracture" bridges KiCad already puts in zone fills.
     */
    void bigPolygon( const Path& ring, int depth = 0 )
    {
        if( ring.size() <= 2500 || depth >= 24 )
        {
            polygon( { ring } );
            return;
        }

        BBox b;

        for( const Pt& p : ring )
            b.add( p );

        int    axis = ( b.maxX - b.minX ) >= ( b.maxY - b.minY ) ? 0 : 1;
        double m = axis == 0 ? ( b.minX + b.maxX ) / 2 : ( b.minY + b.maxY ) / 2;

        for( int side = 0; side < 2; ++side )
        {
            Path   out;
            size_t n = ring.size();
            out.reserve( n / 2 + 16 );

            for( size_t k = 0; k < n; ++k )
            {
                const Pt& a = ring[k];
                const Pt& c = ring[( k + 1 ) % n];
                double    va = ( axis == 0 ? a.x : a.y ) - m;
                double    vc = ( axis == 0 ? c.x : c.y ) - m;

                if( side == 1 )
                {
                    va = -va;
                    vc = -vc;
                }

                bool ina = va <= 0;
                bool inc = vc <= 0;

                if( ina )
                    out.push_back( a );

                if( ina != inc )
                {
                    double t = va / ( va - vc );
                    Pt     q( a.x + ( c.x - a.x ) * t, a.y + ( c.y - a.y ) * t );

                    if( axis == 0 )
                        q.x = m;
                    else
                        q.y = m;

                    out.push_back( q );
                }
            }

            if( out.size() >= 3 && out.size() < n )
                bigPolygon( out, depth + 1 );
            else if( out.size() >= 3 )
                polygon( { out } ); // could not reduce: give up splitting
        }
    }

    void append( const Acc& o )
    {
        uint32_t b = base();
        v.insert( v.end(), o.v.begin(), o.v.end() );

        for( uint32_t k : o.i )
            i.push_back( b + k );
    }
};


Path roundRectPath( double hx, double hy, double r, uint8_t chamferCorners, double chamfer )
{
    // corners: 0 = top-left (-x,-y), 1 = top-right, 2 = bottom-right, 3 = bottom-left (Y down)
    struct C
    {
        double  x, y;
        uint8_t flag;
        double  startAngle; // math angle (atan2 sense in Y-down coords) of arc start
    };

    const C corners[4] = { { -hx, -hy, CHAMFER_TOP_LEFT, PI },
                           { hx, -hy, CHAMFER_TOP_RIGHT, 1.5 * PI },
                           { hx, hy, CHAMFER_BOTTOM_RIGHT, 0.0 },
                           { -hx, hy, CHAMFER_BOTTOM_LEFT, 0.5 * PI } };

    r = std::max( 0.0, std::min( { r, hx, hy } ) );
    chamfer = std::max( 0.0, std::min( { chamfer, 2 * hx, 2 * hy } ) );

    Path out;

    for( int k = 0; k < 4; ++k )
    {
        const C& c = corners[k];
        double   sx = c.x > 0 ? 1.0 : -1.0;
        double   sy = c.y > 0 ? 1.0 : -1.0;

        if( chamfer > 0 && ( chamferCorners & c.flag ) )
        {
            // order: going clockwise in Y-down coords (TL -> TR -> BR -> BL)
            Pt a, b;

            switch( k )
            {
            case 0: a = Pt( c.x, c.y + chamfer ); b = Pt( c.x + chamfer, c.y ); break;
            case 1: a = Pt( c.x - chamfer, c.y ); b = Pt( c.x, c.y + chamfer ); break;
            case 2: a = Pt( c.x, c.y - chamfer ); b = Pt( c.x - chamfer, c.y ); break;
            default: a = Pt( c.x + chamfer, c.y ); b = Pt( c.x, c.y - chamfer ); break;
            }

            out.push_back( a );
            out.push_back( b );
        }
        else if( r > 0 )
        {
            Pt  cc( c.x - sx * r, c.y - sy * r );
            int n = std::max( 2, roundSegments( r ) / 4 );

            for( int s = 0; s <= n; ++s )
            {
                double a = c.startAngle + ( PI / 2 ) * s / n;
                out.push_back( Pt( cc.x + r * std::cos( a ), cc.y + r * std::sin( a ) ) );
            }
        }
        else
        {
            out.push_back( Pt( c.x, c.y ) );
        }
    }

    return out;
}


class Builder
{
public:
    Builder( const Board& aBoard, Mesh& aMesh ) :
            m_board( aBoard ),
            m_mesh( aMesh )
    {
    }

    void Build();

private:
    Acc& acc( int aSlot, ItemKind aKind )
    {
        return m_acc[(size_t) aSlot * (size_t) ItemKind::COUNT + (size_t) aKind];
    }

    void addShape( Acc& a, const Shape& s, const Pt* aPadPos = nullptr, double aPadAngle = 0 );
    void padGeomToAcc( Acc& a, const Pad& p, const PadGeom& g );
    void addHole( Acc& a, const Pt& pos, double angle, const Pt& size, double grow );
    void addText( const Text& t );

    const Board&     m_board;
    Mesh&            m_mesh;
    std::vector<Acc> m_acc;
    int              m_wallSlot = 0;
    int              m_holeSlot = 0;
};


void Builder::addShape( Acc& a, const Shape& s0, const Pt* aPadPos, double aPadAngle )
{
    Shape s = s0;

    if( aPadPos )
    {
        auto tx = [&]( Pt p ) { return RotatePt( p, aPadAngle ) + *aPadPos; };

        if( s.type == ShapeType::RECT && std::fmod( std::fabs( aPadAngle ), 90.0 ) != 0.0 )
        {
            // rotated rectangle -> polygon
            Path p = { s.start, Pt( s.end.x, s.start.y ), s.end, Pt( s.start.x, s.end.y ) };
            s.type = ShapeType::POLY;
            s.poly = p;
        }

        s.start = tx( s.start );
        s.mid = tx( s.mid );
        s.end = tx( s.end );

        for( Pt& p : s.poly )
            p = tx( p );
    }

    switch( s.type )
    {
    case ShapeType::SEGMENT:
        a.capsule( s.start, s.end, s.width );
        break;

    case ShapeType::ARC:
        a.arcStroke( s.start, s.mid, s.end, s.width );
        break;

    case ShapeType::CIRCLE:
    {
        double r = std::hypot( s.end.x - s.start.x, s.end.y - s.start.y );

        if( s.filled )
            a.circle( s.start, r + s.width / 2 );
        else
            a.ring( s.start, r - s.width / 2, r + s.width / 2 );

        break;
    }

    case ShapeType::RECT:
    {
        double x0 = std::min( s.start.x, s.end.x ), x1 = std::max( s.start.x, s.end.x );
        double y0 = std::min( s.start.y, s.end.y ), y1 = std::max( s.start.y, s.end.y );
        Path   outline;

        if( s.radius > 0 )
            outline = roundRectPath( ( x1 - x0 ) / 2, ( y1 - y0 ) / 2, s.radius, 0, 0 );
        else
            outline = { Pt( -( x1 - x0 ) / 2, -( y1 - y0 ) / 2 ), Pt( ( x1 - x0 ) / 2, -( y1 - y0 ) / 2 ),
                        Pt( ( x1 - x0 ) / 2, ( y1 - y0 ) / 2 ), Pt( -( x1 - x0 ) / 2, ( y1 - y0 ) / 2 ) };

        Pt c( ( x0 + x1 ) / 2, ( y0 + y1 ) / 2 );

        for( Pt& p : outline )
            p = p + c;

        if( s.filled )
            a.convex( outline );

        a.polyline( outline, s.width, true );
        break;
    }

    case ShapeType::POLY:
    case ShapeType::BEZIER:
        if( s.filled )
            a.polygon( { s.poly } );

        a.polyline( s.poly, s.width, true );
        break;
    }
}


void Builder::padGeomToAcc( Acc& a, const Pad& p, const PadGeom& g )
{
    const double sx = std::fabs( g.size.x ), sy = std::fabs( g.size.y );
    auto         tx = [&]( const Pt& local ) { return RotatePt( local + g.offset, p.angle ) + p.pos; };

    auto convexLocal = [&]( const Path& local )
    {
        Path pts;
        pts.reserve( local.size() );

        for( const Pt& q : local )
            pts.push_back( tx( q ) );

        a.convex( pts );
    };

    switch( g.shape )
    {
    case PadShape::CIRCLE:
        a.circle( tx( Pt() ), sx / 2 );
        break;

    case PadShape::RECT:
        convexLocal( { Pt( -sx / 2, -sy / 2 ), Pt( sx / 2, -sy / 2 ), Pt( sx / 2, sy / 2 ), Pt( -sx / 2, sy / 2 ) } );
        break;

    case PadShape::OVAL:
        if( sx > sy )
            a.capsule( tx( Pt( -( sx - sy ) / 2, 0 ) ), tx( Pt( ( sx - sy ) / 2, 0 ) ), sy );
        else if( sy > sx )
            a.capsule( tx( Pt( 0, -( sy - sx ) / 2 ) ), tx( Pt( 0, ( sy - sx ) / 2 ) ), sx );
        else
            a.circle( tx( Pt() ), sx / 2 );

        break;

    case PadShape::TRAPEZOID:
    {
        double hx = sx / 2, hy = sy / 2;
        double dx = g.delta.x / 2, dy = g.delta.y / 2;
        convexLocal( { Pt( -hx - dy, hy + dx ), Pt( hx + dy, hy - dx ), Pt( hx - dy, -hy + dx ),
                       Pt( -hx + dy, -hy - dx ) } );
        break;
    }

    case PadShape::ROUNDRECT:
        convexLocal( roundRectPath( sx / 2, sy / 2, g.rratio * std::min( sx, sy ), 0, 0 ) );
        break;

    case PadShape::CHAMFERED_RECT:
        convexLocal( roundRectPath( sx / 2, sy / 2, g.rratio * std::min( sx, sy ), g.chamferCorners,
                                    g.chamferRatio * std::min( sx, sy ) ) );
        break;

    case PadShape::CUSTOM:
    {
        if( g.anchor == PadShape::RECT )
            convexLocal( { Pt( -sx / 2, -sy / 2 ), Pt( sx / 2, -sy / 2 ), Pt( sx / 2, sy / 2 ),
                           Pt( -sx / 2, sy / 2 ) } );
        else
            a.circle( tx( Pt() ), std::min( sx, sy ) / 2 );

        Pt shapePos = RotatePt( g.offset, p.angle ) + p.pos;

        for( const Shape& prim : g.primitives )
        {
            Shape s = prim;

            if( s.type == ShapeType::POLY && !s.filled )
                s.filled = true; // custom pad polygons are always filled copper

            addShape( a, s, &shapePos, p.angle );
        }

        break;
    }
    }
}


void Builder::addHole( Acc& a, const Pt& pos, double angle, const Pt& size, double grow )
{
    double sx = size.x + 2 * grow, sy = size.y + 2 * grow;

    if( sx <= 0 || sy <= 0 )
        return;

    if( std::fabs( sx - sy ) < 1e-9 )
    {
        a.circle( pos, sx / 2 );
    }
    else if( sx > sy )
    {
        Pt d = RotatePt( Pt( ( sx - sy ) / 2, 0 ), angle );
        a.capsule( pos - d, pos + d, sy );
    }
    else
    {
        Pt d = RotatePt( Pt( 0, ( sy - sx ) / 2 ), angle );
        a.capsule( pos - d, pos + d, sx );
    }
}


void Builder::addText( const Text& t )
{
    if( t.layer < 0 || !t.visible )
        return;

    Acc& a = acc( t.layer, ItemKind::TEXT );

    if( !t.renderCache.empty() )
    {
        for( const std::vector<Path>& glyph : t.renderCache )
            a.polygon( glyph );

        return;
    }

    std::string text = m_board.ResolveText( t.text, t.footprint, t.layer );

    if( text.empty() )
        return;

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
        a.polyline( s, t.thickness, false );
}


void Builder::Build()
{
    const auto& layers = m_board.layers;

    // ---- slots
    m_mesh = Mesh();

    for( size_t i = 0; i < layers.size(); ++i )
    {
        const Layer& L = layers[i];
        Slot         s;
        s.name = L.canonical;
        s.display = L.display;
        s.layer = (int) i;
        s.kind = (int) L.kind;
        s.side = L.side;
        s.sortFront = L.sortKey;
        s.sortBack = L.sortKey < 300 ? 270 - L.sortKey : L.sortKey;
        s.color = L.color;
        s.defaultVisible = L.defaultVisible;
        m_mesh.slots.push_back( s );
    }

    {
        Slot w;
        w.name = "HoleWalls";
        w.display = "Delik duvarları";
        w.kind = 10;
        w.sortFront = w.sortBack = 950;
        w.color = { 194 / 255.0f, 194 / 255.0f, 0.0f, 1.0f };
        m_wallSlot = (int) m_mesh.slots.size();
        m_mesh.slots.push_back( w );

        Slot h;
        h.name = "Holes";
        h.display = "Delikler";
        h.kind = 10;
        h.sortFront = h.sortBack = 960;
        h.color = BACKGROUND_COLOR;
        m_holeSlot = (int) m_mesh.slots.size();
        m_mesh.slots.push_back( h );
    }

    m_acc.assign( m_mesh.slots.size() * (size_t) ItemKind::COUNT, Acc() );

    auto isCopper = [&]( int l ) { return l >= 0 && layers[l].kind == LayerKind::COPPER; };

    // ---- tracks
    for( const Track& t : m_board.tracks )
        acc( t.layer, ItemKind::TRACK ).capsule( t.start, t.end, t.width );

    for( const ArcTrack& t : m_board.arcs )
        acc( t.layer, ItemKind::TRACK ).arcStroke( t.start, t.mid, t.end, t.width );

    // ---- vias
    const std::vector<int>& stack = m_board.copperStack;

    for( const Via& v : m_board.vias )
    {
        int oa = 0, ob = (int) stack.size() - 1;

        if( isCopper( v.layerA ) && isCopper( v.layerB ) )
        {
            oa = layers[v.layerA].copperOrder;
            ob = layers[v.layerB].copperOrder;

            if( oa > ob )
                std::swap( oa, ob );
        }

        for( int o = oa; o <= ob && o < (int) stack.size(); ++o )
        {
            int    l = stack[(size_t) o];
            double size = v.size;
            bool   inner = ( o != 0 && o != (int) stack.size() - 1 );

            if( inner && v.innerSize > 0 )
                size = v.innerSize;

            for( const auto& [ol, os] : v.sizeOverride )
            {
                if( ol == l )
                    size = os;
            }

            acc( l, ItemKind::VIA ).circle( v.pos, size / 2 );
        }

        if( v.drill > 0 )
        {
            double wall = std::max( 0.025, v.drill * 0.06 );
            acc( m_wallSlot, ItemKind::WALL_VIA ).circle( v.pos, v.drill / 2 + wall );
            acc( m_holeSlot, ItemKind::HOLE ).circle( v.pos, v.drill / 2 );
        }
    }

    // ---- pads
    for( const Pad& p : m_board.pads )
    {
        Acc defaultGeom;
        bool haveDefault = false;

        for( int l : p.layers )
        {
            if( l < 0 )
                continue;

            const PadGeom* g = &p.geom;
            bool           custom = false;

            for( const auto& [ol, og] : p.layerGeom )
            {
                if( ol == l )
                {
                    g = &og;
                    custom = true;
                }
            }

            if( !custom && p.innerGeom >= 0 && isCopper( l ) && layers[l].side == 0 )
            {
                g = &p.layerGeom[(size_t) p.innerGeom].second;
                custom = true;
            }

            Acc& target = acc( l, ItemKind::PAD );

            if( custom )
            {
                padGeomToAcc( target, p, *g );
            }
            else
            {
                if( !haveDefault )
                {
                    padGeomToAcc( defaultGeom, p, p.geom );
                    haveDefault = true;
                }

                target.append( defaultGeom );
            }
        }

        if( p.drill.x > 0 || p.drill.y > 0 )
        {
            Pt     size = p.drill;
            double wall = std::max( 0.025, std::min( size.x, size.y ) * 0.06 );

            if( !p.drillOval )
                size.y = size.x;

            ItemKind wk = ( p.attr == PadAttr::NPTH ) ? ItemKind::WALL_NPTH : ItemKind::WALL_PTH;
            addHole( acc( m_wallSlot, wk ), p.pos, p.angle, size, wall );
            addHole( acc( m_holeSlot, ItemKind::HOLE ), p.pos, p.angle, size, 0 );
        }
    }

    // ---- zones (triangulated in parallel: this dominates the time on big boards)
    {
        struct Job
        {
            int         layer;
            const Path* poly;
            double      stroke;
            size_t      cost;
        };

        std::vector<Job> jobs;

        for( const Zone& z : m_board.zones )
        {
            if( z.keepout )
                continue;

            for( const ZoneFill& f : z.fills )
            {
                if( f.layer < 0 )
                    continue;

                for( const Path& poly : f.polys )
                    jobs.push_back( { f.layer, &poly, z.strokeFill ? z.minThickness : 0.0, poly.size() } );
            }
        }

        std::vector<Acc>    results( jobs.size() );
        std::atomic<size_t> next( 0 );

        // biggest first for better load balancing
        std::vector<size_t> order( jobs.size() );

        for( size_t k = 0; k < order.size(); ++k )
            order[k] = k;

        std::sort( order.begin(), order.end(),
                   [&]( size_t a, size_t b ) { return jobs[a].cost > jobs[b].cost; } );

        auto worker = [&]()
        {
            for( ;; )
            {
                size_t k = next.fetch_add( 1 );

                if( k >= order.size() )
                    break;

                const Job& j = jobs[order[k]];
                Acc&       r = results[order[k]];
                r.bigPolygon( *j.poly );

                if( j.stroke > 0 )
                    r.polyline( *j.poly, j.stroke, true );
            }
        };

        size_t totalPts = 0;

        for( const Job& j : jobs )
            totalPts += j.cost;

        unsigned hw = std::max( 1u, std::thread::hardware_concurrency() );
        unsigned nThreads = totalPts > 20000 ? std::min( hw, 8u ) : 1u;
        std::vector<std::thread> threads;

        for( unsigned t = 1; t < nThreads; ++t )
            threads.emplace_back( worker );

        worker();

        for( std::thread& t : threads )
            t.join();

        for( size_t k = 0; k < jobs.size(); ++k )
        {
            acc( jobs[k].layer, ItemKind::ZONE ).append( results[k] );
            results[k] = Acc(); // free memory early
        }
    }

    // ---- graphics
    for( const Shape& s : m_board.shapes )
    {
        if( s.layer >= 0 )
            addShape( acc( s.layer, ItemKind::GRAPHIC ), s );
    }

    // ---- text
    for( const Text& t : m_board.texts )
        addText( t );

    // ---- merge in draw order
    std::vector<int> order( m_mesh.slots.size() );

    for( size_t i = 0; i < order.size(); ++i )
        order[i] = (int) i;

    std::stable_sort( order.begin(), order.end(),
                      [&]( int a, int b ) { return m_mesh.slots[a].sortFront < m_mesh.slots[b].sortFront; } );

    static const ItemKind kindOrder[] = { ItemKind::PAD,     ItemKind::VIA,       ItemKind::TRACK,
                                          ItemKind::GRAPHIC, ItemKind::TEXT,      ItemKind::ZONE,
                                          ItemKind::WALL_VIA, ItemKind::WALL_PTH, ItemKind::WALL_NPTH,
                                          ItemKind::HOLE };

    size_t totalV = 0, totalI = 0;

    for( const Acc& a : m_acc )
    {
        totalV += a.v.size();
        totalI += a.i.size();
    }

    m_mesh.vertices.reserve( totalV );
    m_mesh.indices.reserve( totalI );

    const std::array<float, 4> wallPth { 194 / 255.0f, 194 / 255.0f, 0.0f, 1.0f };
    const std::array<float, 4> wallNpth { 26 / 255.0f, 196 / 255.0f, 210 / 255.0f, 1.0f };
    const std::array<float, 4> wallVia { 236 / 255.0f, 236 / 255.0f, 236 / 255.0f, 1.0f };

    for( int slot : order )
    {
        for( ItemKind k : kindOrder )
        {
            Acc& a = acc( slot, k );

            if( a.i.empty() )
                continue;

            DrawGroup g;
            g.slot = slot;
            g.kind = k;
            g.color = m_mesh.slots[slot].color;

            if( k == ItemKind::WALL_PTH )
                g.color = wallPth;
            else if( k == ItemKind::WALL_NPTH )
                g.color = wallNpth;
            else if( k == ItemKind::WALL_VIA )
                g.color = wallVia;
            else if( k == ItemKind::HOLE )
                g.color = BACKGROUND_COLOR;

            uint32_t base = (uint32_t) ( m_mesh.vertices.size() / 2 );
            g.indexStart = (uint32_t) m_mesh.indices.size();
            g.indexCount = (uint32_t) a.i.size();

            m_mesh.vertices.insert( m_mesh.vertices.end(), a.v.begin(), a.v.end() );

            for( uint32_t idx : a.i )
                m_mesh.indices.push_back( base + idx );

            m_mesh.slots[slot].indexCount += g.indexCount;
            m_mesh.groups.push_back( g );

            // release memory as we go
            std::vector<float>().swap( a.v );
            std::vector<uint32_t>().swap( a.i );
        }
    }

    m_acc.clear();

    m_mesh.bounds = m_board.OutlineBBox();

    if( !m_mesh.bounds.valid() )
        m_mesh.bounds = m_board.ContentBBox();

    if( !m_mesh.bounds.valid() )
    {
        m_mesh.bounds.add( Pt( 0, 0 ) );
        m_mesh.bounds.add( Pt( 100, 100 ) );
    }
}

} // anonymous namespace


void BuildMesh( const Board& aBoard, Mesh& aMesh )
{
    Builder b( aBoard, aMesh );
    b.Build();
}

} // namespace kp
