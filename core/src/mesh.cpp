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
#include <unordered_set>

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

    /// Text strokes drawn as quads with square ends: cheap, fine for small labels.
    void strokeQuads( const std::vector<Path>& strokes, double w )
    {
        double hw = w / 2;

        for( const Path& s : strokes )
        {
            for( size_t k = 0; k + 1 < s.size(); ++k )
            {
                const Pt& a = s[k];
                const Pt& b = s[k + 1];
                double    dx = b.x - a.x, dy = b.y - a.y;
                double    len = std::hypot( dx, dy );
                double    ux = len > 1e-12 ? dx / len : 1.0, uy = len > 1e-12 ? dy / len : 0.0;
                Pt        n( -uy * hw, ux * hw );
                Pt        a2( a.x - ux * hw, a.y - uy * hw ), b2( b.x + ux * hw, b.y + uy * hw );
                uint32_t  base_ = base();
                vert( a2 + n );
                vert( a2 - n );
                vert( b2 - n );
                vert( b2 + n );
                tri( base_, base_ + 1, base_ + 2 );
                tri( base_, base_ + 2, base_ + 3 );
            }
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
    static constexpr size_t LABEL_TRIANGLE_BUDGET = 4'000'000;

    size_t accId( int aSlot, ItemKind aKind ) const
    {
        return (size_t) aSlot * (size_t) ItemKind::COUNT + (size_t) aKind;
    }

    Acc& acc( int aSlot, ItemKind aKind ) { return m_acc[accId( aSlot, aKind )]; }

    /// Run aDraw on the accumulator of (slot, kind) and remember the triangles it added as a
    /// range of the current item.
    template <typename F>
    void record( int aSlot, ItemKind aKind, F&& aDraw )
    {
        size_t   id = accId( aSlot, aKind );
        Acc&     a = m_acc[id];
        uint32_t s0 = (uint32_t) a.i.size();
        aDraw( a );
        uint32_t n = (uint32_t) a.i.size() - s0;

        if( n )
            m_pending.push_back( { id, s0, n } );
    }

    void beginItem( ItemType t ) { m_mesh.offsets[(size_t) t].push_back( (uint32_t) m_pending.size() ); }
    void endType( ItemType t ) { m_mesh.offsets[(size_t) t].push_back( (uint32_t) m_pending.size() ); }

    void addShape( Acc& a, const Shape& s, const Pt* aPadPos = nullptr, double aPadAngle = 0 );
    void padGeomToAcc( Acc& a, const Pad& p, const PadGeom& g );
    void addHole( Acc& a, const Pt& pos, double angle, const Pt& size, double grow );
    void addText( size_t aIndex, const Text& t );
    void addLabels();
    void addLabel( int aLayer, const std::string& aText, const Pt& aCenter, double aAngle,
                   double aMaxH, double aMaxW, bool aMirror );

    struct Pending
    {
        size_t   acc;
        uint32_t start;
        uint32_t count;
    };

    const Board&         m_board;
    Mesh&                m_mesh;
    std::vector<Acc>     m_acc;
    std::vector<Pending> m_pending;
    std::vector<int>     m_labelSlot;
    size_t               m_labelTriangles = 0;
    int                  m_wallSlot = 0;
    int                  m_holeSlot = 0;
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


void Builder::addText( size_t aIndex, const Text& t )
{
    if( t.layer < 0 || !t.visible )
        return;

    BBox& box = m_mesh.textBounds[aIndex];

    record( t.layer, ItemKind::TEXT,
            [&]( Acc& a )
            {
                size_t v0 = a.v.size();

                if( !t.renderCache.empty() )
                {
                    for( const std::vector<Path>& glyph : t.renderCache )
                        a.polygon( glyph );
                }
                else
                {
                    std::string text = m_board.ResolveText( t.text, t.footprint, t.layer );

                    if( !text.empty() )
                    {
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
                }

                for( size_t k = v0; k + 1 < a.v.size(); k += 2 )
                    box.add( Pt( a.v[k], a.v[k + 1] ) );
            } );
}


std::string ShortNetName( const std::string& aName )
{
    std::string s = aName;
    size_t      slash = s.rfind( '/' );

    if( slash != std::string::npos && slash + 1 < s.size() )
        s = s.substr( slash + 1 );

    auto replaceAll = [&]( const char* a, const char* b )
    {
        size_t      p = 0;
        std::string from( a ), to( b );

        while( ( p = s.find( from, p ) ) != std::string::npos )
        {
            s.replace( p, from.size(), to );
            p += to.size();
        }
    };

    replaceAll( "{slash}", "/" );
    replaceAll( "{backslash}", "\\" );
    replaceAll( "{colon}", ":" );
    replaceAll( "{dblquote}", "\"" );
    replaceAll( "{tab}", " " );
    replaceAll( "{return}", " " );
    return s;
}


void Builder::addLabel( int aLayer, const std::string& aText, const Pt& aCenter, double aAngle,
                        double aMaxH, double aMaxW, bool aMirror )
{
    if( aLayer < 0 || aText.empty() || m_labelSlot[(size_t) aLayer] < 0 )
        return;

    if( m_labelTriangles > LABEL_TRIANGLE_BUDGET )
        return;

    // measure at size 1
    TextLayout M;
    M.sizeX = M.sizeY = 1.0;
    M.thickness = 0.0;
    std::vector<Path> strokes;
    StrokeFont::Get().Layout( aText, M, strokes );

    BBox ink;

    for( const Path& s : strokes )
    {
        for( const Pt& q : s )
            ink.add( q );
    }

    if( !ink.valid() )
        return;

    double w1 = std::max( ink.maxX - ink.minX, 0.3 );
    double h1 = std::max( ink.maxY - ink.minY, 0.5 );
    double size = std::min( aMaxH / h1, aMaxW / w1 );

    if( size < 0.04 )
        return;

    // keep the text readable: angle in ]-90, 90]
    double a = std::fmod( aAngle, 360.0 );

    while( a > 90.0 )
        a -= 180.0;

    while( a <= -90.0 )
        a += 180.0;

    TextLayout L;
    L.pos = aCenter;
    L.sizeX = L.sizeY = size;
    L.thickness = size * 0.13;
    L.angle = a;
    L.mirror = aMirror;

    strokes.clear();
    StrokeFont::Get().Layout( aText, L, strokes );

    // Layout() anchors the ink box only approximately at the centre: re-centre exactly
    BBox box;

    for( const Path& s : strokes )
    {
        for( const Pt& q : s )
            box.add( q );
    }

    Pt shift( aCenter.x - ( box.minX + box.maxX ) / 2, aCenter.y - ( box.minY + box.maxY ) / 2 );

    for( Path& s : strokes )
    {
        for( Pt& q : s )
            q = q + shift;
    }

    int bucket = size < 0.15 ? 0 : size < 0.3 ? 1 : size < 0.6 ? 2 : size < 1.2 ? 3 : 4;
    Acc& acc_ = acc( m_labelSlot[(size_t) aLayer], (ItemKind) ( (int) ItemKind::LABEL0 + bucket ) );
    size_t i0 = acc_.i.size();
    acc_.strokeQuads( strokes, L.thickness );
    m_labelTriangles += ( acc_.i.size() - i0 ) / 3;
}


void Builder::addLabels()
{
    const auto& layers = m_board.layers;
    int         front = m_board.LayerIndex( "F.Cu" );
    int         back = m_board.LayerIndex( "B.Cu" );

    // ---- pads: number and net name
    for( const Pad& p : m_board.pads )
    {
        bool onFront = false, onBack = false;

        for( int l : p.layers )
        {
            if( l == front )
                onFront = true;
            else if( l == back )
                onBack = true;
        }

        if( !onFront && !onBack )
            continue;

        std::string net = p.net > 0 && p.net < (int) m_board.nets.size()
                                  ? ShortNetName( m_board.nets[(size_t) p.net] )
                                  : std::string();
        std::string text = p.number;

        if( !net.empty() )
            text = text.empty() ? net : text + "\n" + net;

        if( text.empty() )
            continue;

        const PadGeom& g = p.geom;
        double         sx = std::fabs( g.size.x ), sy = std::fabs( g.size.y );
        double         along = std::max( sx, sy ), across = std::min( sx, sy );
        double         angle = p.angle + ( sx >= sy ? 0.0 : 90.0 );
        bool           round = g.shape == PadShape::CIRCLE || g.shape == PadShape::OVAL;
        double         maxW = along * ( round ? 0.72 : 0.85 );
        double         maxH = across * ( round ? 0.62 : 0.75 );

        // a plated hole leaves only the annular ring: keep the label off the hole
        if( p.drill.x > 0 && p.attr != PadAttr::NPTH )
            maxH = std::min( maxH, across * 0.6 );

        Pt c = p.pos + RotatePt( g.offset, p.angle );

        if( onFront )
            addLabel( front, text, c, angle, maxH, maxW, false );

        if( onBack )
            addLabel( back, text, c, angle, maxH, maxW, true );
    }

    // ---- tracks: net names, at most one per net/layer in each 10 mm cell
    std::unordered_set<uint64_t> used;

    for( const Track& t : m_board.tracks )
    {
        if( t.net <= 0 || t.layer < 0 || layers[(size_t) t.layer].kind != LayerKind::COPPER )
            continue;

        double dx = t.end.x - t.start.x, dy = t.end.y - t.start.y;
        double len = std::hypot( dx, dy );

        if( len < std::max( 2.0, 6.0 * t.width ) || t.width < 0.08 )
            continue;

        Pt       mid( ( t.start.x + t.end.x ) / 2, ( t.start.y + t.end.y ) / 2 );
        int64_t  cx = (int64_t) std::floor( mid.x / 10.0 ) & 0xFFFF;
        int64_t  cy = (int64_t) std::floor( mid.y / 10.0 ) & 0xFFFF;
        uint64_t key = ( (uint64_t) ( t.net & 0xFFFFF ) << 44 ) | ( (uint64_t) ( t.layer & 0xFF ) << 36 )
                       | ( (uint64_t) cx << 16 ) | (uint64_t) cy;

        if( !used.insert( key ).second )
            continue;

        double angle = std::atan2( -dy, dx ) * 180.0 / PI;
        bool   mirror = ( t.layer == back );
        addLabel( t.layer, ShortNetName( m_board.nets[(size_t) t.net] ), mid, angle, t.width * 0.7,
                  len * 0.6, mirror );
    }
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

    // one label slot per copper layer (pad numbers, net names), drawn above everything
    m_labelSlot.assign( layers.size(), -1 );

    for( int l : m_board.copperStack )
    {
        const Layer& L = layers[(size_t) l];
        Slot         s;
        s.name = "Labels:" + L.canonical;
        s.display = "Etiketler (" + L.display + ")";
        s.kind = 10;
        s.parentSlot = l;
        s.viewSide = ( L.canonical == "B.Cu" ) ? -1 : 1;
        s.sortFront = s.sortBack = 970 + L.copperOrder;
        s.color = { 1.0f, 1.0f, 1.0f, 0.88f };
        m_labelSlot[(size_t) l] = (int) m_mesh.slots.size();
        m_mesh.slots.push_back( s );
    }

    m_acc.assign( m_mesh.slots.size() * (size_t) ItemKind::COUNT, Acc() );
    m_mesh.offsets.assign( (size_t) ItemType::COUNT, {} );
    m_mesh.textBounds.assign( m_board.texts.size(), BBox() );

    auto isCopper = [&]( int l ) { return l >= 0 && layers[l].kind == LayerKind::COPPER; };

    // ---- tracks
    for( const Track& t : m_board.tracks )
    {
        beginItem( ItemType::TRACK );
        record( t.layer, ItemKind::TRACK, [&]( Acc& a ) { a.capsule( t.start, t.end, t.width ); } );
    }

    endType( ItemType::TRACK );

    for( const ArcTrack& t : m_board.arcs )
    {
        beginItem( ItemType::ARC );
        record( t.layer, ItemKind::TRACK, [&]( Acc& a ) { a.arcStroke( t.start, t.mid, t.end, t.width ); } );
    }

    endType( ItemType::ARC );

    // ---- vias
    const std::vector<int>& stack = m_board.copperStack;

    for( const Via& v : m_board.vias )
    {
        beginItem( ItemType::VIA );
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

            record( l, ItemKind::VIA, [&]( Acc& a ) { a.circle( v.pos, size / 2 ); } );
        }

        if( v.drill > 0 )
        {
            double wall = std::max( 0.025, v.drill * 0.06 );
            acc( m_wallSlot, ItemKind::WALL_VIA ).circle( v.pos, v.drill / 2 + wall );
            acc( m_holeSlot, ItemKind::HOLE ).circle( v.pos, v.drill / 2 );
        }
    }

    endType( ItemType::VIA );

    // ---- pads
    for( const Pad& p : m_board.pads )
    {
        beginItem( ItemType::PAD );
        Acc  defaultGeom;
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

            if( custom )
            {
                record( l, ItemKind::PAD, [&]( Acc& a ) { padGeomToAcc( a, p, *g ); } );
            }
            else
            {
                if( !haveDefault )
                {
                    padGeomToAcc( defaultGeom, p, p.geom );
                    haveDefault = true;
                }

                record( l, ItemKind::PAD, [&]( Acc& a ) { a.append( defaultGeom ); } );
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

    endType( ItemType::PAD );

    // ---- zones (triangulated in parallel: this dominates the time on big boards)
    {
        struct Job
        {
            size_t      zone;
            int         layer;
            const Path* poly;
            double      stroke;
            size_t      cost;
        };

        std::vector<Job> jobs;

        for( size_t zi = 0; zi < m_board.zones.size(); ++zi )
        {
            const Zone& z = m_board.zones[zi];

            if( z.keepout )
                continue;

            for( const ZoneFill& f : z.fills )
            {
                if( f.layer < 0 )
                    continue;

                for( const Path& poly : f.polys )
                    jobs.push_back( { zi, f.layer, &poly, z.strokeFill ? z.minThickness : 0.0, poly.size() } );
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

        size_t k = 0;

        for( size_t zi = 0; zi < m_board.zones.size(); ++zi )
        {
            beginItem( ItemType::ZONE );

            for( ; k < jobs.size() && jobs[k].zone == zi; ++k )
            {
                record( jobs[k].layer, ItemKind::ZONE, [&]( Acc& a ) { a.append( results[k] ); } );
                results[k] = Acc(); // free memory early
            }
        }

        endType( ItemType::ZONE );
    }

    // ---- graphics
    for( const Shape& s : m_board.shapes )
    {
        beginItem( ItemType::SHAPE );

        if( s.layer >= 0 )
            record( s.layer, ItemKind::GRAPHIC, [&]( Acc& a ) { addShape( a, s ); } );
    }

    endType( ItemType::SHAPE );

    // ---- text
    for( size_t i = 0; i < m_board.texts.size(); ++i )
    {
        beginItem( ItemType::TEXT );
        addText( i, m_board.texts[i] );
    }

    endType( ItemType::TEXT );

    // footprints own no geometry of their own (their children carry it)
    m_mesh.offsets[(size_t) ItemType::FOOTPRINT].assign( 1, 0 );

    // ---- pad numbers / net names
    addLabels();

    // ---- merge in draw order
    std::vector<int> order( m_mesh.slots.size() );

    for( size_t i = 0; i < order.size(); ++i )
        order[i] = (int) i;

    std::stable_sort( order.begin(), order.end(),
                      [&]( int a, int b ) { return m_mesh.slots[a].sortFront < m_mesh.slots[b].sortFront; } );

    static const ItemKind kindOrder[] = { ItemKind::PAD,      ItemKind::VIA,       ItemKind::TRACK,
                                          ItemKind::GRAPHIC,  ItemKind::TEXT,      ItemKind::ZONE,
                                          ItemKind::WALL_VIA, ItemKind::WALL_PTH,  ItemKind::WALL_NPTH,
                                          ItemKind::HOLE,     ItemKind::LABEL4,    ItemKind::LABEL3,
                                          ItemKind::LABEL2,   ItemKind::LABEL1,    ItemKind::LABEL0 };

    // a label bucket becomes visible when its smallest text is ~6.5 px tall
    static const float labelMinZoom[] = { 6.5f / 0.06f, 6.5f / 0.15f, 6.5f / 0.3f, 6.5f / 0.6f,
                                          6.5f / 1.2f };

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

    std::vector<uint32_t> groupStart( m_acc.size(), 0 );

    for( int slot : order )
    {
        for( ItemKind k : kindOrder )
        {
            size_t id = accId( slot, k );
            Acc&   a = m_acc[id];

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

            if( k >= ItemKind::LABEL0 && k <= ItemKind::LABEL4 )
                g.minZoom = labelMinZoom[(int) k - (int) ItemKind::LABEL0];

            uint32_t base = (uint32_t) ( m_mesh.vertices.size() / 2 );
            g.indexStart = (uint32_t) m_mesh.indices.size();
            g.indexCount = (uint32_t) a.i.size();
            groupStart[id] = g.indexStart;

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

    // item ranges: local accumulator offsets -> global index positions
    m_mesh.ranges.reserve( m_pending.size() );

    for( const Pending& p : m_pending )
    {
        ItemRange r;
        r.start = groupStart[p.acc] + p.start;
        r.count = p.count;
        r.slot = (int32_t) ( p.acc / (size_t) ItemKind::COUNT );
        r.kind = (uint8_t) ( p.acc % (size_t) ItemKind::COUNT );
        m_mesh.ranges.push_back( r );
    }

    std::vector<Pending>().swap( m_pending );

    // "Zoom to fit" shows everything, like KiCad (labels often sit outside the outline)
    m_mesh.bounds = m_board.ContentBBox();
    m_mesh.bounds.add( m_board.OutlineBBox() );

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
