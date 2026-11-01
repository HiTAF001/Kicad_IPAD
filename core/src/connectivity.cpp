/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "connectivity.h"
#include "pick.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace kp
{

namespace
{

enum CType : uint8_t
{
    C_PAD,
    C_VIA,
    C_TRACK,
    C_ARC,
    C_ZONE
};

/// Row-bucketed edge index for fast point-in-polygon tests on big zone fills.
struct PolyIndex
{
    const Path*                        poly = nullptr;
    BBox                               bb;
    double                             rowH = 1.0;
    int                                rows = 1;
    std::vector<std::vector<uint32_t>> rowEdges;

    void build( const Path& p )
    {
        poly = &p;

        for( const Pt& q : p )
            bb.add( q );

        size_t n = p.size();
        rows = (int) std::clamp<size_t>( n / 8, 1, 4096 );
        rowH = std::max( ( bb.maxY - bb.minY ) / rows, 1e-6 );
        rowEdges.assign( (size_t) rows, {} );

        for( size_t i = 0; i < n; ++i )
        {
            const Pt& a = p[i];
            const Pt& b = p[( i + 1 ) % n];
            int       r0 = rowOf( std::min( a.y, b.y ) );
            int       r1 = rowOf( std::max( a.y, b.y ) );

            for( int r = r0; r <= r1; ++r )
                rowEdges[(size_t) r].push_back( (uint32_t) i );
        }
    }

    int rowOf( double y ) const
    {
        return std::clamp( (int) std::floor( ( y - bb.minY ) / rowH ), 0, rows - 1 );
    }

    bool contains( const Pt& q ) const
    {
        if( !bb.valid() || q.x < bb.minX || q.x > bb.maxX || q.y < bb.minY || q.y > bb.maxY )
            return false;

        const Path& p = *poly;
        size_t      n = p.size();
        bool        inside = false;

        for( uint32_t i : rowEdges[(size_t) rowOf( q.y )] )
        {
            const Pt& a = p[i];
            const Pt& b = p[( i + 1 ) % n];

            if( ( ( a.y > q.y ) != ( b.y > q.y ) ) && ( q.x < ( b.x - a.x ) * ( q.y - a.y ) / ( b.y - a.y ) + a.x ) )
                inside = !inside;
        }

        return inside;
    }

    /// Calls f(a, b) for each edge whose bounding box meets aBox, until f returns true.
    template <typename F>
    bool anyEdge( const BBox& aBox, F&& f ) const
    {
        if( !bb.valid() || aBox.maxX < bb.minX || aBox.minX > bb.maxX || aBox.maxY < bb.minY
            || aBox.minY > bb.maxY )
        {
            return false;
        }

        const Path& p = *poly;
        size_t      n = p.size();
        int         r0 = rowOf( aBox.minY ), r1 = rowOf( aBox.maxY );

        for( int r = r0; r <= r1; ++r )
        {
            for( uint32_t i : rowEdges[(size_t) r] )
            {
                const Pt& a = p[i];
                const Pt& b = p[( i + 1 ) % n];

                // visit each edge once: in the first row of the range that holds it
                if( std::max( rowOf( std::min( a.y, b.y ) ), r0 ) != r )
                    continue;

                if( std::max( a.x, b.x ) < aBox.minX || std::min( a.x, b.x ) > aBox.maxX
                    || std::max( a.y, b.y ) < aBox.minY || std::min( a.y, b.y ) > aBox.maxY )
                {
                    continue;
                }

                if( f( a, b ) )
                    return true;
            }
        }

        return false;
    }
};

struct CItem
{
    CType    type;
    int      index;   ///< pad / via / track / arc / zone index
    uint64_t layers;  ///< copper layers as bits of copperOrder
    BBox     bb;
    Path     pts;     ///< arc polyline
    std::shared_ptr<PolyIndex> zone;
};

double segSegDist( const Pt& a1, const Pt& a2, const Pt& b1, const Pt& b2 )
{
    auto cross = []( const Pt& o, const Pt& a, const Pt& b )
    { return ( a.x - o.x ) * ( b.y - o.y ) - ( a.y - o.y ) * ( b.x - o.x ); };

    double d1 = cross( b1, b2, a1 ), d2 = cross( b1, b2, a2 );
    double d3 = cross( a1, a2, b1 ), d4 = cross( a1, a2, b2 );

    if( ( ( d1 > 0 && d2 < 0 ) || ( d1 < 0 && d2 > 0 ) ) && ( ( d3 > 0 && d4 < 0 ) || ( d3 < 0 && d4 > 0 ) ) )
        return 0.0;

    return std::min( { PointSegmentDistance( a1, b1, b2 ), PointSegmentDistance( a2, b1, b2 ),
                       PointSegmentDistance( b1, a1, a2 ), PointSegmentDistance( b2, a1, a2 ) } );
}

double polylineDist( const Path& p, const Pt& a, const Pt& b )
{
    double d = 1e300;

    for( size_t i = 0; i + 1 < p.size(); ++i )
        d = std::min( d, segSegDist( p[i], p[i + 1], a, b ) );

    return d;
}

double polylinePointDist( const Path& p, const Pt& q )
{
    double d = 1e300;

    for( size_t i = 0; i + 1 < p.size(); ++i )
        d = std::min( d, PointSegmentDistance( q, p[i], p[i + 1] ) );

    return d;
}

Pt closestOnSeg( const Pt& p, const Pt& a, const Pt& b )
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? std::clamp( ( ( p.x - a.x ) * dx + ( p.y - a.y ) * dy ) / l2, 0.0, 1.0 ) : 0.0;
    return Pt( a.x + t * dx, a.y + t * dy );
}

Pt padCenter( const Pad& p )
{
    return p.pos + RotatePt( p.geom.offset, p.angle );
}

/// Centre plus points just inside the pad outline.
void padSamples( const Pad& p, std::vector<Pt>& out )
{
    out.clear();
    Pt     c = padCenter( p );
    double hx = std::fabs( p.geom.size.x ) / 2 * 0.92, hy = std::fabs( p.geom.size.y ) / 2 * 0.92;
    bool   round = p.geom.shape == PadShape::CIRCLE;
    out.push_back( c );

    for( int k = 0; k < 16; ++k )
    {
        double a = k * 3.14159265358979323846 / 8;
        double dx = std::cos( a ), dy = std::sin( a );
        Pt     q;

        if( round )
        {
            q = Pt( dx * hx, dy * hx );
        }
        else
        {
            double s = std::min( std::fabs( dx ) > 1e-9 ? hx / std::fabs( dx ) : 1e300,
                                 std::fabs( dy ) > 1e-9 ? hy / std::fabs( dy ) : 1e300 );
            q = Pt( dx * s, dy * s );
        }

        out.push_back( c + RotatePt( q, p.angle ) );
    }
}

/// Points on the pad outline (corners, edge points), for pad-to-pad contact tests.
void padBoundary( const Pad& p, std::vector<Pt>& out )
{
    out.clear();
    const PadGeom& g = p.geom;
    Pt             c = padCenter( p );
    double         sx = std::fabs( g.size.x ), sy = std::fabs( g.size.y );
    out.push_back( c );

    auto put = [&]( double x, double y ) { out.push_back( c + RotatePt( Pt( x, y ), p.angle ) ); };

    bool rectLike = g.shape == PadShape::RECT || g.shape == PadShape::ROUNDRECT
                    || g.shape == PadShape::CHAMFERED_RECT || g.shape == PadShape::TRAPEZOID
                    || ( g.shape == PadShape::CUSTOM && g.anchor == PadShape::RECT );

    if( rectLike )
    {
        double hx = sx / 2, hy = sy / 2;
        double cut = 0.0;

        if( g.shape == PadShape::ROUNDRECT )
            cut = std::min( sx, sy ) * g.rratio * ( 1 - std::sqrt( 0.5 ) );
        else if( g.shape == PadShape::CHAMFERED_RECT && g.chamferCorners )
            cut = std::min( sx, sy ) * g.chamferRatio / 2;

        for( int k = 0; k <= 4; ++k )
        {
            double t = -1.0 + k * 0.5;
            put( t * hx, -hy );
            put( t * hx, hy );
            put( -hx, t * hy );
            put( hx, t * hy );
        }

        if( cut > 0 )
        {
            // replace the exact corners by points on the rounded / chamfered corner
            for( size_t i = 1; i < out.size(); ++i )
            {
                Pt q = RotatePt( out[i] - c, -p.angle );

                if( std::fabs( std::fabs( q.x ) - hx ) < 1e-9 && std::fabs( std::fabs( q.y ) - hy ) < 1e-9 )
                {
                    q.x -= std::copysign( cut, q.x );
                    q.y -= std::copysign( cut, q.y );
                    out[i] = c + RotatePt( q, p.angle );
                }
            }
        }

        return;
    }

    double r = std::min( sx, sy ) / 2;
    double ax = g.shape == PadShape::OVAL && sx > sy ? ( sx - sy ) / 2 : 0.0;
    double ay = g.shape == PadShape::OVAL && sy > sx ? ( sy - sx ) / 2 : 0.0;

    for( int k = 0; k < 16; ++k )
    {
        double a = k * 3.14159265358979323846 / 8;
        double x = std::cos( a ) * r, y = std::sin( a ) * r;
        put( x + std::copysign( ax, x ), y + std::copysign( ay, y ) );
    }

    if( ax > 0 )
    {
        put( 0, -r );
        put( 0, r );
    }
    else if( ay > 0 )
    {
        put( -r, 0 );
        put( r, 0 );
    }
}

class Solver
{
public:
    explicit Solver( const Board& b ) :
            m_b( b )
    {
    }

    void Run( Connectivity& out );

private:
    uint64_t layerBit( int layer ) const
    {
        if( layer < 0 || layer >= (int) m_b.layers.size() )
            return 0;

        const Layer& L = m_b.layers[(size_t) layer];

        if( L.kind != LayerKind::COPPER || L.copperOrder < 0 || L.copperOrder >= 64 )
            return 0;

        return 1ull << L.copperOrder;
    }

    int find( int i )
    {
        while( m_parent[(size_t) i] != i )
        {
            m_parent[(size_t) i] = m_parent[(size_t) m_parent[(size_t) i]];
            i = m_parent[(size_t) i];
        }

        return i;
    }

    void unite( int a, int b )
    {
        a = find( a );
        b = find( b );

        if( a != b )
            m_parent[(size_t) a] = b;
    }

    bool touches( const CItem& a, const CItem& b );
    bool zoneTouches( const CItem& zone, const CItem& other );
    void anchors( const CItem& it, std::vector<Pt>& out ) const;

    const Board&       m_b;
    std::vector<CItem> m_items;
    std::vector<int>   m_parent;
    std::vector<Pt>    m_samples;
};


bool Solver::zoneTouches( const CItem& z, const CItem& o )
{
    // Same idea as KiCad: anchor points inside the fill, else the item's shape colliding with
    // the fill outline (here: fill edges near the item), and for two fills of one net any
    // outline vertex of one inside the other.
    const PolyIndex& pi = *z.zone;

    switch( o.type )
    {
    case C_PAD:
    {
        const Pad& p = m_b.pads[(size_t) o.index];
        Pt         c = padCenter( p );
        padSamples( p, m_samples );

        for( const Pt& q : m_samples )
        {
            if( pi.contains( q ) )
                return true;
        }

        // fill pieces lying inside the pad (thermal spokes end at the drill) or crossing it
        return pi.anyEdge( o.bb,
                           [&]( const Pt& a, const Pt& b )
                           {
                               return PadContains( p, a, 0.001 )
                                      || PadContains( p, closestOnSeg( c, a, b ), 0.001 );
                           } );
    }

    case C_VIA:
    {
        const Via& v = m_b.vias[(size_t) o.index];

        if( pi.contains( v.pos ) )
            return true;

        return pi.anyEdge( o.bb, [&]( const Pt& a, const Pt& b )
                           { return PointSegmentDistance( v.pos, a, b ) <= v.size / 2; } );
    }

    case C_TRACK:
    {
        const Track& t = m_b.tracks[(size_t) o.index];

        if( pi.contains( t.start ) || pi.contains( t.end ) )
            return true;

        return pi.anyEdge( o.bb, [&]( const Pt& a, const Pt& b )
                           { return segSegDist( a, b, t.start, t.end ) <= t.width / 2; } );
    }

    case C_ARC:
    {
        if( pi.contains( m_b.arcs[(size_t) o.index].start ) || pi.contains( m_b.arcs[(size_t) o.index].end ) )
            return true;

        double hw = m_b.arcs[(size_t) o.index].width / 2;

        return pi.anyEdge( o.bb, [&]( const Pt& a, const Pt& b )
                           { return polylineDist( o.pts, a, b ) <= hw; } );
    }

    case C_ZONE:
    {
        const PolyIndex& po = *o.zone;
        BBox             common;
        common.minX = std::max( pi.bb.minX, po.bb.minX );
        common.minY = std::max( pi.bb.minY, po.bb.minY );
        common.maxX = std::min( pi.bb.maxX, po.bb.maxX );
        common.maxY = std::min( pi.bb.maxY, po.bb.maxY );

        if( common.minX > common.maxX || common.minY > common.maxY )
            return false;

        if( po.anyEdge( common, [&]( const Pt& a, const Pt& ) { return pi.contains( a ); } ) )
            return true;

        return pi.anyEdge( common, [&]( const Pt& a, const Pt& ) { return po.contains( a ); } );
    }
    }

    return false;
}


bool Solver::touches( const CItem& a0, const CItem& b0 )
{
    if( !( a0.layers & b0.layers ) )
        return false;

    const CItem* a = &a0;
    const CItem* b = &b0;

    if( a->type > b->type )
        std::swap( a, b );

    if( b->type == C_ZONE )
        return zoneTouches( *b, *a );

    switch( a->type )
    {
    case C_PAD:
    {
        const Pad& p = m_b.pads[(size_t) a->index];

        if( b->type == C_PAD )
        {
            const Pad& q = m_b.pads[(size_t) b->index];
            padBoundary( q, m_samples );

            for( const Pt& s : m_samples )
            {
                if( PadContains( p, s, 0.001 ) )
                    return true;
            }

            padBoundary( p, m_samples );

            for( const Pt& s : m_samples )
            {
                if( PadContains( q, s, 0.001 ) )
                    return true;
            }

            return false;
        }

        if( b->type == C_VIA )
        {
            const Via& v = m_b.vias[(size_t) b->index];
            return PadContains( p, v.pos, v.size / 2 );
        }

        if( b->type == C_TRACK )
        {
            const Track& t = m_b.tracks[(size_t) b->index];
            double       hw = t.width / 2;
            return PadContains( p, t.start, hw ) || PadContains( p, t.end, hw )
                   || PadContains( p, closestOnSeg( padCenter( p ), t.start, t.end ), hw );
        }

        if( b->type == C_ARC )
        {
            const ArcTrack& t = m_b.arcs[(size_t) b->index];

            for( const Pt& q : b->pts )
            {
                if( PadContains( p, q, t.width / 2 ) )
                    return true;
            }
        }

        return false;
    }

    case C_VIA:
    {
        const Via& v = m_b.vias[(size_t) a->index];

        if( b->type == C_VIA )
        {
            const Via& w = m_b.vias[(size_t) b->index];
            return std::hypot( v.pos.x - w.pos.x, v.pos.y - w.pos.y ) <= ( v.size + w.size ) / 2;
        }

        if( b->type == C_TRACK )
        {
            const Track& t = m_b.tracks[(size_t) b->index];
            return PointSegmentDistance( v.pos, t.start, t.end ) <= v.size / 2 + t.width / 2;
        }

        if( b->type == C_ARC )
            return polylinePointDist( b->pts, v.pos ) <= v.size / 2 + m_b.arcs[(size_t) b->index].width / 2;

        return false;
    }

    case C_TRACK:
    {
        const Track& t = m_b.tracks[(size_t) a->index];

        if( b->type == C_TRACK )
        {
            const Track& u = m_b.tracks[(size_t) b->index];
            return segSegDist( t.start, t.end, u.start, u.end ) <= ( t.width + u.width ) / 2;
        }

        if( b->type == C_ARC )
            return polylineDist( b->pts, t.start, t.end ) <= ( t.width + m_b.arcs[(size_t) b->index].width ) / 2;

        return false;
    }

    case C_ARC:
    {
        if( b->type == C_ARC )
        {
            double lim = ( m_b.arcs[(size_t) a->index].width + m_b.arcs[(size_t) b->index].width ) / 2;

            for( size_t i = 0; i + 1 < a->pts.size(); ++i )
            {
                if( polylineDist( b->pts, a->pts[i], a->pts[i + 1] ) <= lim )
                    return true;
            }
        }

        return false;
    }

    default:
        return false;
    }
}


void Solver::anchors( const CItem& it, std::vector<Pt>& out ) const
{
    switch( it.type )
    {
    case C_PAD: out.push_back( padCenter( m_b.pads[(size_t) it.index] ) ); break;
    case C_VIA: out.push_back( m_b.vias[(size_t) it.index].pos ); break;
    case C_TRACK:
        out.push_back( m_b.tracks[(size_t) it.index].start );
        out.push_back( m_b.tracks[(size_t) it.index].end );
        break;
    case C_ARC:
        out.push_back( m_b.arcs[(size_t) it.index].start );
        out.push_back( m_b.arcs[(size_t) it.index].end );
        break;
    case C_ZONE:
    {
        const Path& p = *it.zone->poly;
        size_t      step = std::max<size_t>( 1, p.size() / 24 );

        for( size_t i = 0; i < p.size(); i += step )
            out.push_back( p[i] );

        break;
    }
    }
}


void Solver::Run( Connectivity& out )
{
    out = Connectivity();

    // ---- items
    for( size_t i = 0; i < m_b.pads.size(); ++i )
    {
        const Pad& p = m_b.pads[i];

        if( p.net <= 0 || p.attr == PadAttr::NPTH )
            continue;

        uint64_t layers = 0;

        for( int l : p.layers )
            layers |= layerBit( l );

        if( !layers )
            continue;

        CItem it { C_PAD, (int) i, layers, {}, {}, nullptr };
        it.bb.add( padCenter( p ), std::hypot( p.geom.size.x, p.geom.size.y ) / 2 + 0.01 );
        m_items.push_back( std::move( it ) );
    }

    for( size_t i = 0; i < m_b.vias.size(); ++i )
    {
        const Via& v = m_b.vias[i];

        if( v.net <= 0 )
            continue;

        int oa = 0, ob = (int) m_b.copperStack.size() - 1;

        auto isCopper = [&]( int l )
        { return l >= 0 && m_b.layers[(size_t) l].kind == LayerKind::COPPER; };

        if( isCopper( v.layerA ) && isCopper( v.layerB ) )
        {
            oa = m_b.layers[(size_t) v.layerA].copperOrder;
            ob = m_b.layers[(size_t) v.layerB].copperOrder;

            if( oa > ob )
                std::swap( oa, ob );
        }

        uint64_t layers = 0;

        for( int o = oa; o <= ob && o < 64; ++o )
            layers |= 1ull << o;

        CItem it { C_VIA, (int) i, layers, {}, {}, nullptr };
        it.bb.add( v.pos, v.size / 2 + 0.01 );
        m_items.push_back( std::move( it ) );
    }

    for( size_t i = 0; i < m_b.tracks.size(); ++i )
    {
        const Track& t = m_b.tracks[i];
        uint64_t     layers = layerBit( t.layer );

        if( t.net <= 0 || !layers )
            continue;

        CItem it { C_TRACK, (int) i, layers, {}, {}, nullptr };
        it.bb.add( t.start, t.width / 2 + 0.01 );
        it.bb.add( t.end, t.width / 2 + 0.01 );
        m_items.push_back( std::move( it ) );
    }

    for( size_t i = 0; i < m_b.arcs.size(); ++i )
    {
        const ArcTrack& a = m_b.arcs[i];
        uint64_t        layers = layerBit( a.layer );

        if( a.net <= 0 || !layers )
            continue;

        CItem it { C_ARC, (int) i, layers, {}, {}, nullptr };
        ArcToPoints( it.pts, a.start, a.mid, a.end, 0.005 );

        for( const Pt& q : it.pts )
            it.bb.add( q, a.width / 2 + 0.01 );

        m_items.push_back( std::move( it ) );
    }

    for( size_t i = 0; i < m_b.zones.size(); ++i )
    {
        const Zone& z = m_b.zones[i];

        if( z.net <= 0 || z.keepout )
            continue;

        for( const ZoneFill& f : z.fills )
        {
            uint64_t layers = layerBit( f.layer );

            if( !layers )
                continue;

            for( const Path& poly : f.polys )
            {
                if( poly.size() < 3 )
                    continue;

                CItem it { C_ZONE, (int) i, layers, {}, {}, std::make_shared<PolyIndex>() };
                it.zone->build( poly );
                it.bb = it.zone->bb;
                m_items.push_back( std::move( it ) );
            }
        }
    }

    auto netOf = [&]( const CItem& it ) -> int
    {
        switch( it.type )
        {
        case C_PAD: return m_b.pads[(size_t) it.index].net;
        case C_VIA: return m_b.vias[(size_t) it.index].net;
        case C_TRACK: return m_b.tracks[(size_t) it.index].net;
        case C_ARC: return m_b.arcs[(size_t) it.index].net;
        case C_ZONE: return m_b.zones[(size_t) it.index].net;
        }

        return 0;
    };

    m_parent.resize( m_items.size() );
    std::iota( m_parent.begin(), m_parent.end(), 0 );

    std::vector<std::vector<int>> byNet( m_b.nets.size() );

    for( size_t i = 0; i < m_items.size(); ++i )
    {
        int n = netOf( m_items[i] );

        if( n > 0 && n < (int) byNet.size() )
            byNet[(size_t) n].push_back( (int) i );
    }

    const double CELL = 2.0;

    for( size_t net = 1; net < byNet.size(); ++net )
    {
        const std::vector<int>& ids = byNet[net];

        if( ids.size() < 2 )
            continue;

        // ---- union touching items, using a grid hash to find candidates
        std::unordered_map<uint64_t, std::vector<int>> grid;

        auto cellKey = []( int64_t x, int64_t y )
        { return ( (uint64_t) ( x & 0xFFFFFFFF ) << 32 ) | (uint64_t) ( y & 0xFFFFFFFF ); };

        for( int id : ids )
        {
            const BBox& bb = m_items[(size_t) id].bb;
            int64_t     x0 = (int64_t) std::floor( bb.minX / CELL ), x1 = (int64_t) std::floor( bb.maxX / CELL );
            int64_t     y0 = (int64_t) std::floor( bb.minY / CELL ), y1 = (int64_t) std::floor( bb.maxY / CELL );

            for( int64_t x = x0; x <= x1; ++x )
            {
                for( int64_t y = y0; y <= y1; ++y )
                    grid[cellKey( x, y )].push_back( id );
            }
        }

        std::unordered_set<uint64_t> tested;

        for( auto& [key, cell] : grid )
        {
            (void) key;

            for( size_t i = 0; i < cell.size(); ++i )
            {
                for( size_t j = i + 1; j < cell.size(); ++j )
                {
                    int a = cell[i], b = cell[j];

                    if( find( a ) == find( b ) )
                        continue;

                    uint64_t pk = ( (uint64_t) std::min( a, b ) << 32 ) | (uint64_t) std::max( a, b );

                    if( !tested.insert( pk ).second )
                        continue;

                    const CItem& A = m_items[(size_t) a];
                    const CItem& B = m_items[(size_t) b];

                    if( A.bb.maxX < B.bb.minX || B.bb.maxX < A.bb.minX || A.bb.maxY < B.bb.minY
                        || B.bb.maxY < A.bb.minY )
                    {
                        continue;
                    }

                    if( touches( A, B ) )
                        unite( a, b );
                }
            }
        }

        // ---- islands of this net
        std::unordered_map<int, int>  clusterOf;
        std::vector<std::vector<Pt>>  clusterPts;
        std::vector<bool>             hasPad;
        std::vector<int>              itemCount;
        std::vector<int>              firstItem;

        for( int id : ids )
        {
            int r = find( id );
            auto it = clusterOf.find( r );
            int  c;

            if( it == clusterOf.end() )
            {
                c = (int) clusterPts.size();
                clusterOf[r] = c;
                clusterPts.emplace_back();
                hasPad.push_back( false );
                itemCount.push_back( 0 );
                firstItem.push_back( id );
            }
            else
            {
                c = it->second;
            }

            anchors( m_items[(size_t) id], clusterPts[(size_t) c] );
            ++itemCount[(size_t) c];

            if( m_items[(size_t) id].type == C_PAD )
                hasPad[(size_t) c] = true;
        }

        // Like KiCad: a lone zone island without pads was kept on purpose, no airwire to it.
        {
            size_t keep = 0;

            for( size_t c = 0; c < clusterPts.size(); ++c )
            {
                if( !hasPad[c] && itemCount[c] == 1 && m_items[(size_t) firstItem[c]].type == C_ZONE )
                    continue;

                if( keep != c )
                    clusterPts[keep] = std::move( clusterPts[c] );

                ++keep;
            }

            clusterPts.resize( keep );
        }

        size_t C = clusterPts.size();

        if( C < 2 )
            continue;

        // keep anchor lists small
        for( std::vector<Pt>& pts : clusterPts )
        {
            if( pts.size() > 32 )
            {
                std::vector<Pt> reduced;
                size_t          step = pts.size() / 32 + 1;

                for( size_t k = 0; k < pts.size(); k += step )
                    reduced.push_back( pts[k] );

                pts.swap( reduced );
            }
        }

        ++out.netsWithIslands;

        // ---- Prim's MST over islands
        std::vector<bool>   inTree( C, false );
        std::vector<double> best( C, 1e300 );
        std::vector<Pt>     bestA( C ), bestB( C );

        auto islandDist = [&]( size_t i, size_t j, Pt& pa, Pt& pb )
        {
            double d = 1e300;

            for( const Pt& p : clusterPts[i] )
            {
                for( const Pt& q : clusterPts[j] )
                {
                    double dd = ( p.x - q.x ) * ( p.x - q.x ) + ( p.y - q.y ) * ( p.y - q.y );

                    if( dd < d )
                    {
                        d = dd;
                        pa = p;
                        pb = q;
                    }
                }
            }

            return d;
        };

        inTree[0] = true;

        for( size_t j = 1; j < C; ++j )
            best[j] = islandDist( 0, j, bestA[j], bestB[j] );

        for( size_t step = 1; step < C; ++step )
        {
            size_t pick = C;

            for( size_t j = 0; j < C; ++j )
            {
                if( !inTree[j] && ( pick == C || best[j] < best[pick] ) )
                    pick = j;
            }

            if( pick == C )
                break;

            inTree[pick] = true;
            out.airwires.push_back( { bestA[pick], bestB[pick], (int) net } );

            for( size_t j = 0; j < C; ++j )
            {
                if( inTree[j] )
                    continue;

                Pt     pa, pb;
                double d = islandDist( pick, j, pa, pb );

                if( d < best[j] )
                {
                    best[j] = d;
                    bestA[j] = pa;
                    bestB[j] = pb;
                }
            }
        }
    }
}

} // anonymous namespace


void ComputeConnectivity( const Board& aBoard, Connectivity& aOut )
{
    Solver s( aBoard );
    s.Run( aOut );
}

} // namespace kp
