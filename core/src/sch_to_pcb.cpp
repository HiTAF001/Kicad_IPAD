/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * The rules follow KiCad's BOARD_NETLIST_UPDATER (pcbnew/netlist_reader) with the options of
 * the "Update PCB from Schematic" dialog left at their defaults: footprints are found through
 * their symbol path, nothing is deleted and footprints are not replaced; and the netlist
 * exporter (eeschema/netlist_exporters/netlist_exporter_xml.cpp) for which symbols, pins and
 * nets reach the board.
 */
#include "sch_to_pcb.h"
#include "i18n.h"
#include "sexpr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace kp
{

namespace
{

// ------------------------------------------------------------------------------ text helpers

std::string lowerAscii( std::string aText )
{
    for( char& c : aText )
        c = (char) std::tolower( (unsigned char) c );

    return aText;
}


/// KiCad's natural order (digit runs compare as numbers), ignoring case.
int naturalCompare( const std::string& aA, const std::string& aB )
{
    size_t i = 0, j = 0;

    while( i < aA.size() && j < aB.size() )
    {
        if( std::isdigit( (unsigned char) aA[i] ) && std::isdigit( (unsigned char) aB[j] ) )
        {
            size_t ie = i, je = j;

            while( ie < aA.size() && std::isdigit( (unsigned char) aA[ie] ) )
                ++ie;

            while( je < aB.size() && std::isdigit( (unsigned char) aB[je] ) )
                ++je;

            size_t is = aA.find_first_not_of( '0', i ), js = aB.find_first_not_of( '0', j );
            is = is == std::string::npos || is > ie ? ie : is;
            js = js == std::string::npos || js > je ? je : js;

            if( ie - is != je - js )
                return ie - is < je - js ? -1 : 1;

            int c = aA.compare( is, ie - is, aB, js, je - js );

            if( c )
                return c < 0 ? -1 : 1;

            i = ie;
            j = je;
        }
        else
        {
            int a = std::tolower( (unsigned char) aA[i] ), b = std::tolower( (unsigned char) aB[j] );

            if( a != b )
                return a < b ? -1 : 1;

            ++i;
            ++j;
        }
    }

    if( i < aA.size() )
        return 1;

    return j < aB.size() ? -1 : 0;
}


/// A UUID the way KiCad's KIID compares it: 16 bytes, so hyphens and case do not matter, and a
/// legacy 8 digit time stamp is the last 4 bytes of an otherwise empty UUID.
std::string normalizeKiid( const std::string& aText )
{
    std::string t = lowerAscii( aText );

    if( !t.empty() && t.size() <= 8
        && std::all_of( t.begin(), t.end(), []( unsigned char c ) { return std::isxdigit( c ); } ) )
        return std::string( 24, '0' ) + std::string( 8 - t.size(), '0' ) + t;

    std::string out;

    for( char c : t )
    {
        if( c != '-' )
            out += c;
    }

    return out;
}


/// "/uuid/uuid/uuid" with every step normalised.
std::string normalizePath( const std::string& aPath )
{
    std::string out;
    size_t      start = 0;

    while( start <= aPath.size() )
    {
        size_t slash = aPath.find( '/', start );
        size_t end = slash == std::string::npos ? aPath.size() : slash;

        if( end > start )
            out += "/" + normalizeKiid( aPath.substr( start, end - start ) );

        if( slash == std::string::npos )
            break;

        start = slash + 1;
    }

    return out;
}


bool isStackedSpecial( char c )
{
    return c == '[' || c == ']' || c == ',' || c == '-' || c == '\\';
}


bool escapeAt( const std::string& aText, size_t aIndex )
{
    return aText[aIndex] == '\\' && aIndex + 1 < aText.size() && isStackedSpecial( aText[aIndex + 1] );
}


std::string unescapeStacked( const std::string& aItem )
{
    std::string out;

    for( size_t i = 0; i < aItem.size(); ++i )
    {
        if( escapeAt( aItem, i ) )
            out += aItem[++i];
        else
            out += aItem[i];
    }

    return out;
}


std::string trim( const std::string& aText )
{
    size_t b = aText.find_first_not_of( " \t\r\n" );

    if( b == std::string::npos )
        return std::string();

    return aText.substr( b, aText.find_last_not_of( " \t\r\n" ) - b + 1 );
}


/// Prefix and trailing number of a pin number ("A12" -> "A", 12); number -1 if there is none.
std::pair<std::string, long> splitAlphaNumeric( const std::string& aPin )
{
    size_t start = aPin.size();

    for( int i = (int) aPin.size() - 1; i >= 0; --i )
    {
        if( !std::isdigit( (unsigned char) aPin[(size_t) i] ) )
        {
            start = (size_t) i + 1;
            break;
        }

        if( i == 0 )
            start = 0;
    }

    if( start >= aPin.size() )
        return { std::string(), -1 };

    return { aPin.substr( 0, start ), std::strtol( aPin.c_str() + start, nullptr, 10 ) };
}


/// Pad numbers a symbol pin number stands for: "[1-3,5]" is 1, 2, 3 and 5 (KiCad's stacked
/// pin notation); anything that is not valid notation is one pin.
std::vector<std::string> expandStackedPins( const std::string& aNumber )
{
    const bool open = aNumber.find( '[' ) != std::string::npos;
    const bool close = aNumber.find( ']' ) != std::string::npos;

    if( ( !open && !close ) || aNumber.size() < 2 || aNumber.front() != '[' || aNumber.back() != ']' )
        return { aNumber };

    const std::string        inner = aNumber.substr( 1, aNumber.size() - 2 );
    std::vector<std::string> parts, expanded;
    std::string              current;

    for( size_t i = 0; i < inner.size(); ++i )
    {
        if( escapeAt( inner, i ) )
        {
            current += inner[i];
            current += inner[i + 1];
            ++i;
        }
        else if( inner[i] == ',' )
        {
            parts.push_back( current );
            current.clear();
        }
        else
        {
            current += inner[i];
        }
    }

    parts.push_back( current );

    for( const std::string& raw : parts )
    {
        std::string part = trim( raw );

        if( part.empty() )
            continue;

        size_t dash = std::string::npos;

        for( size_t i = 0; i < part.size(); ++i )
        {
            if( escapeAt( part, i ) )
                ++i;
            else if( part[i] == '-' )
            {
                dash = i;
                break;
            }
        }

        if( dash == std::string::npos )
        {
            expanded.push_back( unescapeStacked( part ) );
            continue;
        }

        auto [sp, sv] = splitAlphaNumeric( trim( unescapeStacked( part.substr( 0, dash ) ) ) );
        auto [ep, ev] = splitAlphaNumeric( trim( unescapeStacked( part.substr( dash + 1 ) ) ) );

        if( sp != ep || sv == -1 || ev == -1 || sv > ev || ev - sv > 100000 )
            return { aNumber };

        for( long n = sv; n <= ev; ++n )
            expanded.push_back( sp + std::to_string( n ) );
    }

    if( expanded.empty() )
        return { aNumber };

    return expanded;
}


bool isAutoNetName( const std::string& aName )
{
    return aName.compare( 0, 13, "unconnected-(" ) == 0 || aName.compare( 0, 5, "Net-(" ) == 0;
}


bool needsQuotes( const std::string& aText )
{
    if( aText.empty() )
        return true;

    for( char c : aText )
    {
        if( c == ' ' || c == '(' || c == ')' || c == '"' || c == '\\' || c == '\t' || c == '\n' || c == '\r' )
            return true;
    }

    return false;
}


bool fpidsEquivalent( const std::string& aBoardId, const std::string& aSchId )
{
    // a symbol footprint without library name only has to agree in the footprint name
    if( aSchId.find( ':' ) == std::string::npos )
    {
        size_t colon = aBoardId.find( ':' );
        return ( colon == std::string::npos ? aBoardId : aBoardId.substr( colon + 1 ) ) == aSchId;
    }

    return aBoardId == aSchId;
}


// -------------------------------------------------------------------- the schematic's side

struct PinNet
{
    std::string pad;
    std::string net;
    bool        noConnect = false; ///< KiCad's "no_connect" pin type (pin type or a no-connect flag)
};


struct Component
{
    std::string              ref, value, footprint;
    std::string              path;  ///< "/sheet uuids/symbol uuid" of the primary unit as the file spells it
    std::vector<std::string> keys;  ///< normalised paths of all its units
    std::vector<PinNet>      pins;  ///< sorted by net name, like the netlist
    std::unordered_map<std::string, std::vector<size_t>> byPad;

    /// KiCad's COMPONENT::GetNet: the first net of the pad, but a net the user named wins over
    /// one KiCad named after a pin.
    const PinNet* NetOf( const std::string& aPad ) const
    {
        auto it = byPad.find( aPad );

        if( it == byPad.end() )
            return nullptr;

        const PinNet* fallback = nullptr;

        for( size_t i : it->second )
        {
            const PinNet& p = pins[i];

            if( !fallback )
                fallback = &p;

            if( !p.net.empty() && !isAutoNetName( p.net ) )
                return &p;
        }

        return fallback;
    }
};


struct Placed
{
    int         node = 0;
    size_t      symbol = 0;
    std::string ref;
    std::string uuidKey;
    const LibSymbol* lib = nullptr;
};


struct Collected
{
    std::vector<Component> comps;
    std::set<std::string>  netNames; ///< nets with pins of components that go to the board
    int                    symbols = 0;
};


Collected collectComponents( const Schematic& aSch )
{
    Collected out;

    std::vector<std::vector<int>> pinBase( aSch.sheets.size() );
    std::vector<Placed>           placed;                      // all symbols that reach the board
    std::vector<std::vector<size_t>> placedOfNode( aSch.sheets.size() );

    for( size_t si = 0; si < aSch.sheets.size(); ++si )
    {
        const SheetNode& node = aSch.sheets[si];

        if( node.screen < 0 || node.screen >= (int) aSch.screens.size() )
            continue;

        const SchScreen& sc = aSch.screens[(size_t) node.screen];
        pinBase[si] = SchematicPinBase( aSch, sc );

        for( size_t s = 0; s < sc.symbols.size(); ++s )
        {
            const SchSymbol& sym = sc.symbols[s];
            const LibSymbol* lib = aSch.Lib( sc, sym );
            std::string      ref = aSch.Reference( node, sym );

            // KiCad leaves out symbols without library symbol, power and virtual symbols
            // ("#..." references) and those that are not on the board
            if( !lib || lib->power || ref.empty() || ref[0] == '#' || !sym.onBoard )
                continue;

            placed.push_back( { (int) si, s, ref, normalizeKiid( sym.uuid ), lib } );
            placedOfNode[si].push_back( placed.size() - 1 );
        }
    }

    // a sheet's symbols with one reference are the units of one component: the lowest UUID
    // is the primary one. A multi-unit part is one component for the whole hierarchy.
    std::set<std::string>               multiSeen;
    std::map<std::string, size_t>       firstOfRef; // reference -> component index

    for( size_t si = 0; si < aSch.sheets.size(); ++si )
    {
        const SheetNode& node = aSch.sheets[si];

        if( placedOfNode[si].empty() )
            continue;

        const SchScreen&                          sc = aSch.screens[(size_t) node.screen];
        std::map<std::string, std::vector<size_t>> groups; // lower case reference -> placed index

        for( size_t pi : placedOfNode[si] )
            groups[lowerAscii( placed[pi].ref )].push_back( pi );

        std::vector<std::pair<std::string, size_t>> order; // sort key, primary

        for( auto& [key, members] : groups )
        {
            size_t primary = members.front();

            for( size_t m : members )
            {
                if( placed[m].uuidKey < placed[primary].uuidKey )
                    primary = m;
            }

            order.emplace_back( placed[primary].ref, primary );
        }

        std::stable_sort( order.begin(), order.end(),
                          []( const auto& a, const auto& b ) { return naturalCompare( a.first, b.first ) < 0; } );

        for( const auto& [refText, primary] : order )
        {
            const Placed&    p = placed[primary];
            const SchSymbol& sym = sc.symbols[p.symbol];

            if( p.lib->unitCount > 1 && !multiSeen.insert( p.ref ).second )
                continue;

            Component c;
            c.ref = p.ref;
            c.path = ( node.path == "/" ? std::string() : node.path ) + "/" + sym.uuid;

            for( const SchField& f : sym.fields )
            {
                if( f.name == "Value" )
                    c.value = aSch.FieldValue( node, sym, f );
                else if( f.name == "Footprint" )
                    c.footprint = aSch.FieldValue( node, sym, f );
            }

            for( size_t m : groups[lowerAscii( p.ref )] )
            {
                const SchSymbol& unit = sc.symbols[placed[m].symbol];
                c.keys.push_back( normalizePath( ( node.path == "/" ? std::string() : node.path ) + "/" + unit.uuid ) );
            }

            firstOfRef.emplace( c.ref, out.comps.size() );
            out.comps.push_back( std::move( c ) );
        }
    }

    out.symbols = (int) out.comps.size();

    // pins, from every placed unit; they belong to the first component with their reference
    struct Raw
    {
        size_t      comp;
        std::string pad, net;
        int         netIndex;
        bool        pinNc;
        size_t      placed;  ///< index of the symbol the pin belongs to
        std::string number;  ///< the pin's number as the symbol writes it (before stacked pin expansion)
        std::string name;
        int64_t     x, y;    ///< connection point on the sheet, in 1/1000 mm
    };

    std::vector<Raw> raws;

    for( size_t pl = 0; pl < placed.size(); ++pl )
    {
        const Placed& p = placed[pl];
        auto          comp = firstOfRef.find( p.ref );

        if( comp == firstOfRef.end() || p.node >= (int) aSch.sheetNets.size() )
            continue;

        const SheetNode& node = aSch.sheets[(size_t) p.node];
        const SheetNets& sn = aSch.sheetNets[(size_t) p.node];
        const SchSymbol& sym = aSch.screens[(size_t) node.screen].symbols[p.symbol];
        const int        base = pinBase[(size_t) p.node][p.symbol];

        for( size_t pi = 0; pi < p.lib->pins.size(); ++pi )
        {
            const size_t flat = (size_t) base + pi;

            if( flat >= sn.pin.size() || sn.pin[flat] < 0 || sn.pin[flat] >= (int) aSch.nets.size() )
                continue;

            const SchNet& net = aSch.nets[(size_t) sn.pin[flat]];

            if( net.bus || net.name.empty() )
                continue;

            const LibPin& pin = p.lib->pins[pi];
            Pt            tip, root;
            SchematicPinPoints( sym, pin, tip, root );

            for( const std::string& pad : expandStackedPins( pin.number ) )
                raws.push_back( { comp->second, pad, net.name, sn.pin[flat], pin.type == PinType::NO_CONNECT, pl,
                                  pin.number, pin.name, (int64_t) std::llround( tip.x * 1000.0 ),
                                  (int64_t) std::llround( tip.y * 1000.0 ) } );
        }
    }

    // The netlist marks a pin "no_connect" when its type says so, or when its net carries a
    // no-connect flag and the net is one pin, or all its pins are stacked (one symbol, one
    // place, one name). Such pads get a net name of their own on the board.
    std::map<int, std::vector<size_t>> rawsOfNet;

    for( size_t i = 0; i < raws.size(); ++i )
    {
        if( aSch.nets[(size_t) raws[i].netIndex].noConnect )
            rawsOfNet[raws[i].netIndex].push_back( i );
    }

    std::set<int> flaggedNets;

    for( const auto& [netIndex, members] : rawsOfNet )
    {
        std::set<std::pair<std::string, std::string>> nodes; // reference, pin number
        bool                                          stacked = true;

        for( size_t m : members )
        {
            const Raw& r = raws[m];
            nodes.emplace( placed[r.placed].ref, r.number );
            stacked &= r.placed == raws[members.front()].placed && r.x == raws[members.front()].x
                       && r.y == raws[members.front()].y && r.name == raws[members.front()].name;
        }

        if( nodes.size() == 1 || stacked )
            flaggedNets.insert( netIndex );
    }

    for( const Raw& r : raws )
    {
        PinNet pn;
        pn.pad = r.pad;
        pn.net = r.net;
        pn.noConnect = r.pinNc || flaggedNets.count( r.netIndex ) > 0;
        out.comps[r.comp].pins.push_back( pn );
        out.netNames.insert( r.net );
    }

    for( Component& c : out.comps )
    {
        std::stable_sort( c.pins.begin(), c.pins.end(),
                          []( const PinNet& a, const PinNet& b ) { return naturalCompare( a.net, b.net ) < 0; } );

        for( size_t i = 0; i < c.pins.size(); ++i )
            c.byPad[c.pins[i].pad].push_back( i );
    }

    return out;
}


// ----------------------------------------------------------------------------- the board's side

/// KiCad's PAD::IsOnCopperLayer: nets only go to pads with copper.
bool padOnCopper( const Board& aBoard, const Pad& aPad )
{
    if( aPad.attr == PadAttr::NPTH )
    {
        bool ring = true;

        auto check = [&]( const PadGeom& g )
        {
            if( g.shape == PadShape::CIRCLE && g.offset.x == 0 && g.offset.y == 0 && g.size.x <= aPad.drill.x )
                ring = false;
            else if( g.shape == PadShape::OVAL && g.offset.x == 0 && g.offset.y == 0 && g.size.x <= aPad.drill.x
                     && g.size.y <= aPad.drill.y )
                ring = false;
        };

        check( aPad.geom );

        for( const auto& lg : aPad.layerGeom )
            check( lg.second );

        if( !ring )
            return false;
    }

    for( int l : aPad.layers )
    {
        if( l >= 0 && l < (int) aBoard.layers.size() && aBoard.layers[(size_t) l].kind == LayerKind::COPPER )
            return true;
    }

    return false;
}


std::string netNameOf( const Board& aBoard, int aNet )
{
    return aNet > 0 && aNet < (int) aBoard.nets.size() ? aBoard.nets[(size_t) aNet] : std::string();
}


bool isBoardOnly( const Footprint& aFp )
{
    return std::find( aFp.attrs.begin(), aFp.attrs.end(), "board_only" ) != aFp.attrs.end();
}


std::string shown( const std::string& aNetName )
{
    return DisplayNetName( aNetName );
}


/// What a footprint is called in the plan: its reference, or its library name if it has none.
std::string footprintLabel( const Footprint& aFp )
{
    if( !aFp.reference.empty() )
        return aFp.reference;

    size_t colon = aFp.libId.find( ':' );
    return colon == std::string::npos ? aFp.libId : aFp.libId.substr( colon + 1 );
}


struct PadDecision
{
    int         pad;
    std::string current, target;
    bool        noPin;
};


struct FootprintDecision
{
    size_t comp;
    int    footprint;
    bool   byReference = false;
    bool   setReference = false, setValue = false, setPath = false, differs = false;
    std::vector<PadDecision> pads; ///< pads whose net changes, and pads without a pin
};

} // anonymous namespace


// ---------------------------------------------------------------------------------------- plan

void PlanPcbUpdate( const Schematic& aSch, const Board& aBoard, PcbUpdate& aPlan )
{
    aPlan = PcbUpdate();

    const Collected              col = collectComponents( aSch );
    const std::vector<Component>& comps = col.comps;
    UpdateCounts&                 cnt = aPlan.counts;

    cnt.symbols = col.symbols;
    cnt.footprints = (int) aBoard.footprints.size();

    auto add = [&]( UpdateKind aKind, UpdateSeverity aSeverity, int aFootprint, std::string aText )
    {
        aPlan.lines.push_back( { aKind, aSeverity, aFootprint, std::move( aText ) } );

        if( aSeverity == UpdateSeverity::WARNING )
            ++cnt.warnings;
    };

    for( const std::string& missing : aSch.missingFiles )
    {
        aPlan.incomplete = true;
        add( UpdateKind::INCOMPLETE, UpdateSeverity::WARNING, -1,
             TrF( "The schematic is incomplete: sub-sheet file {0} is missing.", { missing } ) );
    }

    // ---- pads of each footprint
    std::vector<std::vector<int>> padsOf( aBoard.footprints.size() );

    for( size_t i = 0; i < aBoard.pads.size(); ++i )
    {
        int fp = aBoard.pads[i].footprint;

        if( fp >= 0 && fp < (int) padsOf.size() )
            padsOf[(size_t) fp].push_back( (int) i );
    }

    // ---- match symbols and footprints: by symbol path, then by reference designator
    std::vector<int>  fpOfComp( comps.size(), -1 );
    std::vector<char> byRef( comps.size(), 0 ), fpUsed( aBoard.footprints.size(), 0 );
    std::unordered_map<std::string, std::vector<int>> fpByPath, fpByRef;
    std::unordered_set<std::string>                   allKeys;

    for( const Component& c : comps )
        allKeys.insert( c.keys.begin(), c.keys.end() );

    for( size_t i = 0; i < aBoard.footprints.size(); ++i )
    {
        const Footprint& fp = aBoard.footprints[i];

        if( isBoardOnly( fp ) )
        {
            fpUsed[i] = 1; // "not in schematic": never matched, never reported
            continue;
        }

        if( !fp.path.empty() )
            fpByPath[normalizePath( fp.path )].push_back( (int) i );

        fpByRef[lowerAscii( fp.reference )].push_back( (int) i );
    }

    auto prefer = [&]( const std::vector<int>& aCandidates, const Component& aComp ) -> int
    {
        int first = -1;

        for( int f : aCandidates )
        {
            if( fpUsed[(size_t) f] )
                continue;

            if( first < 0 )
                first = f;

            if( aComp.footprint.empty() || fpidsEquivalent( aBoard.footprints[(size_t) f].libId, aComp.footprint ) )
                return f;
        }

        return first;
    };

    for( size_t c = 0; c < comps.size(); ++c )
    {
        for( const std::string& key : comps[c].keys )
        {
            auto it = fpByPath.find( key );

            if( it == fpByPath.end() )
                continue;

            int f = prefer( it->second, comps[c] );

            if( f >= 0 )
            {
                fpOfComp[c] = f;
                fpUsed[(size_t) f] = 1;
                break;
            }
        }
    }

    for( size_t c = 0; c < comps.size(); ++c )
    {
        if( fpOfComp[c] >= 0 )
            continue;

        auto it = fpByRef.find( lowerAscii( comps[c].ref ) );

        if( it == fpByRef.end() )
            continue;

        // footprints that belong to another symbol by their path are not taken over
        std::vector<int> free;

        for( int f : it->second )
        {
            const std::string& path = aBoard.footprints[(size_t) f].path;

            if( path.empty() || !allKeys.count( normalizePath( path ) ) )
                free.push_back( f );
        }

        int f = prefer( free, comps[c] );

        if( f >= 0 )
        {
            fpOfComp[c] = f;
            fpUsed[(size_t) f] = 1;
            byRef[c] = 1;
        }
    }

    // ---- decisions per footprint
    std::vector<FootprintDecision> decisions;
    std::vector<std::string>       finalNet( aBoard.pads.size() );

    for( size_t i = 0; i < aBoard.pads.size(); ++i )
        finalNet[i] = netNameOf( aBoard, aBoard.pads[i].net );

    for( size_t c = 0; c < comps.size(); ++c )
    {
        if( fpOfComp[c] < 0 )
            continue;

        const Component& comp = comps[c];
        const Footprint& fp = aBoard.footprints[(size_t) fpOfComp[c]];
        FootprintDecision d;
        d.comp = c;
        d.footprint = fpOfComp[c];
        d.byReference = byRef[c] != 0;
        d.setReference = fp.reference != comp.ref;
        d.setValue = fp.value != comp.value;
        d.setPath = std::find( comp.keys.begin(), comp.keys.end(), normalizePath( fp.path ) ) == comp.keys.end();
        d.differs = !comp.footprint.empty() && !fpidsEquivalent( fp.libId, comp.footprint );

        std::set<std::string> padNetnames;

        // KiCad goes through the pads in the order of their UUIDs: it decides which of several
        // no-connect pads gets the plain net name and which the numbered ones
        std::vector<int> ordered = padsOf[(size_t) fpOfComp[c]];
        std::stable_sort( ordered.begin(), ordered.end(),
                          [&]( int a, int b )
                          {
                              return normalizeKiid( aBoard.pads[(size_t) a].uuid )
                                     < normalizeKiid( aBoard.pads[(size_t) b].uuid );
                          } );

        for( int pi : ordered )
        {
            const Pad&    pad = aBoard.pads[(size_t) pi];
            const PinNet* pin = comp.NetOf( pad.number );
            const bool    onCopper = padOnCopper( aBoard, pad );
            const std::string current = netNameOf( aBoard, pad.net );
            std::string       target;
            bool              noPin = false;

            if( !pin || !onCopper )
            {
                noPin = !pin && onCopper && !pad.number.empty() && current.empty();
            }
            else
            {
                target = pin->net;

                // a pad that must stay unconnected gets a net of its own
                if( pin->noConnect )
                {
                    for( int jj = 1; !padNetnames.insert( target ).second
                                     || ( target != pin->net && col.netNames.count( target ) );
                         ++jj )
                        target = pin->net + "_" + std::to_string( jj );
                }
            }

            finalNet[(size_t) pi] = target;

            if( target != current || noPin )
                d.pads.push_back( { pi, current, target, noPin } );
        }

        std::sort( d.pads.begin(), d.pads.end(), []( const PadDecision& a, const PadDecision& b ) { return a.pad < b.pad; } );
        decisions.push_back( std::move( d ) );
    }

    // ---- nets
    std::unordered_set<std::string> boardNets;
    std::map<std::string, std::vector<int>> padsOnNet; // before

    for( size_t n = 1; n < aBoard.nets.size(); ++n )
    {
        if( !aBoard.nets[n].empty() )
            boardNets.insert( aBoard.nets[n] );
    }

    for( size_t i = 0; i < aBoard.pads.size(); ++i )
    {
        std::string name = netNameOf( aBoard, aBoard.pads[i].net );

        if( !name.empty() )
            padsOnNet[name].push_back( (int) i );
    }

    std::unordered_set<std::string> usedAfter;

    for( const std::string& n : finalNet )
    {
        if( !n.empty() )
            usedAfter.insert( n );
    }

    // renamed in place: every pad of the old net goes to one new net that does not exist yet
    std::map<std::string, std::pair<std::string, size_t>> bestFor; // new name -> old name, pads
    std::unordered_map<std::string, std::string>          renamed; // old -> new

    for( size_t n = 1; n < aBoard.nets.size(); ++n )
    {
        const std::string& old = aBoard.nets[n];
        auto               on = padsOnNet.find( old );

        if( old.empty() || on == padsOnNet.end() || col.netNames.count( old ) || usedAfter.count( old ) )
            continue;

        const std::string& target = finalNet[(size_t) on->second.front()];

        if( target.empty() || boardNets.count( target ) )
            continue;

        bool same = true;

        for( int pi : on->second )
            same &= finalNet[(size_t) pi] == target;

        if( !same )
            continue;

        auto best = bestFor.find( target );

        if( best == bestFor.end() || on->second.size() > best->second.second )
            bestFor[target] = { old, on->second.size() };
    }

    for( const auto& [target, old] : bestFor )
        renamed[old.first] = target;

    // net ids of the board for the item counts
    std::vector<int> tracksOn( aBoard.nets.size(), 0 ), viasOn( aBoard.nets.size(), 0 ), zonesOn( aBoard.nets.size(), 0 );

    auto bump = [&]( std::vector<int>& v, int net )
    {
        if( net > 0 && net < (int) v.size() )
            ++v[(size_t) net];
    };

    for( const Track& t : aBoard.tracks )
        bump( tracksOn, t.net );

    for( const ArcTrack& t : aBoard.arcs )
        bump( tracksOn, t.net );

    for( const Via& v : aBoard.vias )
        bump( viasOn, v.net );

    for( const Zone& z : aBoard.zones )
        bump( zonesOn, z.net );

    // ---- lines, footprint by footprint
    for( size_t c = 0; c < comps.size(); ++c )
    {
        const Component& comp = comps[c];

        if( fpOfComp[c] < 0 )
        {
            ++cnt.missingFootprints;

            if( comp.footprint.empty() )
                add( UpdateKind::MISSING_FOOTPRINT, UpdateSeverity::INFO, -1,
                     TrF( "Symbol {0} has no footprint on the board.", { comp.ref } ) );
            else
                add( UpdateKind::MISSING_FOOTPRINT, UpdateSeverity::INFO, -1,
                     TrF( "Symbol {0} has no footprint on the board ({1}).", { comp.ref, comp.footprint } ) );

            continue;
        }
    }

    for( const FootprintDecision& d : decisions )
    {
        const Component& comp = comps[d.comp];
        const Footprint& fp = aBoard.footprints[(size_t) d.footprint];
        PcbUpdate::FootprintChange change;
        change.footprint = d.footprint;

        if( d.byReference )
        {
            ++cnt.matchedByReference;
            add( UpdateKind::MATCH_REFERENCE, UpdateSeverity::INFO, d.footprint,
                 TrF( "Footprint {0} was matched to its symbol by reference designator.", { fp.reference } ) );
        }
        else
        {
            ++cnt.matchedByPath;
        }

        if( d.setReference )
        {
            ++cnt.referenceChanges;
            change.setReference = true;
            change.reference = comp.ref;
            add( UpdateKind::REFERENCE, UpdateSeverity::ACTION, d.footprint,
                 TrF( "Change {0} reference designator to {1}.", { fp.reference, comp.ref } ) );
        }

        if( d.setValue )
        {
            ++cnt.valueChanges;
            change.setValue = true;
            change.value = comp.value;
            add( UpdateKind::VALUE, UpdateSeverity::ACTION, d.footprint,
                 TrF( "Change {0} value from {1} to {2}.", { comp.ref, fp.value, comp.value } ) );
        }

        if( d.setPath )
        {
            ++cnt.associationChanges;
            change.setPath = true;
            change.path = comp.path;
            add( UpdateKind::ASSOCIATION, UpdateSeverity::ACTION, d.footprint,
                 TrF( "Update {0} symbol association from {1} to {2}.",
                      { comp.ref, fp.path.empty() ? std::string( Tr( "(none)" ) ) : fp.path, comp.path } ) );
        }

        if( d.differs )
        {
            ++cnt.footprintDiffers;
            add( UpdateKind::FOOTPRINT_DIFFERS, UpdateSeverity::INFO, d.footprint,
                 TrF( "{0} uses footprint {1}, the symbol says {2}; the footprint is not replaced.",
                      { comp.ref, fp.libId, comp.footprint } ) );
        }

        for( const PadDecision& p : d.pads )
        {
            const Pad& pad = aBoard.pads[(size_t) p.pad];

            if( p.noPin )
            {
                add( UpdateKind::NO_PIN, UpdateSeverity::WARNING, d.footprint,
                     TrF( "No net found for {0} pad {1} (no pin {1} in the symbol).", { comp.ref, pad.number } ) );
                continue;
            }

            // a pad of a net that is renamed goes with it: no edit of its own
            auto rn = renamed.find( p.current );

            if( rn == renamed.end() || rn->second != p.target )
                change.pads.push_back( { p.pad, p.target } );

            if( p.target.empty() )
            {
                ++cnt.padsDisconnected;
                add( UpdateKind::DISCONNECT, UpdateSeverity::ACTION, d.footprint,
                     TrF( "Disconnect {0} pin {1}.", { comp.ref, pad.number } ) );
            }
            else if( p.current.empty() )
            {
                ++cnt.padsConnected;
                add( UpdateKind::CONNECT, UpdateSeverity::ACTION, d.footprint,
                     TrF( "Connect {0} pin {1} to {2}.", { comp.ref, pad.number, shown( p.target ) } ) );
            }
            else
            {
                ++cnt.padsReconnected;
                add( UpdateKind::RECONNECT, UpdateSeverity::ACTION, d.footprint,
                     TrF( "Reconnect {0} pin {1} from {2} to {3}.",
                          { comp.ref, pad.number, shown( p.current ), shown( p.target ) } ) );
            }
        }

        if( change.setReference || change.setValue || change.setPath || !change.pads.empty() )
            aPlan.footprints.push_back( std::move( change ) );
    }

    // ---- net lines
    for( const auto& [target, old] : bestFor )
    {
        ++cnt.netsRenamed;
        aPlan.renames.push_back( { old.first, target } );
        add( UpdateKind::NET_RENAME, UpdateSeverity::ACTION, -1,
             TrF( "Rename net {0} to {1}; its tracks, vias and zones follow.", { shown( old.first ), shown( target ) } ) );
    }

    std::unordered_set<std::string> added;

    for( const FootprintDecision& d : decisions )
    {
        for( const PadDecision& p : d.pads )
        {
            if( p.noPin || p.target.empty() || boardNets.count( p.target ) || bestFor.count( p.target )
                || !added.insert( p.target ).second )
                continue;

            ++cnt.netsAdded;
            aPlan.addedNets.push_back( p.target );
            add( UpdateKind::NET_ADD, UpdateSeverity::ACTION, -1, TrF( "Add net {0}.", { shown( p.target ) } ) );
        }
    }

    for( size_t n = 1; n < aBoard.nets.size(); ++n )
    {
        const std::string& name = aBoard.nets[n];

        if( name.empty() || renamed.count( name ) )
            continue;

        const bool items = tracksOn[n] + viasOn[n] + zonesOn[n] > 0;

        if( !usedAfter.count( name ) )
        {
            if( items )
            {
                add( UpdateKind::NET_KEPT, UpdateSeverity::WARNING, -1,
                     TrF( "Net {0} has no pads left but is still used by {1} tracks, {2} vias and {3} zones; it is kept.",
                          { shown( name ), std::to_string( tracksOn[n] ), std::to_string( viasOn[n] ),
                            std::to_string( zonesOn[n] ) } ) );
            }
            else if( aBoard.netTable )
            {
                ++cnt.netsRemoved;
                aPlan.removedNets.push_back( name );
                add( UpdateKind::NET_REMOVE, UpdateSeverity::ACTION, -1, TrF( "Remove unused net {0}.", { shown( name ) } ) );
            }

            continue;
        }

        bool lost = false;

        for( int pi : padsOnNet[name] )
            lost |= finalNet[(size_t) pi] != name;

        if( lost && items )
        {
            ++cnt.netsWithItemsLeft;
            add( UpdateKind::NET_ITEMS_STAY, UpdateSeverity::INFO, -1,
                 TrF( "Pads moved away from net {0}, but its {1} tracks, {2} vias and {3} zones stay on it.",
                      { shown( name ), std::to_string( tracksOn[n] ), std::to_string( viasOn[n] ),
                        std::to_string( zonesOn[n] ) } ) );
        }
    }

    // ---- footprints without a symbol stay as they are
    for( size_t i = 0; i < aBoard.footprints.size(); ++i )
    {
        if( fpUsed[i] )
            continue;

        ++cnt.extraFootprints;
        add( UpdateKind::EXTRA_FOOTPRINT, UpdateSeverity::INFO, (int) i,
             TrF( "Footprint {0} has no symbol in the schematic; it is kept.", { footprintLabel( aBoard.footprints[i] ) } ) );
    }

    cnt.changes = cnt.referenceChanges + cnt.valueChanges + cnt.associationChanges + cnt.padsConnected
                  + cnt.padsReconnected + cnt.padsDisconnected + cnt.netsAdded + cnt.netsRenamed + cnt.netsRemoved;
}


std::string PcbUpdateText( const PcbUpdate& aPlan )
{
    std::string out;

    for( const UpdateLine& l : aPlan.lines )
    {
        static const char* sev[] = { "action", "info", "warning" };
        out += std::to_string( (int) l.kind ) + "\t" + sev[(int) l.severity] + "\t" + l.text + "\n";
    }

    return out;
}


// --------------------------------------------------------------------------------------- apply

namespace
{

struct Patch
{
    size_t      begin;
    size_t      end;
    std::string text;
};


std::string applyPatches( const std::string& aSource, std::vector<Patch> aPatches )
{
    std::stable_sort( aPatches.begin(), aPatches.end(),
                      []( const Patch& a, const Patch& b ) { return a.begin < b.begin; } );

    std::string out;
    out.reserve( aSource.size() + 64 );
    size_t pos = 0;

    for( const Patch& p : aPatches )
    {
        if( p.begin < pos )
            continue; // overlapping patch: ignore

        out.append( aSource, pos, p.begin - pos );
        out += p.text;
        pos = p.end;
    }

    out.append( aSource, pos, std::string::npos );
    return out;
}


/// A parsed copy of a document item; the nodes carry byte offsets into the body.
struct ItemTree
{
    std::string body;
    SPool       pool;
    N           root;

    explicit ItemTree( const std::string& aBody ) :
            body( aBody )
    {
        SReader r( body.data(), body.size() );
        root = r.ParseAll( pool );
    }
};


/// A text the way the node it replaces was written: quoted if that was, else if it must be.
std::string textToken( N aOriginal, const std::string& aText )
{
    if( ( aOriginal && aOriginal.isString() ) || needsQuotes( aText ) )
        return QuoteString( aText );

    return aText;
}


/// Replace nodes "(net ... name)", "(net_name name)" and "(add_net name)" that carry aFrom.
void collectNetRenames( const ItemTree& aTree, N aNode, const std::string& aFrom, const std::string& aTo,
                        std::vector<Patch>& aPatches )
{
    for( N c = aNode.first(); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        const std::string_view head = c.head();

        if( head == "net" )
        {
            N a0 = c.arg( 0 ), a1 = c.arg( 1 );

            if( a0 && a0.isNumber() )
            {
                if( a1 && a1.isAtom() && a1.text() == aFrom )
                    aPatches.push_back( { a1.begin(), a1.end(), textToken( a1, aTo ) } );
            }
            else if( a0 && a0.isAtom() && !a1 && a0.text() == aFrom )
            {
                aPatches.push_back( { a0.begin(), a0.end(), textToken( a0, aTo ) } );
            }
        }
        else if( head == "net_name" || head == "add_net" )
        {
            N a0 = c.arg( 0 );

            if( a0 && a0.isAtom() && a0.text() == aFrom )
                aPatches.push_back( { a0.begin(), a0.end(), textToken( a0, aTo ) } );
        }

        collectNetRenames( aTree, c, aFrom, aTo, aPatches );
    }
}


bool isHead( N aNode, std::initializer_list<const char*> aHeads )
{
    if( !aNode.isList() )
        return false;

    for( const char* h : aHeads )
    {
        if( aNode.head() == h )
            return true;
    }

    return false;
}


/// The white space in front of a child node (the text since the previous sibling).
std::string spaceBefore( const ItemTree& aTree, N aParent, N aChild )
{
    size_t prevEnd = aParent.begin() + 1;

    for( N c = aParent.first(); c; c = c.next() )
    {
        if( c.begin() == aChild.begin() )
            break;

        prevEnd = c.end();
    }

    return aTree.body.substr( prevEnd, aChild.begin() - prevEnd );
}


/// The file's text for a net reference of a pad.
struct NetStyle
{
    bool namesOnly = false; ///< "(net "name")", no net numbers (KiCad 10)
    int  version = 0;
};

std::string netNodeText( const NetStyle& aStyle, int aCode, const std::string& aName )
{
    if( aStyle.namesOnly )
        return "(net " + QuoteString( aName ) + ")";

    // KiCad 6 and later always quote net names; KiCad 4 and 5 only when needed
    std::string name = aStyle.version >= 20200000 || needsQuotes( aName ) ? QuoteString( aName ) : aName;
    return "(net " + std::to_string( aCode ) + " " + name + ")";
}

} // anonymous namespace


bool ApplyPcbUpdate( EditTarget& aTarget, const PcbUpdate& aPlan, std::string& aError )
{
    Document&    doc = aTarget.doc;
    const Board& board = aTarget.board;

    if( aPlan.incomplete )
    {
        aError = Tr( "The schematic is incomplete (sub-sheet files are missing); the board was not changed" );
        return false;
    }

    if( aPlan.counts.changes == 0 )
    {
        aError = Tr( "The board is already up to date with the schematic" );
        return false;
    }

    NetStyle style;
    style.namesOnly = !board.netTable;
    style.version = board.version;

    if( style.namesOnly && board.version < 20251000 && !aPlan.addedNets.empty() )
    {
        aError = Tr( "This board file has no net table to add nets to" );
        return false;
    }

    // net declarations "(net N name)" at the top level of the file
    struct Decl
    {
        int         item;
        int         code;
        std::string name;
    };

    std::vector<Decl> decls;
    int               lastDecl = -1, maxCode = 0;

    for( int i : doc.LiveItems() )
    {
        const std::string& body = doc.Item( (size_t) i ).body;

        if( body.size() < 6 || body.compare( 0, 4, "(net" ) != 0 || !std::isspace( (unsigned char) body[4] ) )
            continue;

        ItemTree tree( body );
        decls.push_back( { i, (int) tree.root.num( 0, -1 ), std::string( tree.root.str( 1 ) ) } );
        lastDecl = i;
        maxCode = std::max( maxCode, decls.back().code );
    }

    // net numbers the pads can use
    std::unordered_map<std::string, int> codeOf;

    for( size_t n = 1; n < board.nets.size(); ++n )
    {
        if( n < board.netCode.size() && board.netCode[n] >= 0 )
            codeOf[board.nets[n]] = board.netCode[n];
    }

    for( const PcbUpdate::NetRename& r : aPlan.renames )
    {
        auto from = codeOf.find( r.from );

        if( from != codeOf.end() )
            codeOf[r.to] = from->second;
    }

    for( const std::string& name : aPlan.addedNets )
        codeOf[name] = ++maxCode;

    if( !style.namesOnly && lastDecl < 0 )
    {
        aError = Tr( "This board file has no net table to add nets to" );
        return false;
    }

    std::string label = Tr( "Update PCB from schematic" );
    doc.Begin( label );

    try
    {
        // renamed nets: every place that names them
        for( const PcbUpdate::NetRename& r : aPlan.renames )
        {
            const std::string quoted = QuoteString( r.from );
            const std::string inner = quoted.substr( 1, quoted.size() - 2 );

            for( int i : doc.LiveItems() )
            {
                const std::string& body = doc.Item( (size_t) i ).body;

                if( body.find( r.from ) == std::string::npos && body.find( inner ) == std::string::npos )
                    continue;

                ItemTree           tree( body );
                std::vector<Patch> patches;
                collectNetRenames( tree, tree.root, r.from, r.to, patches );

                if( !patches.empty() )
                    doc.Replace( i, applyPatches( tree.body, patches ) );
            }
        }

        // footprints
        std::vector<std::vector<int>> padsOf( board.footprints.size() );

        for( size_t i = 0; i < board.pads.size(); ++i )
        {
            int fp = board.pads[i].footprint;

            if( fp >= 0 && fp < (int) padsOf.size() )
                padsOf[(size_t) fp].push_back( (int) i );
        }

        for( const PcbUpdate::FootprintChange& fc : aPlan.footprints )
        {
            const Footprint& fp = board.footprints[(size_t) fc.footprint];
            int di = fp.docItem >= 0 && fp.docItem < (int) aTarget.docMap.size() ? aTarget.docMap[(size_t) fp.docItem] : -1;

            if( di < 0 )
                throw std::runtime_error( Tr( "Footprint not found in the file" ) );

            ItemTree           tree( doc.Item( (size_t) di ).body );
            std::vector<Patch> patches;

            auto setText = [&]( const char* aField, const std::string& aValue )
            {
                bool found = false;

                for( N c = tree.root.arg( 0 ); c; c = c.next() )
                {
                    if( !isHead( c, { "property", "fp_text" } ) || lowerAscii( std::string( c.str( 0 ) ) ) != lowerAscii( aField ) )
                        continue;

                    N v = c.arg( 1 );

                    if( v && v.isAtom() )
                    {
                        patches.push_back( { v.begin(), v.end(), textToken( v, aValue ) } );
                        found = true;
                    }
                }

                if( !found )
                    throw std::runtime_error( TrF( "The footprint has no \"{0}\" field", { std::string( aField ) } ) );
            };

            if( fc.setReference )
                setText( "Reference", fc.reference );

            if( fc.setValue )
                setText( "Value", fc.value );

            if( fc.setPath )
            {
                N pathNode = tree.root.find( "path" );

                if( pathNode )
                {
                    N a = pathNode.arg( 0 );
                    patches.push_back( { a ? a.begin() : pathNode.end() - 1, a ? a.end() : pathNode.end() - 1,
                                         QuoteString( fc.path ) } );
                }
                else
                {
                    // right before what KiCad writes after it
                    N anchor;

                    for( N c = tree.root.arg( 0 ); c && !anchor; c = c.next() )
                    {
                        if( c.isList() && !isHead( c, { "property", "descr", "tags", "layer", "uuid", "tstamp", "at",
                                                        "locked", "placed", "version", "generator", "tedit", "sheetname",
                                                        "sheetfile" } ) )
                            anchor = c;
                    }

                    if( anchor )
                        patches.push_back( { anchor.begin(), anchor.begin(),
                                             "(path " + QuoteString( fc.path ) + ")" + spaceBefore( tree, tree.root, anchor ) } );
                    else
                        patches.push_back( { tree.root.end() - 1, tree.root.end() - 1,
                                             " (path " + QuoteString( fc.path ) + ")" } );
                }
            }

            if( !fc.pads.empty() )
            {
                std::vector<N> padNodes;
                tree.root.each( "pad", [&]( N p ) { padNodes.push_back( p ); } );

                if( padNodes.size() != padsOf[(size_t) fc.footprint].size() )
                    throw std::runtime_error(
                            TrF( "The footprint {0} cannot be updated: its pads in the file do not match the board model",
                                 { fp.reference } ) );

                std::unordered_map<int, size_t> position;

                for( size_t k = 0; k < padsOf[(size_t) fc.footprint].size(); ++k )
                    position[padsOf[(size_t) fc.footprint][k]] = k;

                for( const PcbUpdate::PadChange& pc : fc.pads )
                {
                    N pad = padNodes[position.at( pc.pad )];
                    N net = pad.find( "net" );

                    if( pc.net.empty() )
                    {
                        // no net: no node
                        if( net )
                        {
                            size_t prevEnd = pad.begin() + 1;

                            for( N c = pad.first(); c; c = c.next() )
                            {
                                if( c.begin() == net.begin() )
                                    break;

                                prevEnd = c.end();
                            }

                            patches.push_back( { prevEnd, net.end(), std::string() } );
                        }

                        continue;
                    }

                    int         code = codeOf.count( pc.net ) ? codeOf[pc.net] : 0;
                    NetStyle    here = style;
                    std::string text;

                    if( net )
                    {
                        N a0 = net.arg( 0 );
                        here.namesOnly = !( a0 && a0.isNumber() );

                        if( !here.namesOnly && !net.arg( 1 ) )
                            text = "(net " + std::to_string( code ) + ")"; // code only
                        else
                            text = netNodeText( here, code, pc.net );

                        patches.push_back( { net.begin(), net.end(), text } );
                    }
                    else
                    {
                        text = netNodeText( here, code, pc.net );

                        // where KiCad writes it: after the layers, before pin function, margins, uuid
                        N anchor;

                        for( N c = pad.arg( 0 ); c && !anchor; c = c.next() )
                        {
                            if( isHead( c, { "pinfunction", "pintype", "die_length", "die_delay", "solder_mask_margin",
                                             "solder_paste_margin", "solder_paste_margin_ratio", "clearance",
                                             "zone_connect", "thermal_width", "thermal_bridge_width", "thermal_gap",
                                             "thermal_bridge_angle", "teardrops", "options", "primitives", "tstamp",
                                             "uuid", "property", "padstack", "net_tie" } ) )
                                anchor = c;
                        }

                        if( anchor )
                            patches.push_back( { anchor.begin(), anchor.begin(), text + spaceBefore( tree, pad, anchor ) } );
                        else
                        {
                            // last node: same indent as the one before it
                            N last;

                            for( N c = pad.first(); c; c = c.next() )
                            {
                                if( c.isList() )
                                    last = c;
                            }

                            std::string ws = last ? spaceBefore( tree, pad, last ) : std::string( " " );
                            patches.push_back( { pad.end() - 1, pad.end() - 1, ws + text } );
                        }
                    }
                }
            }

            if( !patches.empty() )
                doc.Replace( di, applyPatches( tree.body, patches ) );
        }

        // new nets are declared next to the others, before anything refers to them
        if( !style.namesOnly )
        {
            std::string ws = doc.Item( (size_t) lastDecl ).ws;
            int         anchor = lastDecl;

            for( const std::string& name : aPlan.addedNets )
                anchor = doc.InsertAfter( anchor, netNodeText( style, codeOf[name], name ), ws );

            std::set<std::string> removed( aPlan.removedNets.begin(), aPlan.removedNets.end() );

            for( const Decl& d : decls )
            {
                if( d.code != 0 && removed.count( d.name ) )
                    doc.Remove( d.item );
            }
        }
    }
    catch( const std::exception& e )
    {
        doc.Cancel();
        aError = e.what();
        return false;
    }

    doc.Commit();
    return true;
}


bool UpdatePcbFromSchematic( EditTarget& aTarget, const Schematic& aSch, std::string& aError )
{
    PcbUpdate plan;
    PlanPcbUpdate( aSch, aTarget.board, plan );
    return ApplyPcbUpdate( aTarget, plan, aError );
}

} // namespace kp
