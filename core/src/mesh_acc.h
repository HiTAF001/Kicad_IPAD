/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Triangle accumulator shared by the board and schematic mesh builders: strokes, circles,
 * rings, arcs and polygons (earcut) in one vertex/index list.
 */
#ifndef NETLOOM_MESH_ACC_H
#define NETLOOM_MESH_ACC_H

#include "board.h"

#include "../third_party/earcut.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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

static constexpr double PI = 3.14159265358979323846;

/// Segments for a full circle used for strokes/pads: relative error with sane bounds.
inline int roundSegments( double r )
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


inline Path roundRectPath( double hx, double hy, double r, uint8_t chamferCorners, double chamfer )
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

} // namespace kp

#endif
