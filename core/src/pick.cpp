/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "pick.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace kp
{

namespace
{

double distSeg( const Pt& p, const Pt& a, const Pt& b )
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ( ( p.x - a.x ) * dx + ( p.y - a.y ) * dy ) / l2 : 0.0;
    t = std::clamp( t, 0.0, 1.0 );
    return std::hypot( p.x - ( a.x + t * dx ), p.y - ( a.y + t * dy ) );
}

double distPath( const Pt& p, const Path& path, bool closed )
{
    double d = 1e300;

    for( size_t i = 0; i + 1 < path.size(); ++i )
        d = std::min( d, distSeg( p, path[i], path[i + 1] ) );

    if( closed && path.size() > 2 )
        d = std::min( d, distSeg( p, path.back(), path.front() ) );

    if( path.size() == 1 )
        d = std::hypot( p.x - path[0].x, p.y - path[0].y );

    return d;
}

bool inPoly( const Pt& p, const Path& poly )
{
    bool   inside = false;
    size_t n = poly.size();

    for( size_t i = 0, j = n - 1; i < n; j = i++ )
    {
        const Pt& a = poly[i];
        const Pt& b = poly[j];

        if( ( ( a.y > p.y ) != ( b.y > p.y ) )
            && ( p.x < ( b.x - a.x ) * ( p.y - a.y ) / ( b.y - a.y ) + a.x ) )
        {
            inside = !inside;
        }
    }

    return inside;
}

double polyArea( const Path& poly )
{
    double a = 0;
    size_t n = poly.size();

    for( size_t i = 0, j = n - 1; i < n; j = i++ )
        a += ( poly[j].x + poly[i].x ) * ( poly[j].y - poly[i].y );

    return std::fabs( a ) / 2;
}

double pathLength( const Path& p )
{
    double l = 0;

    for( size_t i = 0; i + 1 < p.size(); ++i )
        l += std::hypot( p[i + 1].x - p[i].x, p[i + 1].y - p[i].y );

    return l;
}

bool shapeHit( const Shape& s, const Pt& p, double tol )
{
    double hw = s.width / 2 + tol;

    switch( s.type )
    {
    case ShapeType::SEGMENT:
        return distSeg( p, s.start, s.end ) <= hw;

    case ShapeType::ARC:
    {
        Path pts;
        ArcToPoints( pts, s.start, s.mid, s.end, 0.01 );
        return distPath( p, pts, false ) <= hw;
    }

    case ShapeType::CIRCLE:
    {
        double r = std::hypot( s.end.x - s.start.x, s.end.y - s.start.y );
        double d = std::hypot( p.x - s.start.x, p.y - s.start.y );
        return s.filled ? d <= r + hw : std::fabs( d - r ) <= hw;
    }

    case ShapeType::RECT:
    {
        double x0 = std::min( s.start.x, s.end.x ), x1 = std::max( s.start.x, s.end.x );
        double y0 = std::min( s.start.y, s.end.y ), y1 = std::max( s.start.y, s.end.y );

        if( s.filled )
            return p.x >= x0 - hw && p.x <= x1 + hw && p.y >= y0 - hw && p.y <= y1 + hw;

        Path r = { Pt( x0, y0 ), Pt( x1, y0 ), Pt( x1, y1 ), Pt( x0, y1 ) };
        return distPath( p, r, true ) <= hw;
    }

    case ShapeType::POLY:
    case ShapeType::BEZIER:
        if( s.filled && s.poly.size() >= 3 && inPoly( p, s.poly ) )
            return true;

        return distPath( p, s.poly, true ) <= hw;
    }

    return false;
}

/// Is aPoint on pad aPad (default geometry), with tolerance?
bool padHit( const Pad& pad, const Pt& point, double tol )
{
    const PadGeom& g = pad.geom;
    Pt             q = RotatePt( point - pad.pos, -pad.angle ) - g.offset;
    double         sx = std::fabs( g.size.x ), sy = std::fabs( g.size.y );

    switch( g.shape )
    {
    case PadShape::CIRCLE:
        return std::hypot( q.x, q.y ) <= sx / 2 + tol;

    case PadShape::OVAL:
        if( sx >= sy )
            return distSeg( q, Pt( -( sx - sy ) / 2, 0 ), Pt( ( sx - sy ) / 2, 0 ) ) <= sy / 2 + tol;

        return distSeg( q, Pt( 0, -( sy - sx ) / 2 ), Pt( 0, ( sy - sx ) / 2 ) ) <= sx / 2 + tol;

    case PadShape::TRAPEZOID:
        sx += std::fabs( g.delta.y );
        sy += std::fabs( g.delta.x );
        [[fallthrough]];

    case PadShape::RECT:
    case PadShape::ROUNDRECT:
    case PadShape::CHAMFERED_RECT:
        return std::fabs( q.x ) <= sx / 2 + tol && std::fabs( q.y ) <= sy / 2 + tol;

    case PadShape::CUSTOM:
    {
        if( g.anchor == PadShape::RECT )
        {
            if( std::fabs( q.x ) <= sx / 2 + tol && std::fabs( q.y ) <= sy / 2 + tol )
                return true;
        }
        else if( std::hypot( q.x, q.y ) <= std::min( sx, sy ) / 2 + tol )
        {
            return true;
        }

        for( const Shape& prim : g.primitives )
        {
            Shape s = prim;

            if( s.type == ShapeType::POLY )
                s.filled = true;

            if( shapeHit( s, q, tol ) )
                return true;
        }

        return false;
    }
    }

    return false;
}

void shapeBounds( const Shape& s, BBox& b )
{
    double hw = s.width / 2;

    switch( s.type )
    {
    case ShapeType::CIRCLE:
        b.add( s.start, std::hypot( s.end.x - s.start.x, s.end.y - s.start.y ) + hw );
        break;

    case ShapeType::ARC:
    {
        Path p;
        ArcToPoints( p, s.start, s.mid, s.end, 0.01 );

        for( const Pt& q : p )
            b.add( q, hw );

        break;
    }

    case ShapeType::POLY:
    case ShapeType::BEZIER:
        for( const Pt& q : s.poly )
            b.add( q, hw );

        break;

    default:
        b.add( s.start, hw );
        b.add( s.end, hw );
    }
}

std::string num( double v, int maxDecimals = 3 )
{
    char buf[64];
    snprintf( buf, sizeof( buf ), "%.*f", maxDecimals, v );
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

std::string mm( double v ) { return num( v ) + " mm"; }

std::string xy( const Pt& p ) { return "X " + num( p.x ) + "   Y " + num( p.y ) + " mm"; }

std::string deg( double a )
{
    a = std::fmod( a, 360.0 );

    if( a < 0 )
        a += 360.0;

    return num( a, 1 ) + "°";
}

double arcLength( const ArcTrack& a )
{
    Path p;
    ArcToPoints( p, a.start, a.mid, a.end, 0.0005 );
    return pathLength( p );
}

struct Info
{
    std::string text;

    explicit Info( const std::string& aTitle ) :
            text( aTitle )
    {
    }

    void add( const std::string& k, const std::string& v )
    {
        if( v.empty() )
            return;

        text += "\n" + k + "\t" + v;
    }
};

std::string netName( const Board& b, int net )
{
    if( net <= 0 || net >= (int) b.nets.size() || b.nets[(size_t) net].empty() )
        return "(bağlantısız)";

    return b.nets[(size_t) net];
}

std::string layerName( const Board& b, int l )
{
    if( l < 0 || l >= (int) b.layers.size() )
        return "?";

    const Layer& L = b.layers[(size_t) l];

    if( L.display.empty() || L.display == L.canonical )
        return L.canonical;

    return L.canonical + " (" + L.display + ")";
}

std::string layerList( const Board& b, const std::vector<int>& ls )
{
    int copper = 0;

    for( int l : ls )
    {
        if( l >= 0 && b.layers[(size_t) l].kind == LayerKind::COPPER )
            ++copper;
    }

    bool        allCopper = copper == (int) b.copperStack.size() && copper > 2;
    std::string out;

    if( allCopper )
        out = "Tüm bakır (" + std::to_string( copper ) + ")";

    for( int l : ls )
    {
        if( l < 0 )
            continue;

        if( allCopper && b.layers[(size_t) l].kind == LayerKind::COPPER )
            continue;

        if( !out.empty() )
            out += ", ";

        out += b.layers[(size_t) l].canonical;
    }

    return out;
}

double netTrackLength( const Board& b, int net )
{
    double len = 0;

    for( const Track& t : b.tracks )
    {
        if( t.net == net )
            len += std::hypot( t.end.x - t.start.x, t.end.y - t.start.y );
    }

    for( const ArcTrack& a : b.arcs )
    {
        if( a.net == net )
            len += arcLength( a );
    }

    return len;
}

void viaSpan( const Board& b, const Via& v, int& oa, int& ob )
{
    oa = 0;
    ob = (int) b.copperStack.size() - 1;

    auto isCopper = [&]( int l ) { return l >= 0 && b.layers[(size_t) l].kind == LayerKind::COPPER; };

    if( isCopper( v.layerA ) && isCopper( v.layerB ) )
    {
        oa = b.layers[(size_t) v.layerA].copperOrder;
        ob = b.layers[(size_t) v.layerB].copperOrder;

        if( oa > ob )
            std::swap( oa, ob );
    }
}

} // anonymous namespace


void Pick( const Board& b, const Mesh& m, const Pt& p, double tol, const PickFilter& f,
           std::vector<PickHit>& out )
{
    out.clear();

    auto order = [&]( int layer ) -> int
    {
        if( layer < 0 || layer >= (int) m.slots.size() )
            return 0;

        return f.flipped ? m.slots[(size_t) layer].sortBack : m.slots[(size_t) layer].sortFront;
    };

    // pads
    if( f.kindVisible( ItemKind::PAD ) )
    {
        for( size_t i = 0; i < b.pads.size(); ++i )
        {
            const Pad& pad = b.pads[i];
            int        best = -1, bestOrder = -1000000;

            for( int l : pad.layers )
            {
                if( f.layerVisible( l ) && order( l ) > bestOrder )
                {
                    best = l;
                    bestOrder = order( l );
                }
            }

            if( best < 0 || !padHit( pad, p, tol ) )
                continue;

            PickHit h;
            h.type = ItemType::PAD;
            h.index = (int) i;
            h.footprint = pad.footprint;
            h.net = pad.net;
            h.layer = best;
            h.priority = 0;
            h.layerOrder = bestOrder;
            h.size = pad.geom.size.x * pad.geom.size.y;
            out.push_back( h );
        }
    }

    // vias
    if( f.kindVisible( ItemKind::VIA ) )
    {
        for( size_t i = 0; i < b.vias.size(); ++i )
        {
            const Via& v = b.vias[i];

            if( std::hypot( p.x - v.pos.x, p.y - v.pos.y ) > v.size / 2 + tol )
                continue;

            int oa, ob;
            viaSpan( b, v, oa, ob );
            int best = -1, bestOrder = -1000000;

            for( int o = oa; o <= ob && o < (int) b.copperStack.size(); ++o )
            {
                int l = b.copperStack[(size_t) o];

                if( f.layerVisible( l ) && order( l ) > bestOrder )
                {
                    best = l;
                    bestOrder = order( l );
                }
            }

            if( best < 0 )
                continue;

            PickHit h;
            h.type = ItemType::VIA;
            h.index = (int) i;
            h.net = v.net;
            h.layer = best;
            h.priority = 0;
            h.layerOrder = bestOrder;
            h.size = v.size * v.size;
            out.push_back( h );
        }
    }

    // tracks
    if( f.kindVisible( ItemKind::TRACK ) )
    {
        for( size_t i = 0; i < b.tracks.size(); ++i )
        {
            const Track& t = b.tracks[i];

            if( !f.layerVisible( t.layer ) || distSeg( p, t.start, t.end ) > t.width / 2 + tol )
                continue;

            PickHit h;
            h.type = ItemType::TRACK;
            h.index = (int) i;
            h.net = t.net;
            h.layer = t.layer;
            h.priority = 1;
            h.layerOrder = order( t.layer );
            h.size = t.width;
            out.push_back( h );
        }

        for( size_t i = 0; i < b.arcs.size(); ++i )
        {
            const ArcTrack& a = b.arcs[i];

            if( !f.layerVisible( a.layer ) )
                continue;

            Path pts;
            ArcToPoints( pts, a.start, a.mid, a.end, 0.005 );

            if( distPath( p, pts, false ) > a.width / 2 + tol )
                continue;

            PickHit h;
            h.type = ItemType::ARC;
            h.index = (int) i;
            h.net = a.net;
            h.layer = a.layer;
            h.priority = 1;
            h.layerOrder = order( a.layer );
            h.size = a.width;
            out.push_back( h );
        }
    }

    // footprint children (texts, drawings) select their footprint
    std::vector<int> fpBest( b.footprints.size(), -1 );

    auto addFootprintHit = [&]( int fp, int layer, int priority )
    {
        if( fp < 0 || fp >= (int) b.footprints.size() )
            return;

        const Footprint& F = b.footprints[(size_t) fp];
        PickHit          h;
        h.type = ItemType::FOOTPRINT;
        h.index = fp;
        h.footprint = fp;
        h.layer = layer;
        h.priority = priority;
        h.layerOrder = order( layer );
        h.size = ( F.localBox.maxX - F.localBox.minX ) * ( F.localBox.maxY - F.localBox.minY );

        if( fpBest[(size_t) fp] >= 0 )
        {
            PickHit& old = out[(size_t) fpBest[(size_t) fp]];

            if( old.priority > h.priority
                || ( old.priority == h.priority && old.layerOrder < h.layerOrder ) )
            {
                old = h;
            }

            return;
        }

        fpBest[(size_t) fp] = (int) out.size();
        out.push_back( h );
    };

    // texts
    if( f.kindVisible( ItemKind::TEXT ) )
    {
        for( size_t i = 0; i < b.texts.size() && i < m.textBounds.size(); ++i )
        {
            const Text& t = b.texts[i];
            const BBox& bb = m.textBounds[i];

            if( !bb.valid() || !f.layerVisible( t.layer ) )
                continue;

            if( p.x < bb.minX - tol || p.x > bb.maxX + tol || p.y < bb.minY - tol || p.y > bb.maxY + tol )
                continue;

            if( t.footprint >= 0 )
            {
                addFootprintHit( t.footprint, t.layer, 2 );
                continue;
            }

            PickHit h;
            h.type = ItemType::TEXT;
            h.index = (int) i;
            h.layer = t.layer;
            h.priority = 2;
            h.layerOrder = order( t.layer );
            h.size = ( bb.maxX - bb.minX ) * ( bb.maxY - bb.minY );
            out.push_back( h );
        }
    }

    // drawings
    if( f.kindVisible( ItemKind::GRAPHIC ) )
    {
        for( size_t i = 0; i < b.shapes.size(); ++i )
        {
            const Shape& s = b.shapes[i];

            if( !f.layerVisible( s.layer ) || !shapeHit( s, p, tol ) )
                continue;

            if( s.footprint >= 0 )
            {
                addFootprintHit( s.footprint, s.layer, 2 );
                continue;
            }

            BBox bb;
            shapeBounds( s, bb );

            PickHit h;
            h.type = ItemType::SHAPE;
            h.index = (int) i;
            h.layer = s.layer;
            h.priority = 3;
            h.layerOrder = order( s.layer );
            h.size = bb.valid() ? ( bb.maxX - bb.minX ) * ( bb.maxY - bb.minY ) : 0.0;
            out.push_back( h );
        }
    }

    // footprint bodies
    {
        bool sideVisible[2] = { false, false }; // [0] = front, [1] = back

        for( size_t l = 0; l < b.layers.size(); ++l )
        {
            const Layer& L = b.layers[l];

            if( L.side == 0 || !f.layerVisible( (int) l ) )
                continue;

            if( L.kind == LayerKind::COPPER || L.kind == LayerKind::SILK || L.kind == LayerKind::FAB
                || L.kind == LayerKind::COURTYARD )
            {
                sideVisible[L.side > 0 ? 0 : 1] = true;
            }
        }

        for( size_t i = 0; i < b.footprints.size(); ++i )
        {
            const Footprint& F = b.footprints[i];

            if( !sideVisible[F.back ? 1 : 0] )
                continue;

            Pt          q = RotatePt( p - F.pos, -F.angle );
            const BBox& bb = F.localBox;

            if( q.x < bb.minX - tol || q.x > bb.maxX + tol || q.y < bb.minY - tol || q.y > bb.maxY + tol )
                continue;

            addFootprintHit( (int) i, F.layer, 4 );
        }
    }

    // zones
    if( f.kindVisible( ItemKind::ZONE ) )
    {
        for( size_t i = 0; i < b.zones.size(); ++i )
        {
            const Zone& z = b.zones[i];

            if( z.keepout )
                continue;

            for( const ZoneFill& fill : z.fills )
            {
                if( !f.layerVisible( fill.layer ) )
                    continue;

                bool hit = false;

                for( const Path& poly : fill.polys )
                {
                    if( inPoly( p, poly ) )
                    {
                        hit = true;
                        break;
                    }
                }

                if( !hit )
                    continue;

                PickHit h;
                h.type = ItemType::ZONE;
                h.index = (int) i;
                h.footprint = z.footprint;
                h.net = z.net;
                h.layer = fill.layer;
                h.priority = 5;
                h.layerOrder = order( fill.layer );
                out.push_back( h );
                break;
            }
        }
    }

    std::stable_sort( out.begin(), out.end(),
                      []( const PickHit& a, const PickHit& c )
                      {
                          if( a.priority != c.priority )
                              return a.priority < c.priority;

                          if( a.layerOrder != c.layerOrder )
                              return a.layerOrder > c.layerOrder;

                          return a.size < c.size;
                      } );
}


bool Snap( const Board& b, const Pt& p, double tol, const PickFilter& f, Pt& outPt )
{
    double best = 1e300;
    bool   found = false;

    for( const Pad& pad : b.pads )
    {
        bool visible = false;

        for( int l : pad.layers )
            visible = visible || f.layerVisible( l );

        if( !visible )
            continue;

        Pt     c = pad.pos + RotatePt( pad.geom.offset, pad.angle );
        double d = std::hypot( p.x - c.x, p.y - c.y );

        if( ( d <= tol || padHit( pad, p, 0 ) ) && d < best )
        {
            best = d;
            outPt = c;
            found = true;
        }
    }

    for( const Via& v : b.vias )
    {
        double d = std::hypot( p.x - v.pos.x, p.y - v.pos.y );

        if( d <= std::max( tol, v.size / 2 ) && d < best )
        {
            best = d;
            outPt = v.pos;
            found = true;
        }
    }

    return found;
}


std::string ItemInfo( const Board& b, const Mesh& m, ItemType type, int idx )
{
    switch( type )
    {
    case ItemType::PAD:
    {
        if( idx < 0 || idx >= (int) b.pads.size() )
            break;

        const Pad&  p = b.pads[(size_t) idx];
        std::string ref;
        std::string fpLine;

        if( p.footprint >= 0 )
        {
            const Footprint& F = b.footprints[(size_t) p.footprint];
            ref = F.reference;
            fpLine = F.value.empty() ? F.reference : F.reference + " (" + F.value + ")";
        }

        std::string title = "Pad " + ( ref.empty() ? p.number : ref + "-" + p.number );
        Info        info( title );
        info.add( "Footprint", fpLine );
        info.add( "Net", netName( b, p.net ) );
        info.add( "Pin işlevi", p.pinFunction );
        info.add( "Pin tipi", p.pinType );

        const char* attr = p.attr == PadAttr::SMD    ? "SMD"
                           : p.attr == PadAttr::PTH  ? "Delikli (kaplamalı)"
                           : p.attr == PadAttr::NPTH ? "Delikli (kaplamasız)"
                                                     : "Konnektör";
        info.add( "Tür", attr );

        const char* shape = "Daire";

        switch( p.geom.shape )
        {
        case PadShape::CIRCLE: shape = "Daire"; break;
        case PadShape::RECT: shape = "Dikdörtgen"; break;
        case PadShape::OVAL: shape = "Oval"; break;
        case PadShape::TRAPEZOID: shape = "Yamuk"; break;
        case PadShape::ROUNDRECT: shape = "Yuvarlak köşeli"; break;
        case PadShape::CHAMFERED_RECT: shape = "Pahlı"; break;
        case PadShape::CUSTOM: shape = "Özel şekil"; break;
        }

        info.add( "Şekil", shape );
        info.add( "Boyut", num( p.geom.size.x ) + " × " + num( p.geom.size.y ) + " mm" );

        if( p.drill.x > 0 )
        {
            if( p.drillOval && std::fabs( p.drill.x - p.drill.y ) > 1e-9 )
                info.add( "Delik", "oval " + num( p.drill.x ) + " × " + num( p.drill.y ) + " mm" );
            else
                info.add( "Delik", "Ø " + mm( p.drill.x ) );
        }

        info.add( "Açı", deg( p.angle ) );
        info.add( "Konum", xy( p.pos ) );
        info.add( "Katmanlar", layerList( b, p.layers ) );
        return info.text;
    }

    case ItemType::VIA:
    {
        if( idx < 0 || idx >= (int) b.vias.size() )
            break;

        const Via&  v = b.vias[(size_t) idx];
        const char* kind = v.type == ViaType::BLIND    ? "Kör (blind)"
                           : v.type == ViaType::BURIED ? "Gömülü (buried)"
                           : v.type == ViaType::MICRO  ? "Mikro"
                                                       : "Geçişli (through)";
        Info info( "Via" );
        info.add( "Net", netName( b, v.net ) );
        info.add( "Tür", kind );
        info.add( "Çap", mm( v.size ) );
        info.add( "Delik", "Ø " + mm( v.drill ) );
        info.add( "Halka", mm( ( v.size - v.drill ) / 2 ) );

        int oa, ob;
        viaSpan( b, v, oa, ob );

        if( !b.copperStack.empty() )
        {
            info.add( "Katmanlar", b.layers[(size_t) b.copperStack[(size_t) oa]].canonical + " → "
                                           + b.layers[(size_t) b.copperStack[(size_t) ob]].canonical );
        }

        info.add( "Konum", xy( v.pos ) );
        return info.text;
    }

    case ItemType::TRACK:
    {
        if( idx < 0 || idx >= (int) b.tracks.size() )
            break;

        const Track& t = b.tracks[(size_t) idx];
        Info         info( "Pist" );
        info.add( "Net", netName( b, t.net ) );
        info.add( "Katman", layerName( b, t.layer ) );
        info.add( "Genişlik", mm( t.width ) );
        info.add( "Uzunluk", mm( std::hypot( t.end.x - t.start.x, t.end.y - t.start.y ) ) );

        if( t.net > 0 )
            info.add( "Netin toplam pisti", mm( netTrackLength( b, t.net ) ) );

        info.add( "Başlangıç", xy( t.start ) );
        info.add( "Bitiş", xy( t.end ) );
        return info.text;
    }

    case ItemType::ARC:
    {
        if( idx < 0 || idx >= (int) b.arcs.size() )
            break;

        const ArcTrack& a = b.arcs[(size_t) idx];
        Info            info( "Yay pist" );
        info.add( "Net", netName( b, a.net ) );
        info.add( "Katman", layerName( b, a.layer ) );
        info.add( "Genişlik", mm( a.width ) );
        info.add( "Uzunluk", mm( arcLength( a ) ) );

        if( a.net > 0 )
            info.add( "Netin toplam pisti", mm( netTrackLength( b, a.net ) ) );

        info.add( "Başlangıç", xy( a.start ) );
        info.add( "Bitiş", xy( a.end ) );
        return info.text;
    }

    case ItemType::FOOTPRINT:
    {
        if( idx < 0 || idx >= (int) b.footprints.size() )
            break;

        const Footprint& F = b.footprints[(size_t) idx];
        Info             info( F.reference.empty() ? "Footprint" : F.reference );
        info.add( "Değer", F.value );
        info.add( "Kütüphane", F.libId );
        info.add( "Taraf", F.back ? "Arka (B)" : "Ön (F)" );
        info.add( "Konum", xy( F.pos ) );
        info.add( "Açı", deg( F.angle ) );
        info.add( "Pad sayısı", std::to_string( F.padCount ) );
        info.add( "Boyut", num( F.localBox.maxX - F.localBox.minX ) + " × "
                                   + num( F.localBox.maxY - F.localBox.minY ) + " mm" );

        std::string attrs;

        for( const std::string& a : F.attrs )
        {
            std::string t = a == "smd"                      ? "SMD"
                            : a == "through_hole"           ? "Delikli"
                            : a == "board_only"             ? "Sadece kart"
                            : a == "exclude_from_pos_files" ? "Dizgi dosyasına girmez"
                            : a == "exclude_from_bom"       ? "BOM'a girmez"
                            : a == "dnp"                    ? "Takılmayacak (DNP)"
                            : a == "allow_missing_courtyard" ? "Courtyard gerekmez"
                            : a == "allow_soldermask_bridges" ? "Maske köprüsüne izin"
                            : a == "virtual"                ? "Sanal"
                                                            : a;

            attrs += ( attrs.empty() ? "" : ", " ) + t;
        }

        info.add( "Özellikler", attrs );

        // remaining fields, alphabetically
        std::set<std::string> keys;

        for( const auto& [k, v] : F.fields )
        {
            if( v.empty() || v == "~" || k == "Reference" || k == "Value" || k == "Footprint"
                || k.rfind( "ki_", 0 ) == 0 )
            {
                continue;
            }

            keys.insert( k );
        }

        for( const std::string& k : keys )
        {
            std::string label = k == "Datasheet" ? "Datasheet" : k == "Description" ? "Açıklama" : k;
            info.add( label, F.fields.at( k ) );
        }

        return info.text;
    }

    case ItemType::ZONE:
    {
        if( idx < 0 || idx >= (int) b.zones.size() )
            break;

        const Zone& z = b.zones[(size_t) idx];
        Info        info( z.name.empty() ? "Bakır bölge" : "Bakır bölge: " + z.name );
        info.add( "Net", netName( b, z.net ) );
        info.add( "Katmanlar", layerList( b, z.layers ) );

        double area = 0;

        for( const ZoneFill& f : z.fills )
        {
            for( const Path& poly : f.polys )
                area += polyArea( poly );
        }

        if( area > 0 )
            info.add( "Dolu alan", num( area, 1 ) + " mm²" );

        info.add( "Kenar köşe sayısı", std::to_string( z.outline.size() ) );

        if( z.minThickness > 0 )
            info.add( "Min. kalınlık", mm( z.minThickness ) );

        return info.text;
    }

    case ItemType::SHAPE:
    {
        if( idx < 0 || idx >= (int) b.shapes.size() )
            break;

        const Shape& s = b.shapes[(size_t) idx];
        const char*  kind = s.type == ShapeType::SEGMENT  ? "Çizgi"
                            : s.type == ShapeType::ARC    ? "Yay"
                            : s.type == ShapeType::CIRCLE ? "Daire"
                            : s.type == ShapeType::RECT   ? "Dikdörtgen"
                                                          : "Çokgen";
        Info info( std::string( "Çizim: " ) + kind );
        info.add( "Katman", layerName( b, s.layer ) );
        info.add( "Kalınlık", mm( s.width ) );

        switch( s.type )
        {
        case ShapeType::SEGMENT:
            info.add( "Uzunluk", mm( std::hypot( s.end.x - s.start.x, s.end.y - s.start.y ) ) );
            info.add( "Başlangıç", xy( s.start ) );
            info.add( "Bitiş", xy( s.end ) );
            break;

        case ShapeType::ARC:
        {
            Path pts;
            ArcToPoints( pts, s.start, s.mid, s.end, 0.0005 );
            info.add( "Uzunluk", mm( pathLength( pts ) ) );
            break;
        }

        case ShapeType::CIRCLE:
            info.add( "Merkez", xy( s.start ) );
            info.add( "Yarıçap", mm( std::hypot( s.end.x - s.start.x, s.end.y - s.start.y ) ) );
            break;

        case ShapeType::RECT:
            info.add( "Boyut", num( std::fabs( s.end.x - s.start.x ) ) + " × "
                                       + num( std::fabs( s.end.y - s.start.y ) ) + " mm" );
            break;

        default:
            info.add( "Köşe sayısı", std::to_string( s.poly.size() ) );
        }

        info.add( "Dolgu", s.filled ? "Var" : "Yok" );
        return info.text;
    }

    case ItemType::TEXT:
    {
        if( idx < 0 || idx >= (int) b.texts.size() )
            break;

        const Text& t = b.texts[(size_t) idx];
        Info        info( "Yazı" );
        info.add( "İçerik", b.ResolveText( t.text, t.footprint, t.layer ) );
        info.add( "Katman", layerName( b, t.layer ) );
        info.add( "Boyut", num( t.sizeX ) + " × " + num( t.sizeY ) + " mm" );
        info.add( "Kalınlık", mm( t.thickness ) );
        info.add( "Açı", deg( t.angle ) );

        if( t.mirror )
            info.add( "Ayna", "Evet" );

        if( !t.renderCache.empty() )
            info.add( "Yazı tipi", "TrueType" );

        return info.text;
    }

    default:
        break;
    }

    (void) m;
    return std::string();
}


std::string NetInfo( const Board& b, int net )
{
    if( net <= 0 || net >= (int) b.nets.size() )
        return std::string();

    int                   pads = 0, vias = 0, tracks = 0, zones = 0;
    std::set<std::string> refs;

    for( const Pad& p : b.pads )
    {
        if( p.net != net )
            continue;

        ++pads;

        if( p.footprint >= 0 && !b.footprints[(size_t) p.footprint].reference.empty() )
            refs.insert( b.footprints[(size_t) p.footprint].reference );
    }

    for( const Via& v : b.vias )
        vias += v.net == net;

    for( const Track& t : b.tracks )
        tracks += t.net == net;

    for( const ArcTrack& a : b.arcs )
        tracks += a.net == net;

    for( const Zone& z : b.zones )
        zones += ( z.net == net && !z.keepout );

    Info info( b.nets[(size_t) net] );
    info.add( "Pad", std::to_string( pads ) );
    info.add( "Via", std::to_string( vias ) );
    info.add( "Pist", std::to_string( tracks ) );
    info.add( "Toplam pist uzunluğu", mm( netTrackLength( b, net ) ) );

    if( zones )
        info.add( "Bakır bölge", std::to_string( zones ) );

    std::string list;
    int         n = 0;

    for( const std::string& r : refs )
    {
        if( n++ == 24 )
        {
            list += ", …";
            break;
        }

        list += ( list.empty() ? "" : ", " ) + r;
    }

    info.add( "Bağlı elemanlar", list );
    return info.text;
}


BBox ItemBounds( const Board& b, const Mesh& m, ItemType type, int idx )
{
    BBox bb;

    switch( type )
    {
    case ItemType::PAD:
        if( idx >= 0 && idx < (int) b.pads.size() )
        {
            const Pad& p = b.pads[(size_t) idx];
            bb.add( p.pos + RotatePt( p.geom.offset, p.angle ), std::max( p.geom.size.x, p.geom.size.y ) / 2 );
        }

        break;

    case ItemType::VIA:
        if( idx >= 0 && idx < (int) b.vias.size() )
            bb.add( b.vias[(size_t) idx].pos, b.vias[(size_t) idx].size / 2 );

        break;

    case ItemType::TRACK:
        if( idx >= 0 && idx < (int) b.tracks.size() )
        {
            const Track& t = b.tracks[(size_t) idx];
            bb.add( t.start, t.width / 2 );
            bb.add( t.end, t.width / 2 );
        }

        break;

    case ItemType::ARC:
        if( idx >= 0 && idx < (int) b.arcs.size() )
        {
            const ArcTrack& a = b.arcs[(size_t) idx];
            Path            pts;
            ArcToPoints( pts, a.start, a.mid, a.end, 0.01 );

            for( const Pt& q : pts )
                bb.add( q, a.width / 2 );
        }

        break;

    case ItemType::FOOTPRINT:
        if( idx >= 0 && idx < (int) b.footprints.size() )
        {
            const Footprint& F = b.footprints[(size_t) idx];
            const BBox&      L = F.localBox;

            for( const Pt& c : { Pt( L.minX, L.minY ), Pt( L.maxX, L.minY ), Pt( L.maxX, L.maxY ),
                                 Pt( L.minX, L.maxY ) } )
            {
                bb.add( RotatePt( c, F.angle ) + F.pos );
            }
        }

        break;

    case ItemType::ZONE:
        if( idx >= 0 && idx < (int) b.zones.size() )
        {
            for( const Pt& q : b.zones[(size_t) idx].outline )
                bb.add( q );
        }

        break;

    case ItemType::SHAPE:
        if( idx >= 0 && idx < (int) b.shapes.size() )
            shapeBounds( b.shapes[(size_t) idx], bb );

        break;

    case ItemType::TEXT:
        if( idx >= 0 && idx < (int) m.textBounds.size() )
            bb = m.textBounds[(size_t) idx];

        break;

    default:
        break;
    }

    return bb;
}


BBox NetBounds( const Board& b, int net )
{
    BBox bb;

    if( net <= 0 )
        return bb;

    for( const Track& t : b.tracks )
    {
        if( t.net == net )
        {
            bb.add( t.start, t.width / 2 );
            bb.add( t.end, t.width / 2 );
        }
    }

    for( const ArcTrack& a : b.arcs )
    {
        if( a.net == net )
        {
            bb.add( a.start, a.width / 2 );
            bb.add( a.mid, a.width / 2 );
            bb.add( a.end, a.width / 2 );
        }
    }

    for( const Via& v : b.vias )
    {
        if( v.net == net )
            bb.add( v.pos, v.size / 2 );
    }

    for( const Pad& p : b.pads )
    {
        if( p.net == net )
            bb.add( p.pos, std::max( p.geom.size.x, p.geom.size.y ) / 2 );
    }

    for( const Zone& z : b.zones )
    {
        if( z.net == net && !z.keepout )
        {
            for( const Pt& q : z.outline )
                bb.add( q );
        }
    }

    return bb;
}


void HighlightRanges( const Board& b, const Mesh& m, ItemType type, int idx, bool wholeNet, int net,
                      std::vector<ItemRange>& out )
{
    out.clear();

    auto addItem = [&]( ItemType t, size_t i )
    {
        if( (size_t) t >= m.offsets.size() )
            return;

        const std::vector<uint32_t>& off = m.offsets[(size_t) t];

        if( i + 1 >= off.size() )
            return;

        for( uint32_t k = off[i]; k < off[i + 1] && k < m.ranges.size(); ++k )
            out.push_back( m.ranges[k] );
    };

    if( wholeNet && net > 0 )
    {
        for( size_t i = 0; i < b.tracks.size(); ++i )
        {
            if( b.tracks[i].net == net )
                addItem( ItemType::TRACK, i );
        }

        for( size_t i = 0; i < b.arcs.size(); ++i )
        {
            if( b.arcs[i].net == net )
                addItem( ItemType::ARC, i );
        }

        for( size_t i = 0; i < b.vias.size(); ++i )
        {
            if( b.vias[i].net == net )
                addItem( ItemType::VIA, i );
        }

        for( size_t i = 0; i < b.pads.size(); ++i )
        {
            if( b.pads[i].net == net )
                addItem( ItemType::PAD, i );
        }

        for( size_t i = 0; i < b.zones.size(); ++i )
        {
            if( b.zones[i].net == net )
                addItem( ItemType::ZONE, i );
        }
    }
    else if( type == ItemType::FOOTPRINT )
    {
        for( size_t i = 0; i < b.pads.size(); ++i )
        {
            if( b.pads[i].footprint == idx )
                addItem( ItemType::PAD, i );
        }

        for( size_t i = 0; i < b.shapes.size(); ++i )
        {
            if( b.shapes[i].footprint == idx )
                addItem( ItemType::SHAPE, i );
        }

        for( size_t i = 0; i < b.texts.size(); ++i )
        {
            if( b.texts[i].footprint == idx )
                addItem( ItemType::TEXT, i );
        }

        for( size_t i = 0; i < b.zones.size(); ++i )
        {
            if( b.zones[i].footprint == idx )
                addItem( ItemType::ZONE, i );
        }
    }
    else if( type != ItemType::NONE && idx >= 0 )
    {
        addItem( type, (size_t) idx );
    }

    // merge neighbouring ranges of the same slot/kind (fewer draw calls)
    std::sort( out.begin(), out.end(),
               []( const ItemRange& a, const ItemRange& c ) { return a.start < c.start; } );

    std::vector<ItemRange> merged;
    merged.reserve( out.size() );

    for( const ItemRange& r : out )
    {
        if( !merged.empty() && merged.back().slot == r.slot && merged.back().kind == r.kind
            && merged.back().start + merged.back().count == r.start )
        {
            merged.back().count += r.count;
        }
        else
        {
            merged.push_back( r );
        }
    }

    out.swap( merged );
}

} // namespace kp
