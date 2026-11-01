/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "geom.h"
#include "pick.h"

#include <algorithm>
#include <cmath>

namespace kp
{

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr double ARC_ERR = 0.002; // mm, default max deviation of flattened arcs

double cross( const Pt& o, const Pt& a, const Pt& b )
{
    return ( a.x - o.x ) * ( b.y - o.y ) - ( a.y - o.y ) * ( b.x - o.x );
}

double dist( const Pt& a, const Pt& b )
{
    return std::hypot( a.x - b.x, a.y - b.y );
}

Pt closestOnSeg( const Pt& p, const Pt& a, const Pt& b )
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? std::clamp( ( ( p.x - a.x ) * dx + ( p.y - a.y ) * dy ) / l2, 0.0, 1.0 ) : 0.0;
    return Pt( a.x + t * dx, a.y + t * dy );
}

/// First point of a primitive's core (used for containment tests).
Pt corePoint( const Prim& p )
{
    return p.kind == Prim::POLYGON ? ( p.poly.empty() ? Pt() : p.poly[0] ) : p.a;
}

/// Distance from a primitive's core to a segment.
double coreSegDist( const Prim& p, const Pt& s1, const Pt& s2 )
{
    switch( p.kind )
    {
    case Prim::POINT: return PointSegmentDistance( p.a, s1, s2 );
    case Prim::SEGMENT: return SegSegDistance( p.a, p.b, s1, s2 );
    case Prim::CHAIN:
    {
        double d = 1e300;

        for( size_t i = 0; i + 1 < p.poly.size() && d > 0; ++i )
            d = std::min( d, SegSegDistance( p.poly[i], p.poly[i + 1], s1, s2 ) );

        return d;
    }
    default:
    {
        double d = 1e300;
        size_t n = p.poly.size();

        for( size_t i = 0; i < n && d > 0; ++i )
            d = std::min( d, SegSegDistance( p.poly[i], p.poly[( i + 1 ) % n], s1, s2 ) );

        return d;
    }
    }
}

double coreDistance( const Prim& A, const Prim& B )
{
    if( A.kind == Prim::CHAIN && B.kind != Prim::CHAIN )
        return coreDistance( B, A );

    if( B.kind == Prim::CHAIN )
    {
        double d = 1e300;

        for( size_t i = 0; i + 1 < B.poly.size() && d > 0; ++i )
            d = std::min( d, coreSegDist( A, B.poly[i], B.poly[i + 1] ) );

        return d;
    }

    if( A.kind == Prim::POLYGON && B.kind != Prim::POLYGON )
        return coreDistance( B, A );

    if( B.kind == Prim::POLYGON )
    {
        if( B.poly.size() >= 3 && PointInPolygon( corePoint( A ), B.poly ) )
            return 0.0;

        if( A.kind == Prim::POLYGON && A.poly.size() >= 3 && PointInPolygon( corePoint( B ), A.poly ) )
            return 0.0;

        double d = 1e300;
        size_t n = B.poly.size();

        for( size_t i = 0; i < n && d > 0; ++i )
            d = std::min( d, coreSegDist( A, B.poly[i], B.poly[( i + 1 ) % n] ) );

        return d;
    }

    if( A.kind == Prim::POINT && B.kind == Prim::POINT )
        return dist( A.a, B.a );

    if( A.kind == Prim::POINT )
        return PointSegmentDistance( A.a, B.a, B.b );

    if( B.kind == Prim::POINT )
        return PointSegmentDistance( B.a, A.a, A.b );

    return SegSegDistance( A.a, A.b, B.a, B.b );
}

Prim point( const Pt& c, double r )
{
    Prim p;
    p.kind = Prim::POINT;
    p.a = c;
    p.r = r;
    p.Finish();
    return p;
}

Prim segment( const Pt& a, const Pt& b, double r )
{
    Prim p;
    p.kind = dist( a, b ) < 1e-9 ? Prim::POINT : Prim::SEGMENT;
    p.a = a;
    p.b = b;
    p.r = r;
    p.Finish();
    return p;
}

Prim polygon( Path poly, double r )
{
    Prim p;
    p.kind = Prim::POLYGON;
    p.poly = std::move( poly );
    p.r = r;
    p.Finish();
    return p;
}

void polylinePrims( const Path& pts, double r, bool closed, std::vector<Prim>& out )
{
    for( size_t i = 0; i + 1 < pts.size(); ++i )
        out.push_back( segment( pts[i], pts[i + 1], r ) );

    if( closed && pts.size() > 2 )
        out.push_back( segment( pts.back(), pts.front(), r ) );
}

/// Rectangle with optional rounded and chamfered corners, centred on the origin, Y down.
Path cornerRect( double hx, double hy, double r, uint8_t corners, double chamfer, double err = ARC_ERR )
{
    struct C
    {
        double  x, y;
        uint8_t flag;
        double  a0;
    };

    const C cs[4] = { { -hx, -hy, CHAMFER_TOP_LEFT, PI },
                      { hx, -hy, CHAMFER_TOP_RIGHT, 1.5 * PI },
                      { hx, hy, CHAMFER_BOTTOM_RIGHT, 0.0 },
                      { -hx, hy, CHAMFER_BOTTOM_LEFT, 0.5 * PI } };

    r = std::max( 0.0, std::min( { r, hx, hy } ) );
    chamfer = std::max( 0.0, std::min( { chamfer, 2 * hx, 2 * hy } ) );
    Path out;

    for( int k = 0; k < 4; ++k )
    {
        const C& c = cs[k];
        double   sx = c.x > 0 ? 1.0 : -1.0, sy = c.y > 0 ? 1.0 : -1.0;

        if( chamfer > 0 && ( corners & c.flag ) )
        {
            switch( k )
            {
            case 0: out.push_back( Pt( c.x, c.y + chamfer ) ); out.push_back( Pt( c.x + chamfer, c.y ) ); break;
            case 1: out.push_back( Pt( c.x - chamfer, c.y ) ); out.push_back( Pt( c.x, c.y + chamfer ) ); break;
            case 2: out.push_back( Pt( c.x, c.y - chamfer ) ); out.push_back( Pt( c.x - chamfer, c.y ) ); break;
            default: out.push_back( Pt( c.x + chamfer, c.y ) ); out.push_back( Pt( c.x, c.y - chamfer ) ); break;
            }
        }
        else if( r > 0 )
        {
            Pt  cc( c.x - sx * r, c.y - sy * r );
            int n = std::max( 2, CircleSegments( r, err ) / 4 );

            for( int s = 0; s <= n; ++s )
            {
                double a = c.a0 + ( PI / 2 ) * s / n;
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

} // anonymous namespace


void Prim::Finish()
{
    box = BBox();

    switch( kind )
    {
    case POINT: box.add( a, r ); break;
    case SEGMENT: box.add( a, r ); box.add( b, r ); break;
    case POLYGON:
    case CHAIN:
        for( const Pt& p : poly )
            box.add( p, r );

        break;
    }
}


void CuShape::Finish()
{
    box = BBox();

    for( const Prim& p : prims )
        box.add( p.box );
}


double SegSegDistance( const Pt& a1, const Pt& a2, const Pt& b1, const Pt& b2 )
{
    double d1 = cross( b1, b2, a1 ), d2 = cross( b1, b2, a2 );
    double d3 = cross( a1, a2, b1 ), d4 = cross( a1, a2, b2 );

    if( ( ( d1 > 0 && d2 < 0 ) || ( d1 < 0 && d2 > 0 ) ) && ( ( d3 > 0 && d4 < 0 ) || ( d3 < 0 && d4 > 0 ) ) )
        return 0.0;

    return std::min( { PointSegmentDistance( a1, b1, b2 ), PointSegmentDistance( a2, b1, b2 ),
                       PointSegmentDistance( b1, a1, a2 ), PointSegmentDistance( b2, a1, a2 ) } );
}


double PrimDistance( const Prim& A, const Prim& B )
{
    return std::max( 0.0, coreDistance( A, B ) - A.r - B.r );
}


void PrimGrid::Build( const std::vector<Prim>& prims, const BBox& box )
{
    double w = std::max( 1e-3, box.maxX - box.minX ), h = std::max( 1e-3, box.maxY - box.minY );
    cell = std::max( 0.05, std::sqrt( w * h / (double) std::max<size_t>( 1, prims.size() ) ) * 1.5 );
    nx = std::max( 1, std::min( 1024, (int) std::ceil( w / cell ) ) );
    ny = std::max( 1, std::min( 1024, (int) std::ceil( h / cell ) ) );
    cell = std::max( w / nx, h / ny );
    x0 = box.minX;
    y0 = box.minY;
    cells.assign( (size_t) nx * ny, {} );

    for( size_t i = 0; i < prims.size(); ++i )
    {
        const BBox& b = prims[i].box;
        int         cx0 = std::clamp( (int) ( ( b.minX - x0 ) / cell ), 0, nx - 1 );
        int         cx1 = std::clamp( (int) ( ( b.maxX - x0 ) / cell ), 0, nx - 1 );
        int         cy0 = std::clamp( (int) ( ( b.minY - y0 ) / cell ), 0, ny - 1 );
        int         cy1 = std::clamp( (int) ( ( b.maxY - y0 ) / cell ), 0, ny - 1 );

        for( int y = cy0; y <= cy1; ++y )
        {
            for( int x = cx0; x <= cx1; ++x )
                cells[(size_t) y * nx + x].push_back( (int) i );
        }
    }
}


void PrimGrid::Query( const BBox& b, std::vector<int>& out ) const
{
    out.clear();
    int cx0 = std::clamp( (int) std::floor( ( b.minX - x0 ) / cell ), 0, nx - 1 );
    int cx1 = std::clamp( (int) std::floor( ( b.maxX - x0 ) / cell ), 0, nx - 1 );
    int cy0 = std::clamp( (int) std::floor( ( b.minY - y0 ) / cell ), 0, ny - 1 );
    int cy1 = std::clamp( (int) std::floor( ( b.maxY - y0 ) / cell ), 0, ny - 1 );

    if( b.maxX < x0 || b.maxY < y0 || b.minX > x0 + nx * cell || b.minY > y0 + ny * cell )
        return;

    for( int y = cy0; y <= cy1; ++y )
    {
        for( int x = cx0; x <= cx1; ++x )
        {
            const std::vector<int>& c = cells[(size_t) y * nx + x];
            out.insert( out.end(), c.begin(), c.end() );
        }
    }

    std::sort( out.begin(), out.end() );
    out.erase( std::unique( out.begin(), out.end() ), out.end() );
}


void AreaIndex::Build( const Path& aPoly )
{
    poly = &aPoly;
    box = BBox();

    for( const Pt& p : aPoly )
        box.add( p );

    size_t n = aPoly.size();
    rows = std::max( 1, std::min( 4096, (int) std::sqrt( (double) n ) ) );
    y0 = box.minY;
    rowH = std::max( 1e-6, ( box.maxY - box.minY ) / rows );
    buckets.assign( (size_t) rows, {} );

    for( size_t i = 0; i < n; ++i )
    {
        const Pt& a = aPoly[i];
        const Pt& b = aPoly[( i + 1 ) % n];
        int       r0 = std::clamp( (int) ( ( std::min( a.y, b.y ) - y0 ) / rowH ), 0, rows - 1 );
        int       r1 = std::clamp( (int) ( ( std::max( a.y, b.y ) - y0 ) / rowH ), 0, rows - 1 );

        for( int r = r0; r <= r1; ++r )
            buckets[(size_t) r].push_back( (uint32_t) i );
    }
}


bool AreaIndex::Contains( const Pt& p ) const
{
    if( !poly || poly->size() < 3 || p.x < box.minX || p.x > box.maxX || p.y < box.minY || p.y > box.maxY )
        return false;

    int        r = std::clamp( (int) ( ( p.y - y0 ) / rowH ), 0, rows - 1 );
    const Path& P = *poly;
    size_t     n = P.size();
    bool       inside = false;

    for( uint32_t i : buckets[(size_t) r] )
    {
        const Pt& a = P[i];
        const Pt& b = P[( i + 1 ) % n];

        if( ( a.y > p.y ) != ( b.y > p.y ) )
        {
            double x = a.x + ( p.y - a.y ) * ( b.x - a.x ) / ( b.y - a.y );

            if( p.x < x )
                inside = !inside;
        }
    }

    return inside;
}


static bool insideAreas( const CuShape& aArea, const CuShape& aOther )
{
    if( aArea.areas.empty() || aOther.prims.empty() )
        return false;

    Pt p = corePoint( aOther.prims.front() );

    if( p.x < aArea.box.minX || p.x > aArea.box.maxX || p.y < aArea.box.minY || p.y > aArea.box.maxY )
        return false;

    for( const AreaIndex* a : aArea.areas )
    {
        if( a->Contains( p ) )
            return true;
    }

    return false;
}


double ShapeDistance( const CuShape& A, const CuShape& B )
{
    double best = ShapeDistanceTo( A, B, -1.0 );
    return best;
}


double ShapeDistanceTo( const CuShape& A, const CuShape& B, double aCutoff )
{
    if( aCutoff > 0 )
    {
        double gx = std::max( { 0.0, A.box.minX - B.box.maxX, B.box.minX - A.box.maxX } );
        double gy = std::max( { 0.0, A.box.minY - B.box.maxY, B.box.minY - A.box.maxY } );

        if( std::max( gx, gy ) >= aCutoff )
            return aCutoff;
    }

    if( insideAreas( A, B ) || insideAreas( B, A ) )
        return 0.0;

    double best = aCutoff > 0 ? aCutoff : 1e300;

    // a shape with many primitives (zone fill) against a small one: look only near the small one
    if( ( A.grid && !B.grid ) || ( B.grid && !A.grid ) )
    {
        const CuShape&   big = A.grid ? A : B;
        const CuShape&   small = A.grid ? B : A;
        std::vector<int> ids;

        for( const Prim& p : small.prims )
        {
            double reach = best < 1e299 ? best : 1e3;
            BBox   q = p.box;
            q.minX -= reach;
            q.minY -= reach;
            q.maxX += reach;
            q.maxY += reach;
            big.grid->Query( q, ids );

            for( int id : ids )
            {
                const Prim& o = big.prims[(size_t) id];
                double      gx = std::max( { 0.0, p.box.minX - o.box.maxX, o.box.minX - p.box.maxX } );
                double      gy = std::max( { 0.0, p.box.minY - o.box.maxY, o.box.minY - p.box.maxY } );

                if( std::max( gx, gy ) >= best )
                    continue;

                best = std::min( best, PrimDistance( p, o ) );

                if( best <= 0 )
                    return 0.0;
            }
        }

        return best;
    }

    for( const Prim& p : A.prims )
    {
        for( const Prim& q : B.prims )
        {
            // cheap reject: boxes further apart than the best so far
            double gx = std::max( { 0.0, p.box.minX - q.box.maxX, q.box.minX - p.box.maxX } );
            double gy = std::max( { 0.0, p.box.minY - q.box.maxY, q.box.minY - p.box.maxY } );

            if( std::max( gx, gy ) >= best )
                continue;

            best = std::min( best, PrimDistance( p, q ) );

            if( best <= 0 )
                return 0.0;
        }
    }

    return best;
}


Pt PrimNearPoint( const Prim& A, const Prim& B )
{
    Pt pa = corePoint( A ), pb = corePoint( B );

    if( A.kind == Prim::SEGMENT )
        pa = closestOnSeg( pb, A.a, A.b );

    if( B.kind == Prim::SEGMENT )
        pb = closestOnSeg( pa, B.a, B.b );

    if( A.kind == Prim::SEGMENT )
        pa = closestOnSeg( pb, A.a, A.b );

    return Pt( ( pa.x + pb.x ) / 2, ( pa.y + pb.y ) / 2 );
}


const PadGeom& PadGeomOnLayer( const Board& b, const Pad& p, int layer )
{
    for( const auto& [ol, og] : p.layerGeom )
    {
        if( ol == layer )
            return og;
    }

    if( p.innerGeom >= 0 && layer >= 0 && layer < (int) b.layers.size()
        && b.layers[(size_t) layer].kind == LayerKind::COPPER && b.layers[(size_t) layer].side == 0 )
        return p.layerGeom[(size_t) p.innerGeom].second;

    return p.geom;
}


void PadPrims( const Pad& p, const PadGeom& g, double grow, std::vector<Prim>& out, double err )
{
    const double sx = std::fabs( g.size.x ), sy = std::fabs( g.size.y );
    auto         tx = [&]( const Pt& local ) { return RotatePt( local + g.offset, p.angle ) + p.pos; };

    auto polyLocal = [&]( const Path& local, double r )
    {
        Path pts;
        pts.reserve( local.size() );

        for( const Pt& q : local )
            pts.push_back( tx( q ) );

        out.push_back( polygon( std::move( pts ), r ) );
    };

    auto rect = [&]( double hx, double hy, double r )
    {
        const double eps = 1e-7;

        if( hx < eps && hy < eps )
            out.push_back( point( tx( Pt() ), r ) );
        else if( hx < eps )
            out.push_back( segment( tx( Pt( 0, -hy ) ), tx( Pt( 0, hy ) ), r ) );
        else if( hy < eps )
            out.push_back( segment( tx( Pt( -hx, 0 ) ), tx( Pt( hx, 0 ) ), r ) );
        else
            polyLocal( { Pt( -hx, -hy ), Pt( hx, -hy ), Pt( hx, hy ), Pt( -hx, hy ) }, r );
    };

    switch( g.shape )
    {
    case PadShape::CIRCLE:
        out.push_back( point( tx( Pt() ), sx / 2 + grow ) );
        break;

    case PadShape::RECT:
        rect( sx / 2, sy / 2, grow );
        break;

    case PadShape::OVAL:
        if( sx > sy )
            out.push_back( segment( tx( Pt( -( sx - sy ) / 2, 0 ) ), tx( Pt( ( sx - sy ) / 2, 0 ) ), sy / 2 + grow ) );
        else
            out.push_back( segment( tx( Pt( 0, -( sy - sx ) / 2 ) ), tx( Pt( 0, ( sy - sx ) / 2 ) ), sx / 2 + grow ) );

        break;

    case PadShape::ROUNDRECT:
    {
        double rr = std::min( g.rratio * std::min( sx, sy ), std::min( sx, sy ) / 2 );
        rect( sx / 2 - rr, sy / 2 - rr, rr + grow );
        break;
    }

    case PadShape::CHAMFERED_RECT:
        polyLocal( cornerRect( sx / 2, sy / 2, g.rratio * std::min( sx, sy ), g.chamferCorners,
                               g.chamferRatio * std::min( sx, sy ), err ),
                   grow );
        break;

    case PadShape::TRAPEZOID:
    {
        double hx = sx / 2, hy = sy / 2, dx = g.delta.x / 2, dy = g.delta.y / 2;
        polyLocal( { Pt( -hx - dy, hy + dx ), Pt( hx + dy, hy - dx ), Pt( hx - dy, -hy + dx ), Pt( -hx + dy, -hy - dx ) },
                   grow );
        break;
    }

    case PadShape::CUSTOM:
    {
        if( g.anchor == PadShape::RECT )
            rect( sx / 2, sy / 2, grow );
        else
            out.push_back( point( tx( Pt() ), std::min( sx, sy ) / 2 + grow ) );

        Pt shapePos = RotatePt( g.offset, p.angle ) + p.pos;

        for( const Shape& prim : g.primitives )
        {
            Shape s = prim;
            auto  t = [&]( const Pt& q ) { return RotatePt( q, p.angle ) + shapePos; };

            if( s.type == ShapeType::RECT )
            {
                s.type = ShapeType::POLY;
                s.poly = { prim.start, Pt( prim.end.x, prim.start.y ), prim.end, Pt( prim.start.x, prim.end.y ) };
            }

            if( s.type == ShapeType::POLY || s.type == ShapeType::BEZIER )
                s.filled = true; // custom pad polygons are always filled copper

            s.start = t( s.start );
            s.mid = t( s.mid );
            s.end = t( s.end );

            for( Pt& q : s.poly )
                q = t( q );

            size_t first = out.size();
            ShapePrims( s, out, err );

            for( size_t k = first; k < out.size(); ++k )
            {
                out[k].r += grow;
                out[k].Finish();
            }
        }

        break;
    }
    }
}


bool ViaOnLayer( const Board& b, const Via& v, int layer, double* aSize )
{
    if( layer < 0 || layer >= (int) b.layers.size() || b.layers[(size_t) layer].kind != LayerKind::COPPER )
        return false;

    const std::vector<int>& stack = b.copperStack;
    int oa = 0, ob = (int) stack.size() - 1;
    auto isCu = [&]( int l )
    { return l >= 0 && l < (int) b.layers.size() && b.layers[(size_t) l].kind == LayerKind::COPPER; };

    if( isCu( v.layerA ) && isCu( v.layerB ) )
    {
        oa = b.layers[(size_t) v.layerA].copperOrder;
        ob = b.layers[(size_t) v.layerB].copperOrder;

        if( oa > ob )
            std::swap( oa, ob );
    }

    int o = b.layers[(size_t) layer].copperOrder;

    if( o < oa || o > ob )
        return false;

    if( aSize )
    {
        double size = v.size;
        bool   inner = o != 0 && o != (int) stack.size() - 1;

        if( inner && v.innerSize > 0 )
            size = v.innerSize;

        for( const auto& [ol, os] : v.sizeOverride )
        {
            if( ol == layer )
                size = os;
        }

        *aSize = size;
    }

    return true;
}


void ShapePrims( const Shape& s, std::vector<Prim>& out, double err )
{
    double hw = s.width / 2;

    switch( s.type )
    {
    case ShapeType::SEGMENT:
        out.push_back( segment( s.start, s.end, hw ) );
        break;

    case ShapeType::ARC:
    {
        Path pts;
        ArcToPoints( pts, s.start, s.mid, s.end, err );
        polylinePrims( pts, hw, false, out );
        break;
    }

    case ShapeType::CIRCLE:
    {
        double r = dist( s.start, s.end );

        if( s.filled )
        {
            out.push_back( point( s.start, r + hw ) );
        }
        else
        {
            int  n = CircleSegments( r, err );
            Path pts;

            for( int k = 0; k < n; ++k )
                pts.push_back( Pt( s.start.x + r * std::cos( 2 * PI * k / n ), s.start.y + r * std::sin( 2 * PI * k / n ) ) );

            polylinePrims( pts, hw, true, out );
        }

        break;
    }

    case ShapeType::RECT:
    {
        double x0 = std::min( s.start.x, s.end.x ), x1 = std::max( s.start.x, s.end.x );
        double y0 = std::min( s.start.y, s.end.y ), y1 = std::max( s.start.y, s.end.y );
        Path   outline = cornerRect( ( x1 - x0 ) / 2, ( y1 - y0 ) / 2, s.radius, 0, 0, err );

        for( Pt& p : outline )
            p = p + Pt( ( x0 + x1 ) / 2, ( y0 + y1 ) / 2 );

        if( s.filled )
            out.push_back( polygon( outline, hw ) );
        else
            polylinePrims( outline, hw, true, out );

        break;
    }

    case ShapeType::POLY:
    case ShapeType::BEZIER:
        if( s.poly.size() >= 3 && s.filled )
            out.push_back( polygon( s.poly, hw ) );
        else
            polylinePrims( s.poly, hw, s.type == ShapeType::POLY, out );

        break;
    }
}


void CopperIndex::Build( const Board& b, bool aZones, bool aGraphics, double aArcError, bool aFlashing )
{
    m_shapes.clear();
    m_areas.clear();
    m_holeOnly.clear();
    m_grids.assign( b.layers.size(), Grid() );

    auto isCu = [&]( int l )
    { return l >= 0 && l < (int) b.layers.size() && b.layers[(size_t) l].kind == LayerKind::COPPER; };

    auto add = [&]( CuShape&& s )
    {
        s.Finish();

        if( s.box.valid() && !s.prims.empty() )
            m_shapes.push_back( std::move( s ) );
    };

    for( size_t i = 0; i < b.tracks.size(); ++i )
    {
        const Track& t = b.tracks[i];

        if( !isCu( t.layer ) )
            continue;

        CuShape s;
        s.type = ItemType::TRACK;
        s.index = (int) i;
        s.net = t.net;
        s.layer = t.layer;
        s.prims.push_back( segment( t.start, t.end, t.width / 2 ) );
        add( std::move( s ) );
    }

    for( size_t i = 0; i < b.arcs.size(); ++i )
    {
        const ArcTrack& t = b.arcs[i];

        if( !isCu( t.layer ) )
            continue;

        CuShape s;
        s.type = ItemType::ARC;
        s.index = (int) i;
        s.net = t.net;
        s.layer = t.layer;
        Path pts;
        ArcToPoints( pts, t.start, t.mid, t.end, aArcError );
        polylinePrims( pts, t.width / 2, false, s.prims );
        add( std::move( s ) );
    }

    for( size_t i = 0; i < b.vias.size(); ++i )
    {
        const Via& v = b.vias[i];

        for( int l : b.copperStack )
        {
            double size = 0;

            if( !ViaOnLayer( b, v, l, &size ) )
                continue;

            CuShape s;
            s.type = ItemType::VIA;
            s.index = (int) i;
            s.net = v.net;
            s.layer = l;
            s.prims.push_back( point( v.pos, size / 2 ) );
            add( std::move( s ) );
        }
    }

    for( size_t i = 0; i < b.pads.size(); ++i )
    {
        const Pad& p = b.pads[i];

        if( p.attr == PadAttr::NPTH )
        {
            // only copper if the pad has a shape larger than its hole
            bool copper = p.geom.size.x > p.drill.x + 1e-6 || p.geom.size.y > std::max( p.drill.x, p.drill.y ) + 1e-6;

            if( !copper )
                continue;
        }

        for( int l : p.layers )
        {
            if( !isCu( l ) )
                continue;

            CuShape s;
            s.type = ItemType::PAD;
            s.index = (int) i;
            s.net = p.net;
            s.layer = l;
            s.footprint = p.footprint;
            PadPrims( p, PadGeomOnLayer( b, p, l ), 0.0, s.prims, aArcError );
            add( std::move( s ) );
        }
    }

    if( aGraphics )
    {
        for( size_t i = 0; i < b.shapes.size(); ++i )
        {
            const Shape& sh = b.shapes[i];

            if( !isCu( sh.layer ) )
                continue;

            CuShape s;
            s.type = ItemType::SHAPE;
            s.index = (int) i;
            s.layer = sh.layer;
            s.footprint = sh.footprint;
            ShapePrims( sh, s.prims, aArcError );
            add( std::move( s ) );
        }
    }

    if( aZones )
    {
        for( size_t i = 0; i < b.zones.size(); ++i )
        {
            const Zone& z = b.zones[i];

            if( z.keepout )
                continue;

            for( const ZoneFill& f : z.fills )
            {
                if( !isCu( f.layer ) )
                    continue;

                CuShape s;
                s.type = ItemType::ZONE;
                s.index = (int) i;
                s.net = z.net;
                s.layer = f.layer;
                s.footprint = z.footprint;

                // The outline in short open pieces (so distance queries only look at the nearby
                // part of a large fill), plus the polygon itself for containment.
                double r = z.strokeFill ? z.minThickness / 2 : 0.0;

                for( const Path& poly : f.polys )
                {
                    if( poly.size() < 3 )
                        continue;

                    const size_t CHUNK = 24;
                    size_t       n = poly.size();

                    for( size_t i0 = 0; i0 < n; i0 += CHUNK )
                    {
                        Prim c;
                        c.kind = Prim::CHAIN;
                        c.r = r;

                        for( size_t k = i0; k <= std::min( n, i0 + CHUNK ); ++k )
                            c.poly.push_back( poly[k % n] );

                        c.Finish();
                        s.prims.push_back( std::move( c ) );
                    }

                    m_areas.push_back( std::make_unique<AreaIndex>() );
                    m_areas.back()->Build( poly );
                    s.areas.push_back( m_areas.back().get() );
                }

                add( std::move( s ) );
            }
        }
    }

    // grids
    std::vector<std::vector<int>> byLayer( b.layers.size() );

    for( size_t i = 0; i < m_shapes.size(); ++i )
        byLayer[(size_t) m_shapes[i].layer].push_back( (int) i );

    for( size_t l = 0; l < byLayer.size(); ++l )
    {
        const std::vector<int>& ids = byLayer[l];

        if( ids.empty() )
            continue;

        BBox all;

        for( int id : ids )
            all.add( m_shapes[(size_t) id].box );

        Grid&  g = m_grids[l];
        double area = std::max( 1.0, ( all.maxX - all.minX ) * ( all.maxY - all.minY ) );
        g.cell = std::clamp( std::sqrt( area / (double) ids.size() ) * 2.0, 0.5, 20.0 );
        g.x0 = all.minX;
        g.y0 = all.minY;
        g.nx = std::max( 1, (int) std::ceil( ( all.maxX - all.minX ) / g.cell ) + 1 );
        g.ny = std::max( 1, (int) std::ceil( ( all.maxY - all.minY ) / g.cell ) + 1 );

        while( (double) g.nx * g.ny > 4e6 )
        {
            g.cell *= 2;
            g.nx = std::max( 1, (int) std::ceil( ( all.maxX - all.minX ) / g.cell ) + 1 );
            g.ny = std::max( 1, (int) std::ceil( ( all.maxY - all.minY ) / g.cell ) + 1 );
        }

        g.cells.assign( (size_t) g.nx * g.ny, {} );

        for( int id : ids )
        {
            const BBox& bb = m_shapes[(size_t) id].box;
            int         cx0 = std::clamp( (int) ( ( bb.minX - g.x0 ) / g.cell ), 0, g.nx - 1 );
            int         cx1 = std::clamp( (int) ( ( bb.maxX - g.x0 ) / g.cell ), 0, g.nx - 1 );
            int         cy0 = std::clamp( (int) ( ( bb.minY - g.y0 ) / g.cell ), 0, g.ny - 1 );
            int         cy1 = std::clamp( (int) ( ( bb.maxY - g.y0 ) / g.cell ), 0, g.ny - 1 );

            if( (long) ( cx1 - cx0 + 1 ) * ( cy1 - cy0 + 1 ) > 256 )
            {
                g.big.push_back( id );
                continue;
            }

            for( int cy = cy0; cy <= cy1; ++cy )
            {
                for( int cx = cx0; cx <= cx1; ++cx )
                    g.cells[(size_t) cy * g.nx + cx].push_back( id );
            }
        }
    }

    m_stamp.assign( m_shapes.size(), 0 );
    m_stampValue = 0;

    for( CuShape& sh : m_shapes )
    {
        if( sh.prims.size() > 48 )
        {
            sh.grid = std::make_shared<PrimGrid>();
            sh.grid->Build( sh.prims, sh.box );
        }
    }

    if( aFlashing )
        applyFlashing( b );
}


void CopperIndex::applyFlashing( const Board& b )
{
    std::vector<size_t> holeOnly;
    std::vector<int>    near;

    for( size_t i = 0; i < m_shapes.size(); ++i )
    {
        const CuShape& s = m_shapes[i];
        bool           removable = false, keepEnd = false;
        int            firstOrder = 0, lastOrder = (int) b.copperStack.size() - 1;

        if( s.type == ItemType::VIA )
        {
            const Via& v = b.vias[(size_t) s.index];
            removable = v.removeUnused;
            keepEnd = v.keepEnd;

            auto cu = [&]( int l ) { return l >= 0 && l < (int) b.layers.size() && b.layers[(size_t) l].kind == LayerKind::COPPER; };

            if( cu( v.layerA ) && cu( v.layerB ) )
            {
                firstOrder = std::min( b.layers[(size_t) v.layerA].copperOrder, b.layers[(size_t) v.layerB].copperOrder );
                lastOrder = std::max( b.layers[(size_t) v.layerA].copperOrder, b.layers[(size_t) v.layerB].copperOrder );
            }
        }
        else if( s.type == ItemType::PAD )
        {
            const Pad& p = b.pads[(size_t) s.index];
            removable = p.removeUnused && p.attr == PadAttr::PTH && p.drill.x > 0;
            keepEnd = p.keepEnd;
        }

        if( !removable )
            continue;

        int order = b.layers[(size_t) s.layer].copperOrder;

        if( keepEnd && ( order == firstOrder || order == lastOrder ) )
            continue;

        bool connected = false;
        Query( s.layer, s.box, near );

        for( int id : near )
        {
            const CuShape& o = m_shapes[(size_t) id];

            if( (size_t) id == i || s.net <= 0 || o.net != s.net )
                continue;

            // zones do not count (KiCad flashes for them only when its zone filler says so)
            if( o.type != ItemType::TRACK && o.type != ItemType::ARC && o.type != ItemType::PAD
                && o.type != ItemType::VIA )
                continue;

            if( s.type == ItemType::PAD && o.type == ItemType::PAD && s.footprint >= 0 && o.footprint == s.footprint )
                continue;

            if( ShapeDistance( s, o ) <= 0 )
            {
                connected = true;
                break;
            }
        }

        if( !connected )
            holeOnly.push_back( i );
    }

    for( size_t i : holeOnly )
    {
        CuShape& s = m_shapes[i];
        m_holeOnly.insert( flashKey( s.type, s.index, s.layer ) );
        s.prims.clear();
        s.grid.reset(); // its primitive grid no longer matches

        if( s.type == ItemType::VIA )
        {
            const Via& v = b.vias[(size_t) s.index];
            s.prims.push_back( point( v.pos, v.drill / 2 ) );
        }
        else
        {
            const Pad& p = b.pads[(size_t) s.index];
            double     dx = std::fabs( p.drill.x ), dy = std::fabs( p.drill.y > 0 ? p.drill.y : p.drill.x );
            Pt         half = dx > dy ? Pt( ( dx - dy ) / 2, 0 ) : Pt( 0, ( dy - dx ) / 2 );
            s.prims.push_back( segment( p.pos - RotatePt( half, p.angle ), p.pos + RotatePt( half, p.angle ),
                                        std::min( dx, dy ) / 2 ) );
        }

        s.Finish();
    }
}


int CopperIndex::AddUnindexed( const CuShape& aShape )
{
    m_shapes.push_back( aShape );
    m_shapes.back().Finish();
    m_stamp.push_back( 0 );
    return (int) m_shapes.size() - 1;
}


void CopperIndex::Query( int layer, const BBox& box, std::vector<int>& out ) const
{
    out.clear();

    if( layer < 0 || layer >= (int) m_grids.size() || !box.valid() )
        return;

    const Grid& g = m_grids[(size_t) layer];

    if( g.cells.empty() && g.big.empty() )
        return;

    if( ++m_stampValue == 0 )
    {
        std::fill( m_stamp.begin(), m_stamp.end(), 0 );
        m_stampValue = 1;
    }

    auto consider = [&]( int id )
    {
        if( m_stamp[(size_t) id] == m_stampValue )
            return;

        m_stamp[(size_t) id] = m_stampValue;
        const BBox& b = m_shapes[(size_t) id].box;

        if( b.minX <= box.maxX && b.maxX >= box.minX && b.minY <= box.maxY && b.maxY >= box.minY )
            out.push_back( id );
    };

    for( int id : g.big )
        consider( id );

    if( g.cells.empty() )
        return;

    int cx0 = std::clamp( (int) std::floor( ( box.minX - g.x0 ) / g.cell ), 0, g.nx - 1 );
    int cx1 = std::clamp( (int) std::floor( ( box.maxX - g.x0 ) / g.cell ), 0, g.nx - 1 );
    int cy0 = std::clamp( (int) std::floor( ( box.minY - g.y0 ) / g.cell ), 0, g.ny - 1 );
    int cy1 = std::clamp( (int) std::floor( ( box.maxY - g.y0 ) / g.cell ), 0, g.ny - 1 );

    if( box.maxX < g.x0 || box.maxY < g.y0 || box.minX > g.x0 + g.nx * g.cell || box.minY > g.y0 + g.ny * g.cell )
        return;

    for( int cy = cy0; cy <= cy1; ++cy )
    {
        for( int cx = cx0; cx <= cx1; ++cx )
        {
            for( int id : g.cells[(size_t) cy * g.nx + cx] )
                consider( id );
        }
    }
}

} // namespace kp
