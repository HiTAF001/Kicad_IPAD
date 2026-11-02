/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Schematic connectivity, after KiCad's connectivity engine (eeschema/connectivity):
 *
 *  1. Items of a sheet that touch form islands. Wires and buses touch at their ends and where a
 *     point of another item lies on them; bus entries join wires only on their wire side; pins of
 *     the "no connect" type join nothing.
 *  2. In every sheet instance, labels, power pins, sheet pins and symbol pins claim a name for
 *     their island. A claim also links the island to name scopes: the sheet (local, global and
 *     hierarchical labels, power pins), the whole design (global labels, global power) and the
 *     sheet pin <-> hierarchical label port of a child sheet.
 *  3. Islands that share a scope key are one net. Buses are joined first; every member of a bus
 *     then becomes a slot that links the member names on each sheet the bus reaches.
 *  4. The strongest claim names the net (priority, then the shorter sheet path, then the name).
 */
#include "schematic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>

namespace kp
{

namespace
{

// ---------------------------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------------------------

/// KiCad's EscapeString( CTX_NETNAME ).
std::string escapeNet( const std::string& aText )
{
    std::string out;
    out.reserve( aText.size() );

    for( char c : aText )
    {
        if( c == '/' )
            out += "{slash}";
        else if( c != '\n' && c != '\r' )
            out += c;
    }

    return out;
}


std::string strip( const std::string& s )
{
    size_t a = s.find_first_not_of( " \t\r\n" );

    if( a == std::string::npos )
        return std::string();

    size_t b = s.find_last_not_of( " \t\r\n" );
    return s.substr( a, b - a + 1 );
}


void replaceAll( std::string& s, const std::string& aFrom, const std::string& aTo )
{
    for( size_t p = s.find( aFrom ); p != std::string::npos; p = s.find( aFrom, p + aTo.size() ) )
        s.replace( p, aFrom.size(), aTo );
}


// ---------------------------------------------------------------------------------------------
// Bus names (NET_SETTINGS::ParseBusVector / ParseBusGroup, SCH_CONNECTIVITY::BUS_SCHEMA)
// ---------------------------------------------------------------------------------------------

bool isSuperSubOverbar( char c )
{
    return c == '_' || c == '^' || c == '~';
}


bool isEscaped( const std::string& s, size_t i )
{
    size_t n = 0;

    while( i > n && s[i - n - 1] == '\\' )
        ++n;

    return n % 2 == 1;
}


bool parseBusVector( const std::string& aBus, std::string* aName, std::vector<std::string>* aMembers )
{
    const size_t len = aBus.size();
    size_t       i = 0;
    std::string  prefix, suffix, tmp;
    long long    begin = 0, end = 0;
    int          nesting = 0;
    bool         fmtWrapsName = false, inQuotes = false, parsedEnd = false, padded = false;
    size_t       width = 0;

    for( ; i < len; ++i )
    {
        char c = aBus[i];

        if( c == '"' && !isEscaped( aBus, i ) )
        {
            inQuotes = !inQuotes;
            continue;
        }

        if( inQuotes )
        {
            if( c == '\\' && i + 1 < len )
                prefix += aBus[++i];
            else
                prefix += c;

            continue;
        }

        if( c == '{' )
        {
            if( i > 0 && isSuperSubOverbar( aBus[i - 1] ) )
            {
                nesting++;
                prefix += '{';
                continue;
            }

            return false;
        }

        if( c == '}' )
        {
            nesting--;
            prefix += '}';
            continue;
        }

        if( c == '\\' && i + 1 < len && aBus[i + 1] == ' ' )
        {
            prefix += aBus[++i];
            continue;
        }

        if( c == ' ' || c == ']' )
            return false;

        if( c == '[' )
        {
            if( nesting > 0 )
            {
                size_t fmt = prefix.rfind( '{' );

                if( fmt != std::string::npos && fmt > 0 && isSuperSubOverbar( prefix[fmt - 1] ) )
                {
                    if( fmt == prefix.size() - 1 )
                        prefix.erase( fmt - 1 ); // D_{[1..2]}: the formatting decorates the range
                    else
                        fmtWrapsName = true;     // ~{BE[0..3]}: it wraps the name
                }
            }

            break;
        }

        prefix += c;
    }

    if( ++i >= len )
        return false;

    auto toNum = []( const std::string& t, long long& v )
    {
        if( t.empty() || t.size() > 15 )
            return false;

        v = std::stoll( t );
        return true;
    };

    for( ; i < len; ++i )
    {
        if( aBus[i] == '.' && i + 1 < len && aBus[i + 1] == '.' )
        {
            if( !toNum( tmp, begin ) )
                return false;

            width = tmp.size();
            padded = width > 1 && tmp[0] == '0';
            i += 2;
            break;
        }

        if( aBus[i] < '0' || aBus[i] > '9' )
            return false;

        tmp += aBus[i];
    }

    tmp.clear();

    if( i >= len )
        return false;

    for( ; i < len; ++i )
    {
        if( aBus[i] == ']' )
        {
            if( !toNum( tmp, end ) )
                return false;

            padded |= tmp.size() > 1 && tmp[0] == '0';
            width = std::max( width, tmp.size() );
            parsedEnd = true;
            ++i;
            break;
        }

        if( aBus[i] < '0' || aBus[i] > '9' )
            return false;

        tmp += aBus[i];
    }

    if( !parsedEnd )
        return false;

    for( ; i < len; ++i )
    {
        char c = aBus[i];

        if( c == '}' )
        {
            nesting--;

            if( fmtWrapsName )
                suffix += c;
        }
        else if( c == '+' || c == '-' || c == 'P' || c == 'N' )
        {
            suffix += c;
        }
        else
        {
            return false;
        }
    }

    if( nesting != 0 || begin == end )
        return false;

    if( begin > end )
        std::swap( begin, end );

    if( end - begin > 4096 ) // a typo such as D[0..99999999] must not exhaust memory
        return false;

    if( aName )
        *aName = prefix;

    if( aMembers )
    {
        for( long long idx = begin; idx <= end; ++idx )
        {
            std::string number = std::to_string( idx );
            std::string str = prefix;

            if( padded && number.size() < width )
                str += std::string( width - number.size(), '0' );

            aMembers->push_back( str + number + suffix );
        }
    }

    return true;
}


bool parseBusGroup( const std::string& aGroup, std::string* aName, std::vector<std::string>* aMembers )
{
    const size_t len = aGroup.size();
    size_t       i = 0;
    std::string  prefix, tmp;
    int          nesting = 0;
    bool         inQuotes = false;

    auto escapeSpaces = []( const std::string& m )
    {
        std::string out;

        for( char c : m )
        {
            if( c == ' ' )
                out += "\\ ";
            else
                out += c;
        }

        return escapeNet( out );
    };

    for( ; i < len; ++i )
    {
        char c = aGroup[i];

        if( c == '"' && !isEscaped( aGroup, i ) )
        {
            inQuotes = !inQuotes;
            continue;
        }

        if( inQuotes )
        {
            if( c == '\\' && i + 1 < len )
                prefix += aGroup[++i];
            else
                prefix += c;

            continue;
        }

        if( c == '{' )
        {
            if( i > 0 && isSuperSubOverbar( aGroup[i - 1] ) )
            {
                nesting++;
                prefix += '{';
                continue;
            }

            break;
        }

        if( c == '}' )
        {
            nesting--;
            prefix += '}';
            continue;
        }

        if( c == '\\' && i + 1 < len && aGroup[i + 1] == ' ' )
        {
            prefix += aGroup[++i];
            continue;
        }

        if( c == ' ' || c == '[' || c == ']' )
            return false;

        prefix += c;
    }

    if( nesting != 0 )
        return false;

    if( aName )
        *aName = prefix;

    if( ++i >= len )
        return false;

    inQuotes = false;

    for( ; i < len; ++i )
    {
        char c = aGroup[i];

        if( c == '"' && !isEscaped( aGroup, i ) )
        {
            inQuotes = !inQuotes;
            continue;
        }

        if( inQuotes )
        {
            if( c == '\\' && i + 1 < len )
                tmp += aGroup[++i];
            else
                tmp += c;

            continue;
        }

        if( c == '{' )
        {
            if( i > 0 && isSuperSubOverbar( aGroup[i - 1] ) )
            {
                nesting++;
                tmp += '{';
                continue;
            }

            return false;
        }

        if( c == '}' )
        {
            if( nesting )
            {
                nesting--;
                tmp += '}';
                continue;
            }

            if( aMembers && !tmp.empty() )
                aMembers->push_back( escapeSpaces( tmp ) );

            return true;
        }

        if( c == '\\' && i + 1 < len && aGroup[i + 1] == ' ' )
        {
            tmp += aGroup[++i];
            continue;
        }

        if( c == ' ' || c == ',' )
        {
            if( aMembers && !tmp.empty() )
                aMembers->push_back( escapeSpaces( tmp ) );

            tmp.clear();
            continue;
        }

        tmp += c;
    }

    return false;
}


struct BusLeaf
{
    std::string name;      ///< with group prefixes: "I2C.SDA"
    std::string localName; ///< "SDA"
    std::vector<std::string> groupPath; ///< inner named groups (not the outermost)
};

struct BusSchema
{
    bool        group = true; ///< false: vector D[0..7]
    std::string prefix;
    std::vector<BusLeaf> leaves;
};

using Aliases = std::map<std::string, std::vector<std::string>>;

class SchemaParser
{
public:
    explicit SchemaParser( const Aliases& aAliases ) : m_aliases( aAliases ) {}

    std::shared_ptr<const BusSchema> Parse( const std::string& aText )
    {
        if( aText.find_first_of( "[{" ) == std::string::npos )
            return nullptr;

        m_schema = BusSchema();

        if( !expand( aText, std::string(), {}, true ) || m_schema.leaves.empty() )
            return nullptr;

        return std::make_shared<const BusSchema>( std::move( m_schema ) );
    }

private:
    bool expandMember( const std::string& aText, const std::string& aPrefix, const std::vector<std::string>& aPath )
    {
        std::string key = DisplayNetName( aText );
        replaceAll( key, "\\ ", " " );
        key = strip( key );
        auto alias = m_aliases.find( key );

        if( alias == m_aliases.end() )
            return expand( aText, aPrefix, aPath, false );

        if( !m_active.insert( key ).second || ++m_depth > 64 )
            return false;

        for( const std::string& text : alias->second )
        {
            const std::string member = strip( text );

            if( !member.empty() && !expandMember( escapeNet( member ), aPrefix, aPath ) )
                return false;
        }

        m_active.erase( key );
        --m_depth;
        return true;
    }

    bool expand( const std::string& aText, std::string aPrefix, std::vector<std::string> aPath, bool aRoot )
    {
        if( m_schema.leaves.size() > 8192 )
            return false;

        const std::string        text = DisplayNetName( aText );
        std::string              prefix;
        std::vector<std::string> members;

        if( parseBusVector( text, &prefix, &members ) )
        {
            if( aRoot )
            {
                m_schema.group = false;
                m_schema.prefix = escapeNet( prefix );
            }

            for( const std::string& m : members )
                addLeaf( m, aPrefix, aPath );

            return true;
        }

        if( parseBusGroup( text, &prefix, &members ) )
        {
            if( aRoot )
            {
                m_schema.group = true;
                m_schema.prefix = escapeNet( prefix );
            }
            else if( !prefix.empty() )
            {
                aPath.push_back( escapeNet( prefix ) );
            }

            if( !prefix.empty() )
                aPrefix += prefix + ".";

            for( const std::string& m : members )
            {
                if( !expandMember( m, aPrefix, aPath ) )
                    return false;
            }

            return true;
        }

        if( aRoot )
            return false;

        std::string local = text;
        replaceAll( local, "\\ ", " " );
        addLeaf( local, aPrefix, aPath );
        return true;
    }

    void addLeaf( const std::string& aLocal, const std::string& aPrefix, const std::vector<std::string>& aPath )
    {
        m_schema.leaves.push_back( { escapeNet( aPrefix + aLocal ), escapeNet( aLocal ), aPath } );
    }

    const Aliases&        m_aliases;
    std::set<std::string> m_active;
    int                   m_depth = 0;
    BusSchema             m_schema;
};


/// Pairs of (left leaf, right leaf) that are the same member; left leaves without a partner.
void align( const BusSchema& aLeft, const BusSchema& aRight, std::vector<std::pair<size_t, size_t>>& aMatched,
            std::vector<size_t>& aUnmapped )
{
    const bool byName = aLeft.group && aRight.group;
    std::map<std::pair<std::vector<std::string>, std::string>, std::vector<size_t>> candidates;

    if( byName )
    {
        for( size_t r = aRight.leaves.size(); r-- > 0; )
            candidates[{ aRight.leaves[r].groupPath, aRight.leaves[r].localName }].push_back( r );
    }

    for( size_t l = 0; l < aLeft.leaves.size(); ++l )
    {
        size_t r = l;

        if( byName )
        {
            auto found = candidates.find( { aLeft.leaves[l].groupPath, aLeft.leaves[l].localName } );
            r = aRight.leaves.size();

            if( found != candidates.end() && !found->second.empty() )
            {
                r = found->second.back();
                found->second.pop_back();
            }
        }

        if( r < aRight.leaves.size() )
            aMatched.emplace_back( l, r );
        else
            aUnmapped.push_back( l );
    }
}


// ---------------------------------------------------------------------------------------------
// Claims
// ---------------------------------------------------------------------------------------------

enum Prio : uint8_t
{
    P_NONE, P_PIN, P_SHEET_PIN, P_HIER_LABEL, P_LOCAL_LABEL, P_LOCAL_POWER_PIN, P_BUS_MEMBER,
    P_GLOBAL_POWER_PIN, P_GLOBAL
};

struct Claim
{
    Prio        prio = P_NONE;
    int         depth = 0;
    std::string path, name, ncName, fullName;
    int         inst = 0;
    int64_t     source = 0; ///< claiming item, the last tie-break (smaller wins)
    bool        gpParent = false, lpParent = false, output = false, hasPad = false;
    std::shared_ptr<const BusSchema> schema;
    int         portInstance = -1; ///< sheet pins: the child sheet instance

    bool strong() const { return prio >= P_HIER_LABEL; }
};

/// True if b names the net rather than a (KiCad CLAIM_LESS).
bool claimLess( const Claim& a, const Claim& b )
{
    auto rank = []( const Claim& c )
    {
        return std::make_tuple( (int) c.prio, -c.depth, c.gpParent, c.lpParent, c.output, !c.hasPad );
    };

    const auto ra = rank( a ), rb = rank( b );

    if( ra != rb )
        return ra < rb;

    if( a.fullName != b.fullName )
        return b.fullName < a.fullName;

    return b.source < a.source;
}


// ---------------------------------------------------------------------------------------------
// Islands of one sheet
// ---------------------------------------------------------------------------------------------

enum VType : uint8_t { V_WIRE, V_BUS, V_JUNCTION, V_NOCONNECT, V_ENTRY, V_LABEL, V_SHEETPIN, V_PIN };

enum Kind : uint8_t { K_ANCHOR, K_WIRE, K_BUS, K_ENTRY, K_NC, K_BUS_SIDE };

const bool PROPAGATES[6][6] = {
    { true, true, true, true, false, false },     // anchor: pins, labels, junctions, flags
    { true, true, false, true, false, false },    // wire
    { true, false, true, false, false, false },   // bus
    { true, true, false, false, false, false },   // bus entry, wire side
    { false, false, false, false, false, false }, // pin of type "no connect"
    { false, false, false, false, false, false }  // bus entry, bus side
};

struct Vertex
{
    VType type;
    int   index;  ///< line / junction / flag / entry / label index, flat sheet pin, pin group
    bool  segment = false;
    int64_t ax = 0, ay = 0, bx = 0, by = 0; ///< segment ends (0.1 um)
};

struct Islands
{
    std::vector<Vertex>           verts;
    std::vector<std::vector<int>> pinGroups; ///< flat pin indices of each V_PIN vertex
    std::vector<char>             ncPin;     ///< per pin group: a "no connect" type pin
    std::vector<int>              islandOf;  ///< per vertex
    std::vector<std::vector<int>> members;   ///< vertices of each island
    std::vector<char>             hasWire, hasBusLine;
};

int64_t q( double v )
{
    return (int64_t) std::llround( v * 1e4 );
}


uint64_t pointKey( int64_t x, int64_t y )
{
    return ( (uint64_t) (uint32_t) (int32_t) x << 32 ) | (uint32_t) (int32_t) y;
}


struct UnionFind
{
    std::vector<int> p;

    explicit UnionFind( size_t n = 0 ) : p( n ) { for( size_t i = 0; i < n; ++i ) p[i] = (int) i; }

    int add()
    {
        p.push_back( (int) p.size() );
        return (int) p.size() - 1;
    }

    int find( int x )
    {
        while( p[(size_t) x] != x )
        {
            p[(size_t) x] = p[(size_t) p[(size_t) x]];
            x = p[(size_t) x];
        }

        return x;
    }

    void unite( int a, int b )
    {
        a = find( a );
        b = find( b );

        if( a != b )
            p[(size_t) std::max( a, b )] = std::min( a, b );
    }
};


bool pinActive( const LibSymbol& aLib, const LibPin& aPin, int aUnit, int aBodyStyle )
{
    if( aPin.unit != 0 && aUnit != 0 && aPin.unit != aUnit )
        return false;

    if( aLib.bodyStyleCount > 1 && aPin.bodyStyle != 0 && aPin.bodyStyle != aBodyStyle )
        return false;

    return true;
}


Islands buildIslands( const Schematic& aSch, const SchScreen& aScreen, const std::vector<int>& aUnits,
                      const std::vector<int>& aPinBase )
{
    Islands r;

    struct Port
    {
        int  vertex;
        Kind kind;
    };

    struct Point
    {
        int64_t           x, y;
        std::vector<Port> ports;
    };

    std::unordered_map<uint64_t, Point> points;

    auto addPort = [&]( int aVertex, double aX, double aY, Kind aKind )
    {
        int64_t x = q( aX ), y = q( aY );
        Point&  p = points[pointKey( x, y )];
        p.x = x;
        p.y = y;
        p.ports.push_back( { aVertex, aKind } );
    };

    auto addVertex = [&]( VType aType, int aIndex ) -> int
    {
        r.verts.push_back( { aType, aIndex } );
        return (int) r.verts.size() - 1;
    };

    for( size_t i = 0; i < aScreen.lines.size(); ++i )
    {
        const SchLine& l = aScreen.lines[i];

        if( l.kind == SchLine::NOTES )
            continue;

        bool bus = l.kind == SchLine::BUS;
        int  v = addVertex( bus ? V_BUS : V_WIRE, (int) i );
        Vertex& vx = r.verts[(size_t) v];
        vx.segment = true;
        vx.ax = q( l.a.x );
        vx.ay = q( l.a.y );
        vx.bx = q( l.b.x );
        vx.by = q( l.b.y );
        addPort( v, l.a.x, l.a.y, bus ? K_BUS : K_WIRE );
        addPort( v, l.b.x, l.b.y, bus ? K_BUS : K_WIRE );
    }

    for( size_t i = 0; i < aScreen.junctions.size(); ++i )
        addPort( addVertex( V_JUNCTION, (int) i ), aScreen.junctions[i].pos.x, aScreen.junctions[i].pos.y, K_ANCHOR );

    for( size_t i = 0; i < aScreen.noConnects.size(); ++i )
        addPort( addVertex( V_NOCONNECT, (int) i ), aScreen.noConnects[i].pos.x, aScreen.noConnects[i].pos.y, K_ANCHOR );

    for( size_t i = 0; i < aScreen.busEntries.size(); ++i )
    {
        const SchBusEntry& e = aScreen.busEntries[i];
        int                v = addVertex( V_ENTRY, (int) i );
        addPort( v, e.pos.x, e.pos.y, K_ENTRY );
        addPort( v, e.pos.x + e.size.x, e.pos.y + e.size.y, K_ENTRY );
    }

    for( size_t i = 0; i < aScreen.labels.size(); ++i )
        addPort( addVertex( V_LABEL, (int) i ), aScreen.labels[i].pos.x, aScreen.labels[i].pos.y, K_ANCHOR );

    int flatSheetPin = 0;

    for( const SchSheet& sh : aScreen.sheets )
    {
        for( const SchSheetPin& p : sh.pins )
            addPort( addVertex( V_SHEETPIN, flatSheetPin++ ), p.pos.x, p.pos.y, K_ANCHOR );
    }

    // pins: those with one number at one place are stacked and act as one
    std::vector<std::pair<int, int>> jumpers;

    for( size_t s = 0; s < aScreen.symbols.size(); ++s )
    {
        const SchSymbol& sym = aScreen.symbols[s];
        const LibSymbol* lib = aSch.Lib( aScreen, sym );

        if( !lib )
            continue;

        std::map<std::tuple<std::string, int64_t, int64_t>, int> groups;
        std::map<std::string, std::vector<int>>                  byNumber;
        // pins that share a number are one pin wherever they sit when the part says so
        const bool joinApart = lib->duplicatePinNumbersAreJumpers;

        for( size_t pi = 0; pi < lib->pins.size(); ++pi )
        {
            const LibPin& pin = lib->pins[pi];

            if( !pinActive( *lib, pin, aUnits[s], sym.bodyStyle ) )
                continue;

            Pt tip, root;
            SchematicPinPoints( sym, pin, tip, root );
            const bool nc = pin.type == PinType::NO_CONNECT;
            const int  flat = aPinBase[s] + (int) pi;
            auto       key = std::make_tuple( pin.number, joinApart ? 0 : q( tip.x ), joinApart ? 0 : q( tip.y ) );
            auto       found = nc ? groups.end() : groups.find( key );

            if( found != groups.end() )
            {
                r.pinGroups[(size_t) r.verts[(size_t) found->second].index].push_back( flat );
                addPort( found->second, tip.x, tip.y, K_ANCHOR );
                continue;
            }

            r.pinGroups.push_back( { flat } );
            r.ncPin.push_back( nc );
            int v = addVertex( V_PIN, (int) r.pinGroups.size() - 1 );
            addPort( v, tip.x, tip.y, nc ? K_NC : K_ANCHOR );

            if( !nc )
            {
                groups.emplace( key, v );
                byNumber[pin.number].push_back( v );
            }
        }

        for( const std::vector<std::string>& group : lib->jumperPinGroups )
        {
            std::vector<int> vs;

            for( const std::string& number : group )
            {
                auto it = byNumber.find( number );

                if( it != byNumber.end() )
                    vs.insert( vs.end(), it->second.begin(), it->second.end() );
            }

            for( size_t i = 1; i < vs.size(); ++i )
                jumpers.emplace_back( vs[0], vs[i] );
        }
    }

    // wires and buses touched in their interior: a coarse grid finds the candidates
    const int64_t CELL = 254000; // 25.4 mm
    std::unordered_map<uint64_t, std::vector<int>> grid;

    for( size_t v = 0; v < r.verts.size(); ++v )
    {
        const Vertex& s = r.verts[v];

        if( !s.segment )
            continue;

        auto cell = []( int64_t c ) { return (int64_t) std::floor( (double) c / (double) CELL ); };
        int64_t x0 = cell( std::min( s.ax, s.bx ) - 2 ), x1 = cell( std::max( s.ax, s.bx ) + 2 );
        int64_t y0 = cell( std::min( s.ay, s.by ) - 2 ), y1 = cell( std::max( s.ay, s.by ) + 2 );

        if( ( x1 - x0 + 1 ) * ( y1 - y0 + 1 ) > 4096 )
            continue; // absurdly long line; its ends still connect

        for( int64_t cx = x0; cx <= x1; ++cx )
            for( int64_t cy = y0; cy <= y1; ++cy )
                grid[pointKey( cx, cy )].push_back( (int) v );
    }

    UnionFind uf( r.verts.size() );

    for( const auto& [a, b] : jumpers )
        uf.unite( a, b );

    for( auto& [key, pt] : points )
    {
        auto cellIt = grid.find( pointKey( (int64_t) std::floor( (double) pt.x / (double) CELL ),
                                           (int64_t) std::floor( (double) pt.y / (double) CELL ) ) );

        if( cellIt != grid.end() )
        {
            for( int v : cellIt->second )
            {
                const Vertex& s = r.verts[(size_t) v];

                if( ( pt.x == s.ax && pt.y == s.ay ) || ( pt.x == s.bx && pt.y == s.by ) )
                    continue;

                const double dx = (double) ( s.bx - s.ax ), dy = (double) ( s.by - s.ay );
                const double px = (double) ( pt.x - s.ax ), py = (double) ( pt.y - s.ay );
                const double len2 = dx * dx + dy * dy;
                const double dot = px * dx + py * dy;

                if( len2 <= 0 || dot < 0 || dot > len2 )
                    continue;

                const double cross = px * dy - py * dx;

                if( cross * cross > 3.0 * len2 ) // farther than ~1.7 internal units
                    continue;

                pt.ports.push_back( { v, s.type == V_BUS ? K_BUS : K_WIRE } );
            }
        }

        const bool hasBus = std::any_of( pt.ports.begin(), pt.ports.end(), []( const Port& p ) { return p.kind == K_BUS; } );

        if( hasBus )
        {
            for( Port& p : pt.ports )
            {
                if( p.kind == K_ENTRY )
                    p.kind = K_BUS_SIDE;
                else if( r.verts[(size_t) p.vertex].type == V_JUNCTION )
                    p.kind = K_BUS;
            }
        }

        for( size_t i = 0; i < pt.ports.size(); ++i )
        {
            for( size_t j = i + 1; j < pt.ports.size(); ++j )
            {
                const Port& a = pt.ports[i];
                const Port& b = pt.ports[j];

                if( a.vertex != b.vertex && PROPAGATES[a.kind][b.kind] )
                    uf.unite( a.vertex, b.vertex );
            }
        }
    }

    std::map<int, int> ids;
    r.islandOf.resize( r.verts.size() );

    for( size_t v = 0; v < r.verts.size(); ++v )
    {
        auto [it, inserted] = ids.emplace( uf.find( (int) v ), (int) r.members.size() );

        if( inserted )
        {
            r.members.emplace_back();
            r.hasWire.push_back( 0 );
            r.hasBusLine.push_back( 0 );
        }

        const int island = it->second;
        r.islandOf[v] = island;
        r.members[(size_t) island].push_back( (int) v );
        r.hasWire[(size_t) island] |= r.verts[v].type == V_WIRE;
        r.hasBusLine[(size_t) island] |= r.verts[v].type == V_BUS;
    }

    return r;
}


// ---------------------------------------------------------------------------------------------
// Pin net names (SCH_CONNECTIVITY::RenderPinNetName)
// ---------------------------------------------------------------------------------------------

std::string shownPinName( const LibPin& p )
{
    return p.name == "~" ? std::string() : p.name;
}


/// Smallest pad number of a stacked pin "[1,3-5]"; the number itself otherwise.
std::string padNumber( const std::string& aNumber )
{
    if( aNumber.size() < 3 || aNumber.front() != '[' || aNumber.back() != ']' )
        return aNumber;

    std::vector<std::string> parts;
    std::string              cur;

    for( size_t i = 1; i + 1 < aNumber.size(); ++i )
    {
        char c = aNumber[i];

        if( c == ',' || c == '-' )
        {
            if( !cur.empty() )
                parts.push_back( strip( cur ) );

            cur.clear();
        }
        else
        {
            cur += c;
        }
    }

    if( !cur.empty() )
        parts.push_back( strip( cur ) );

    if( parts.empty() )
        return aNumber;

    auto numeric = []( const std::string& s )
    {
        return !s.empty() && s.size() < 10 && std::all_of( s.begin(), s.end(), []( char c ) { return c >= '0' && c <= '9'; } );
    };

    std::string best = parts[0];

    for( const std::string& p : parts )
    {
        bool smaller = numeric( p ) && numeric( best ) ? std::stol( p ) < std::stol( best ) : p < best;

        if( smaller )
            best = p;
    }

    return best;
}


std::string renderPinNetName( const LibPin& aPin, bool aDuplicateName, const std::string& aRef,
                              const std::string& aRefWithUnit, const std::string& aUuid, bool aForceNoConnect )
{
    const bool        unconnected = aForceNoConnect || aPin.type == PinType::NO_CONNECT;
    const std::string pinName = shownPinName( aPin );
    const std::string pad = padNumber( aPin.number );
    std::string       name = unconnected ? "unconnected-(" : "Net-(";

    if( aRef.empty() || aRef.back() == '?' )
        name += ( aRef.empty() ? aUuid : aRef + "-" + aUuid.substr( 0, 8 ) ) + "-Pad" + pad + ")";
    else if( !pinName.empty() && pinName != aPin.number )
    {
        name += aRefWithUnit + "-" + escapeNet( pinName );

        if( unconnected || aDuplicateName )
            name += "-Pad" + escapeNet( pad );

        name += ")";
    }
    else
    {
        name += aRef + "-Pad" + escapeNet( pad ) + ")";
    }

    return name;
}


// ---------------------------------------------------------------------------------------------
// The whole hierarchy
// ---------------------------------------------------------------------------------------------

struct Record
{
    int  inst = 0, island = 0;
    bool bundle = false;
    std::vector<Claim>       claims; ///< strongest first
    std::vector<std::string> edges;  ///< name scope keys
    int  witnesses = 0;              ///< pins of non-power symbols
    bool noConnect = false;
};

struct Slot
{
    Claim                    claim;
    int                      bundle = -1;
    std::vector<std::string> edges, parentEdges;
};

std::string sheetKey( int aInst, const std::string& aName )
{
    return "S" + std::to_string( aInst ) + "\x01" + aName;
}


class NetBuilder
{
public:
    explicit NetBuilder( Schematic& aSch ) : m_sch( aSch ) {}

    void Build()
    {
        m_sch.nets.clear();
        m_sch.sheetNets.assign( m_sch.sheets.size(), SheetNets() );

        for( const SchScreen& sc : m_sch.screens )
        {
            for( const auto& [name, members] : sc.busAliases )
                m_aliases.emplace( strip( name ), members );
        }

        m_islands.resize( m_sch.sheets.size(), nullptr );

        for( size_t i = 0; i < m_sch.sheets.size(); ++i )
        {
            if( m_sch.sheets[i].screen >= 0 )
                prepareSheet( (int) i );
        }

        partitionBundles();
        partitionSignals();
    }

private:
    // --- per sheet instance ---

    int depth( int aInst ) const
    {
        int d = 0;

        for( int s = aInst; s >= 0; s = m_sch.sheets[(size_t) s].parent )
            ++d;

        return d;
    }

    std::shared_ptr<const BusSchema> schema( const std::string& aName )
    {
        auto it = m_schemas.find( aName );

        if( it != m_schemas.end() )
            return it->second;

        auto s = SchemaParser( m_aliases ).Parse( aName );
        m_schemas.emplace( aName, s );
        return s;
    }

    void prepareSheet( int aInst )
    {
        const SheetNode& node = m_sch.sheets[(size_t) aInst];
        const SchScreen& sc = m_sch.screens[(size_t) node.screen];
        const std::vector<int> pinBase = SchematicPinBase( m_sch, sc );

        // islands depend on the screen and the unit of each symbol on this sheet
        std::vector<int> units( sc.symbols.size() );
        std::string      sig = std::to_string( node.screen ) + ":";

        for( size_t s = 0; s < sc.symbols.size(); ++s )
        {
            units[s] = m_sch.Unit( node, sc.symbols[s] );
            sig += std::to_string( units[s] ) + ",";
        }

        auto& cached = m_islandCache[sig];

        if( !cached )
            cached = std::make_shared<Islands>( buildIslands( m_sch, sc, units, pinBase ) );

        m_islands[(size_t) aInst] = cached;
        const Islands& isl = *cached;

        SheetNets& sn = m_sch.sheetNets[(size_t) aInst];
        sn.wire.assign( sc.lines.size(), -1 );
        sn.pin.assign( (size_t) pinBase.back(), -1 );
        sn.label.assign( sc.labels.size(), -1 );
        sn.junction.assign( sc.junctions.size(), -1 );
        sn.noConnect.assign( sc.noConnects.size(), -1 );
        sn.busEntry.assign( sc.busEntries.size(), -1 );

        std::vector<std::pair<int, int>> sheetPinOf; // flat sheet pin -> (sheet, pin)

        for( size_t s = 0; s < sc.sheets.size(); ++s )
            for( size_t p = 0; p < sc.sheets[s].pins.size(); ++p )
                sheetPinOf.emplace_back( (int) s, (int) p );

        sn.sheetPin.assign( sheetPinOf.size(), -1 );

        std::map<int, int> childOf; // sheet item -> child instance

        for( int c : node.children )
            childOf[m_sch.sheets[(size_t) c].sheetItem] = c;

        const int         d = depth( aInst );
        const std::string path = SheetPathName( m_sch, aInst );

        // per symbol: reference, numbers of each pin name (duplicate names get the pad number)
        struct SymText
        {
            std::string ref, refWithUnit, value;
            std::map<std::string, std::set<std::string>> numbersByName;
        };

        std::vector<SymText> symText( sc.symbols.size() );

        for( size_t s = 0; s < sc.symbols.size(); ++s )
        {
            const SchSymbol& sym = sc.symbols[s];
            const LibSymbol* lib = m_sch.Lib( sc, sym );
            SymText&         t = symText[s];
            t.ref = m_sch.Reference( node, sym );
            t.refWithUnit = SchematicShownReference( m_sch, node, sym );

            for( const SchField& f : sym.fields )
            {
                if( f.name == "Value" )
                    t.value = m_sch.FieldValue( node, sym, f );
            }

            if( lib )
            {
                for( const LibPin& p : lib->pins )
                {
                    if( p.type != PinType::NO_CONNECT && pinActive( *lib, p, units[s], sym.bodyStyle ) )
                        t.numbersByName[shownPinName( p )].insert( p.number );
                }
            }
        }

        // pin flat index -> symbol
        std::vector<int> symOfPin( (size_t) pinBase.back(), 0 );

        for( size_t s = 0; s < sc.symbols.size(); ++s )
            for( int f = pinBase[s]; f < pinBase[s + 1]; ++f )
                symOfPin[(size_t) f] = (int) s;

        auto makeClaim = [&]( Prio aPrio, const std::string& aName, int64_t aSource ) -> Claim
        {
            Claim c;
            c.prio = aPrio;
            c.inst = aInst;
            c.source = ( (int64_t) aInst << 32 ) | aSource;
            const bool unscoped = aPrio == P_GLOBAL || aPrio == P_GLOBAL_POWER_PIN || aPrio == P_PIN;
            c.depth = unscoped ? 0 : d;
            c.path = unscoped ? std::string() : path;
            c.name = aName;
            c.ncName = aName;
            c.fullName = c.path + aName;
            c.hasPad = aName.find( "-Pad" ) != std::string::npos;
            c.schema = schema( aName );
            return c;
        };

        for( size_t island = 0; island < isl.members.size(); ++island )
        {
            Record rec;
            rec.inst = aInst;
            rec.island = (int) island;

            for( int v : isl.members[island] )
            {
                const Vertex& vx = isl.verts[(size_t) v];
                const int64_t src = (int64_t) v << 8;

                switch( vx.type )
                {
                case V_NOCONNECT: rec.noConnect = true; break;

                case V_LABEL:
                {
                    const SchLabel& L = sc.labels[(size_t) vx.index];

                    if( L.kind == SchLabel::DIRECTIVE )
                        break;

                    Prio p = L.kind == SchLabel::GLOBAL ? P_GLOBAL : L.kind == SchLabel::HIERARCHICAL ? P_HIER_LABEL : P_LOCAL_LABEL;
                    Claim c = makeClaim( p, escapeNet( L.text ), src );

                    if( p == P_HIER_LABEL )
                        c.portInstance = aInst;

                    rec.claims.push_back( std::move( c ) );
                    break;
                }

                case V_SHEETPIN:
                {
                    auto [si, pi] = sheetPinOf[(size_t) vx.index];
                    const SchSheetPin& sp = sc.sheets[(size_t) si].pins[(size_t) pi];
                    Claim c = makeClaim( P_SHEET_PIN, escapeNet( sp.name ), src );
                    c.output = sp.shape == LabelShape::OUTPUT;
                    auto child = childOf.find( si );
                    c.portInstance = child == childOf.end() ? -1 : child->second;
                    rec.claims.push_back( std::move( c ) );
                    break;
                }

                case V_PIN:
                {
                    const std::vector<int>& group = isl.pinGroups[(size_t) vx.index];

                    for( size_t m = 0; m < group.size(); ++m )
                    {
                        const int        flat = group[m];
                        const int        s = symOfPin[(size_t) flat];
                        const SchSymbol& sym = sc.symbols[(size_t) s];
                        const LibSymbol* lib = m_sch.Lib( sc, sym );
                        const LibPin&    pin = lib->pins[(size_t) ( flat - pinBase[(size_t) s] )];
                        const SymText&   t = symText[(size_t) s];
                        const bool powerIn = pin.type == PinType::POWER_IN;
                        const bool gpParent = lib->power && !lib->localPower;
                        const bool lpParent = lib->power && lib->localPower;
                        const bool globalPower = powerIn && ( gpParent || ( !lpParent && !pin.visible ) );
                        const bool localPower = powerIn && lpParent;

                        if( !gpParent && !lpParent )
                            rec.witnesses++;

                        if( globalPower || localPower )
                        {
                            std::string name = escapeNet( gpParent || lpParent ? t.value : pin.name );
                            Claim       c = makeClaim( globalPower ? P_GLOBAL_POWER_PIN : P_LOCAL_POWER_PIN, name, src + (int64_t) m );
                            c.gpParent = gpParent;
                            c.lpParent = lpParent;
                            rec.claims.push_back( std::move( c ) );
                            continue;
                        }

                        if( !sym.onBoard || ( !t.ref.empty() && t.ref[0] == '#' ) )
                            continue;

                        bool dup = false;
                        auto names = t.numbersByName.find( shownPinName( pin ) );

                        if( names != t.numbersByName.end() )
                            dup = names->second.size() > 1 || !names->second.count( pin.number );

                        Claim c = makeClaim( P_PIN, renderPinNetName( pin, dup, t.ref, t.refWithUnit, sym.uuid, false ),
                                             src + (int64_t) m );
                        c.ncName = renderPinNetName( pin, dup, t.ref, t.refWithUnit, sym.uuid, true );
                        c.schema = nullptr;
                        rec.claims.push_back( std::move( c ) );
                    }

                    break;
                }

                default: break;
                }
            }

            std::sort( rec.claims.begin(), rec.claims.end(), []( const Claim& a, const Claim& b ) { return claimLess( b, a ); } );
            rec.bundle = rec.claims.empty() ? isl.hasBusLine[island] : (bool) rec.claims.front().schema;
            const char kind = rec.bundle ? 'B' : 'N';

            for( const Claim& c : rec.claims )
            {
                switch( c.prio )
                {
                case P_GLOBAL:
                case P_GLOBAL_POWER_PIN:
                    rec.edges.push_back( "G\x01" + c.name );
                    rec.edges.push_back( sheetKey( aInst, c.name ) );
                    break;
                case P_LOCAL_LABEL:
                case P_LOCAL_POWER_PIN:
                    rec.edges.push_back( sheetKey( aInst, c.name ) );
                    break;
                case P_HIER_LABEL:
                    rec.edges.push_back( sheetKey( aInst, c.name ) );
                    rec.edges.push_back( "P" + std::to_string( aInst ) + kind + "\x01" + c.name );
                    break;
                case P_SHEET_PIN:
                    if( c.portInstance >= 0 )
                        rec.edges.push_back( "P" + std::to_string( c.portInstance ) + kind + "\x01" + c.name );

                    break;
                default: break;
                }
            }

            std::sort( rec.edges.begin(), rec.edges.end() );
            rec.edges.erase( std::unique( rec.edges.begin(), rec.edges.end() ), rec.edges.end() );
            m_records.push_back( std::move( rec ) );
        }
    }

    // --- buses ---

    /// Connected sets of the given nodes: each node has edges (name keys).
    template <typename EDGES>
    std::vector<int> partition( size_t aCount, EDGES aEdges )
    {
        UnionFind uf( aCount );
        std::unordered_map<std::string, int> names;

        for( size_t i = 0; i < aCount; ++i )
        {
            for( const std::string& e : aEdges( i ) )
            {
                auto [it, inserted] = names.emplace( e, (int) i );

                if( !inserted )
                    uf.unite( (int) i, it->second );
            }
        }

        std::vector<int> part( aCount );

        for( size_t i = 0; i < aCount; ++i )
            part[i] = uf.find( (int) i );

        return part;
    }

    struct BusClaim
    {
        const Claim*             claim;
        bool                     drives;
        std::vector<std::string> leafName, leafFull;
    };

    void partitionBundles()
    {
        std::vector<int> recs;

        for( size_t i = 0; i < m_records.size(); ++i )
        {
            if( m_records[i].bundle )
                recs.push_back( (int) i );
        }

        std::vector<int> part = partition( recs.size(), [&]( size_t i ) -> const std::vector<std::string>& { return m_records[(size_t) recs[i]].edges; } );
        std::map<int, std::vector<int>> bundles; // root -> records

        for( size_t i = 0; i < recs.size(); ++i )
            bundles[part[i]].push_back( recs[i] );

        for( const auto& [root, members] : bundles )
        {
            const int bundle = (int) m_bundleNets.size();
            m_bundleNets.push_back( -1 );

            for( int r : members )
                m_bundleOfRecord[r] = bundle;

            std::vector<BusClaim> claims;

            for( int r : members )
            {
                const Record& rec = m_records[(size_t) r];

                for( size_t c = 0; c < rec.claims.size(); ++c )
                {
                    if( !rec.claims[c].schema )
                        continue;

                    BusClaim bc { &rec.claims[c], c == 0, {}, {} };

                    for( const BusLeaf& leaf : bc.claim->schema->leaves )
                    {
                        bc.leafName.push_back( leaf.name );
                        bc.leafFull.push_back( bc.claim->path + leaf.name );
                    }

                    claims.push_back( std::move( bc ) );
                }
            }

            if( claims.empty() )
                continue;

            size_t canon = 0;

            for( size_t i = 1; i < claims.size(); ++i )
            {
                if( claimLess( *claims[canon].claim, *claims[i].claim ) )
                    canon = i;
            }

            const BusClaim& canonical = claims[canon];

            // the bus itself is a net of its own for highlighting
            SchNet bus;
            bus.name = canonical.claim->fullName;
            bus.bus = true;
            bus.global = canonical.claim->prio == P_GLOBAL;
            m_bundleNets[(size_t) bundle] = (int) m_sch.nets.size();
            m_sch.nets.push_back( bus );

            auto makeSlot = [&]( const BusClaim& aOwner, size_t aLeaf )
            {
                Slot s;
                s.claim.prio = P_BUS_MEMBER;
                s.claim.depth = aOwner.claim->depth;
                s.claim.path = aOwner.claim->path;
                s.claim.name = s.claim.ncName = aOwner.leafName[aLeaf];
                s.claim.fullName = aOwner.leafFull[aLeaf];
                s.claim.source = aOwner.claim->source;
                s.claim.inst = aOwner.claim->inst;
                s.bundle = bundle;
                return s;
            };

            const size_t first = m_slots.size();

            for( size_t i = 0; i < canonical.leafName.size(); ++i )
                m_slots.push_back( makeSlot( canonical, i ) );

            std::map<std::pair<int, std::string>, const BusClaim*> answers;

            for( const BusClaim& bc : claims )
            {
                if( bc.claim->prio != P_HIER_LABEL )
                    continue;

                auto [it, inserted] = answers.emplace( std::make_pair( bc.claim->inst, bc.claim->name ), &bc );

                if( !inserted && claimLess( *it->second->claim, *bc.claim ) )
                    it->second = &bc;
            }

            for( const BusClaim& bc : claims )
            {
                const bool      sheetPin = bc.claim->prio == P_SHEET_PIN;
                const BusClaim* answer = nullptr;

                if( sheetPin && bc.claim->portInstance >= 0 )
                {
                    auto it = answers.find( { bc.claim->portInstance, bc.claim->name } );

                    if( it != answers.end() )
                        answer = it->second;
                }

                // a sheet pin names members of the parent only while it drives its bus there
                const bool named = !sheetPin || bc.drives;

                if( !named && !answer )
                    continue;

                std::vector<std::pair<size_t, size_t>> matched;
                std::vector<size_t>                    unmapped;
                align( *bc.claim->schema, *canonical.claim->schema, matched, unmapped );

                if( named )
                {
                    for( const auto& [from, to] : matched )
                    {
                        Slot& s = m_slots[first + to];
                        ( sheetPin ? s.parentEdges : s.edges ).push_back( sheetKey( bc.claim->inst, bc.leafName[from] ) );
                    }
                }

                for( size_t from : unmapped )
                {
                    const BusClaim& owner = answer && from < answer->leafName.size() ? *answer : bc;
                    Slot            s = makeSlot( owner, from );
                    s.claim.source = bc.claim->source;

                    if( named )
                        ( sheetPin ? s.parentEdges : s.edges ).push_back( sheetKey( bc.claim->inst, bc.leafName[from] ) );

                    if( answer )
                        s.edges.push_back( sheetKey( answer->claim->inst, bc.leafName[from] ) );

                    m_slots.push_back( std::move( s ) );
                }
            }
        }
    }

    // --- nets ---

    void partitionSignals()
    {
        std::vector<int> recs;
        std::set<std::string> claimed;

        for( size_t i = 0; i < m_records.size(); ++i )
        {
            if( m_records[i].bundle )
                continue;

            recs.push_back( (int) i );
            claimed.insert( m_records[i].edges.begin(), m_records[i].edges.end() );
        }

        // slots join only through names that a net on that sheet claims
        for( Slot& s : m_slots )
        {
            for( const std::string& e : s.parentEdges )
            {
                if( claimed.count( e ) )
                    s.edges.push_back( e );
            }
        }

        const size_t count = recs.size() + m_slots.size();
        std::vector<int> part = partition( count, [&]( size_t i ) -> const std::vector<std::string>&
                                           {
                                               return i < recs.size() ? m_records[(size_t) recs[i]].edges : m_slots[i - recs.size()].edges;
                                           } );

        struct Net
        {
            const Claim*     best = nullptr;
            int              witnesses = 0;
            bool             noConnect = false;
            std::vector<int> records;
        };

        std::map<int, Net> nets;
        std::vector<int>   order; // roots in first-seen order

        for( size_t i = 0; i < count; ++i )
        {
            auto [it, inserted] = nets.emplace( part[i], Net() );

            if( inserted )
                order.push_back( part[i] );

            Net&         n = it->second;
            const Claim* c = nullptr;

            if( i < recs.size() )
            {
                const Record& rec = m_records[(size_t) recs[i]];
                n.records.push_back( recs[i] );
                n.witnesses += rec.witnesses;
                n.noConnect |= rec.noConnect;
                c = rec.claims.empty() ? nullptr : &rec.claims.front();
            }
            else
            {
                c = &m_slots[i - recs.size()].claim;
            }

            if( c && ( !n.best || claimLess( *n.best, *c ) ) )
                n.best = c;
        }

        // names; a name that two nets would share stays with the stronger one, the others get _1, _2...
        std::vector<std::pair<int, std::string>> named; // root, base name
        std::map<std::string, std::vector<int>>  buckets;

        for( int root : order )
        {
            const Net& n = nets[root];

            if( !n.best )
                continue;

            std::string base = n.best->prio == P_PIN && ( n.witnesses < 2 || n.noConnect ) ? n.best->ncName : n.best->fullName;
            buckets[base].push_back( (int) named.size() );
            named.emplace_back( root, base );
        }

        std::vector<std::string> finalName( named.size() );
        std::set<std::string>    occupied;
        std::vector<int>         remaining;

        for( const auto& [base, list] : buckets )
        {
            int holder = list[0];

            for( int k : list )
            {
                const Claim& a = *nets[named[(size_t) k].first].best;
                const Claim& h = *nets[named[(size_t) holder].first].best;

                if( a.strong() != h.strong() ? a.strong() : ( a.strong() && claimLess( h, a ) ) )
                    holder = k;
            }

            finalName[(size_t) holder] = base;
            occupied.insert( base );

            for( int k : list )
            {
                if( k != holder )
                    remaining.push_back( k );
            }
        }

        std::sort( remaining.begin(), remaining.end() );
        std::map<std::string, int> nextSuffix;

        for( int k : remaining )
        {
            const std::string& base = named[(size_t) k].second;
            int&               suffix = nextSuffix.emplace( base, 1 ).first->second;

            while( occupied.count( base + "_" + std::to_string( suffix ) ) )
                ++suffix;

            finalName[(size_t) k] = base + "_" + std::to_string( suffix );
            occupied.insert( finalName[(size_t) k] );
        }

        for( size_t k = 0; k < named.size(); ++k )
        {
            const Net& n = nets[named[k].first];
            SchNet     net;
            net.name = finalName[k];
            net.global = n.best->prio == P_GLOBAL || n.best->prio == P_GLOBAL_POWER_PIN;
            net.noConnect = n.noConnect;
            net.pinCount = n.witnesses;
            const int index = (int) m_sch.nets.size();
            m_sch.nets.push_back( net );

            for( int r : n.records )
                assign( m_records[(size_t) r], index );
        }

        // bus islands
        for( const auto& [r, bundle] : m_bundleOfRecord )
        {
            if( m_bundleNets[(size_t) bundle] >= 0 )
                assign( m_records[(size_t) r], m_bundleNets[(size_t) bundle] );
        }
    }

    void assign( const Record& aRec, int aNet )
    {
        const Islands& isl = *m_islands[(size_t) aRec.inst];
        SheetNets&     sn = m_sch.sheetNets[(size_t) aRec.inst];

        for( int v : isl.members[(size_t) aRec.island] )
        {
            const Vertex& vx = isl.verts[(size_t) v];
            const size_t  i = (size_t) vx.index;

            switch( vx.type )
            {
            case V_WIRE:
            case V_BUS: sn.wire[i] = aNet; break;
            case V_JUNCTION: sn.junction[i] = aNet; break;
            case V_NOCONNECT: sn.noConnect[i] = aNet; break;
            case V_ENTRY: sn.busEntry[i] = aNet; break;
            case V_LABEL: sn.label[i] = aNet; break;
            case V_SHEETPIN: sn.sheetPin[i] = aNet; break;
            case V_PIN:
                for( int flat : isl.pinGroups[i] )
                    sn.pin[(size_t) flat] = aNet;

                break;
            }
        }
    }

    Schematic&                                        m_sch;
    Aliases                                           m_aliases;
    std::map<std::string, std::shared_ptr<const BusSchema>> m_schemas;
    std::map<std::string, std::shared_ptr<Islands>>   m_islandCache;
    std::vector<std::shared_ptr<Islands>>             m_islands; ///< per sheet instance
    std::vector<Record>                               m_records;
    std::vector<Slot>                                 m_slots;
    std::vector<int>                                  m_bundleNets; ///< bundle -> net (-1 none)
    std::map<int, int>                                m_bundleOfRecord;
};

} // anonymous namespace


std::string SheetPathName( const Schematic& aSch, int aSheet )
{
    std::vector<std::string> names;

    for( int s = aSheet; s > 0 && s < (int) aSch.sheets.size(); s = aSch.sheets[(size_t) s].parent )
        names.push_back( aSch.sheets[(size_t) s].name );

    std::string path = "/";

    for( auto it = names.rbegin(); it != names.rend(); ++it )
        path += *it + "/";

    return path;
}


std::vector<std::string> BusMembers( const std::string& aLabel )
{
    std::vector<std::string> out;
    Aliases                  none;
    auto                     schema = SchemaParser( none ).Parse( escapeNet( aLabel ) );

    if( !schema )
        return { aLabel };

    for( const BusLeaf& leaf : schema->leaves )
        out.push_back( DisplayNetName( leaf.name ) );

    return out;
}


void BuildSchematicNets( Schematic& aSch )
{
    NetBuilder( aSch ).Build();
}


std::string SchematicNetlistText( const Schematic& aSch )
{
    std::vector<std::set<std::pair<std::string, std::string>>> nodes( aSch.nets.size() );

    for( size_t si = 0; si < aSch.sheets.size() && si < aSch.sheetNets.size(); ++si )
    {
        const SheetNode& node = aSch.sheets[si];

        if( node.screen < 0 )
            continue;

        const SchScreen&       sc = aSch.screens[(size_t) node.screen];
        const SheetNets&       sn = aSch.sheetNets[si];
        const std::vector<int> base = SchematicPinBase( aSch, sc );

        for( size_t s = 0; s < sc.symbols.size(); ++s )
        {
            const SchSymbol& sym = sc.symbols[s];
            const LibSymbol* lib = aSch.Lib( sc, sym );
            const std::string ref = aSch.Reference( node, sym );

            if( !lib || !sym.onBoard || ref.empty() || ref[0] == '#' )
                continue;

            for( size_t p = 0; p < lib->pins.size(); ++p )
            {
                const size_t flat = (size_t) base[s] + p;

                if( flat < sn.pin.size() && sn.pin[flat] >= 0 )
                    nodes[(size_t) sn.pin[flat]].emplace( ref, lib->pins[p].number );
            }
        }
    }

    std::string out;

    for( size_t n = 0; n < aSch.nets.size(); ++n )
    {
        if( aSch.nets[n].bus || nodes[n].empty() )
            continue;

        out += DisplayNetName( aSch.nets[n].name ) + "\t";
        bool first = true;

        for( const auto& [ref, pin] : nodes[n] )
        {
            out += ( first ? "" : " " ) + ref + ":" + pin;
            first = false;
        }

        out += "\n";
    }

    return out;
}

} // namespace kp
