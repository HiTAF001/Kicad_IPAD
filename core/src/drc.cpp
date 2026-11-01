/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "drc.h"
#include "i18n.h"
#include "json.h"
#include "pick.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace kp
{

namespace
{

constexpr double PI_ = 3.14159265358979323846;
constexpr double EPS = 0.0005;        // KiCad's DRC epsilon (mm)
constexpr double DRC_ARC_ERR = 0.0001; // arcs are flattened finer than the epsilon
constexpr double CHAIN_EPS = 0.02;     // outline chaining tolerance (mm)

struct CodeInfo
{
    const char* key;
    const char* title;
    Severity    severity;
};

const CodeInfo CODES[] = {
    { "clearance", N_( "Clearance violation" ), Severity::ERROR },
    { "shorting_items", N_( "Items shorting two nets" ), Severity::ERROR },
    { "tracks_crossing", N_( "Tracks crossing" ), Severity::ERROR },
    { "hole_clearance", N_( "Hole clearance violation" ), Severity::ERROR },
    { "copper_edge_clearance", N_( "Board edge clearance violation" ), Severity::ERROR },
    { "track_width", N_( "Track width too small" ), Severity::ERROR },
    { "via_diameter", N_( "Via diameter too small" ), Severity::ERROR },
    { "annular_width", N_( "Annular ring too small" ), Severity::ERROR },
    { "drill_out_of_range", N_( "Hole too small" ), Severity::ERROR },
    { "microvia_drill_out_of_range", N_( "Microvia hole too small" ), Severity::ERROR },
    { "hole_near_hole", N_( "Drilled holes too close together" ), Severity::ERROR },
    { "holes_co_located", N_( "Drilled holes at the same place" ), Severity::WARNING },
    { "courtyards_overlap", N_( "Courtyards overlap" ), Severity::ERROR },
    { "malformed_courtyard", N_( "Courtyard is not a closed shape" ), Severity::ERROR },
    { "unconnected_items", N_( "Missing connection" ), Severity::ERROR },
    { "track_dangling", N_( "Track has an unconnected end" ), Severity::WARNING },
    { "via_dangling", N_( "Via is not connected or connected on only one layer" ), Severity::WARNING },
    { "invalid_outline", N_( "Board outline is missing or not closed" ), Severity::ERROR },
};

static_assert( sizeof( CODES ) / sizeof( CODES[0] ) == (size_t) DrcCode::COUNT, "code table" );

bool wildcardMatch( const char* pat, const char* s )
{
    // '*' and '?' wildcards, as KiCad's net class patterns
    const char* star = nullptr;
    const char* ss = nullptr;

    while( *s )
    {
        if( *pat == '?' || *pat == *s )
        {
            ++pat;
            ++s;
        }
        else if( *pat == '*' )
        {
            star = pat++;
            ss = s;
        }
        else if( star )
        {
            pat = star + 1;
            s = ++ss;
        }
        else
        {
            return false;
        }
    }

    while( *pat == '*' )
        ++pat;

    return *pat == 0;
}

std::string mm( double v )
{
    char buf[32];
    snprintf( buf, sizeof( buf ), "%.3f", v );
    return buf;
}

bool isCu( const Board& b, int l )
{
    return l >= 0 && l < (int) b.layers.size() && b.layers[(size_t) l].kind == LayerKind::COPPER;
}

bool isTrackish( ItemType t )
{
    return t == ItemType::TRACK || t == ItemType::ARC || t == ItemType::VIA;
}

uint64_t pairKey( const CuShape& a, const CuShape& b, int layer )
{
    uint64_t ka = ( (uint64_t) a.type << 28 ) | (uint64_t) ( a.index & 0x0FFFFFFF );
    uint64_t kb = ( (uint64_t) b.type << 28 ) | (uint64_t) ( b.index & 0x0FFFFFFF );

    if( ka > kb )
        std::swap( ka, kb );

    uint64_t k = ka * 0x9E3779B97F4A7C15ull ^ ( kb + 0x632BE59BD9B4E019ull + ( ka << 6 ) + ( ka >> 2 ) );

    if( layer >= 0 )
        k ^= (uint64_t) ( layer + 1 ) * 0xC2B2AE3D27D4EB4Full;

    return k;
}

/// Hole of a pad or via as a segment with a radius (drill axis).
struct Hole
{
    ItemType type;
    int      index;
    Pt       a, b;
    double   r;
    int      net;
    int      footprint;
    bool     micro = false;
    int      orderA = 0, orderB = 0; ///< copper span (vias)
};

} // anonymous namespace


const char* DrcKey( DrcCode c )
{
    return CODES[(int) c].key;
}


const char* DrcTitle( DrcCode c )
{
    return CODES[(int) c].title;
}


Severity DefaultSeverity( DrcCode c )
{
    return CODES[(int) c].severity;
}


DesignRules::DesignRules()
{
    for( int i = 0; i < (int) DrcCode::COUNT; ++i )
        severity[i] = CODES[i].severity;
}


const NetClassRule& DesignRules::ClassOf( int aNet ) const
{
    if( aNet > 0 && aNet < (int) netClass.size() )
    {
        int c = netClass[(size_t) aNet];

        if( c >= 0 && c < (int) classes.size() )
            return classes[(size_t) c];
    }

    return classes[0];
}


// ------------------------------------------------------------------------------------------
// Rules
// ------------------------------------------------------------------------------------------

DesignRules ResolveRules( const Board& b, const char* aProject, size_t aProjectLen, std::string& aNote )
{
    DesignRules R;
    R.netClass.assign( b.nets.size(), 0 );

    auto classIndex = [&]( const std::string& name ) -> int
    {
        for( size_t i = 0; i < R.classes.size(); ++i )
        {
            if( R.classes[i].name == name )
                return (int) i;
        }

        return -1;
    };

    bool fromProject = false;

    if( aProject && aProjectLen > 0 )
    {
        JValue      root;
        std::string err;

        if( !ParseJson( aProject, aProjectLen, root, err ) )
        {
            aNote = TrF( "The project file could not be read ({0}); using the board's own rules.", { err } );
        }
        else if( const JValue* ds = root.Path( { "board", "design_settings" } ) )
        {
            fromProject = true;
            R.source = DesignRules::Source::PROJECT;

            if( const JValue* r = ds->Get( "rules" ) )
            {
                auto num = [&]( const char* k, double& out )
                {
                    if( const JValue* v = r->Get( k ) )
                        out = v->Num( out );
                };

                num( "min_clearance", R.minClearance );
                num( "min_track_width", R.minTrackWidth );
                num( "min_via_diameter", R.minViaDiameter );
                num( "min_via_annular_width", R.minViaAnnular );
                num( "min_through_hole_diameter", R.minThroughHole );
                num( "min_microvia_diameter", R.minMicroviaDiameter );
                num( "min_microvia_drill", R.minMicroviaDrill );
                num( "min_hole_to_hole", R.minHoleToHole );
                num( "min_hole_clearance", R.minHoleClearance );
                num( "min_copper_edge_clearance", R.copperEdgeClearance );
            }

            if( const JValue* sev = ds->Get( "rule_severities" ) )
            {
                for( int i = 0; i < (int) DrcCode::COUNT; ++i )
                {
                    const JValue* v = sev->Get( CODES[i].key );

                    if( v && v->IsString() )
                    {
                        if( v->s == "error" )
                            R.severity[i] = Severity::ERROR;
                        else if( v->s == "warning" )
                            R.severity[i] = Severity::WARNING;
                        else if( v->s == "ignore" )
                            R.severity[i] = Severity::IGNORE;
                    }
                }
            }

            if( const JValue* ns = root.Get( "net_settings" ) )
            {
                const JValue* classes = ns->Get( "classes" );
                std::vector<std::pair<int, std::vector<std::string>>> explicitNets;

                if( classes && classes->IsArray() )
                {
                    // Default first: other classes fall back to its values
                    for( int pass = 0; pass < 2; ++pass )
                    {
                        for( const JValue& c : classes->arr )
                        {
                            const JValue* nm = c.Get( "name" );
                            std::string   name = nm && nm->IsString() ? nm->s : std::string();
                            bool          isDefault = name == "Default";

                            if( name.empty() || ( pass == 0 ) != isDefault )
                                continue;

                            NetClassRule nc = R.classes[0];
                            nc.name = name;

                            auto num = [&]( const char* k, double& out )
                            {
                                if( const JValue* v = c.Get( k ) )
                                    out = v->Num( out );
                            };

                            num( "clearance", nc.clearance );
                            num( "track_width", nc.trackWidth );
                            num( "via_diameter", nc.viaDiameter );
                            num( "via_drill", nc.viaDrill );
                            num( "microvia_diameter", nc.uviaDiameter );
                            num( "microvia_drill", nc.uviaDrill );

                            int idx;

                            if( isDefault )
                            {
                                R.classes[0] = nc;
                                idx = 0;
                            }
                            else
                            {
                                R.classes.push_back( nc );
                                idx = (int) R.classes.size() - 1;
                            }

                            if( const JValue* nets = c.Get( "nets" ) )
                            {
                                std::vector<std::string> names;

                                for( const JValue& n : nets->arr )
                                {
                                    if( n.IsString() )
                                        names.push_back( n.s );
                                }

                                explicitNets.emplace_back( idx, std::move( names ) );
                            }
                        }
                    }
                }

                auto assign = [&]( const std::string& net, int cls )
                {
                    auto it = b.netByName.find( net );

                    if( it != b.netByName.end() && it->second > 0 && cls >= 0 )
                    {
                        // a net in several classes: the strictest clearance wins
                        int& cur = R.netClass[(size_t) it->second];

                        if( cur == 0 || R.classes[(size_t) cls].clearance > R.classes[(size_t) cur].clearance )
                            cur = cls;
                    }
                };

                // KiCad 8+: patterns, then explicit assignments
                if( const JValue* pats = ns->Get( "netclass_patterns" ) )
                {
                    for( const JValue& p : pats->arr )
                    {
                        const JValue* cn = p.Get( "netclass" );
                        const JValue* pt = p.Get( "pattern" );

                        if( !cn || !pt || !cn->IsString() || !pt->IsString() )
                            continue;

                        int cls = classIndex( cn->s );

                        for( size_t n = 1; n < b.nets.size(); ++n )
                        {
                            if( wildcardMatch( pt->s.c_str(), b.nets[n].c_str() ) )
                                assign( b.nets[n], cls );
                        }
                    }
                }

                if( const JValue* as = ns->Get( "netclass_assignments" ) )
                {
                    if( as->IsObject() )
                    {
                        for( const auto& [net, v] : as->obj )
                        {
                            if( v.IsString() )
                                assign( net, classIndex( v.s ) );
                            else if( v.IsArray() )
                            {
                                for( const JValue& c : v.arr )
                                {
                                    if( c.IsString() )
                                        assign( net, classIndex( c.s ) );
                                }
                            }
                        }
                    }
                }

                // KiCad 6/7: class member lists
                for( const auto& [cls, names] : explicitNets )
                {
                    for( const std::string& n : names )
                        assign( n, cls );
                }
            }
        }
        else
        {
            aNote = Tr( "The project file has no board settings; using the board's own rules." );
        }
    }

    if( !fromProject )
    {
        const LegacyRules& L = b.legacyRules;
        bool               any = !L.netClasses.empty() || L.traceMin >= 0 || L.viaMinSize >= 0;

        if( any )
            R.source = DesignRules::Source::BOARD;

        if( L.traceMin >= 0 )
            R.minTrackWidth = L.traceMin;

        if( L.viaMinSize >= 0 )
            R.minViaDiameter = L.viaMinSize;

        if( L.viaMinDrill >= 0 )
            R.minThroughHole = L.viaMinDrill;

        if( L.uviaMinSize >= 0 )
            R.minMicroviaDiameter = L.uviaMinSize;

        if( L.uviaMinDrill >= 0 )
            R.minMicroviaDrill = L.uviaMinDrill;

        if( L.holeToHoleMin >= 0 )
            R.minHoleToHole = L.holeToHoleMin;

        for( const LegacyRules::NetClass& c : L.netClasses )
        {
            NetClassRule nc = R.classes[0];
            nc.name = c.name;

            if( c.clearance >= 0 )
                nc.clearance = c.clearance;

            if( c.trackWidth >= 0 )
                nc.trackWidth = c.trackWidth;

            if( c.viaDiameter >= 0 )
                nc.viaDiameter = c.viaDiameter;

            if( c.viaDrill >= 0 )
                nc.viaDrill = c.viaDrill;

            if( c.uviaDiameter >= 0 )
                nc.uviaDiameter = c.uviaDiameter;

            if( c.uviaDrill >= 0 )
                nc.uviaDrill = c.uviaDrill;

            int idx;

            if( c.name == "Default" )
            {
                R.classes[0] = nc;
                idx = 0;
            }
            else
            {
                R.classes.push_back( nc );
                idx = (int) R.classes.size() - 1;
            }

            for( const std::string& n : c.nets )
            {
                auto it = b.netByName.find( n );

                if( it != b.netByName.end() && it->second > 0 )
                    R.netClass[(size_t) it->second] = idx;
            }
        }
    }

    return R;
}


// ------------------------------------------------------------------------------------------
// Descriptions
// ------------------------------------------------------------------------------------------

std::string DescribeItem( const Board& b, const DrcRef& r )
{
    auto netName = [&]( int n ) -> std::string
    { return n > 0 && n < (int) b.nets.size() ? b.nets[(size_t) n] : std::string( Tr( "no net" ) ); };

    auto layerName = [&]( int l ) -> std::string
    { return l >= 0 && l < (int) b.layers.size() ? b.layers[(size_t) l].display : std::string( "?" ); };

    switch( r.type )
    {
    case ItemType::PAD:
    {
        if( r.index < 0 || r.index >= (int) b.pads.size() )
            break;

        const Pad&  p = b.pads[(size_t) r.index];
        std::string ref = p.footprint >= 0 ? b.footprints[(size_t) p.footprint].reference : std::string();
        return TrF( "Pad {0} of {1} ({2})", { p.number, ref, netName( p.net ) } );
    }
    case ItemType::VIA:
        if( r.index < 0 || r.index >= (int) b.vias.size() )
            break;

        return TrF( "Via ({0})", { netName( b.vias[(size_t) r.index].net ) } );

    case ItemType::TRACK:
        if( r.index < 0 || r.index >= (int) b.tracks.size() )
            break;

        return TrF( "Track ({0}) on {1}",
                    { netName( b.tracks[(size_t) r.index].net ), layerName( b.tracks[(size_t) r.index].layer ) } );

    case ItemType::ARC:
        if( r.index < 0 || r.index >= (int) b.arcs.size() )
            break;

        return TrF( "Arc track ({0}) on {1}",
                    { netName( b.arcs[(size_t) r.index].net ), layerName( b.arcs[(size_t) r.index].layer ) } );

    case ItemType::ZONE:
    {
        if( r.index < 0 || r.index >= (int) b.zones.size() )
            break;

        const Zone& z = b.zones[(size_t) r.index];
        return TrF( "Zone ({0})", { netName( z.net ) } );
    }
    case ItemType::SHAPE:
    {
        if( r.index < 0 || r.index >= (int) b.shapes.size() )
            break;

        const Shape& s = b.shapes[(size_t) r.index];

        if( s.footprint >= 0 )
            return TrF( "Graphic of {0} on {1}", { b.footprints[(size_t) s.footprint].reference, layerName( s.layer ) } );

        return TrF( "Graphic on {0}", { layerName( s.layer ) } );
    }
    case ItemType::FOOTPRINT:
        if( r.index < 0 || r.index >= (int) b.footprints.size() )
            break;

        return b.footprints[(size_t) r.index].reference;

    default: break;
    }

    return std::string();
}


std::string DrcMessage( const Board& b, const DrcItem& it )
{
    std::string items;
    std::string da = DescribeItem( b, it.a ), db = DescribeItem( b, it.b );

    if( !da.empty() && !db.empty() )
        items = da + " / " + db;
    else
        items = da.empty() ? db : da;

    std::string detail;

    switch( it.code )
    {
    case DrcCode::CLEARANCE:
    case DrcCode::HOLE_CLEARANCE:
    case DrcCode::EDGE_CLEARANCE:
    case DrcCode::HOLE_NEAR_HOLE:
        detail = TrF( "{0} mm, needs {1} mm", { mm( it.actual ), mm( it.required ) } );
        break;

    case DrcCode::TRACK_WIDTH:
    case DrcCode::VIA_DIAMETER:
    case DrcCode::ANNULAR_WIDTH:
    case DrcCode::DRILL_OUT_OF_RANGE:
    case DrcCode::MICROVIA_DRILL_OUT_OF_RANGE:
        detail = TrF( "{0} mm, minimum {1} mm", { mm( it.actual ), mm( it.required ) } );
        break;

    case DrcCode::SHORTING_ITEMS:
    case DrcCode::UNCONNECTED_ITEMS:
        if( it.netA > 0 && it.netB > 0 && it.netA != it.netB )
            detail = TrF( "nets {0} and {1}", { b.nets[(size_t) it.netA], b.nets[(size_t) it.netB] } );
        else if( it.netA > 0 )
            detail = TrF( "net {0}", { b.nets[(size_t) it.netA] } );

        break;

    default: break;
    }

    std::string out = Tr( CODES[(int) it.code].title );

    if( !detail.empty() )
        out += ": " + detail;

    if( !items.empty() )
        out += " — " + items;

    return out;
}


// ------------------------------------------------------------------------------------------
// The checks
// ------------------------------------------------------------------------------------------

namespace
{

class Checker
{
public:
    Checker( const Board& b, const DesignRules& r, const Connectivity& c, std::vector<DrcItem>& out ) :
            m_b( b ),
            m_r( r ),
            m_conn( c ),
            m_out( out )
    {
    }

    void Run()
    {
        bool profile = getenv( "NETLOOM_DRC_PROFILE" ) != nullptr;
        auto t = std::chrono::steady_clock::now();
        auto lap = [&]( const char* what )
        {
            if( !profile )
                return;

            auto now = std::chrono::steady_clock::now();
            fprintf( stderr, "  %-16s %8.1f ms\n", what, std::chrono::duration<double, std::milli>( now - t ).count() );
            t = now;
        };

        m_idx.Build( m_b, true, true, DRC_ARC_ERR, true );
        lap( "index" );
        collectHoles();
        clearances();
        lap( "clearances" );
        holeClearances();
        lap( "holes" );
        edgeClearances();
        lap( "edge" );
        itemSizes();
        holeToHole();
        lap( "sizes+h2h" );
        courtyards();
        lap( "courtyards" );
        unconnected();
        dangling();
        lap( "dangling" );
        outline();
        lap( "outline" );
    }

private:
    bool wants( DrcCode c ) const { return m_r.severity[(int) c] != Severity::IGNORE; }

    DrcItem& add( DrcCode c, const Pt& pos, int layer = -1 )
    {
        DrcItem it;
        it.code = c;
        it.severity = m_r.severity[(int) c];
        it.pos = pos;
        it.layer = layer;
        m_out.push_back( it );
        return m_out.back();
    }

    int netOf( const CuShape& s ) const { return s.type == ItemType::SHAPE ? 0 : s.net; }

    /// Local clearance override (pads: own value, else the footprint's), 0 = none.
    double overrideOf( const CuShape& s ) const
    {
        if( s.type != ItemType::PAD )
            return 0.0;

        const Pad& p = m_b.pads[(size_t) s.index];

        if( p.localClearance > 0 )
            return p.localClearance;

        if( p.footprint >= 0 && m_b.footprints[(size_t) p.footprint].localClearance > 0 )
            return m_b.footprints[(size_t) p.footprint].localClearance;

        return 0.0;
    }

    double localOf( const CuShape& s ) const
    {
        if( s.type == ItemType::ZONE )
            return std::max( 0.0, m_b.zones[(size_t) s.index].localClearance );

        return 0.0;
    }

    double requiredClearance( const CuShape& a, const CuShape& b ) const
    {
        double oa = overrideOf( a ), ob = overrideOf( b );

        if( oa > 0 || ob > 0 )
            return std::max( { oa, ob, m_r.minClearance } );

        double c = std::max( m_r.ClassOf( netOf( a ) ).clearance, m_r.ClassOf( netOf( b ) ).clearance );
        c = std::max( { c, localOf( a ), localOf( b ) } );
        return std::max( c, m_r.minClearance );
    }

    double maxClearance() const
    {
        double c = m_r.minClearance;

        for( const NetClassRule& nc : m_r.classes )
            c = std::max( c, nc.clearance );

        for( const Pad& p : m_b.pads )
            c = std::max( c, p.localClearance );

        for( const Footprint& f : m_b.footprints )
            c = std::max( c, f.localClearance );

        for( const Zone& z : m_b.zones )
            c = std::max( c, z.localClearance );

        return c;
    }

    bool netTied( const Footprint& fp, const std::string& n1, const std::string& n2 ) const
    {
        for( const std::string& g : fp.netTieGroups )
        {
            bool has1 = false, has2 = false;
            size_t start = 0;

            while( start <= g.size() )
            {
                size_t      comma = g.find( ',', start );
                std::string tok = g.substr( start, comma == std::string::npos ? std::string::npos : comma - start );

                while( !tok.empty() && tok.front() == ' ' )
                    tok.erase( tok.begin() );

                while( !tok.empty() && tok.back() == ' ' )
                    tok.pop_back();

                has1 |= tok == n1;
                has2 |= tok == n2;

                if( comma == std::string::npos )
                    break;

                start = comma + 1;
            }

            if( has1 && has2 )
                return true;
        }

        return false;
    }

    /// Is aNet the net of one of the net-tie pads of a footprint?
    bool netOfTiePads( int aFootprint, int aNet ) const
    {
        if( aNet <= 0 )
            return false;

        for( const Pad& p : m_b.pads )
        {
            if( p.footprint == aFootprint && p.net == aNet )
                return true;
        }

        return false;
    }

    /// Closest point between two shapes, for the marker.
    Pt nearPoint( const CuShape& a, const CuShape& b ) const
    {
        double best = 1e300;
        Pt     pos = Pt( ( a.box.minX + a.box.maxX ) / 2, ( a.box.minY + a.box.maxY ) / 2 );

        for( const Prim& p : a.prims )
        {
            for( const Prim& q : b.prims )
            {
                double gx = std::max( { 0.0, p.box.minX - q.box.maxX, q.box.minX - p.box.maxX } );
                double gy = std::max( { 0.0, p.box.minY - q.box.maxY, q.box.minY - p.box.maxY } );

                if( std::max( gx, gy ) >= best )
                    continue;

                double d = PrimDistance( p, q );

                if( d < best )
                {
                    best = d;
                    pos = PrimNearPoint( p, q );
                }
            }
        }

        return pos;
    }

    // ---- copper to copper ---------------------------------------------------------------

    const std::string* uuidOf( const CuShape& s ) const
    {
        switch( s.type )
        {
        case ItemType::TRACK: return &m_b.tracks[(size_t) s.index].uuid;
        case ItemType::ARC: return &m_b.arcs[(size_t) s.index].uuid;
        case ItemType::VIA: return &m_b.vias[(size_t) s.index].uuid;
        default: return nullptr;
        }
    }

    /// The order tracks are checked in (KiCad: by UUID), so each pair is looked at once.
    bool before( const CuShape& a, const CuShape& b ) const
    {
        const std::string* ua = uuidOf( a );
        const std::string* ub = uuidOf( b );

        if( ua && ub && !ua->empty() && !ub->empty() && *ua != *ub )
            return *ua < *ub;

        if( a.type != b.type )
            return a.type < b.type;

        return a.index < b.index;
    }

    void clearances()
    {
        if( !wants( DrcCode::CLEARANCE ) && !wants( DrcCode::SHORTING_ITEMS ) && !wants( DrcCode::TRACKS_CROSSING ) )
            return;

        const std::vector<CuShape>& shapes = m_idx.Shapes();
        double                      reach = maxClearance();
        std::unordered_set<uint64_t> done;
        std::vector<int>             near;

        for( size_t i = 0; i < shapes.size(); ++i )
        {
            const CuShape& A = shapes[i];
            BBox           box = A.box;
            box.minX -= reach;
            box.minY -= reach;
            box.maxX += reach;
            box.maxY += reach;
            m_idx.Query( A.layer, box, near );

            if( isTrackish( A.type ) )
            {
                // A track or via reports its first problem with other copper on each layer
                // (as KiCad does unless asked to report all track errors), and every zone it
                // runs into.
                bool reported = false;

                for( int j : near )
                {
                    const CuShape& B = shapes[(size_t) j];

                    if( ( A.type == B.type && A.index == B.index ) || B.type == ItemType::ZONE )
                        continue;

                    if( isTrackish( B.type ) && !before( A, B ) )
                        continue;

                    if( ( !reported || m_r.reportAllTrackErrors ) && checkPair( A, B, nullptr ) )
                        reported = true;
                }

                for( int j : near )
                {
                    const CuShape& B = shapes[(size_t) j];

                    if( B.type == ItemType::ZONE )
                        checkPair( A, B, nullptr );
                }

                continue;
            }

            for( int j : near )
            {
                if( j <= (int) i )
                    continue;

                const CuShape& B = shapes[(size_t) j];

                if( isTrackish( B.type ) || ( A.type == B.type && A.index == B.index ) )
                    continue;

                checkPair( A, B, &done );
            }
        }
    }

    /// Check one pair; true if a violation was reported. done: pairs already seen (or null).
    bool checkPair( const CuShape& A0, const CuShape& B0, std::unordered_set<uint64_t>* done )
    {
        const CuShape* A = &A0;
        const CuShape* B = &B0;
        int            na = netOf( *A ), nb = netOf( *B );
        bool           zone = A->type == ItemType::ZONE || B->type == ItemType::ZONE;
        bool           graphic = A->type == ItemType::SHAPE || B->type == ItemType::SHAPE;
        bool           shortOnly = false;

        if( A->type == ItemType::ZONE && B->type == ItemType::ZONE )
        {
            if( na == nb )
                return false;
        }
        else if( zone )
        {
            const CuShape& z = A->type == ItemType::ZONE ? *A : *B;
            const CuShape& o = A->type == ItemType::ZONE ? *B : *A;

            if( z.net != 0 && o.type != ItemType::SHAPE && z.net == netOf( o ) )
                return false;
        }
        else if( graphic )
        {
            if( A->type == ItemType::SHAPE && B->type == ItemType::SHAPE )
            {
                if( A->footprint >= 0 && A->footprint == B->footprint )
                    return false;
            }
            else
            {
                const CuShape& g = A->type == ItemType::SHAPE ? *A : *B;
                const CuShape& o = A->type == ItemType::SHAPE ? *B : *A;

                if( o.type == ItemType::PAD && o.footprint >= 0 && o.footprint == g.footprint
                    && !m_b.footprints[(size_t) o.footprint].netTieGroups.empty() )
                    return false;

                // copper of a net tie may touch the nets it ties
                if( g.footprint >= 0 && !m_b.footprints[(size_t) g.footprint].netTieGroups.empty()
                    && netOfTiePads( g.footprint, netOf( o ) ) )
                    return false;
            }
        }
        else if( isTrackish( A->type ) || isTrackish( B->type ) )
        {
            if( na == nb )
                return false;
        }
        else if( A->type == ItemType::PAD && B->type == ItemType::PAD )
        {
            if( nb != 0 && na == nb )
                return false;

            const Pad& pa = m_b.pads[(size_t) A->index];
            const Pad& pb = m_b.pads[(size_t) B->index];

            if( pa.footprint >= 0 && pa.footprint == pb.footprint )
            {
                const Footprint& fp = m_b.footprints[(size_t) pa.footprint];

                if( netTied( fp, pa.number, pb.number ) )
                    return false;

                if( pa.number == pb.number && !pa.number.empty() )
                {
                    // the same pin twice (e.g. a stacked pad): only a real net mismatch matters
                    if( pa.net == 0 || pb.net == 0 || pa.net == pb.net )
                        return false;

                    auto unconnected = [&]( int n ) { return m_b.nets[(size_t) n].rfind( "unconnected-(", 0 ) == 0; };

                    if( unconnected( pa.net ) && unconnected( pb.net ) )
                        return false;

                    shortOnly = true;
                }
            }
        }

        int      keyLayer = zone ? A->layer : -1;
        uint64_t key = pairKey( *A, *B, keyLayer );

        if( done && done->count( key ) )
            return false;

        double required = shortOnly ? EPS * 2 : requiredClearance( *A, *B );

        if( required <= 0 )
            return false;

        // tracks whose centre lines cross
        if( A->type == ItemType::TRACK && B->type == ItemType::TRACK )
        {
            const Track& ta = m_b.tracks[(size_t) A->index];
            const Track& tb = m_b.tracks[(size_t) B->index];

            if( properCross( ta.start, ta.end, tb.start, tb.end ) )
            {
                if( done )
                    done->insert( key );

                if( wants( DrcCode::TRACKS_CROSSING ) )
                {
                    DrcItem& it = add( DrcCode::TRACKS_CROSSING, crossPoint( ta.start, ta.end, tb.start, tb.end ), A->layer );
                    it.a = { A->type, A->index };
                    it.b = { B->type, B->index };
                    it.netA = na;
                    it.netB = nb;
                }

                return true;
            }
        }

        double limit = std::max( 0.0, required - EPS );
        double d;

        if( A->type == ItemType::ZONE && B->type == ItemType::ZONE )
        {
            // zone fills are compared edge to edge, without the epsilon (as KiCad)
            limit = required;
            d = 1e300;

            for( const Prim& p : A->prims )
            {
                for( const Prim& q : B->prims )
                {
                    double gx = std::max( { 0.0, p.box.minX - q.box.maxX, q.box.minX - p.box.maxX } );
                    double gy = std::max( { 0.0, p.box.minY - q.box.maxY, q.box.minY - p.box.maxY } );

                    if( std::max( gx, gy ) < std::min( d, limit ) )
                        d = std::min( d, PrimDistance( p, q ) );
                }
            }
        }
        else
        {
            d = ShapeDistanceTo( *A, *B, limit + 1e-9 );
        }

        if( d >= limit && d > 0 )
            return false;

        if( done )
            done->insert( key );
        DrcCode code = d <= 0 ? DrcCode::SHORTING_ITEMS : DrcCode::CLEARANCE;

        if( shortOnly && code != DrcCode::SHORTING_ITEMS )
            return false;

        if( !wants( code ) )
            return false;

        DrcItem& it = add( code, nearPoint( *A, *B ), A->layer );
        it.a = { A->type, A->index };
        it.b = { B->type, B->index };
        it.actual = std::max( 0.0, d );
        it.required = required;
        it.netA = na;
        it.netB = nb;
        return true;
    }

    static double cross( const Pt& o, const Pt& a, const Pt& b )
    {
        return ( a.x - o.x ) * ( b.y - o.y ) - ( a.y - o.y ) * ( b.x - o.x );
    }

    static bool properCross( const Pt& a1, const Pt& a2, const Pt& b1, const Pt& b2 )
    {
        double d1 = cross( b1, b2, a1 ), d2 = cross( b1, b2, a2 );
        double d3 = cross( a1, a2, b1 ), d4 = cross( a1, a2, b2 );

        if( ( ( d1 > 0 && d2 < 0 ) || ( d1 < 0 && d2 > 0 ) ) && ( ( d3 > 0 && d4 < 0 ) || ( d3 < 0 && d4 > 0 ) ) )
            return true;

        // touching end to segment also counts as an intersection (KiCad SEG::Intersect)
        auto on = [&]( const Pt& p, const Pt& s1, const Pt& s2 ) { return PointSegmentDistance( p, s1, s2 ) < 1e-9; };
        return on( a1, b1, b2 ) || on( a2, b1, b2 ) || on( b1, a1, a2 ) || on( b2, a1, a2 );
    }

    static Pt crossPoint( const Pt& a1, const Pt& a2, const Pt& b1, const Pt& b2 )
    {
        double den = ( a2.x - a1.x ) * ( b2.y - b1.y ) - ( a2.y - a1.y ) * ( b2.x - b1.x );

        if( std::fabs( den ) < 1e-12 )
            return a1;

        double t = ( ( b1.x - a1.x ) * ( b2.y - b1.y ) - ( b1.y - a1.y ) * ( b2.x - b1.x ) ) / den;
        return Pt( a1.x + t * ( a2.x - a1.x ), a1.y + t * ( a2.y - a1.y ) );
    }

    // ---- holes ----------------------------------------------------------------------------

    void collectHoles()
    {
        for( size_t i = 0; i < m_b.pads.size(); ++i )
        {
            const Pad& p = m_b.pads[i];

            if( p.drill.x <= 0 && p.drill.y <= 0 )
                continue;

            if( p.attr == PadAttr::SMD || p.attr == PadAttr::CONN )
                continue;

            double dx = std::fabs( p.drill.x ), dy = std::fabs( p.drill.y > 0 ? p.drill.y : p.drill.x );
            Hole   h { ItemType::PAD, (int) i, p.pos, p.pos, std::min( dx, dy ) / 2, p.net, p.footprint };

            if( std::fabs( dx - dy ) > 1e-9 )
            {
                Pt half = dx > dy ? Pt( ( dx - dy ) / 2, 0 ) : Pt( 0, ( dy - dx ) / 2 );
                h.a = p.pos - RotatePt( half, p.angle );
                h.b = p.pos + RotatePt( half, p.angle );
            }

            h.orderA = 0;
            h.orderB = (int) m_b.copperStack.size() - 1;
            m_holes.push_back( h );
        }

        for( size_t i = 0; i < m_b.vias.size(); ++i )
        {
            const Via& v = m_b.vias[i];

            if( v.drill <= 0 )
                continue;

            Hole h { ItemType::VIA, (int) i, v.pos, v.pos, v.drill / 2, v.net, -1 };
            h.micro = v.type == ViaType::MICRO;
            h.orderA = 0;
            h.orderB = (int) m_b.copperStack.size() - 1;

            if( isCu( m_b, v.layerA ) && isCu( m_b, v.layerB ) )
            {
                h.orderA = m_b.layers[(size_t) v.layerA].copperOrder;
                h.orderB = m_b.layers[(size_t) v.layerB].copperOrder;

                if( h.orderA > h.orderB )
                    std::swap( h.orderA, h.orderB );
            }

            m_holes.push_back( h );
        }
    }

    void holeClearances()
    {
        if( !wants( DrcCode::HOLE_CLEARANCE ) )
            return;

        double maxOverride = 0;

        for( const Pad& p : m_b.pads )
            maxOverride = std::max( maxOverride, p.localClearance );

        for( const Footprint& f : m_b.footprints )
            maxOverride = std::max( maxOverride, f.localClearance );

        double reach = std::max( m_r.minHoleClearance, maxOverride );

        if( reach <= 0 )
            return;

        std::unordered_set<uint64_t> done;
        std::vector<int>             near;

        for( const Hole& h : m_holes )
        {
            CuShape hs;
            hs.type = h.type;
            hs.index = h.index;
            Prim pr;
            pr.kind = std::hypot( h.b.x - h.a.x, h.b.y - h.a.y ) < 1e-9 ? Prim::POINT : Prim::SEGMENT;
            pr.a = h.a;
            pr.b = h.b;
            pr.r = h.r;
            pr.Finish();
            hs.prims.push_back( pr );
            hs.Finish();

            BBox box = hs.box;
            box.minX -= reach;
            box.minY -= reach;
            box.maxX += reach;
            box.maxY += reach;

            for( int o = h.orderA; o <= h.orderB && o < (int) m_b.copperStack.size(); ++o )
            {
                int layer = m_b.copperStack[(size_t) o];

                if( h.type == ItemType::PAD )
                {
                    const Pad& p = m_b.pads[(size_t) h.index];

                    if( std::find( p.layers.begin(), p.layers.end(), layer ) == p.layers.end() )
                        continue;
                }

                m_idx.Query( layer, box, near );

                for( int id : near )
                {
                    const CuShape& s = m_idx.Shapes()[(size_t) id];

                    if( s.type == h.type && s.index == h.index )
                        continue;

                    int on = netOf( s );

                    if( s.type == ItemType::ZONE )
                    {
                        if( s.net != 0 && s.net == h.net )
                            continue;
                    }
                    else if( s.type == ItemType::PAD && h.type == ItemType::PAD )
                    {
                        if( on != 0 && on == h.net )
                            continue;

                        const Pad& a = m_b.pads[(size_t) h.index];
                        const Pad& c = m_b.pads[(size_t) s.index];

                        if( a.footprint >= 0 && a.footprint == c.footprint && a.number == c.number )
                            continue; // the same logical pad
                    }
                    else if( s.type != ItemType::SHAPE && on == h.net )
                    {
                        continue;
                    }

                    uint64_t key = pairKey( hs, s, -1 );

                    if( done.count( key ) )
                        continue;

                    double ov = std::max( s.type == ItemType::PAD ? overrideOf( s ) : 0.0,
                                          h.type == ItemType::PAD ? overrideOf( hs ) : 0.0 );
                    double req = ov > 0 ? std::max( ov, m_r.minHoleClearance ) : m_r.minHoleClearance;

                    if( req <= 0 )
                        continue;

                    double limit = std::max( 0.0, req - EPS );
                    double d = ShapeDistanceTo( hs, s, limit + 1e-9 );

                    if( d >= limit && d > 0 )
                        continue;

                    done.insert( key );
                    DrcItem& it = add( DrcCode::HOLE_CLEARANCE, nearPoint( hs, s ), layer );
                    it.a = { h.type, h.index };
                    it.b = { s.type, s.index };
                    it.actual = std::max( 0.0, d );
                    it.required = req;
                    it.netA = h.net;
                    it.netB = on;
                }
            }
        }
    }

    void holeToHole()
    {
        bool near = wants( DrcCode::HOLE_NEAR_HOLE ), colo = wants( DrcCode::HOLES_CO_LOCATED );

        if( ( !near && !colo ) || m_holes.empty() )
            return;

        double reach = std::max( m_r.minHoleToHole, 0.0 );
        double maxR = 0;

        for( const Hole& h : m_holes )
            maxR = std::max( maxR, h.r + std::hypot( h.b.x - h.a.x, h.b.y - h.a.y ) );

        // grid over hole centres
        double cell = std::max( 1.0, 2 * maxR + reach );
        std::unordered_map<uint64_t, std::vector<int>> grid;

        auto keyOf = [&]( long x, long y ) { return ( (uint64_t) (uint32_t) x << 32 ) | (uint32_t) y; };

        for( size_t i = 0; i < m_holes.size(); ++i )
        {
            const Hole& h = m_holes[i];
            grid[keyOf( (long) std::floor( h.a.x / cell ), (long) std::floor( h.a.y / cell ) )].push_back( (int) i );
        }

        for( size_t i = 0; i < m_holes.size(); ++i )
        {
            const Hole& h = m_holes[i];
            long        cx = (long) std::floor( h.a.x / cell ), cy = (long) std::floor( h.a.y / cell );

            for( long y = cy - 1; y <= cy + 1; ++y )
            {
                for( long x = cx - 1; x <= cx + 1; ++x )
                {
                    auto it = grid.find( keyOf( x, y ) );

                    if( it == grid.end() )
                        continue;

                    for( int j : it->second )
                    {
                        if( j <= (int) i )
                            continue;

                        const Hole& o = m_holes[(size_t) j];

                        if( h.micro && o.micro )
                            continue;

                        if( h.type == ItemType::VIA && o.type == ItemType::VIA
                            && ( h.orderB < o.orderA || o.orderB < h.orderA ) )
                            continue;

                        if( std::hypot( h.a.x - o.a.x, h.a.y - o.a.y ) < EPS
                            && std::hypot( h.b.x - o.b.x, h.b.y - o.b.y ) < EPS )
                        {
                            if( colo )
                            {
                                DrcItem& d = add( DrcCode::HOLES_CO_LOCATED, h.a );
                                d.a = { h.type, h.index };
                                d.b = { o.type, o.index };
                            }

                            continue;
                        }

                        if( !near || reach <= 0 )
                            continue;

                        double axis = SegSegDistance( h.a, h.b, o.a, o.b );
                        double gap = std::max( 0.0, axis - h.r - o.r );

                        if( gap < std::max( 0.0, reach - EPS ) )
                        {
                            DrcItem& d = add( DrcCode::HOLE_NEAR_HOLE, Pt( ( h.a.x + o.a.x ) / 2, ( h.a.y + o.a.y ) / 2 ) );
                            d.a = { h.type, h.index };
                            d.b = { o.type, o.index };
                            d.actual = gap;
                            d.required = reach;
                        }
                    }
                }
            }
        }
    }

    // ---- board edge -----------------------------------------------------------------------

    void edgePrims( std::vector<Prim>& out ) const
    {
        for( const Shape& s0 : m_b.shapes )
        {
            if( s0.layer < 0 || s0.layer >= (int) m_b.layers.size() || s0.decoration )
                continue;

            LayerKind k = m_b.layers[(size_t) s0.layer].kind;

            if( k != LayerKind::EDGE && k != LayerKind::MARGIN )
                continue;

            Shape s = s0;
            s.width = 0; // edges are lines
            s.filled = false;
            ShapePrims( s, out, DRC_ARC_ERR * 5 );
        }
    }

    void edgeClearances()
    {
        if( !wants( DrcCode::EDGE_CLEARANCE ) )
            return;

        std::vector<Prim> edges;
        edgePrims( edges );

        if( edges.empty() )
            return;

        double req = m_r.copperEdgeClearance;

        if( req < 0 )
            req = 0;

        // grid over edge pieces
        BBox all;

        for( const Prim& e : edges )
            all.add( e.box );

        double cell = std::max( 1.0, std::sqrt( std::max( 1.0, ( all.maxX - all.minX ) * ( all.maxY - all.minY ) )
                                                / (double) edges.size() ) * 2 );
        std::unordered_map<uint64_t, std::vector<int>> grid;
        auto keyOf = [&]( long x, long y ) { return ( (uint64_t) (uint32_t) x << 32 ) | (uint32_t) y; };

        for( size_t i = 0; i < edges.size(); ++i )
        {
            const BBox& bb = edges[i].box;

            for( long y = (long) std::floor( bb.minY / cell ); y <= (long) std::floor( bb.maxY / cell ); ++y )
            {
                for( long x = (long) std::floor( bb.minX / cell ); x <= (long) std::floor( bb.maxX / cell ); ++x )
                    grid[keyOf( x, y )].push_back( (int) i );
            }
        }

        std::unordered_set<uint64_t> done;
        std::vector<int>             stamp( edges.size(), -1 );
        int                          stampValue = 0;
        double                       limit = std::max( 0.0, req - EPS );

        for( const CuShape& s : m_idx.Shapes() )
        {
            if( s.type == ItemType::PAD )
            {
                const Pad& p = m_b.pads[(size_t) s.index];

                if( p.castellated || p.attr == PadAttr::CONN )
                    continue;
            }

            // one report per item (per layer for zones)
            uint64_t key = ( (uint64_t) s.type << 40 ) ^ ( (uint64_t) s.index << 8 )
                           ^ ( s.type == ItemType::ZONE ? (uint64_t) ( s.layer + 1 ) : 0 );

            if( done.count( key ) )
                continue;

            BBox box = s.box;
            box.minX -= req;
            box.minY -= req;
            box.maxX += req;
            box.maxY += req;

            if( box.maxX < all.minX || box.minX > all.maxX || box.maxY < all.minY || box.minY > all.maxY )
                continue;

            ++stampValue;
            double best = 1e300;
            Pt     where;

            for( long y = (long) std::floor( box.minY / cell ); y <= (long) std::floor( box.maxY / cell ); ++y )
            {
                for( long x = (long) std::floor( box.minX / cell ); x <= (long) std::floor( box.maxX / cell ); ++x )
                {
                    auto it = grid.find( keyOf( x, y ) );

                    if( it == grid.end() )
                        continue;

                    for( int e : it->second )
                    {
                        if( stamp[(size_t) e] == stampValue )
                            continue;

                        stamp[(size_t) e] = stampValue;
                        const Prim& ep = edges[(size_t) e];

                        for( const Prim& p : s.prims )
                        {
                            double gx = std::max( { 0.0, p.box.minX - ep.box.maxX, ep.box.minX - p.box.maxX } );
                            double gy = std::max( { 0.0, p.box.minY - ep.box.maxY, ep.box.minY - p.box.maxY } );

                            if( std::max( gx, gy ) > std::min( best, limit ) )
                                continue;

                            double d = PrimDistance( p, ep );

                            if( d < best )
                            {
                                best = d;
                                where = PrimNearPoint( p, ep );
                            }
                        }
                    }
                }
            }

            if( best < limit || best <= 0 )
            {
                done.insert( key );
                DrcItem& it = add( DrcCode::EDGE_CLEARANCE, where, s.layer );
                it.a = { s.type, s.index };
                it.actual = std::max( 0.0, best );
                it.required = req;
                it.netA = netOf( s );
            }
        }
    }

    // ---- sizes ----------------------------------------------------------------------------

    void itemSizes()
    {
        if( wants( DrcCode::TRACK_WIDTH ) && m_r.minTrackWidth > 0 )
        {
            for( size_t i = 0; i < m_b.tracks.size(); ++i )
            {
                const Track& t = m_b.tracks[i];

                if( isCu( m_b, t.layer ) && t.width < m_r.minTrackWidth - EPS )
                {
                    DrcItem& it = add( DrcCode::TRACK_WIDTH, ( t.start + t.end ) * 0.5, t.layer );
                    it.a = { ItemType::TRACK, (int) i };
                    it.actual = t.width;
                    it.required = m_r.minTrackWidth;
                    it.netA = t.net;
                }
            }

            for( size_t i = 0; i < m_b.arcs.size(); ++i )
            {
                const ArcTrack& t = m_b.arcs[i];

                if( isCu( m_b, t.layer ) && t.width < m_r.minTrackWidth - EPS )
                {
                    DrcItem& it = add( DrcCode::TRACK_WIDTH, t.mid, t.layer );
                    it.a = { ItemType::ARC, (int) i };
                    it.actual = t.width;
                    it.required = m_r.minTrackWidth;
                    it.netA = t.net;
                }
            }
        }

        for( size_t i = 0; i < m_b.vias.size(); ++i )
        {
            const Via& v = m_b.vias[i];
            bool       micro = v.type == ViaType::MICRO;
            double     size = v.size;

            for( const auto& [l, s] : v.sizeOverride )
            {
                (void) l;
                size = std::min( size, s );
            }

            if( v.innerSize > 0 )
                size = std::min( size, v.innerSize );

            double minSize = micro ? m_r.minMicroviaDiameter : m_r.minViaDiameter;

            if( wants( DrcCode::VIA_DIAMETER ) && minSize > 0 && size < minSize - EPS )
            {
                DrcItem& it = add( DrcCode::VIA_DIAMETER, v.pos );
                it.a = { ItemType::VIA, (int) i };
                it.actual = size;
                it.required = minSize;
                it.netA = v.net;
            }

            double ring = ( size - v.drill ) / 2;

            if( wants( DrcCode::ANNULAR_WIDTH ) && m_r.minViaAnnular > 0 && v.drill > 0
                && ring < m_r.minViaAnnular - EPS )
            {
                DrcItem& it = add( DrcCode::ANNULAR_WIDTH, v.pos );
                it.a = { ItemType::VIA, (int) i };
                it.actual = std::max( 0.0, ring );
                it.required = m_r.minViaAnnular;
                it.netA = v.net;
            }

            DrcCode dc = micro ? DrcCode::MICROVIA_DRILL_OUT_OF_RANGE : DrcCode::DRILL_OUT_OF_RANGE;
            double  minDrill = micro ? m_r.minMicroviaDrill : m_r.minThroughHole;

            if( wants( dc ) && minDrill > 0 && v.drill > 0 && v.drill < minDrill - EPS )
            {
                DrcItem& it = add( dc, v.pos );
                it.a = { ItemType::VIA, (int) i };
                it.actual = v.drill;
                it.required = minDrill;
                it.netA = v.net;
            }
        }

        for( size_t i = 0; i < m_b.pads.size(); ++i )
        {
            const Pad& p = m_b.pads[i];

            if( p.attr != PadAttr::PTH && p.attr != PadAttr::NPTH )
                continue;

            double dx = std::fabs( p.drill.x ), dy = std::fabs( p.drill.y > 0 ? p.drill.y : p.drill.x );

            if( dx <= 0 )
                continue;

            if( wants( DrcCode::DRILL_OUT_OF_RANGE ) && m_r.minThroughHole > 0
                && std::min( dx, dy ) < m_r.minThroughHole - EPS )
            {
                DrcItem& it = add( DrcCode::DRILL_OUT_OF_RANGE, p.pos );
                it.a = { ItemType::PAD, (int) i };
                it.actual = std::min( dx, dy );
                it.required = m_r.minThroughHole;
                it.netA = p.net;
            }

            if( p.attr != PadAttr::PTH || !wants( DrcCode::ANNULAR_WIDTH ) || m_r.minViaAnnular <= 0 )
                continue;

            // ring left around the hole on the narrowest side (simple shapes only)
            const PadGeom& g = p.geom;
            double         sx = std::fabs( g.size.x ), sy = std::fabs( g.size.y );
            double         ring;

            if( g.shape == PadShape::CIRCLE )
                ring = sx / 2 - std::hypot( g.offset.x, g.offset.y ) - std::max( dx, dy ) / 2;
            else if( g.shape == PadShape::OVAL || g.shape == PadShape::RECT || g.shape == PadShape::ROUNDRECT )
                ring = std::min( sx / 2 - std::fabs( g.offset.x ) - dx / 2, sy / 2 - std::fabs( g.offset.y ) - dy / 2 );
            else
                continue;

            if( sx <= dx && sy <= dy )
                continue; // no copper ring at all: a plated slot or mechanical pad

            if( ring < m_r.minViaAnnular - EPS )
            {
                DrcItem& it = add( DrcCode::ANNULAR_WIDTH, p.pos );
                it.a = { ItemType::PAD, (int) i };
                it.actual = std::max( 0.0, ring );
                it.required = m_r.minViaAnnular;
                it.netA = p.net;
            }
        }
    }

    // ---- courtyards -----------------------------------------------------------------------

    /// Chain loose outline pieces into closed polygons. Returns false (and the gap) if open.
    static bool chainOutlines( std::vector<Path> pieces, std::vector<Path>& closed, Pt* aGap )
    {
        closed.clear();

        while( !pieces.empty() )
        {
            Path cur = std::move( pieces.back() );
            pieces.pop_back();

            auto near = []( const Pt& a, const Pt& b )
            { return std::hypot( a.x - b.x, a.y - b.y ) <= CHAIN_EPS; };

            bool grew = true;

            while( !near( cur.front(), cur.back() ) && grew )
            {
                grew = false;

                for( size_t i = 0; i < pieces.size(); ++i )
                {
                    Path& p = pieces[i];

                    if( near( cur.back(), p.front() ) )
                        cur.insert( cur.end(), p.begin() + 1, p.end() );
                    else if( near( cur.back(), p.back() ) )
                        cur.insert( cur.end(), p.rbegin() + 1, p.rend() );
                    else if( near( cur.front(), p.back() ) )
                        cur.insert( cur.begin(), p.begin(), p.end() - 1 );
                    else if( near( cur.front(), p.front() ) )
                        cur.insert( cur.begin(), p.rbegin(), p.rend() - 1 );
                    else
                        continue;

                    pieces.erase( pieces.begin() + (long) i );
                    grew = true;
                    break;
                }
            }

            if( !near( cur.front(), cur.back() ) || cur.size() < 3 )
            {
                if( aGap )
                    *aGap = cur.back();

                return false;
            }

            cur.pop_back();
            closed.push_back( std::move( cur ) );
        }

        return true;
    }

    /// Outline pieces of shapes on a layer kind (closed shapes become one closed piece).
    void outlinePieces( int aFootprint, bool aAnyFootprint, LayerKind aKind, int aSide, std::vector<Path>& out,
                        int* aCount = nullptr )
    {
        if( m_shapesByFp.empty() && !m_b.footprints.empty() )
        {
            m_shapesByFp.resize( m_b.footprints.size() );

            for( size_t i = 0; i < m_b.shapes.size(); ++i )
            {
                int f = m_b.shapes[i].footprint;

                if( f >= 0 && f < (int) m_shapesByFp.size() )
                    m_shapesByFp[(size_t) f].push_back( (int) i );
            }
        }

        static const std::vector<int> none;
        std::vector<int>              all;

        if( aAnyFootprint )
        {
            all.resize( m_b.shapes.size() );

            for( size_t i = 0; i < all.size(); ++i )
                all[i] = (int) i;
        }

        const std::vector<int>& list = aAnyFootprint ? all
                                       : ( aFootprint >= 0 && aFootprint < (int) m_shapesByFp.size()
                                                   ? m_shapesByFp[(size_t) aFootprint]
                                                   : none );

        for( int si : list )
        {
            const Shape& s = m_b.shapes[(size_t) si];

            if( s.decoration )
                continue;

            if( s.layer < 0 || s.layer >= (int) m_b.layers.size() )
                continue;

            const Layer& L = m_b.layers[(size_t) s.layer];

            if( L.kind != aKind || ( aSide != 0 && L.side != aSide ) )
                continue;

            if( aCount )
                ++*aCount;

            Path p;

            switch( s.type )
            {
            case ShapeType::SEGMENT: p = { s.start, s.end }; break;
            case ShapeType::ARC: ArcToPoints( p, s.start, s.mid, s.end, 0.005 ); break;
            case ShapeType::CIRCLE:
            {
                double r = std::hypot( s.end.x - s.start.x, s.end.y - s.start.y );
                int    n = std::max( 16, CircleSegments( r, 0.005 ) );

                for( int k = 0; k <= n; ++k )
                    p.push_back( Pt( s.start.x + r * std::cos( 2 * PI_ * k / n ), s.start.y + r * std::sin( 2 * PI_ * k / n ) ) );

                break;
            }
            case ShapeType::RECT:
                p = { s.start, Pt( s.end.x, s.start.y ), s.end, Pt( s.start.x, s.end.y ), s.start };
                break;
            case ShapeType::POLY:
                p = s.poly;

                if( !p.empty() )
                    p.push_back( p.front() );

                break;
            case ShapeType::BEZIER: p = s.poly; break;
            }

            if( p.size() >= 2 )
                out.push_back( std::move( p ) );
        }
    }

    static double boundaryDistance( const Pt& p, const Path& poly )
    {
        double d = 1e300;

        for( size_t i = 0; i < poly.size(); ++i )
            d = std::min( d, PointSegmentDistance( p, poly[i], poly[( i + 1 ) % poly.size()] ) );

        return d;
    }

    static bool strictlyInside( const Pt& p, const Path& poly )
    {
        return poly.size() >= 3 && PointInPolygon( p, poly ) && boundaryDistance( p, poly ) > 1e-6;
    }

    static bool strictCross( const Pt& a1, const Pt& a2, const Pt& b1, const Pt& b2 )
    {
        const double e = 1e-12;
        double d1 = cross( b1, b2, a1 ), d2 = cross( b1, b2, a2 );
        double d3 = cross( a1, a2, b1 ), d4 = cross( a1, a2, b2 );
        return ( ( d1 > e && d2 < -e ) || ( d1 < -e && d2 > e ) ) && ( ( d3 > e && d4 < -e ) || ( d3 < -e && d4 > e ) );
    }

    /// Do two sets of closed outlines overlap with some area? (touching does not count)
    static bool polysTouch( const std::vector<Path>& A, const std::vector<Path>& B, Pt& where )
    {
        auto probe = [&]( const Path& a, const Path& b ) -> bool
        {
            Pt centre;

            for( size_t i = 0; i < a.size(); ++i )
            {
                const Pt& p = a[i];
                const Pt& q = a[( i + 1 ) % a.size()];
                centre = centre + p;

                if( strictlyInside( p, b ) )
                {
                    where = p;
                    return true;
                }

                Pt mid = ( p + q ) * 0.5;

                if( strictlyInside( mid, b ) )
                {
                    where = mid;
                    return true;
                }
            }

            if( !a.empty() )
            {
                centre = centre * ( 1.0 / (double) a.size() );

                if( strictlyInside( centre, a ) && strictlyInside( centre, b ) )
                {
                    where = centre;
                    return true;
                }
            }

            return false;
        };

        for( const Path& a : A )
        {
            for( const Path& b : B )
            {
                for( size_t i = 0; i < a.size(); ++i )
                {
                    const Pt& a1 = a[i];
                    const Pt& a2 = a[( i + 1 ) % a.size()];

                    for( size_t j = 0; j < b.size(); ++j )
                    {
                        const Pt& b1 = b[j];
                        const Pt& b2 = b[( j + 1 ) % b.size()];

                        if( strictCross( a1, a2, b1, b2 ) )
                        {
                            where = crossPoint( a1, a2, b1, b2 );
                            return true;
                        }
                    }
                }

                if( probe( a, b ) || probe( b, a ) )
                    return true;
            }
        }

        return false;
    }

    void courtyards()
    {
        bool overlap = wants( DrcCode::COURTYARDS_OVERLAP ), malformed = wants( DrcCode::MALFORMED_COURTYARD );

        if( !overlap && !malformed )
            return;

        struct Cy
        {
            std::vector<Path> front, back;
            BBox              fb, bb;
        };

        std::vector<Cy> cys( m_b.footprints.size() );

        for( size_t f = 0; f < m_b.footprints.size(); ++f )
        {
            for( int side : { 1, -1 } )
            {
                std::vector<Path> pieces, closed;
                outlinePieces( (int) f, false, LayerKind::COURTYARD, side, pieces );

                if( pieces.empty() )
                    continue;

                Pt gap;

                if( !chainOutlines( pieces, closed, &gap ) )
                {
                    if( malformed )
                    {
                        DrcItem& it = add( DrcCode::MALFORMED_COURTYARD, gap );
                        it.a = { ItemType::FOOTPRINT, (int) f };
                    }

                    continue;
                }

                std::vector<Path>& dst = side > 0 ? cys[f].front : cys[f].back;
                BBox&              box = side > 0 ? cys[f].fb : cys[f].bb;
                dst = closed;

                for( const Path& p : dst )
                {
                    for( const Pt& q : p )
                        box.add( q );
                }
            }
        }

        if( !overlap )
            return;

        auto boxes = []( const BBox& a, const BBox& b )
        { return a.valid() && b.valid() && a.minX <= b.maxX && b.minX <= a.maxX && a.minY <= b.maxY && b.minY <= a.maxY; };

        for( size_t i = 0; i < cys.size(); ++i )
        {
            for( size_t j = i + 1; j < cys.size(); ++j )
            {
                Pt   where;
                bool hit = ( boxes( cys[i].fb, cys[j].fb ) && polysTouch( cys[i].front, cys[j].front, where ) )
                           || ( boxes( cys[i].bb, cys[j].bb ) && polysTouch( cys[i].back, cys[j].back, where ) );

                if( hit )
                {
                    DrcItem& it = add( DrcCode::COURTYARDS_OVERLAP, where );
                    it.a = { ItemType::FOOTPRINT, (int) i };
                    it.b = { ItemType::FOOTPRINT, (int) j };
                }
            }
        }
    }

    // ---- connectivity ---------------------------------------------------------------------

    void unconnected()
    {
        if( !wants( DrcCode::UNCONNECTED_ITEMS ) )
            return;

        for( const Airwire& a : m_conn.airwires )
        {
            DrcItem& it = add( DrcCode::UNCONNECTED_ITEMS, a.a );
            it.netA = a.net;
            it.actual = std::hypot( a.b.x - a.a.x, a.b.y - a.a.y );

            // the pads at the ends, when there are pads there
            for( int end = 0; end < 2; ++end )
            {
                const Pt& p = end == 0 ? a.a : a.b;
                DrcRef&   r = end == 0 ? it.a : it.b;

                for( size_t k = 0; k < m_b.pads.size(); ++k )
                {
                    const Pad& pad = m_b.pads[k];

                    if( pad.net == a.net && std::hypot( pad.pos.x - p.x, pad.pos.y - p.y ) < 1e-4 )
                    {
                        r = { ItemType::PAD, (int) k };
                        break;
                    }
                }
            }

            it.required = -1;
            m_airwireEnds.push_back( a.b );
        }
    }

    void dangling()
    {
        bool tracks = wants( DrcCode::TRACK_DANGLING ), vias = wants( DrcCode::VIA_DANGLING );

        if( !tracks && !vias )
            return;

        const std::vector<CuShape>& shapes = m_idx.Shapes();
        std::vector<int>            near;

        auto connectedKind = []( ItemType t )
        { return t == ItemType::PAD || t == ItemType::VIA || t == ItemType::TRACK || t == ItemType::ARC || t == ItemType::ZONE; };

        auto pointDist = [&]( const CuShape& s, const Pt& p ) -> double
        {
            if( s.type == ItemType::ZONE )
            {
                for( const AreaIndex* a : s.areas )
                {
                    if( a->Contains( p ) )
                        return 0.0;
                }
            }

            Prim probe;
            probe.kind = Prim::POINT;
            probe.a = p;
            probe.Finish();
            double d = 1e300;

            for( const Prim& q : s.prims )
                d = std::min( d, PrimDistance( probe, q ) );

            return d;
        };

        if( tracks )
        {
            for( const CuShape& s : shapes )
            {
                if( ( s.type != ItemType::TRACK && s.type != ItemType::ARC ) || s.net <= 0 )
                    continue; // tracks without a net are not part of any connection

                Pt     st, en;
                double w;

                if( s.type == ItemType::TRACK )
                {
                    const Track& t = m_b.tracks[(size_t) s.index];
                    st = t.start;
                    en = t.end;
                    w = t.width;
                }
                else
                {
                    const ArcTrack& t = m_b.arcs[(size_t) s.index];
                    st = t.start;
                    en = t.end;
                    w = t.width;
                }

                double acc = w / 2;
                int    startCount = 0, endCount = 0;
                bool   settled = false;
                BBox   box = s.box;
                m_idx.Query( s.layer, box, near );

                for( int id : near )
                {
                    const CuShape& o = shapes[(size_t) id];

                    if( ( o.type == s.type && o.index == s.index ) || !connectedKind( o.type ) )
                        continue;

                    if( s.net <= 0 || o.net != s.net )
                        continue;

                    bool hs = pointDist( o, st ) <= acc + 1e-9;
                    bool he = pointDist( o, en ) <= acc + 1e-9;

                    if( hs && he )
                    {
                        if( o.type == ItemType::ZONE || o.type == ItemType::PAD || o.type == ItemType::VIA )
                        {
                            settled = true;
                            break;
                        }

                        // which end it belongs to: by distance to the item's own anchor points
                        auto anchorDist = [&]( const Pt& p ) -> double
                        {
                            if( o.type == ItemType::TRACK )
                            {
                                const Track& t = m_b.tracks[(size_t) o.index];
                                return std::min( std::hypot( p.x - t.start.x, p.y - t.start.y ),
                                                 std::hypot( p.x - t.end.x, p.y - t.end.y ) );
                            }

                            const ArcTrack& t = m_b.arcs[(size_t) o.index];
                            return std::min( std::hypot( p.x - t.start.x, p.y - t.start.y ),
                                             std::hypot( p.x - t.end.x, p.y - t.end.y ) );
                        };

                        double ds = anchorDist( st ), de = anchorDist( en );

                        if( ds < de )
                            ++startCount;
                        else
                            ++endCount;
                    }
                    else if( hs )
                    {
                        ++startCount;
                    }
                    else if( he )
                    {
                        ++endCount;
                    }

                    if( startCount > 0 && endCount > 0 )
                    {
                        settled = true;
                        break;
                    }
                }

                if( settled )
                    continue;

                DrcItem& it = add( DrcCode::TRACK_DANGLING, startCount == 0 ? st : en, s.layer );
                it.a = { s.type, s.index };
                it.netA = s.net;
            }
        }

        if( vias )
        {
            for( size_t i = 0; i < m_b.vias.size(); ++i )
            {
                const Via& v = m_b.vias[i];

                if( v.net <= 0 )
                    continue;

                int  firstLayer = -1;
                bool multi = false, any = false;

                auto note = [&]( int layer )
                {
                    if( !any )
                    {
                        any = true;
                        firstLayer = layer;
                    }
                    else if( layer != firstLayer )
                    {
                        multi = true;
                    }
                };

                for( int l : m_b.copperStack )
                {
                    double size = 0;

                    if( !ViaOnLayer( m_b, v, l, &size ) )
                        continue;

                    BBox box;
                    box.add( v.pos, size / 2 );
                    m_idx.Query( l, box, near );

                    for( int id : near )
                    {
                        const CuShape& o = shapes[(size_t) id];

                        if( ( o.type == ItemType::VIA && o.index == (int) i ) || !connectedKind( o.type ) || o.net != v.net )
                            continue;

                        bool hit = false;

                        switch( o.type )
                        {
                        case ItemType::TRACK:
                        {
                            const Track& t = m_b.tracks[(size_t) o.index];
                            hit = std::hypot( t.start.x - v.pos.x, t.start.y - v.pos.y ) <= size / 2
                                  || std::hypot( t.end.x - v.pos.x, t.end.y - v.pos.y ) <= size / 2
                                  || PointSegmentDistance( v.pos, t.start, t.end ) <= t.width / 2;
                            break;
                        }
                        case ItemType::ARC:
                        {
                            const ArcTrack& t = m_b.arcs[(size_t) o.index];
                            hit = std::hypot( t.start.x - v.pos.x, t.start.y - v.pos.y ) <= size / 2
                                  || std::hypot( t.end.x - v.pos.x, t.end.y - v.pos.y ) <= size / 2
                                  || pointDist( o, v.pos ) <= 0;
                            break;
                        }
                        case ItemType::ZONE: hit = pointDist( o, v.pos ) <= 0; break;
                        case ItemType::PAD:
                        {
                            const Pad& p = m_b.pads[(size_t) o.index];
                            hit = pointDist( o, v.pos ) <= 0 || std::hypot( p.pos.x - v.pos.x, p.pos.y - v.pos.y ) <= size / 2;
                            break;
                        }
                        case ItemType::VIA: hit = pointDist( o, v.pos ) <= 0; break;
                        default: break;
                        }

                        if( !hit )
                            continue;

                        // KiCad keys multi-layer items (pads, vias) by their first layer
                        if( o.type == ItemType::PAD )
                        {
                            const Pad& p = m_b.pads[(size_t) o.index];
                            int        first = -1;

                            for( int pl : m_b.copperStack )
                            {
                                if( std::find( p.layers.begin(), p.layers.end(), pl ) != p.layers.end() )
                                {
                                    first = pl;
                                    break;
                                }
                            }

                            note( first );
                        }
                        else if( o.type == ItemType::VIA )
                        {
                            const Via& ov = m_b.vias[(size_t) o.index];
                            int        first = -1;

                            for( int vl : m_b.copperStack )
                            {
                                if( ViaOnLayer( m_b, ov, vl ) )
                                {
                                    first = vl;
                                    break;
                                }
                            }

                            note( first );
                        }
                        else
                        {
                            note( l );
                        }
                    }
                }

                if( multi )
                    continue;

                DrcItem& it = add( DrcCode::VIA_DANGLING, v.pos );
                it.a = { ItemType::VIA, (int) i };
                it.netA = v.net;
            }
        }
    }

    // ---- outline --------------------------------------------------------------------------

    void outline()
    {
        if( !wants( DrcCode::INVALID_OUTLINE ) )
            return;

        std::vector<Path> pieces, closed;
        int               count = 0;
        outlinePieces( -1, true, LayerKind::EDGE, 0, pieces, &count );

        if( pieces.empty() )
        {
            BBox all = m_b.ContentBBox();
            add( DrcCode::INVALID_OUTLINE, all.valid() ? Pt( all.minX, all.minY ) : Pt() );
            return;
        }

        Pt gap;

        if( !chainOutlines( pieces, closed, &gap ) )
            add( DrcCode::INVALID_OUTLINE, gap );
    }

    const Board&          m_b;
    const DesignRules&    m_r;
    const Connectivity&   m_conn;
    std::vector<DrcItem>& m_out;
    CopperIndex           m_idx;
    std::vector<Hole>     m_holes;
    std::vector<Pt>       m_airwireEnds;
    std::vector<std::vector<int>> m_shapesByFp;
};

} // anonymous namespace


void RunDrc( const Board& aBoard, const DesignRules& aRules, const Connectivity& aConn, std::vector<DrcItem>& aOut )
{
    aOut.clear();
    Checker c( aBoard, aRules, aConn, aOut );
    c.Run();

    aOut.erase( std::remove_if( aOut.begin(), aOut.end(), []( const DrcItem& i ) { return i.severity == Severity::IGNORE; } ),
                aOut.end() );

    std::stable_sort( aOut.begin(), aOut.end(),
                      []( const DrcItem& a, const DrcItem& b )
                      {
                          if( a.severity != b.severity )
                              return a.severity < b.severity;

                          if( a.code != b.code )
                              return a.code < b.code;

                          if( a.pos.y != b.pos.y )
                              return a.pos.y < b.pos.y;

                          return a.pos.x < b.pos.x;
                      } );
}

} // namespace kp
