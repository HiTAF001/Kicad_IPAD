/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "sch_pick.h"

#include "i18n.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace kp
{

namespace
{

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

    return s == "-0" ? "0" : s;
}


std::string xy( const Pt& p )
{
    return "X " + num( p.x ) + "   Y " + num( p.y ) + " mm";
}


struct Info
{
    std::string text;

    explicit Info( const std::string& aTitle ) : text( aTitle ) {}

    void add( const char* k, const std::string& v ) { addRaw( Tr( k ), v ); }

    void addRaw( const std::string& k, const std::string& v )
    {
        if( !v.empty() )
            text += "\n" + k + "\t" + v;
    }
};


std::string unitLetter( int unit )
{
    std::string s;
    int         u = std::max( 1, unit );

    do
    {
        --u;
        s.insert( s.begin(), (char) ( 'A' + u % 26 ) );
        u /= 26;
    } while( u > 0 );

    return s;
}


const char* pinTypeName( PinType t )
{
    switch( t )
    {
    case PinType::INPUT: return N_( "Input" );
    case PinType::OUTPUT: return N_( "Output" );
    case PinType::BIDI: return N_( "Bidirectional" );
    case PinType::TRISTATE: return N_( "Tri-state" );
    case PinType::PASSIVE: return N_( "Passive" );
    case PinType::UNSPECIFIED: return N_( "Unspecified" );
    case PinType::POWER_IN: return N_( "Power input" );
    case PinType::POWER_OUT: return N_( "Power output" );
    case PinType::OPEN_COLLECTOR: return N_( "Open collector" );
    case PinType::OPEN_EMITTER: return N_( "Open emitter" );
    case PinType::NO_CONNECT: return N_( "Not connected" );
    case PinType::FREE: return N_( "Free" );
    }

    return N_( "Unspecified" );
}


const char* shapeName( LabelShape s )
{
    switch( s )
    {
    case LabelShape::INPUT: return N_( "Input" );
    case LabelShape::OUTPUT: return N_( "Output" );
    case LabelShape::BIDI: return N_( "Bidirectional" );
    case LabelShape::TRISTATE: return N_( "Tri-state" );
    case LabelShape::PASSIVE: return N_( "Passive" );
    case LabelShape::DOT: return N_( "Dot" );
    case LabelShape::ROUND: return N_( "Round" );
    case LabelShape::DIAMOND: return N_( "Diamond" );
    case LabelShape::RECTANGLE: return N_( "Rectangle" );
    }

    return N_( "Passive" );
}


/// What a flat SCH_NOTE index refers to.
struct NoteRef
{
    enum Kind { TEXT, GRAPHIC, IMAGE, NOCONNECT, BUSENTRY, NONE } kind = NONE;
    int index = -1;
};

NoteRef noteRef( const SchScreen& sc, int idx )
{
    if( idx < 0 )
        return {};

    size_t i = (size_t) idx;

    if( i < sc.texts.size() )
        return { NoteRef::TEXT, (int) i };

    i -= sc.texts.size();

    if( i < sc.graphics.size() )
        return { NoteRef::GRAPHIC, (int) i };

    i -= sc.graphics.size();

    if( i < sc.images.size() )
        return { NoteRef::IMAGE, (int) i };

    i -= sc.images.size();

    if( i < sc.noConnects.size() )
        return { NoteRef::NOCONNECT, (int) i };

    i -= sc.noConnects.size();

    if( i < sc.busEntries.size() )
        return { NoteRef::BUSENTRY, (int) i };

    return {};
}


int noteIndex( const SchScreen& sc, NoteRef::Kind k, int i )
{
    int base = (int) sc.texts.size() + (int) sc.graphics.size() + (int) sc.images.size();
    return k == NoteRef::NOCONNECT ? base + i : base + (int) sc.noConnects.size() + i;
}


/// Flat sheet pin index -> (sheet, pin).
bool sheetPinRef( const SchScreen& sc, int idx, int& sheet, int& pin )
{
    if( idx < 0 )
        return false;

    for( size_t s = 0; s < sc.sheets.size(); ++s )
    {
        if( idx < (int) sc.sheets[s].pins.size() )
        {
            sheet = (int) s;
            pin = idx;
            return true;
        }

        idx -= (int) sc.sheets[s].pins.size();
    }

    return false;
}


int sheetPinBase( const SchScreen& sc, int sheet )
{
    int n = 0;

    for( int s = 0; s < sheet && s < (int) sc.sheets.size(); ++s )
        n += (int) sc.sheets[(size_t) s].pins.size();

    return n;
}


int pinSymbol( const std::vector<int>& base, int flat )
{
    auto it = std::upper_bound( base.begin(), base.end(), flat );
    return (int) ( it - base.begin() ) - 1;
}


bool graphicShown( const LibSymbol& lib, int unit, int bodyStyle, int gUnit, int gBody )
{
    if( gUnit != 0 && gUnit != unit )
        return false;

    return lib.bodyStyleCount <= 1 || gBody == 0 || gBody == bodyStyle;
}


/// Box of a symbol's body (its graphics and pins, without the fields).
BBox symbolBody( const Schematic& sch, const SheetNode& node, const SchScreen& sc, const SchSymbol& sym )
{
    BBox             b;
    const LibSymbol* lib = sch.Lib( sc, sym );

    if( !lib )
        return b;

    const int unit = sch.Unit( node, sym );
    auto      add = [&]( const Pt& p ) { b.add( sym.pos + sym.transform.apply( p ) ); };

    for( const SchGraphic& g : lib->graphics )
    {
        if( !graphicShown( *lib, unit, sym.bodyStyle, g.unit, g.bodyStyle ) || g.kind == SchGraphic::TEXT )
            continue;

        switch( g.kind )
        {
        case SchGraphic::CIRCLE:
            add( g.center - Pt( g.radius, g.radius ) );
            add( g.center + Pt( g.radius, g.radius ) );
            break;
        case SchGraphic::ARC:
            add( g.start );
            add( g.mid );
            add( g.end );
            break;
        default:
            for( const Pt& p : g.pts )
                add( p );
        }
    }

    for( const LibPin& p : lib->pins )
    {
        if( graphicShown( *lib, unit, sym.bodyStyle, p.unit, p.bodyStyle ) )
        {
            Pt tip, root;
            SchematicPinPoints( sym, p, tip, root );
            b.add( tip );
            b.add( root );
        }
    }

    return b;
}


void itemRanges( const Mesh& m, ItemType t, int i, std::vector<ItemRange>& out )
{
    if( (size_t) t >= m.offsets.size() )
        return;

    const auto& off = m.offsets[(size_t) t];

    if( i < 0 || (size_t) i + 1 >= off.size() )
        return;

    for( uint32_t r = off[(size_t) i]; r < off[(size_t) i + 1] && r < m.ranges.size(); ++r )
        out.push_back( m.ranges[r] );
}


BBox rangesBox( const Mesh& m, const std::vector<ItemRange>& rs )
{
    BBox b;

    for( const ItemRange& r : rs )
    {
        for( uint32_t k = r.start; k < r.start + r.count && k < m.indices.size(); ++k )
        {
            uint32_t v = m.indices[k];
            b.add( Pt( m.vertices[2 * (size_t) v], m.vertices[2 * (size_t) v + 1] ) );
        }
    }

    return b;
}


double triangleDistance( const Pt& p, const Pt& a, const Pt& b, const Pt& c )
{
    auto cross = []( const Pt& o, const Pt& u, const Pt& v ) { return ( u.x - o.x ) * ( v.y - o.y ) - ( u.y - o.y ) * ( v.x - o.x ); };
    double d1 = cross( a, b, p ), d2 = cross( b, c, p ), d3 = cross( c, a, p );
    bool   neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;

    if( !( neg && pos ) )
        return 0.0;

    return std::min( { PointSegmentDistance( p, a, b ), PointSegmentDistance( p, b, c ), PointSegmentDistance( p, c, a ) } );
}


const SheetNets* netsOf( const Schematic& sch, int sheet )
{
    if( sheet < 0 || sheet >= (int) sch.sheetNets.size() )
        return nullptr;

    return &sch.sheetNets[(size_t) sheet];
}


int at( const std::vector<int>& v, int i )
{
    return i >= 0 && i < (int) v.size() ? v[(size_t) i] : -1;
}


std::string netLabel( const Schematic& sch, int net )
{
    if( net < 0 || net >= (int) sch.nets.size() )
        return Tr( "(no net)" );

    return DisplayNetName( sch.nets[(size_t) net].name );
}


std::string fieldValue( const Schematic& sch, const SheetNode& node, const SchSymbol& sym, const char* name )
{
    for( const SchField& f : sym.fields )
    {
        if( f.name == name )
            return sch.FieldValue( node, sym, f );
    }

    return std::string();
}

} // anonymous namespace


int SchItemNet( const Schematic& aSch, int aSheet, ItemType aType, int aIndex )
{
    const SheetNets* sn = netsOf( aSch, aSheet );

    if( !sn || aSch.sheets[(size_t) aSheet].screen < 0 )
        return -1;

    const SchScreen& sc = aSch.screens[(size_t) aSch.sheets[(size_t) aSheet].screen];

    switch( aType )
    {
    case ItemType::SCH_PIN: return at( sn->pin, aIndex );
    case ItemType::SCH_WIRE: return at( sn->wire, aIndex );
    case ItemType::SCH_JUNCTION: return at( sn->junction, aIndex );
    case ItemType::SCH_LABEL: return at( sn->label, aIndex );
    case ItemType::SCH_SHEET_PIN: return at( sn->sheetPin, aIndex );
    case ItemType::SCH_NOTE:
    {
        NoteRef r = noteRef( sc, aIndex );

        if( r.kind == NoteRef::NOCONNECT )
            return at( sn->noConnect, r.index );

        if( r.kind == NoteRef::BUSENTRY )
            return at( sn->busEntry, r.index );

        return -1;
    }
    default: return -1;
    }
}


void SchPick( const Schematic& aSch, int aSheet, const Mesh& aMesh, SchPickCache& aCache, const Pt& aPoint,
              double aTolerance, const PickFilter& aFilter, std::vector<PickHit>& aOut )
{
    aOut.clear();

    if( aSheet < 0 || aSheet >= (int) aSch.sheets.size() || aSch.sheets[(size_t) aSheet].screen < 0 )
        return;

    const SheetNode& node = aSch.sheets[(size_t) aSheet];
    const SchScreen& sc = aSch.screens[(size_t) node.screen];
    const std::vector<int> pinBase = SchematicPinBase( aSch, sc );

    static const ItemType TYPES[] = { ItemType::SCH_PIN,   ItemType::SCH_JUNCTION, ItemType::SCH_SHEET_PIN,
                                      ItemType::SCH_LABEL, ItemType::SCH_WIRE,     ItemType::SCH_NOTE,
                                      ItemType::SCH_SYMBOL, ItemType::SCH_SHEET };

    if( aCache.boxes.size() != (size_t) ItemType::COUNT )
    {
        aCache.boxes.assign( (size_t) ItemType::COUNT, {} );

        for( ItemType t : TYPES )
        {
            if( (size_t) t >= aMesh.offsets.size() || aMesh.offsets[(size_t) t].empty() )
                continue;

            const size_t count = aMesh.offsets[(size_t) t].size() - 1;
            auto&        boxes = aCache.boxes[(size_t) t];
            boxes.resize( count );

            for( size_t i = 0; i < count; ++i )
            {
                std::vector<ItemRange> rs;
                itemRanges( aMesh, t, (int) i, rs );
                boxes[i] = rangesBox( aMesh, rs );
            }
        }
    }

    auto priority = [&]( ItemType t, int idx )
    {
        switch( t )
        {
        case ItemType::SCH_PIN: return 0;
        case ItemType::SCH_JUNCTION:
        case ItemType::SCH_SHEET_PIN: return 1;
        case ItemType::SCH_LABEL: return 2;
        case ItemType::SCH_NOTE:
        {
            NoteRef r = noteRef( sc, idx );
            return r.kind == NoteRef::NOCONNECT || r.kind == NoteRef::BUSENTRY ? 2 : 5;
        }
        case ItemType::SCH_WIRE: return 3;
        case ItemType::SCH_SYMBOL: return 4;
        default: return 6;
        }
    };

    auto addHit = [&]( ItemType t, int idx, int prio, double dist, double size )
    {
        PickHit h;
        h.type = t;
        h.index = idx;
        h.priority = prio;
        h.size = size;
        h.layerOrder = (int) ( -dist * 1e4 );
        h.net = SchItemNet( aSch, aSheet, t, idx );

        if( t == ItemType::SCH_PIN )
            h.footprint = pinSymbol( pinBase, idx );
        else if( t == ItemType::SCH_SHEET_PIN )
        {
            int s = -1, p = -1;

            if( sheetPinRef( sc, idx, s, p ) )
                h.footprint = s;
        }

        aOut.push_back( h );
    };

    std::set<std::pair<int, int>> hit;

    for( ItemType t : TYPES )
    {
        if( (size_t) t >= aCache.boxes.size() )
            continue;

        const auto& boxes = aCache.boxes[(size_t) t];

        for( size_t i = 0; i < boxes.size(); ++i )
        {
            const BBox& b = boxes[i];

            if( !b.valid() || aPoint.x < b.minX - aTolerance || aPoint.x > b.maxX + aTolerance
                || aPoint.y < b.minY - aTolerance || aPoint.y > b.maxY + aTolerance )
                continue;

            std::vector<ItemRange> rs;
            itemRanges( aMesh, t, (int) i, rs );
            double best = 1e30;

            for( const ItemRange& r : rs )
            {
                if( !aFilter.layerVisible( r.slot ) )
                    continue;

                for( uint32_t k = r.start; k + 3 <= r.start + r.count && k + 3 <= aMesh.indices.size(); k += 3 )
                {
                    auto v = [&]( uint32_t n )
                    {
                        uint32_t vi = aMesh.indices[n];
                        return Pt( aMesh.vertices[2 * (size_t) vi], aMesh.vertices[2 * (size_t) vi + 1] );
                    };

                    best = std::min( best, triangleDistance( aPoint, v( k ), v( k + 1 ), v( k + 2 ) ) );

                    if( best == 0.0 )
                        break;
                }

                if( best == 0.0 )
                    break;
            }

            if( best <= aTolerance )
            {
                addHit( t, (int) i, priority( t, (int) i ), best, ( b.maxX - b.minX ) * ( b.maxY - b.minY ) );
                hit.insert( { (int) t, (int) i } );
            }
        }
    }

    // inside an unfilled symbol body or sheet box
    auto visible = [&]( int slot ) { return aFilter.layerVisible( slot ); };

    for( size_t s = 0; s < sc.symbols.size(); ++s )
    {
        if( hit.count( { (int) ItemType::SCH_SYMBOL, (int) s } ) )
            continue;

        BBox b = symbolBody( aSch, node, sc, sc.symbols[s] );
        std::vector<ItemRange> rs;
        itemRanges( aMesh, ItemType::SCH_SYMBOL, (int) s, rs );

        if( b.valid() && !rs.empty() && visible( rs[0].slot ) && aPoint.x >= b.minX && aPoint.x <= b.maxX
            && aPoint.y >= b.minY && aPoint.y <= b.maxY )
            addHit( ItemType::SCH_SYMBOL, (int) s, 7, 0.0, ( b.maxX - b.minX ) * ( b.maxY - b.minY ) );
    }

    for( size_t s = 0; s < sc.sheets.size(); ++s )
    {
        const SchSheet& sh = sc.sheets[s];
        std::vector<ItemRange> rs;
        itemRanges( aMesh, ItemType::SCH_SHEET, (int) s, rs );

        if( hit.count( { (int) ItemType::SCH_SHEET, (int) s } ) || rs.empty() || !visible( rs[0].slot ) )
            continue;

        if( aPoint.x >= sh.pos.x && aPoint.x <= sh.pos.x + sh.size.x && aPoint.y >= sh.pos.y && aPoint.y <= sh.pos.y + sh.size.y )
            addHit( ItemType::SCH_SHEET, (int) s, 8, 0.0, sh.size.x * sh.size.y );
    }

    std::stable_sort( aOut.begin(), aOut.end(), []( const PickHit& a, const PickHit& b )
                      {
                          if( a.priority != b.priority )
                              return a.priority < b.priority;

                          if( a.layerOrder != b.layerOrder )
                              return a.layerOrder > b.layerOrder; // nearer first

                          return a.size < b.size;
                      } );
}


std::string SchItemInfo( const Schematic& aSch, int aSheet, ItemType aType, int aIndex )
{
    if( aSheet < 0 || aSheet >= (int) aSch.sheets.size() || aSch.sheets[(size_t) aSheet].screen < 0 )
        return std::string();

    const SheetNode& node = aSch.sheets[(size_t) aSheet];
    const SchScreen& sc = aSch.screens[(size_t) node.screen];
    const int        net = SchItemNet( aSch, aSheet, aType, aIndex );

    switch( aType )
    {
    case ItemType::SCH_SYMBOL:
    {
        if( aIndex < 0 || aIndex >= (int) sc.symbols.size() )
            break;

        const SchSymbol& sym = sc.symbols[(size_t) aIndex];
        const LibSymbol* lib = aSch.Lib( sc, sym );
        std::string      ref = SchematicShownReference( aSch, node, sym );
        Info             info( ref.empty() ? Tr( "Symbol" ) : ref );
        info.add( "Value", DisplayNetName( fieldValue( aSch, node, sym, "Value" ) ) );
        info.add( "Footprint", fieldValue( aSch, node, sym, "Footprint" ) );
        info.add( "Library", sym.libId );

        if( lib && lib->power )
            info.add( "Type", Tr( "Power symbol" ) );

        if( lib && lib->unitCount > 1 )
        {
            int unit = aSch.Unit( node, sym );
            info.add( "Unit", TrF( "{0} of {1}", { unitLetter( unit ), std::to_string( lib->unitCount ) } ) );
        }

        if( lib )
        {
            int pins = 0;
            int unit = aSch.Unit( node, sym );

            for( const LibPin& p : lib->pins )
                pins += graphicShown( *lib, unit, sym.bodyStyle, p.unit, p.bodyStyle );

            info.add( "Pins", std::to_string( pins ) );
        }

        info.add( "Position", xy( sym.pos ) );

        std::string attrs;
        auto        attr = [&]( bool on, const char* t ) { if( on ) attrs += ( attrs.empty() ? "" : ", " ) + std::string( Tr( t ) ); };
        attr( sym.dnp, N_( "Do not populate (DNP)" ) );
        attr( !sym.inBom, N_( "Not in BOM" ) );
        attr( !sym.onBoard, N_( "Not on board" ) );
        attr( sym.excludeFromSim, N_( "Excluded from simulation" ) );
        info.add( "Attributes", attrs );

        std::string ds = fieldValue( aSch, node, sym, "Datasheet" );

        if( ds != "~" )
            info.add( "Datasheet", ds );

        info.add( "Description", fieldValue( aSch, node, sym, "Description" ) );

        std::set<std::string> skip = { "Reference", "Value", "Footprint", "Datasheet", "Description" };
        std::vector<std::pair<std::string, std::string>> extra;

        for( const SchField& f : sym.fields )
        {
            if( !skip.count( f.name ) && !f.name.empty() && f.name.rfind( "ki_", 0 ) != 0 )
                extra.emplace_back( f.name, aSch.FieldValue( node, sym, f ) );
        }

        std::sort( extra.begin(), extra.end() );

        for( const auto& [k, v] : extra )
            info.addRaw( k, v );

        return info.text;
    }

    case ItemType::SCH_PIN:
    {
        const std::vector<int> base = SchematicPinBase( aSch, sc );
        const int              s = pinSymbol( base, aIndex );

        if( s < 0 || s >= (int) sc.symbols.size() )
            break;

        const SchSymbol& sym = sc.symbols[(size_t) s];
        const LibSymbol* lib = aSch.Lib( sc, sym );
        const int        li = aIndex - base[(size_t) s];

        if( !lib || li < 0 || li >= (int) lib->pins.size() )
            break;

        const LibPin& pin = lib->pins[(size_t) li];
        std::string   ref = aSch.Reference( node, sym );
        std::string   value = DisplayNetName( fieldValue( aSch, node, sym, "Value" ) );
        Info          info( TrF( "Pin {0}", { ref.empty() ? pin.number : ref + "-" + pin.number } ) );
        std::string   shownRef = SchematicShownReference( aSch, node, sym );
        info.add( "Symbol", value.empty() ? shownRef : shownRef + " (" + value + ")" );

        if( !pin.name.empty() && pin.name != "~" )
            info.add( "Name", pin.name );

        info.add( "Number", pin.number );
        info.add( "Electrical type", Tr( pinTypeName( pin.type ) ) );
        info.add( "Net", netLabel( aSch, net ) );
        Pt tip, root;
        SchematicPinPoints( sym, pin, tip, root );
        info.add( "Position", xy( tip ) );
        return info.text;
    }

    case ItemType::SCH_WIRE:
    {
        if( aIndex < 0 || aIndex >= (int) sc.lines.size() )
            break;

        const SchLine& l = sc.lines[(size_t) aIndex];
        Info info( Tr( l.kind == SchLine::WIRE ? N_( "Wire" ) : l.kind == SchLine::BUS ? N_( "Bus" ) : N_( "Graphic line" ) ) );

        if( l.kind != SchLine::NOTES )
            info.add( l.kind == SchLine::BUS ? N_( "Bus" ) : N_( "Net" ), netLabel( aSch, net ) );

        info.add( "Length", num( std::hypot( l.b.x - l.a.x, l.b.y - l.a.y ) ) + " mm" );
        info.add( "Start", xy( l.a ) );
        info.add( "End", xy( l.b ) );
        return info.text;
    }

    case ItemType::SCH_JUNCTION:
    {
        if( aIndex < 0 || aIndex >= (int) sc.junctions.size() )
            break;

        Info info( Tr( "Junction" ) );
        info.add( "Net", netLabel( aSch, net ) );
        info.add( "Position", xy( sc.junctions[(size_t) aIndex].pos ) );
        return info.text;
    }

    case ItemType::SCH_LABEL:
    {
        if( aIndex < 0 || aIndex >= (int) sc.labels.size() )
            break;

        const SchLabel& L = sc.labels[(size_t) aIndex];
        const char*     title = L.kind == SchLabel::GLOBAL         ? N_( "Global label" )
                                : L.kind == SchLabel::HIERARCHICAL ? N_( "Hierarchical label" )
                                : L.kind == SchLabel::DIRECTIVE    ? N_( "Directive label" )
                                                                   : N_( "Label" );
        Info info( Tr( title ) );
        info.add( "Text", DisplayNetName( L.text ) );

        if( L.kind == SchLabel::GLOBAL || L.kind == SchLabel::HIERARCHICAL )
            info.add( "Shape", Tr( shapeName( L.shape ) ) );

        if( L.kind != SchLabel::DIRECTIVE )
        {
            const bool bus = net >= 0 && aSch.nets[(size_t) net].bus;
            info.add( bus ? N_( "Bus" ) : N_( "Net" ), netLabel( aSch, net ) );

            std::vector<std::string> members = BusMembers( L.text );

            if( members.size() > 1 || ( members.size() == 1 && members[0] != L.text ) )
            {
                std::string list;

                for( size_t i = 0; i < members.size() && i < 16; ++i )
                    list += ( i ? ", " : "" ) + members[i];

                if( members.size() > 16 )
                    list += ", …";

                info.add( "Members", list );
            }
        }

        for( const SchField& f : L.fields )
        {
            if( !f.value.empty() && f.name != "Intersheetrefs" )
                info.addRaw( f.name, f.value );
        }

        info.add( "Position", xy( L.pos ) );
        return info.text;
    }

    case ItemType::SCH_SHEET:
    {
        if( aIndex < 0 || aIndex >= (int) sc.sheets.size() )
            break;

        const SchSheet& sh = sc.sheets[(size_t) aIndex];
        Info            info( sh.name.empty() ? Tr( "Sheet" ) : sh.name );
        info.add( "File", sh.file );

        int child = -1;

        for( int c : node.children )
        {
            if( aSch.sheets[(size_t) c].sheetItem == aIndex )
                child = c;
        }

        if( child >= 0 )
            info.add( "Page", aSch.sheets[(size_t) child].page );
        else
            info.add( "Status", Tr( "File not available" ) );

        info.add( "Pins", std::to_string( sh.pins.size() ) );
        info.add( "Size", num( sh.size.x ) + " × " + num( sh.size.y ) + " mm" );
        info.add( "Position", xy( sh.pos ) );
        return info.text;
    }

    case ItemType::SCH_SHEET_PIN:
    {
        int s = -1, p = -1;

        if( !sheetPinRef( sc, aIndex, s, p ) )
            break;

        const SchSheetPin& pin = sc.sheets[(size_t) s].pins[(size_t) p];
        Info               info( TrF( "Sheet pin {0}", { DisplayNetName( pin.name ) } ) );
        info.add( "Sheet", sc.sheets[(size_t) s].name );
        info.add( "Shape", Tr( shapeName( pin.shape ) ) );
        const bool bus = net >= 0 && aSch.nets[(size_t) net].bus;
        info.add( bus ? N_( "Bus" ) : N_( "Net" ), netLabel( aSch, net ) );
        info.add( "Position", xy( pin.pos ) );
        return info.text;
    }

    case ItemType::SCH_NOTE:
    {
        NoteRef r = noteRef( sc, aIndex );

        switch( r.kind )
        {
        case NoteRef::TEXT:
        {
            const SchText& t = sc.texts[(size_t) r.index];
            std::string    first = t.text.substr( 0, t.text.find( '\n' ) );

            if( first.size() > 80 )
                first = first.substr( 0, 77 ) + "…";

            Info info( Tr( "Text" ) );
            info.add( "Text", first );
            info.add( "Position", xy( t.pos ) );
            return info.text;
        }
        case NoteRef::GRAPHIC:
        {
            const SchGraphic& g = sc.graphics[(size_t) r.index];
            Info info( Tr( g.kind == SchGraphic::TEXTBOX ? N_( "Text box" ) : N_( "Graphic" ) ) );

            if( g.kind == SchGraphic::TEXTBOX )
                info.add( "Text", g.text.substr( 0, std::min<size_t>( 80, g.text.find( '\n' ) ) ) );

            return info.text;
        }
        case NoteRef::IMAGE:
        {
            Info info( Tr( "Image" ) );
            info.add( "Position", xy( sc.images[(size_t) r.index].pos ) );
            return info.text;
        }
        case NoteRef::NOCONNECT:
        {
            Info info( Tr( "No-connect flag" ) );
            info.add( "Net", netLabel( aSch, net ) );
            info.add( "Position", xy( sc.noConnects[(size_t) r.index].pos ) );
            return info.text;
        }
        case NoteRef::BUSENTRY:
        {
            Info info( Tr( "Bus entry" ) );
            info.add( "Net", netLabel( aSch, net ) );
            info.add( "Position", xy( sc.busEntries[(size_t) r.index].pos ) );
            return info.text;
        }
        default: break;
        }

        break;
    }

    default: break;
    }

    return std::string();
}


std::vector<int> SchNetSheets( const Schematic& aSch, int aNet )
{
    std::vector<int> out;

    for( size_t s = 0; s < aSch.sheetNets.size(); ++s )
    {
        const SheetNets& sn = aSch.sheetNets[s];
        auto has = [&]( const std::vector<int>& v ) { return std::find( v.begin(), v.end(), aNet ) != v.end(); };

        if( has( sn.wire ) || has( sn.pin ) || has( sn.label ) || has( sn.sheetPin ) || has( sn.junction )
            || has( sn.busEntry ) || has( sn.noConnect ) )
            out.push_back( (int) s );
    }

    return out;
}


std::string SchNetInfo( const Schematic& aSch, int aSheet, int aNet )
{
    if( aNet < 0 || aNet >= (int) aSch.nets.size() )
        return std::string();

    const SchNet& n = aSch.nets[(size_t) aNet];
    Info          info( DisplayNetName( n.name ) );

    if( n.bus )
        info.add( "Type", Tr( "Bus" ) );
    else
        info.add( "Pins", std::to_string( n.pinCount ) );

    if( n.global )
        info.add( "Scope", Tr( "Global" ) );

    // symbols on the net, over the whole hierarchy
    std::set<std::string> refs;

    for( size_t si = 0; si < aSch.sheets.size() && si < aSch.sheetNets.size(); ++si )
    {
        const SheetNode& node = aSch.sheets[si];

        if( node.screen < 0 )
            continue;

        const SchScreen&       sc = aSch.screens[(size_t) node.screen];
        const std::vector<int> base = SchematicPinBase( aSch, sc );
        const SheetNets&       sn = aSch.sheetNets[si];

        for( size_t f = 0; f < sn.pin.size(); ++f )
        {
            if( sn.pin[f] != aNet )
                continue;

            int s = pinSymbol( base, (int) f );

            if( s < 0 || s >= (int) sc.symbols.size() )
                continue;

            std::string ref = aSch.Reference( node, sc.symbols[(size_t) s] );

            if( !ref.empty() && ref[0] != '#' )
                refs.insert( ref );
        }
    }

    std::string list;
    int         count = 0;

    for( const std::string& r : refs )
    {
        if( count++ == 24 )
        {
            list += ", …";
            break;
        }

        list += ( list.empty() ? "" : ", " ) + r;
    }

    info.add( "Components", list );

    std::vector<int> sheets = SchNetSheets( aSch, aNet );

    if( sheets.size() > 1 || ( sheets.size() == 1 && sheets[0] != aSheet ) )
    {
        std::string names;

        for( size_t i = 0; i < sheets.size() && i < 12; ++i )
            names += ( i ? ", " : "" ) + SheetPathName( aSch, sheets[i] );

        if( sheets.size() > 12 )
            names += ", …";

        info.add( "Sheets", names );
    }

    return info.text;
}


BBox SchItemBounds( const Schematic& aSch, int aSheet, const Mesh& aMesh, ItemType aType, int aIndex )
{
    std::vector<ItemRange> rs;
    SchHighlightRanges( aSch, aSheet, aMesh, aType, aIndex, false, -1, rs );
    BBox b = rangesBox( aMesh, rs );

    if( !b.valid() && aSheet >= 0 && aSheet < (int) aSch.sheets.size() && aSch.sheets[(size_t) aSheet].screen >= 0 )
    {
        // invisible pins and the like: their position at least
        const SheetNode& node = aSch.sheets[(size_t) aSheet];
        const SchScreen& sc = aSch.screens[(size_t) node.screen];

        if( aType == ItemType::SCH_PIN )
        {
            const std::vector<int> base = SchematicPinBase( aSch, sc );
            int                    s = pinSymbol( base, aIndex );

            if( s >= 0 && s < (int) sc.symbols.size() )
            {
                const LibSymbol* lib = aSch.Lib( sc, sc.symbols[(size_t) s] );
                int              li = aIndex - base[(size_t) s];

                if( lib && li >= 0 && li < (int) lib->pins.size() )
                {
                    Pt tip, root;
                    SchematicPinPoints( sc.symbols[(size_t) s], lib->pins[(size_t) li], tip, root );
                    b.add( tip );
                    b.add( root );
                }
            }
        }
    }

    return b;
}


BBox SchNetBounds( const Schematic& aSch, int aSheet, const Mesh& aMesh, int aNet )
{
    std::vector<ItemRange> rs;
    SchHighlightRanges( aSch, aSheet, aMesh, ItemType::NONE, -1, true, aNet, rs );
    return rangesBox( aMesh, rs );
}


void SchHighlightRanges( const Schematic& aSch, int aSheet, const Mesh& aMesh, ItemType aType, int aIndex,
                         bool aWholeNet, int aNet, std::vector<ItemRange>& aOut )
{
    aOut.clear();

    if( aSheet < 0 || aSheet >= (int) aSch.sheets.size() || aSch.sheets[(size_t) aSheet].screen < 0 )
        return;

    const SheetNode& node = aSch.sheets[(size_t) aSheet];
    const SchScreen& sc = aSch.screens[(size_t) node.screen];

    if( aWholeNet )
    {
        const SheetNets* sn = netsOf( aSch, aSheet );

        if( !sn || aNet < 0 )
            return;

        auto each = [&]( const std::vector<int>& nets, ItemType t, auto toIndex )
        {
            for( size_t i = 0; i < nets.size(); ++i )
            {
                if( nets[i] == aNet )
                    itemRanges( aMesh, t, toIndex( (int) i ), aOut );
            }
        };

        auto same = []( int i ) { return i; };
        each( sn->pin, ItemType::SCH_PIN, same );
        each( sn->wire, ItemType::SCH_WIRE, same );
        each( sn->junction, ItemType::SCH_JUNCTION, same );
        each( sn->label, ItemType::SCH_LABEL, same );
        each( sn->sheetPin, ItemType::SCH_SHEET_PIN, same );
        each( sn->noConnect, ItemType::SCH_NOTE, [&]( int i ) { return noteIndex( sc, NoteRef::NOCONNECT, i ); } );
        each( sn->busEntry, ItemType::SCH_NOTE, [&]( int i ) { return noteIndex( sc, NoteRef::BUSENTRY, i ); } );
        return;
    }

    itemRanges( aMesh, aType, aIndex, aOut );

    if( aType == ItemType::SCH_SYMBOL && aIndex >= 0 && aIndex < (int) sc.symbols.size() )
    {
        const std::vector<int> base = SchematicPinBase( aSch, sc );

        for( int f = base[(size_t) aIndex]; f < base[(size_t) aIndex + 1]; ++f )
            itemRanges( aMesh, ItemType::SCH_PIN, f, aOut );
    }
    else if( aType == ItemType::SCH_SHEET && aIndex >= 0 && aIndex < (int) sc.sheets.size() )
    {
        const int first = sheetPinBase( sc, aIndex );

        for( size_t p = 0; p < sc.sheets[(size_t) aIndex].pins.size(); ++p )
            itemRanges( aMesh, ItemType::SCH_SHEET_PIN, first + (int) p, aOut );
    }
}

} // namespace kp
