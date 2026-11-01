/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "route.h"
#include "i18n.h"
#include "pick.h"

#include <algorithm>
#include <cmath>

namespace kp
{

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr double SQRT2 = 1.41421356237309504880;

double dot( const Pt& a, const Pt& b ) { return a.x * b.x + a.y * b.y; }
double cross2( const Pt& a, const Pt& b ) { return a.x * b.y - a.y * b.x; }
double len( const Pt& a ) { return std::hypot( a.x, a.y ); }

Pt unit( const Pt& a )
{
    double l = len( a );
    return l > 0 ? Pt( a.x / l, a.y / l ) : Pt( 1, 0 );
}

Pt dirOf( int q )
{
    q = ( ( q % 8 ) + 8 ) % 8;
    static const double c = 1.0 / SQRT2;
    static const Pt     d[8] = { { 1, 0 }, { c, c }, { 0, 1 }, { -c, c }, { -1, 0 }, { -c, -c }, { 0, -1 }, { c, -c } };
    return d[q];
}

int quantize( const Pt& d )
{
    int q = (int) std::lround( std::atan2( d.y, d.x ) / ( PI / 4 ) );
    return ( ( q % 8 ) + 8 ) % 8;
}

/// Direction index if d is (very nearly) horizontal, vertical or diagonal, else -1.
int octant( const Pt& d )
{
    double l = len( d );

    if( l < 1e-9 )
        return -1;

    int q = quantize( d );
    Pt  u = dirOf( q );
    return std::fabs( cross2( u, d ) ) <= 1e-7 * std::max( 1.0, l ) && dot( u, d ) > 0 ? q : -1;
}

double strokeDistance( const Path& raw, const Pt& p )
{
    double best = 1e300;

    for( size_t i = 0; i + 1 < raw.size(); ++i )
        best = std::min( best, PointSegmentDistance( p, raw[i], raw[i + 1] ) );

    if( raw.size() == 1 )
        best = len( p - raw[0] );

    return best;
}

/// Cost of a polyline's deviation from the stroke (sampled).
double deviation( const Path& raw, const Path& pts )
{
    double sum = 0;

    for( size_t i = 0; i + 1 < pts.size(); ++i )
    {
        for( int k = 1; k <= 3; ++k )
        {
            Pt p = pts[i] + ( pts[i + 1] - pts[i] ) * ( k / 4.0 );
            sum += strokeDistance( raw, p );
        }
    }

    return sum;
}

/// Corner of the two-leg 45 degree path from a to b that best follows the stroke.
Pt postureCorner( const Pt& a, const Pt& b, const Path& raw )
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double ax = std::fabs( dx ), ay = std::fabs( dy );
    double sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
    double diag = std::min( ax, ay );
    Pt     straightFirst = ax > ay ? Pt( a.x + sx * ( ax - ay ), a.y ) : Pt( a.x, a.y + sy * ( ay - ax ) );
    Pt     diagonalFirst( a.x + sx * diag, a.y + sy * diag );
    return deviation( raw, { a, straightFirst, b } ) <= deviation( raw, { a, diagonalFirst, b } ) ? straightFirst
                                                                                                    : diagonalFirst;
}

bool intersectLines( const Pt& p1, const Pt& u1, const Pt& p2, const Pt& u2, Pt& out )
{
    double den = cross2( u1, u2 );

    if( std::fabs( den ) < 1e-9 )
        return false;

    double t = cross2( p2 - p1, u2 ) / den;
    out = p1 + u1 * t;
    return true;
}

void dedupe( Path& pts, double eps )
{
    Path out;

    for( const Pt& p : pts )
    {
        if( out.empty() || len( p - out.back() ) > eps )
            out.push_back( p );
    }

    if( out.size() > 1 && pts.size() > 1 && len( pts.back() - out.back() ) > 0 )
        out.back() = pts.back(); // keep the exact end

    pts.swap( out );
}

/// Remove zero-length legs, reversals and collinear joints.
void tidy( Path& w )
{
    bool changed = true;

    while( changed )
    {
        changed = false;
        dedupe( w, 1e-6 );

        for( size_t i = 1; i + 1 < w.size(); ++i )
        {
            Pt d1 = w[i] - w[i - 1], d2 = w[i + 1] - w[i];

            if( std::fabs( cross2( unit( d1 ), unit( d2 ) ) ) < 1e-9 )
            {
                // collinear (same direction) or a reversal: drop the joint
                w.erase( w.begin() + (long) i );
                changed = true;
                break;
            }
        }
    }
}

/// Make every leg horizontal, vertical or diagonal.
void octilinearize( Path& w, const Path& raw )
{
    Path out;

    for( size_t i = 0; i < w.size(); ++i )
    {
        if( i > 0 && octant( w[i] - out.back() ) < 0 )
            out.push_back( postureCorner( out.back(), w[i], raw ) );

        out.push_back( w[i] );
    }

    w.swap( out );
}

/// Replace 90 and 135 degree corners with 45 degree steps of about aSize.
void chamfer( Path& w, double aSize )
{
    if( w.size() < 3 )
        return;

    Path out { w[0] };

    for( size_t i = 1; i + 1 < w.size(); ++i )
    {
        Pt  din = w[i] - w[i - 1], dout = w[i + 1] - w[i];
        int qi = octant( din ), qo = octant( dout );

        if( qi < 0 || qo < 0 )
        {
            out.push_back( w[i] );
            continue;
        }

        int turn = ( ( qo - qi ) % 8 + 8 ) % 8; // 1..7 (left turns 1-3, right 5-7)
        int s = turn <= 4 ? 1 : -1;
        int k = turn <= 4 ? turn : 8 - turn;

        if( k < 2 || k > 3 )
        {
            out.push_back( w[i] );
            continue;
        }

        // the room available on both legs (half of each, the other half belongs to the
        // neighbouring corners)
        double room = std::min( len( din ), len( dout ) ) / 2;
        double c = aSize;

        for( int attempt = 0; attempt < 8; ++attempt, c *= 0.5 )
        {
            Pt D;

            for( int j = 1; j < k; ++j )
                D = D + dirOf( qi + s * j ) * c;

            // D = t * u_in + t' * u_out
            Pt     ui = dirOf( qi ), uo = dirOf( qo );
            double den = cross2( ui, uo );
            double t = cross2( D, uo ) / den;
            double t2 = cross2( ui, D ) / den;

            if( t <= room && t2 <= room && t > 0 && t2 > 0 )
            {
                Pt p = w[i] - ui * t;
                out.push_back( p );

                for( int j = 1; j < k; ++j )
                {
                    p = p + dirOf( qi + s * j ) * c;
                    out.push_back( p );
                }

                break;
            }

            if( attempt == 7 )
                out.push_back( w[i] );
        }
    }

    out.push_back( w.back() );
    w.swap( out );
}

Pt snapNm( const Pt& p )
{
    return Pt( std::round( p.x * 1e6 ) / 1e6, std::round( p.y * 1e6 ) / 1e6 );
}

int kindRank( ItemType t )
{
    switch( t )
    {
    case ItemType::PAD: return 0;
    case ItemType::VIA: return 1;
    default: return 2;
    }
}

Pt padCentre( const Board& b, const Pad& p, int layer )
{
    return p.pos + RotatePt( PadGeomOnLayer( b, p, layer ).offset, p.angle );
}

double anchorRadius( const Board& b, const Anchor& a )
{
    switch( a.type )
    {
    case ItemType::PAD:
    {
        const PadGeom& g = PadGeomOnLayer( b, b.pads[(size_t) a.index], a.layer );
        return std::min( std::fabs( g.size.x ), std::fabs( g.size.y ) ) / 2;
    }
    case ItemType::VIA: return b.vias[(size_t) a.index].size / 2;
    case ItemType::TRACK: return b.tracks[(size_t) a.index].width / 2;
    case ItemType::ARC: return b.arcs[(size_t) a.index].width / 2;
    default: return 0;
    }
}

/// Copper under / near p: on aLayer first; on other layers only track ends and vias (a via
/// is added there). aOtherSidePad reports a pad found only on another layer.
Anchor findAnchor( const Board& b, const CopperIndex& idx, const Pt& p, int aLayer, double reach,
                   bool& aOtherSidePad )
{
    Anchor           best;
    double           bestD = 1e300;
    int              bestRank = 99;
    std::vector<int> ids;
    BBox             box;
    box.add( p, reach );

    Prim probe;
    probe.kind = Prim::POINT;
    probe.a = p;
    probe.Finish();

    auto consider = [&]( int layer, bool otherLayer )
    {
        idx.Query( layer, box, ids );

        for( int id : ids )
        {
            const CuShape& s = idx.Shapes()[(size_t) id];

            if( s.type != ItemType::PAD && s.type != ItemType::VIA && s.type != ItemType::TRACK
                && s.type != ItemType::ARC )
                continue;

            if( otherLayer && s.type == ItemType::PAD )
            {
                double d = 1e300;

                for( const Prim& q : s.prims )
                    d = std::min( d, PrimDistance( probe, q ) );

                if( d <= 0 )
                    aOtherSidePad = true;

                continue;
            }

            double d = 1e300;

            for( const Prim& q : s.prims )
                d = std::min( d, PrimDistance( probe, q ) );

            if( d > reach )
                continue;

            int  rank = kindRank( s.type );
            bool better = d < bestD - 0.02 || ( std::fabs( d - bestD ) <= 0.02 && rank < bestRank );

            if( !better )
                continue;

            bestD = d;
            bestRank = rank;
            best = Anchor();
            best.type = s.type;
            best.index = s.index;
            best.net = s.net;
            best.layer = layer;
            best.needsVia = otherLayer && s.type != ItemType::VIA;
        }
    };

    consider( aLayer, false );

    if( best.type == ItemType::NONE )
    {
        for( int l : b.copperStack )
        {
            if( l != aLayer )
                consider( l, true );
        }
    }

    switch( best.type )
    {
    case ItemType::PAD:
        best.kind = AnchorKind::PAD;
        best.pos = padCentre( b, b.pads[(size_t) best.index], best.layer );
        break;

    case ItemType::VIA:
        best.kind = AnchorKind::VIA;
        best.pos = b.vias[(size_t) best.index].pos;
        best.layer = aLayer;
        break;

    case ItemType::TRACK:
    case ItemType::ARC:
    {
        Pt     s0, s1;
        double w;
        Path   line;

        if( best.type == ItemType::TRACK )
        {
            const Track& t = b.tracks[(size_t) best.index];
            s0 = t.start;
            s1 = t.end;
            w = t.width;
            line = { s0, s1 };
        }
        else
        {
            const ArcTrack& a = b.arcs[(size_t) best.index];
            s0 = a.start;
            s1 = a.end;
            w = a.width;
            ArcToPoints( line, a.start, a.mid, a.end, 0.002 );
        }

        double endReach = std::max( reach, w / 2 ) + w / 2;

        if( len( p - s0 ) <= endReach || len( p - s1 ) <= endReach )
        {
            best.kind = AnchorKind::TRACK_END;
            best.pos = len( p - s0 ) <= len( p - s1 ) ? s0 : s1;
        }
        else
        {
            best.kind = AnchorKind::TRACK_MID;
            double bd = 1e300;

            for( size_t i = 0; i + 1 < line.size(); ++i )
            {
                Pt a = line[i], c = line[i + 1], d = c - a;
                double l2 = dot( d, d );
                double t = l2 > 0 ? std::clamp( dot( p - a, d ) / l2, 0.0, 1.0 ) : 0.0;
                Pt q = a + d * t;

                if( len( q - p ) < bd )
                {
                    bd = len( q - p );
                    best.pos = snapNm( q );
                }
            }
        }

        break;
    }

    default:
        break;
    }

    return best;
}

struct Run
{
    int    q;
    size_t i0, i1; ///< raw point span
    double c = 0;  ///< line offset along the normal
};

Pt normalOf( int q )
{
    Pt u = dirOf( q );
    return Pt( -u.y, u.x );
}

double chord( const Path& raw, const Run& r )
{
    return len( raw[r.i1] - raw[r.i0] );
}


/// Intersection of the lines through segments a0-a1 and b0-b1.
bool lineX( const Pt& a0, const Pt& a1, const Pt& b0, const Pt& b1, Pt& out )
{
    return intersectLines( a0, unit( a1 - a0 ), b0, unit( b1 - b0 ), out );
}

/// Remove small steps between two parallel legs ("jogs") by merging their lines.
void removeJogs( Path& w, double aMax, bool aFixedStart, bool aFixedEnd )
{
    bool changed = true;

    for( int guard = 0; changed && guard < 64; ++guard )
    {
        changed = false;

        for( size_t i = 1; i + 2 < w.size() && !changed; ++i )
        {
            Pt  l0 = w[i] - w[i - 1], l1 = w[i + 1] - w[i], l2 = w[i + 2] - w[i + 1];
            int q0 = octant( l0 ), q2 = octant( l2 );

            if( q0 < 0 || q0 != q2 || len( l1 ) > aMax )
                continue;

            double offset = std::fabs( cross2( dirOf( q0 ), l1 ) );

            if( offset > aMax )
                continue;

            bool hasBefore = i >= 2, hasAfter = i + 3 < w.size();
            bool keepFirst = len( l0 ) >= len( l2 );

            if( i == 1 && aFixedStart )
                keepFirst = true;

            if( i + 2 == w.size() - 1 && aFixedEnd )
                keepFirst = false;

            Pt x;

            if( keepFirst && hasAfter && lineX( w[i - 1], w[i], w[i + 2], w[i + 3], x ) )
            {
                // first leg's line continues until it meets the leg after the jog
                w[i] = x;
                w.erase( w.begin() + (long) i + 1, w.begin() + (long) i + 3 );
                changed = true;
            }
            else if( !keepFirst && hasBefore && lineX( w[i + 1], w[i + 2], w[i - 2], w[i - 1], x ) )
            {
                w[i - 1] = x;
                w.erase( w.begin() + (long) i, w.begin() + (long) i + 2 );
                changed = true;
            }
            else if( !hasBefore && !hasAfter )
            {
                // a lone step between two fixed ends: one 45 degree bend instead of two
                Pt a = w.front(), b = w.back();
                w = { a, b };
                changed = true;
            }
        }
    }
}

/// Largest distance from any point of one polyline to the other (both directions).
double hausdorff( const Path& raw, const Path& w )
{
    double worst = 0;

    for( const Pt& p : raw )
    {
        double d = 1e300;

        for( size_t i = 0; i + 1 < w.size(); ++i )
            d = std::min( d, PointSegmentDistance( p, w[i], w[i + 1] ) );

        worst = std::max( worst, d );
    }

    for( size_t i = 0; i + 1 < w.size(); ++i )
    {
        for( int k = 0; k <= 4; ++k )
            worst = std::max( worst, strokeDistance( raw, w[i] + ( w[i + 1] - w[i] ) * ( k / 4.0 ) ) );
    }

    return worst;
}

double meanDeviation( const Path& raw, const Path& w )
{
    double sum = 0;
    size_t n = 0;

    for( const Pt& p : raw )
    {
        double d = 1e300;

        for( size_t i = 0; i + 1 < w.size(); ++i )
            d = std::min( d, PointSegmentDistance( p, w[i], w[i + 1] ) );

        sum += d;
        ++n;
    }

    return n ? sum / (double) n : 0.0;
}

} // anonymous namespace


std::vector<size_t> SimplifyPath( const Path& pts, double tol )
{
    std::vector<size_t> keep;

    if( pts.empty() )
        return keep;

    std::vector<char>                      mark( pts.size(), 0 );
    std::vector<std::pair<size_t, size_t>> stack { { 0, pts.size() - 1 } };
    mark[0] = mark[pts.size() - 1] = 1;

    while( !stack.empty() )
    {
        auto [a, b] = stack.back();
        stack.pop_back();

        double worst = -1;
        size_t at = a;

        for( size_t i = a + 1; i < b; ++i )
        {
            double d = PointSegmentDistance( pts[i], pts[a], pts[b] );

            if( d > worst )
            {
                worst = d;
                at = i;
            }
        }

        if( worst > tol )
        {
            mark[at] = 1;
            stack.push_back( { a, at } );
            stack.push_back( { at, b } );
        }
    }

    for( size_t i = 0; i < pts.size(); ++i )
    {
        if( mark[i] )
            keep.push_back( i );
    }

    return keep;
}


RouteResult RoutePencil( const Board& b, const CopperIndex& idx, const RouteRequest& req )
{
    RouteResult R;

    if( req.layer < 0 || req.layer >= (int) b.layers.size() || b.layers[(size_t) req.layer].kind != LayerKind::COPPER )
    {
        R.message = Tr( "Choose a copper layer to draw tracks on" );
        return R;
    }

    Path raw = req.stroke;
    dedupe( raw, 1e-4 );
    double strokeLen = 0;

    for( size_t i = 0; i + 1 < raw.size(); ++i )
        strokeLen += len( raw[i + 1] - raw[i] );

    if( raw.size() < 2 || strokeLen < std::max( req.width, 0.05 ) )
    {
        R.message = Tr( "The stroke is too short" );
        return R;
    }

    double tol = std::clamp( req.tolerance, 0.02, 5.0 );
    double reach = std::max( req.snapRadius, 0.0 );
    bool   otherSideStart = false, otherSideEnd = false;

    R.start = findAnchor( b, idx, raw.front(), req.layer, reach, otherSideStart );
    R.end = findAnchor( b, idx, raw.back(), req.layer, reach, otherSideEnd );

    // Copper on another layer only counts as an end (joined with a via) when it does not clash
    // with the net at the other end: a stroke that merely ends above someone else's track on
    // the far side of the board is not meant to connect to it.
    auto clash = []( const Anchor& a, const Anchor& o ) { return a.net > 0 && o.net > 0 && a.net != o.net; };

    if( R.end.needsVia && clash( R.end, R.start ) )
    {
        R.end = Anchor();
    }
    else if( R.start.needsVia && clash( R.start, R.end ) )
    {
        R.start = Anchor();
    }

    if( R.start.type != ItemType::NONE && R.start.type == R.end.type && R.start.index == R.end.index
        && R.start.kind != AnchorKind::TRACK_MID && len( R.start.pos - R.end.pos ) < 1e-6 )
    {
        R.message = Tr( "The stroke starts and ends on the same spot" );
        return R;
    }

    if( R.start.net > 0 && R.end.net > 0 && R.start.net != R.end.net )
    {
        R.blocked = true;
        R.message = TrF( "This would connect {0} and {1}",
                         { b.nets[(size_t) R.start.net], b.nets[(size_t) R.end.net] } );
    }

    R.net = R.start.net > 0 ? R.start.net : R.end.net;

    // other nets' copper touched (or closer than the clearance) by a candidate path
    double clearance = std::max( 0.0, req.clearance );
    auto   countConflicts = [&]( const Path& p, int& aShorts, int& aClose )
    {
        std::vector<int> found;

        for( size_t i = 0; i + 1 < p.size(); ++i )
        {
            Prim seg;
            seg.kind = Prim::SEGMENT;
            seg.a = p[i];
            seg.b = p[i + 1];
            seg.r = req.width / 2;
            seg.Finish();
            BBox box = seg.box;
            box.minX -= clearance;
            box.minY -= clearance;
            box.maxX += clearance;
            box.maxY += clearance;
            idx.Query( req.layer, box, found );

            for( int id : found )
            {
                const CuShape& o = idx.Shapes()[(size_t) id];

                if( ( o.type == R.start.type && o.index == R.start.index )
                    || ( o.type == R.end.type && o.index == R.end.index ) || ( R.net > 0 && o.net == R.net ) )
                    continue;

                double d = 1e300;

                for( const Prim& q : o.prims )
                    d = std::min( d, PrimDistance( seg, q ) );

                if( d <= 0 )
                    ++aShorts;
                else if( d < clearance )
                    ++aClose;
            }
        }
    };

    bool anchoredS = R.start.kind != AnchorKind::NONE;
    bool anchoredE = R.end.kind != AnchorKind::NONE;
    Pt   S = anchoredS ? R.start.pos : raw.front();
    Pt   E = anchoredE ? R.end.pos : raw.back();

    // ---- simplify and split into runs of one 45 degree direction
    std::vector<size_t> keep = SimplifyPath( raw, tol );
    std::vector<Run>    runs;

    for( size_t k = 0; k + 1 < keep.size(); ++k )
    {
        int q = quantize( raw[keep[k + 1]] - raw[keep[k]] );

        if( !runs.empty() && runs.back().q == q )
            runs.back().i1 = keep[k + 1];
        else
            runs.push_back( { q, keep[k], keep[k + 1] } );
    }

    // absorb wobbles: reversals and short runs
    auto removeRun = [&]( size_t k )
    {
        if( k > 0 )
            runs[k - 1].i1 = runs[k].i1;
        else
            runs[k + 1].i0 = runs[k].i0;

        runs.erase( runs.begin() + (long) k );

        for( size_t j = 0; j + 1 < runs.size(); ++j )
        {
            if( runs[j].q == runs[j + 1].q )
            {
                runs[j].i1 = runs[j + 1].i1;
                runs.erase( runs.begin() + (long) j + 1 );
                --j;
            }
        }
    };

    bool changed = true;

    while( changed && runs.size() > 1 )
    {
        changed = false;

        for( size_t k = 0; k + 1 < runs.size() && !changed; ++k )
        {
            if( ( ( runs[k].q - runs[k + 1].q + 8 ) % 8 ) == 4 )
            {
                removeRun( chord( raw, runs[k] ) <= chord( raw, runs[k + 1] ) ? k : k + 1 );
                changed = true;
            }
        }

        for( size_t k = 0; k < runs.size() && !changed && runs.size() > 1; ++k )
        {
            if( chord( raw, runs[k] ) < tol * 1.5 )
            {
                removeRun( k );
                changed = true;
            }
        }
    }

    // fit each run's line to the raw points
    for( Run& r : runs )
    {
        Pt     n = normalOf( r.q );
        double sum = 0;

        for( size_t i = r.i0; i <= r.i1; ++i )
            sum += dot( raw[i], n );

        r.c = sum / (double) ( r.i1 - r.i0 + 1 );
    }

    auto onLine = [&]( const Run& r, const Pt& p ) { return p - normalOf( r.q ) * ( dot( p, normalOf( r.q ) ) - r.c ); };

    // Build the clean path from the fitted runs. The end runs may be moved to pass through the
    // anchors (shift), otherwise a 45 degree lead joins the anchor to its run.
    auto buildFromRuns = [&]( bool aShiftStart, bool aShiftEnd ) -> Path
    {
        std::vector<Run> rr = runs;
        auto             shift = [&]( Run& r, const Pt& p, const Anchor& a )
        {
            double allowance = std::max( tol, anchorRadius( b, a ) ) * 2;
            double off = dot( p, normalOf( r.q ) ) - r.c;

            if( std::fabs( off ) > allowance )
                return false;

            r.c += off;
            return true;
        };

        bool shiftedS = anchoredS && aShiftStart && shift( rr.front(), S, R.start );
        bool shiftedE = false;

        if( anchoredE && aShiftEnd && !( rr.size() == 1 && shiftedS ) )
            shiftedE = shift( rr.back(), E, R.end );

        Path w;

        if( !anchoredS )
        {
            w.push_back( onLine( rr.front(), S ) );
        }
        else
        {
            w.push_back( S );

            if( !shiftedS )
            {
                Pt     u = dirOf( rr.front().q ), n = normalOf( rr.front().q );
                double off = dot( S, n ) - rr.front().c;
                w.push_back( S + u * std::fabs( off ) - n * off );
            }
        }

        for( size_t k = 0; k + 1 < rr.size(); ++k )
        {
            const Run& r1 = rr[k];
            const Run& r2 = rr[k + 1];
            Pt         J = raw[r1.i1];
            Pt         P;
            Pt         p1 = normalOf( r1.q ) * r1.c, p2 = normalOf( r2.q ) * r2.c;

            if( intersectLines( p1, dirOf( r1.q ), p2, dirOf( r2.q ), P )
                && ( len( P - J ) <= 4 * tol || strokeDistance( raw, P ) <= 2 * tol ) )
            {
                w.push_back( P );
            }
            else
            {
                w.push_back( onLine( r1, J ) );
                w.push_back( onLine( r2, J ) );
            }
        }

        if( !anchoredE )
        {
            w.push_back( onLine( rr.back(), E ) );
        }
        else
        {
            if( !shiftedE )
            {
                Pt     u = dirOf( rr.back().q ), n = normalOf( rr.back().q );
                double off = dot( E, n ) - rr.back().c;
                w.push_back( E - u * std::fabs( off ) - n * off );
            }

            w.push_back( E );
        }

        return w;
    };

    auto finish = [&]( Path w ) -> Path
    {
        for( Pt& p : w )
            p = snapNm( p );

        if( anchoredS )
            w.front() = S;

        if( anchoredE )
            w.back() = E;

        tidy( w );
        octilinearize( w, raw );
        tidy( w );
        removeJogs( w, std::max( 3 * tol, 3 * req.width ), anchoredS, anchoredE );
        tidy( w );
        octilinearize( w, raw );
        tidy( w );
        return w;
    };

    std::vector<Path> cands;

    for( int v = 0; v < 4; ++v )
        cands.push_back( finish( buildFromRuns( v & 1, v & 2 ) ) );

    {
        // the two single-bend postures between the ends
        Path direct { S, E };

        double dx = E.x - S.x, dy = E.y - S.y;
        double ax = std::fabs( dx ), ay = std::fabs( dy );
        double sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
        double diag = std::min( ax, ay );
        Pt     straightFirst = ax > ay ? Pt( S.x + sx * ( ax - ay ), S.y ) : Pt( S.x, S.y + sy * ( ay - ax ) );
        Pt     diagonalFirst( S.x + sx * diag, S.y + sy * diag );
        cands.push_back( finish( { S, straightFirst, E } ) );
        cands.push_back( finish( { S, diagonalFirst, E } ) );
    }

    // Prefer the path that follows the stroke with the fewest bends, and steer clear of other
    // nets' copper when a nearly as faithful path does.
    double bestScore = 1e300;
    Path   w;
    double minH = 1e300;

    std::vector<double> H( cands.size() ), score( cands.size() );

    for( size_t c = 0; c < cands.size(); ++c )
    {
        H[c] = cands[c].size() >= 2 ? hausdorff( raw, cands[c] ) : 1e300;
        minH = std::min( minH, H[c] );
    }

    for( size_t c = 0; c < cands.size(); ++c )
    {
        const Path& p = cands[c];

        if( p.size() < 2 )
            continue;

        int    bends = (int) p.size() - 2;
        double sc = meanDeviation( raw, p ) + 0.5 * H[c] + 0.3 * bends;

        // copper of other nets along this path
        int shortsHit = 0, closeHit = 0;
        countConflicts( p, shortsHit, closeHit );

        if( H[c] <= std::max( minH * 1.5 + 0.5, 1.0 ) )
            sc += 50.0 * shortsHit + 2.0 * closeHit;
        else
            sc += 1000.0; // too far from what was drawn to win on avoiding copper

        score[c] = sc;

        if( sc < bestScore )
        {
            bestScore = sc;
            w = p;
        }
    }

    chamfer( w, std::clamp( req.width * 2.5, 0.25, 1.5 ) );
    tidy( w );

    for( size_t i = 1; i + 1 < w.size(); ++i )
        w[i] = snapNm( w[i] );

    if( w.size() < 2 )
    {
        R.message = Tr( "The stroke is too short" );
        return R;
    }

    for( size_t i = 0; i + 1 < w.size(); ++i )
    {
        NewTrack t;
        t.start = w[i];
        t.end = w[i + 1];
        t.width = req.width;
        t.layer = req.layer;
        t.net = R.net;
        R.tracks.push_back( t );
    }

    auto addVia = [&]( const Pt& at )
    {
        NewVia v;
        v.pos = at;
        v.size = req.viaSize;
        v.drill = req.viaDrill;
        v.net = R.net;
        R.vias.push_back( v );
    };

    if( R.start.needsVia )
        addVia( w.front() );

    if( R.end.needsVia )
        addVia( w.back() );

    if( !anchoredS && otherSideStart && R.message.empty() )
        R.message = Tr( "That pad is on the other side of the board; switch the layer to reach it" );

    if( !anchoredE && otherSideEnd && R.message.empty() )
        R.message = Tr( "That pad is on the other side of the board; switch the layer to reach it" );

    // ---- conflicts with other nets' copper
    auto isAnchor = [&]( const CuShape& s )
    {
        return ( s.type == R.start.type && s.index == R.start.index )
               || ( s.type == R.end.type && s.index == R.end.index );
    };

    std::vector<CuShape> mine;

    for( const NewTrack& t : R.tracks )
    {
        CuShape s;
        s.type = ItemType::TRACK;
        s.layer = t.layer;
        s.net = R.net;
        Prim p;
        p.kind = Prim::SEGMENT;
        p.a = t.start;
        p.b = t.end;
        p.r = t.width / 2;
        p.Finish();
        s.prims.push_back( p );
        s.Finish();
        mine.push_back( s );
    }

    for( const NewVia& v : R.vias )
    {
        for( int l : b.copperStack )
        {
            CuShape s;
            s.type = ItemType::VIA;
            s.layer = l;
            s.net = R.net;
            Prim p;
            p.kind = Prim::POINT;
            p.a = v.pos;
            p.r = v.size / 2;
            p.Finish();
            s.prims.push_back( p );
            s.Finish();
            mine.push_back( s );
        }
    }

    std::vector<int> ids;
    double           cl = std::max( 0.0, req.clearance );

    for( const CuShape& m : mine )
    {
        BBox box = m.box;
        box.minX -= cl;
        box.minY -= cl;
        box.maxX += cl;
        box.maxY += cl;
        idx.Query( m.layer, box, ids );

        for( int id : ids )
        {
            const CuShape& s = idx.Shapes()[(size_t) id];

            if( isAnchor( s ) || ( R.net > 0 && s.net == R.net ) )
                continue;

            double d = ShapeDistance( m, s );

            if( d >= cl )
                continue;

            RouteConflict c;
            c.net = s.net;
            c.shorts = d <= 0;

            // marker: closest primitive pair
            double bd = 1e300;

            for( const Prim& p : m.prims )
            {
                for( const Prim& q : s.prims )
                {
                    double dd = PrimDistance( p, q );

                    if( dd < bd )
                    {
                        bd = dd;
                        c.pos = PrimNearPoint( p, q );
                    }
                }
            }

            R.shorts |= c.shorts;
            R.tooClose |= !c.shorts;

            bool dup = false;

            for( const RouteConflict& o : R.conflicts )
                dup |= o.net == c.net && len( o.pos - c.pos ) < 0.5;

            if( !dup && R.conflicts.size() < 64 )
                R.conflicts.push_back( c );
        }
    }

    if( R.shorts && !R.blocked )
    {
        int other = 0;

        for( const RouteConflict& c : R.conflicts )
        {
            if( c.shorts )
            {
                other = c.net;
                break;
            }
        }

        R.blocked = true;
        R.message = other > 0 ? TrF( "This track would touch {0}", { b.nets[(size_t) other] } )
                              : std::string( Tr( "This track would touch other copper" ) );
    }

    R.ok = true;
    return R;
}

} // namespace kp
