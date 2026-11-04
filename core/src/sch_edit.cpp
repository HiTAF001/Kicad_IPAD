/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Wire changes (rubber band, splits, junctions) are planned on a small model of the sheet's
 * connections first and then written as a difference: items that keep their place keep their
 * bytes, and an item that goes away is reused for a new one of its kind (same UUID, new
 * points), so that an edit followed by the opposite edit gives the file back.
 */
#include "sch_edit.h"
#include "edit.h"
#include "i18n.h"
#include "sexpr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <tuple>

namespace kp
{

namespace
{

constexpr double SCH_PI = 3.14159265358979323846;

// ---- numbers and points ----------------------------------------------------------------------

/// Schematic coordinates are whole internal units of 100 nm.
int64_t iu( double aMM )
{
    return (int64_t) std::llround( aMM * 1e4 );
}


double roundIU( double aMM )
{
    return (double) iu( aMM ) / 1e4;
}


Pt roundIU( const Pt& p )
{
    return Pt( roundIU( p.x ), roundIU( p.y ) );
}


bool same( const Pt& a, const Pt& b )
{
    return iu( a.x ) == iu( b.x ) && iu( a.y ) == iu( b.y );
}


std::string num( double aMM )
{
    return FormatNumber( roundIU( aMM ) );
}


Pt gridPoint( const Pt& p )
{
    return roundIU( Pt( std::round( p.x / SCH_GRID ) * SCH_GRID, std::round( p.y / SCH_GRID ) * SCH_GRID ) );
}


/// Is p on segment a-b, but not at its ends? (same tolerance as the connectivity)
bool inside( const Pt& p, const Pt& a, const Pt& b )
{
    if( same( p, a ) || same( p, b ) )
        return false;

    const double dx = (double) ( iu( b.x ) - iu( a.x ) ), dy = (double) ( iu( b.y ) - iu( a.y ) );
    const double px = (double) ( iu( p.x ) - iu( a.x ) ), py = (double) ( iu( p.y ) - iu( a.y ) );
    const double len2 = dx * dx + dy * dy, dot = px * dx + py * dy;

    if( len2 <= 0 || dot <= 0 || dot >= len2 )
        return false;

    const double cross = px * dy - py * dx;
    return cross * cross <= 3.0 * len2;
}


/// Direction from one point to another, tenths of a degree.
int angleKey( const Pt& aFrom, const Pt& aTo )
{
    int a = (int) std::lround( std::atan2( (double) ( iu( aTo.y ) - iu( aFrom.y ) ),
                                           (double) ( iu( aTo.x ) - iu( aFrom.x ) ) ) * 1800.0 / SCH_PI );
    return a == -1800 ? 1800 : a;
}


std::string newUuid()
{
    static std::mt19937_64 rng( std::random_device {}() ^ 0x5c4e3a7bULL );
    uint64_t hi = rng(), lo = rng();
    hi = ( hi & 0xFFFFFFFFFFFF0FFFULL ) | 0x0000000000004000ULL; // version 4
    lo = ( lo & 0x3FFFFFFFFFFFFFFFULL ) | 0x8000000000000000ULL; // variant 1
    char buf[40];
    snprintf( buf, sizeof( buf ), "%08x-%04x-%04x-%04x-%012llx", (unsigned) ( hi >> 32 ),
              (unsigned) ( ( hi >> 16 ) & 0xFFFF ), (unsigned) ( hi & 0xFFFF ), (unsigned) ( lo >> 48 ),
              (unsigned long long) ( lo & 0xFFFFFFFFFFFFULL ) );
    return buf;
}


std::string lower( std::string s )
{
    for( char& ch : s )
        ch = (char) std::tolower( (unsigned char) ch );

    return s;
}

// ---- patching items --------------------------------------------------------------------------

struct Patch
{
    size_t      begin;
    size_t      end;
    std::string text;
};

std::string applyPatches( const std::string& aSrc, std::vector<Patch> aPatches )
{
    std::stable_sort( aPatches.begin(), aPatches.end(),
                      []( const Patch& a, const Patch& b ) { return a.begin < b.begin; } );

    std::string out;
    out.reserve( aSrc.size() + 64 );
    size_t pos = 0;

    for( const Patch& p : aPatches )
    {
        if( p.begin < pos ) // overlapping patch: ignore (should not happen)
            continue;

        out.append( aSrc, pos, p.begin - pos );
        out += p.text;
        pos = p.end;
    }

    out.append( aSrc, pos, std::string::npos );
    return out;
}

/// A parsed copy of one document item, with byte offsets. Not copyable (nodes point into body).
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

    ItemTree( const ItemTree& ) = delete;
    ItemTree& operator=( const ItemTree& ) = delete;
};

/// Start of the whitespace in front of offset aPos.
size_t wsStart( const std::string& aBody, size_t aPos )
{
    while( aPos > 0 && std::isspace( (unsigned char) aBody[aPos - 1] ) )
        --aPos;

    return aPos;
}


/// Replace a number atom if its value changes (unchanged numbers keep their bytes).
void setNumber( std::vector<Patch>& aPatches, N aAtom, double aValue )
{
    if( !aAtom.isAtom() || aAtom.isString() )
        return;

    bool   ok = false;
    double old = ParseNumber( aAtom.text(), 0.0, &ok );

    if( ok && iu( old ) == iu( aValue ) )
        return;

    aPatches.push_back( { aAtom.begin(), aAtom.end(), num( aValue ) } );
}


void setPoint( std::vector<Patch>& aPatches, N aNode, const Pt& aPos )
{
    setNumber( aPatches, aNode.arg( 0 ), aPos.x );
    setNumber( aPatches, aNode.arg( 1 ), aPos.y );
}


/// Angle of an "(at x y angle)" node, inserted when the node has none.
void setAngle( std::vector<Patch>& aPatches, N aAt, double aAngle )
{
    N a = aAt.arg( 2 );

    if( a && a.isNumber() )
        setNumber( aPatches, a, aAngle );
    else if( N y = aAt.arg( 1 ) )
        aPatches.push_back( { y.end(), y.end(), " " + num( aAngle ) } );
}


/// Replace a string atom if the text changes.
void setString( std::vector<Patch>& aPatches, N aAtom, const std::string& aValue )
{
    if( !aAtom || !aAtom.isAtom() || std::string( aAtom.text() ) == aValue )
        return;

    aPatches.push_back( { aAtom.begin(), aAtom.end(), QuoteString( aValue ) } );
}


/// Move every point of an item: (at), (xy), (start), (end), (center), (mid) at any depth.
void translateNodes( N aNode, const Pt& aDelta, std::vector<Patch>& aPatches )
{
    for( N c = aNode.first(); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        std::string_view h = c.head();

        if( ( h == "at" || h == "xy" || h == "start" || h == "end" || h == "center" || h == "mid" )
            && c.arg( 0 ).isNumber() && c.arg( 1 ).isNumber() )
        {
            setPoint( aPatches, c, Pt( c.num( 0 ) + aDelta.x, c.num( 1 ) + aDelta.y ) );
        }
        else
        {
            translateNodes( c, aDelta, aPatches );
        }
    }
}


/// "left"/"right" of a text's "(effects (justify ...))", "" when centred.
std::string hJustify( N aText )
{
    N eff = aText.find( "effects" );
    N just = eff ? eff.find( "justify" ) : N();

    for( N c = just ? just.arg( 0 ) : N(); c; c = c.next() )
    {
        if( c.text() == "left" || c.text() == "right" )
            return std::string( c.text() );
    }

    return std::string();
}


/// Set the horizontal justification of a text to "left" or "right".
void setHJustify( const ItemTree& t, N aText, const std::string& aWant, std::vector<Patch>& aPatches )
{
    N eff = aText.find( "effects" );

    if( !eff )
        return;

    if( N just = eff.find( "justify" ) )
    {
        for( N c = just.arg( 0 ); c; c = c.next() )
        {
            if( c.text() == "left" || c.text() == "right" )
            {
                if( c.text() != aWant )
                    aPatches.push_back( { c.begin(), c.end(), aWant } );

                return;
            }
        }

        aPatches.push_back( { just.first().end(), just.first().end(), " " + aWant } );
        return;
    }

    // no justify yet: add one after the font, indented like it
    N      font = eff.find( "font" );
    size_t at = font ? font.end() : eff.first().end();
    size_t ws = font ? wsStart( t.body, font.begin() ) : at;
    std::string indent = font ? t.body.substr( ws, font.begin() - ws ) : std::string( " " );

    if( indent.empty() )
        indent = " ";

    aPatches.push_back( { at, at, indent + "(justify " + aWant + ")" } );
}


void flipHJustify( const ItemTree& t, N aText, std::vector<Patch>& aPatches )
{
    std::string h = hJustify( aText );

    if( !h.empty() )
        setHJustify( t, aText, h == "left" ? "right" : "left", aPatches );
}


std::string headOf( const std::string& aBody )
{
    size_t i = aBody.find( '(' );

    if( i == std::string::npos )
        return std::string();

    size_t e = i + 1;

    while( e < aBody.size() && !std::isspace( (unsigned char) aBody[e] ) && aBody[e] != '(' && aBody[e] != ')' )
        ++e;

    return aBody.substr( i + 1, e - i - 1 );
}


/// "\n" plus the indentation of a whitespace run.
std::string lineWs( const std::string& aWs, const std::string& aDefaultIndent )
{
    size_t nl = aWs.rfind( '\n' );
    return "\n" + ( nl == std::string::npos ? aDefaultIndent : aWs.substr( nl + 1 ) );
}

// ---- writing new items -----------------------------------------------------------------------

/// How the file's KiCad version writes schematic items.
struct Style
{
    bool multiline = true;    ///< KiCad 8+: one token per line, tab indented
    bool quoteUuid = true;    ///< KiCad 8+ quotes UUIDs
    bool strokeColor = false; ///< KiCad 6 writes the stroke colour
};

Style styleOf( int aVersion )
{
    Style s;
    s.multiline = aVersion >= 20231120;
    s.quoteUuid = aVersion >= 20231120;
    s.strokeColor = aVersion < 20220000;
    return s;
}


std::string uuidText( const Style& st )
{
    std::string u = newUuid();
    return st.quoteUuid ? "(uuid \"" + u + "\")" : "(uuid " + u + ")";
}


/// A new item for the KiCad 8+ writer: head, atoms, then child lists.
struct Sx
{
    std::string              head;
    std::vector<std::string> atoms;
    std::vector<Sx>          kids;
};

std::string printSx( const Sx& n, int aDepth )
{
    std::string out = "(" + n.head;

    for( const std::string& a : n.atoms )
        out += " " + a;

    if( n.kids.empty() )
        return out + ")";

    const std::string in( (size_t) aDepth + 1, '\t' );

    if( n.head == "pts" )
    {
        out += "\n" + in;

        for( size_t i = 0; i < n.kids.size(); ++i )
            out += ( i ? " " : "" ) + printSx( n.kids[i], aDepth + 1 );
    }
    else
    {
        for( const Sx& k : n.kids )
            out += "\n" + in + printSx( k, aDepth + 1 );
    }

    return out + "\n" + std::string( (size_t) aDepth, '\t' ) + ")";
}


Sx uuidSx()
{
    return { "uuid", { QuoteString( newUuid() ) }, {} };
}


Sx pointSx( const char* aKey, const Pt& p )
{
    return { aKey, { num( p.x ), num( p.y ) }, {} };
}


std::string wireText( const Style& st, bool aBus, const Pt& a, const Pt& b )
{
    const char* head = aBus ? "bus" : "wire";

    if( st.multiline )
    {
        Sx w { head, {}, { { "pts", {}, { pointSx( "xy", a ), pointSx( "xy", b ) } },
                           { "stroke", {}, { { "width", { "0" }, {} }, { "type", { "default" }, {} } } },
                           uuidSx() } };
        return printSx( w, 1 );
    }

    return std::string( "(" ) + head + " (pts (xy " + num( a.x ) + " " + num( a.y ) + ") (xy " + num( b.x ) + " "
           + num( b.y ) + "))\n    (stroke (width 0) (type default)" + ( st.strokeColor ? " (color 0 0 0 0)" : "" )
           + ")\n    " + uuidText( st ) + "\n  )";
}


std::string junctionText( const Style& st, const Pt& p )
{
    if( st.multiline )
    {
        Sx j { "junction", {}, { pointSx( "at", p ), { "diameter", { "0" }, {} }, { "color", { "0", "0", "0", "0" }, {} },
                                 uuidSx() } };
        return printSx( j, 1 );
    }

    return "(junction (at " + num( p.x ) + " " + num( p.y ) + ") (diameter 0) (color 0 0 0 0)\n    " + uuidText( st )
           + "\n  )";
}


std::string noConnectText( const Style& st, const Pt& p )
{
    if( st.multiline )
        return printSx( { "no_connect", {}, { pointSx( "at", p ), uuidSx() } }, 1 );

    return "(no_connect (at " + num( p.x ) + " " + num( p.y ) + ") " + uuidText( st ) + ")";
}


const char* labelHead( SchLabelKind aKind )
{
    return aKind == SchLabelKind::GLOBAL ? "global_label"
           : aKind == SchLabelKind::HIERARCHICAL ? "hierarchical_label"
                                                 : "label";
}


std::string labelText( const Style& st, SchLabelKind aKind, const std::string& aText, const Pt& p, int aAngle,
                       double aSize )
{
    const bool  right = aAngle == 180 || aAngle == 270;
    std::string just = right ? "right" : "left";
    std::string size = num( aSize ) + " " + num( aSize );

    if( aKind == SchLabelKind::LOCAL )
        just += " bottom";

    if( st.multiline )
    {
        Sx L { labelHead( aKind ), { QuoteString( aText ) }, {} };

        if( aKind != SchLabelKind::LOCAL )
            L.kids.push_back( { "shape", { "input" }, {} } );

        L.kids.push_back( { "at", { num( p.x ), num( p.y ), std::to_string( aAngle ) }, {} } );
        L.kids.push_back( { "effects", {}, { { "font", {}, { { "size", { num( aSize ), num( aSize ) }, {} } } },
                                             { "justify", { just }, {} } } } );
        L.kids.push_back( uuidSx() );
        return printSx( L, 1 );
    }

    return std::string( "(" ) + labelHead( aKind ) + " " + QuoteString( aText )
           + ( aKind != SchLabelKind::LOCAL ? " (shape input)" : "" ) + " (at " + num( p.x ) + " " + num( p.y ) + " "
           + std::to_string( aAngle ) + ")\n    (effects (font (size " + size + ")) (justify " + just + "))\n    "
           + uuidText( st ) + "\n  )";
}

// ---- the sheet being edited ------------------------------------------------------------------

bool validTarget( const SchEditTarget& t, std::string& err )
{
    const Schematic& sch = t.state.sch;

    if( t.sheet < 0 || t.sheet >= (int) sch.sheets.size() )
    {
        err = Tr( "Not a schematic" );
        return false;
    }

    int screen = sch.sheets[(size_t) t.sheet].screen;

    if( screen < 0 || screen >= (int) sch.screens.size() )
    {
        err = Tr( "Not a schematic" );
        return false;
    }

    int file = sch.screens[(size_t) screen].sourceFile;

    if( file < 0 || file >= (int) t.doc.FileCount() || file >= (int) t.state.items.size() )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    return true;
}

struct Ctx
{
    SchEditTarget&          t;
    const Schematic&        sch;
    const SheetNode&        node;
    const SchScreen&        sc;
    int                     file;
    const std::vector<int>& items;
    Style                   style;

    explicit Ctx( SchEditTarget& aT ) :
            t( aT ),
            sch( aT.state.sch ),
            node( sch.sheets[(size_t) aT.sheet] ),
            sc( sch.screens[(size_t) node.screen] ),
            file( sc.sourceFile ),
            items( aT.state.items[(size_t) sc.sourceFile] ),
            style( styleOf( sc.version ) )
    {
    }

    /// Document item of a parsed element (-1 if unknown).
    int docItem( int aParseIndex ) const
    {
        return aParseIndex >= 0 && aParseIndex < (int) items.size() ? items[(size_t) aParseIndex] : -1;
    }

    const std::string& body( int aDocItem ) const { return t.doc.Doc( (size_t) file ).Item( (size_t) aDocItem ).body; }

    void replace( int aDocItem, const std::string& aOld, const std::vector<Patch>& aPatches )
    {
        if( !aPatches.empty() )
            t.doc.Replace( file, aDocItem, applyPatches( aOld, aPatches ) );
    }

    std::string shownRef( int aSymbol ) const
    {
        return SchematicShownReference( sch, node, sc.symbols[(size_t) aSymbol] );
    }
};

/// Insert a new item where KiCad keeps its kind: after the last item of the same kind, else
/// in front of the closing sections (sheet and symbol instances, embedded files).
int insertItem( Ctx& c, const std::string& aBody, int aFile = -1 )
{
    const int        file = aFile < 0 ? c.file : aFile;
    const Document&  d = c.t.doc.Doc( (size_t) file );
    const std::string head = headOf( aBody );
    const std::string indent = c.style.multiline ? "\t" : "  ";
    int              lastSame = -1, firstTail = -1;

    for( int i : d.LiveItems() )
    {
        std::string h = headOf( d.Item( (size_t) i ).body );

        if( h == head )
            lastSame = i;

        if( firstTail < 0
            && ( h == "sheet_instances" || h == "symbol_instances" || h == "embedded_fonts" || h == "embedded_files" ) )
        {
            firstTail = i;
        }
    }

    if( lastSame >= 0 && ( firstTail < 0 || lastSame != firstTail ) )
        return c.t.doc.InsertAfter( file, lastSame, aBody, lineWs( d.Item( (size_t) lastSame ).ws, indent ) );

    if( firstTail >= 0 )
        return c.t.doc.InsertBefore( file, firstTail, aBody, lineWs( d.Item( (size_t) firstTail ).ws, indent ) );

    return c.t.doc.InsertAfter( file, -1, aBody, lineWs( d.LastItemWhitespace(), indent ) );
}

// ---- items of the sheet ----------------------------------------------------------------------

struct Sel
{
    std::set<int> symbols, sheets, labels, junctions, noConnects, entries, texts, graphics, images, lines, sheetPins;

    bool empty() const
    {
        return symbols.empty() && sheets.empty() && labels.empty() && junctions.empty() && noConnects.empty()
               && entries.empty() && texts.empty() && graphics.empty() && images.empty() && lines.empty()
               && sheetPins.empty();
    }
};

int pinOwner( const Ctx& c, int aFlatPin )
{
    std::vector<int> base = SchematicPinBase( c.sch, c.sc );

    for( size_t s = 0; s + 1 < base.size(); ++s )
    {
        if( aFlatPin >= base[s] && aFlatPin < base[s + 1] )
            return (int) s;
    }

    return -1;
}


bool sheetPinRef( const SchScreen& sc, int aFlat, int& aSheet, int& aPin )
{
    if( aFlat < 0 )
        return false;

    for( size_t s = 0; s < sc.sheets.size(); ++s )
    {
        if( aFlat < (int) sc.sheets[s].pins.size() )
        {
            aSheet = (int) s;
            aPin = aFlat;
            return true;
        }

        aFlat -= (int) sc.sheets[s].pins.size();
    }

    return false;
}


/// Sort picked items into a selection. Pins stand for their symbol (or sheet) when
/// aPinIsOwner, else a sheet pin is an item of its own and a symbol pin an error.
bool resolve( const Ctx& c, const std::vector<SchItemRef>& aItems, Sel& sel, bool aPinIsOwner, std::string& err )
{
    const SchScreen& sc = c.sc;

    auto check = [&]( int i, size_t n ) -> bool
    {
        if( i >= 0 && i < (int) n )
            return true;

        err = Tr( "Invalid item" );
        return false;
    };

    for( const SchItemRef& r : aItems )
    {
        int i = r.index;

        switch( r.type )
        {
        case ItemType::SCH_SYMBOL:
            if( !check( i, sc.symbols.size() ) )
                return false;

            sel.symbols.insert( i );
            break;

        case ItemType::SCH_PIN:
        {
            if( !aPinIsOwner )
            {
                err = Tr( "A pin cannot be deleted on its own; delete the symbol instead" );
                return false;
            }

            int s = pinOwner( c, i );

            if( !check( s, sc.symbols.size() ) )
                return false;

            sel.symbols.insert( s );
            break;
        }

        case ItemType::SCH_WIRE:
            if( !check( i, sc.lines.size() ) )
                return false;

            sel.lines.insert( i );
            break;

        case ItemType::SCH_JUNCTION:
            if( !check( i, sc.junctions.size() ) )
                return false;

            sel.junctions.insert( i );
            break;

        case ItemType::SCH_LABEL:
            if( !check( i, sc.labels.size() ) )
                return false;

            sel.labels.insert( i );
            break;

        case ItemType::SCH_SHEET:
            if( !check( i, sc.sheets.size() ) )
                return false;

            sel.sheets.insert( i );
            break;

        case ItemType::SCH_SHEET_PIN:
        {
            int sh = -1, pin = -1;

            if( !sheetPinRef( sc, i, sh, pin ) )
                return check( -1, 0 );

            if( aPinIsOwner )
                sel.sheets.insert( sh );
            else
                sel.sheetPins.insert( i );

            break;
        }

        case ItemType::SCH_NOTE:
        {
            size_t n = i < 0 ? SIZE_MAX : (size_t) i;

            if( n < sc.texts.size() )
            {
                sel.texts.insert( (int) n );
                break;
            }

            n -= sc.texts.size();

            if( n < sc.graphics.size() )
            {
                sel.graphics.insert( (int) n );
                break;
            }

            n -= sc.graphics.size();

            if( n < sc.images.size() )
            {
                sel.images.insert( (int) n );
                break;
            }

            n -= sc.images.size();

            if( n < sc.noConnects.size() )
            {
                sel.noConnects.insert( (int) n );
                break;
            }

            n -= sc.noConnects.size();

            if( n < sc.busEntries.size() )
            {
                sel.entries.insert( (int) n );
                break;
            }

            return check( -1, 0 );
        }

        default:
            err = Tr( "Invalid item" );
            return false;
        }
    }

    return true;
}


/// A wire with several points is one file item: select all of its segments (likewise the
/// cells of a table).
void wholeItems( const Ctx& c, Sel& sel )
{
    std::set<int> lineItems, graphicItems;

    for( int i : sel.lines )
        lineItems.insert( c.sc.lines[(size_t) i].docItem );

    for( int i : sel.graphics )
        graphicItems.insert( c.sc.graphics[(size_t) i].docItem );

    for( size_t i = 0; i < c.sc.lines.size(); ++i )
    {
        if( lineItems.count( c.sc.lines[i].docItem ) )
            sel.lines.insert( (int) i );
    }

    for( size_t i = 0; i < c.sc.graphics.size(); ++i )
    {
        if( graphicItems.count( c.sc.graphics[i].docItem ) )
            sel.graphics.insert( (int) i );
    }
}


/// File items of a selection (parse indices).
std::set<int> selItems( const Ctx& c, const Sel& sel )
{
    std::set<int> out;
    const SchScreen& sc = c.sc;

    for( int i : sel.symbols ) out.insert( sc.symbols[(size_t) i].docItem );
    for( int i : sel.sheets ) out.insert( sc.sheets[(size_t) i].docItem );
    for( int i : sel.labels ) out.insert( sc.labels[(size_t) i].docItem );
    for( int i : sel.junctions ) out.insert( sc.junctions[(size_t) i].docItem );
    for( int i : sel.noConnects ) out.insert( sc.noConnects[(size_t) i].docItem );
    for( int i : sel.entries ) out.insert( sc.busEntries[(size_t) i].docItem );
    for( int i : sel.texts ) out.insert( sc.texts[(size_t) i].docItem );
    for( int i : sel.graphics ) out.insert( sc.graphics[(size_t) i].docItem );
    for( int i : sel.images ) out.insert( sc.images[(size_t) i].docItem );
    for( int i : sel.lines ) out.insert( sc.lines[(size_t) i].docItem );

    out.erase( -1 );
    return out;
}


/// Connection points of the pins a symbol shows on this sheet.
std::vector<Pt> pinTips( const Ctx& c, int aSymbol )
{
    std::vector<Pt>  out;
    const SchSymbol& s = c.sc.symbols[(size_t) aSymbol];
    const LibSymbol* lib = c.sch.Lib( c.sc, s );

    if( !lib )
        return out;

    const int unit = c.sch.Unit( c.node, s );

    for( const LibPin& pin : lib->pins )
    {
        if( pin.unit != 0 && unit != 0 && pin.unit != unit )
            continue;

        if( lib->bodyStyleCount > 1 && pin.bodyStyle != 0 && pin.bodyStyle != s.bodyStyle )
            continue;

        out.push_back( roundIU( s.pos + s.transform.apply( pin.pos ) ) );
    }

    return out;
}


bool isBusLabel( const std::string& aText )
{
    std::vector<std::string> m = BusMembers( aText );
    return !( m.size() == 1 && m[0] == aText ) && !m.empty();
}

// ---- connection model ------------------------------------------------------------------------

struct GSeg
{
    Pt   a, b;
    bool bus = false;
    int  item = -1;      ///< document item, -1 = new
    bool fixed = false;  ///< from a wire item with more than two points: not reshaped
    bool moved = false;  ///< moved as a whole with the selection
    bool dead = false;
    bool changed = false;
    Pt   a0, b0;         ///< as in the file before the edit (for reusing items)
};

struct GJunction
{
    Pt   p;
    Pt   p0;
    int  item = -1; ///< document item, -1 = new
    bool keep = false; ///< selected by the user: never removed here
    bool dead = false;
};

struct GAnchor
{
    enum Kind { PIN, SHEET_PIN, LABEL, BUS_LABEL, ENTRY, NO_CONNECT };

    Pt   p;
    Kind kind;
    int  label = -1;     ///< SchScreen::labels index of a label
    bool moving = false; ///< moves with the selection
};

struct Geo
{
    std::vector<GSeg>      segs;
    std::vector<GJunction> juncs;
    std::vector<GAnchor>   anchors;
};

/// The sheet's connections after moving aMoved by aDelta and leaving out aGone.
Geo buildGeo( const Ctx& c, const Sel& aMoved, const Pt& aDelta, const Sel& aGone )
{
    Geo              g;
    const SchScreen& sc = c.sc;
    std::map<int, int> perItem;

    for( const SchLine& l : sc.lines )
        perItem[l.docItem]++;

    auto mv = [&]( const Pt& p, bool aMove ) { return aMove ? roundIU( p + aDelta ) : roundIU( p ); };

    for( size_t i = 0; i < sc.lines.size(); ++i )
    {
        const SchLine& l = sc.lines[i];

        if( l.kind == SchLine::NOTES || aGone.lines.count( (int) i ) )
            continue;

        GSeg s;
        s.moved = aMoved.lines.count( (int) i ) > 0;
        s.a = mv( l.a, s.moved );
        s.b = mv( l.b, s.moved );
        s.bus = l.kind == SchLine::BUS;
        s.item = c.docItem( l.docItem );
        s.fixed = perItem[l.docItem] > 1;
        s.a0 = roundIU( l.a );
        s.b0 = roundIU( l.b );
        g.segs.push_back( s );
    }

    for( size_t i = 0; i < sc.junctions.size(); ++i )
    {
        if( aGone.junctions.count( (int) i ) )
            continue;

        GJunction j;
        j.keep = aMoved.junctions.count( (int) i ) > 0;
        j.p = mv( sc.junctions[i].pos, j.keep );
        j.p0 = roundIU( sc.junctions[i].pos );
        j.item = c.docItem( sc.junctions[i].docItem );
        g.juncs.push_back( j );
    }

    for( size_t s = 0; s < sc.symbols.size(); ++s )
    {
        if( aGone.symbols.count( (int) s ) )
            continue;

        bool moved = aMoved.symbols.count( (int) s ) > 0;

        for( const Pt& p : pinTips( c, (int) s ) )
            g.anchors.push_back( { mv( p, moved ), GAnchor::PIN, -1, moved } );
    }

    int flat = 0;

    for( size_t s = 0; s < sc.sheets.size(); ++s )
    {
        bool moved = aMoved.sheets.count( (int) s ) > 0;

        for( const SchSheetPin& pin : sc.sheets[s].pins )
        {
            if( !aGone.sheets.count( (int) s ) && !aGone.sheetPins.count( flat ) )
                g.anchors.push_back( { mv( pin.pos, moved ), GAnchor::SHEET_PIN, -1, moved } );

            ++flat;
        }
    }

    for( size_t i = 0; i < sc.labels.size(); ++i )
    {
        if( aGone.labels.count( (int) i ) )
            continue;

        const SchLabel& L = sc.labels[i];
        bool            moved = aMoved.labels.count( (int) i ) > 0;
        g.anchors.push_back( { mv( L.pos, moved ), isBusLabel( L.text ) ? GAnchor::BUS_LABEL : GAnchor::LABEL, (int) i,
                               moved } );
    }

    for( size_t i = 0; i < sc.busEntries.size(); ++i )
    {
        if( aGone.entries.count( (int) i ) )
            continue;

        const SchBusEntry& e = sc.busEntries[i];
        bool               moved = aMoved.entries.count( (int) i ) > 0;
        g.anchors.push_back( { mv( e.pos, moved ), GAnchor::ENTRY, -1, moved } );
        g.anchors.push_back( { mv( e.pos + e.size, moved ), GAnchor::ENTRY, -1, moved } );
    }

    for( size_t i = 0; i < sc.noConnects.size(); ++i )
    {
        if( !aGone.noConnects.count( (int) i ) )
        {
            bool moved = aMoved.noConnects.count( (int) i ) > 0;
            g.anchors.push_back( { mv( sc.noConnects[i].pos, moved ), GAnchor::NO_CONNECT, -1, moved } );
        }
    }

    return g;
}


bool alive( const GSeg& s )
{
    return !s.dead && !same( s.a, s.b );
}


/// KiCad's rule (JUNCTION_HELPERS::AnalyzePoint): a junction dot belongs where items leave a
/// point in three or more directions on the wire layer (or on the bus layer). Pins, sheet pins
/// and bus entries count as directions of their own; a wire passing through counts twice when
/// something ends on it there.
bool junctionNeeded( const Geo& g, const Pt& p )
{
    std::set<int>            exits[2];
    bool                     brk[2] = { false, false };
    std::vector<const GSeg*> mids[2];
    int                      unique = 100000, entries = 0, busEnds = 0;

    for( const GSeg& s : g.segs )
    {
        if( !alive( s ) )
            continue;

        int L = s.bus ? 1 : 0;

        if( same( p, s.a ) || same( p, s.b ) )
        {
            exits[L].insert( angleKey( p, same( p, s.a ) ? s.b : s.a ) );
            brk[L] = true;
            busEnds += L;
        }
        else if( inside( p, s.a, s.b ) )
        {
            mids[L].push_back( &s );
        }
    }

    for( const GAnchor& a : g.anchors )
    {
        if( !same( a.p, p ) )
            continue;

        switch( a.kind )
        {
        case GAnchor::PIN:
        case GAnchor::SHEET_PIN:
            exits[0].insert( unique++ );
            brk[0] = true;
            break;

        case GAnchor::LABEL: brk[0] = true; break;
        case GAnchor::BUS_LABEL: brk[1] = true; break;

        case GAnchor::ENTRY:
            exits[0].insert( unique++ );
            exits[1].insert( unique++ );
            brk[0] = brk[1] = true;
            ++entries;
            break;

        case GAnchor::NO_CONNECT: break;
        }
    }

    for( int L = 0; L < 2; ++L )
    {
        if( !brk[L] )
            continue;

        for( const GSeg* s : mids[L] )
        {
            exits[L].insert( angleKey( p, s->a ) );
            exits[L].insert( angleKey( p, s->b ) );
        }
    }

    if( exits[0].size() < 3 && exits[1].size() < 3 )
        return false;

    if( entries == 0 )
        return true;

    bool toWires = exits[0].size() > 2 && exits[1].size() == 1;
    bool toBuses = (int) exits[1].size() - entries >= 3 && busEnds >= 1;
    return toWires || toBuses;
}


bool hasJunction( const Geo& g, const Pt& p )
{
    for( const GJunction& j : g.juncs )
    {
        if( !j.dead && same( j.p, p ) )
            return true;
    }

    return false;
}


void addSeg( Geo& g, const Pt& a, const Pt& b, bool aBus )
{
    if( same( a, b ) )
        return;

    for( const GSeg& s : g.segs )
    {
        if( alive( s ) && s.bus == aBus && ( ( same( s.a, a ) && same( s.b, b ) ) || ( same( s.a, b ) && same( s.b, a ) ) ) )
            return;
    }

    GSeg s;
    s.a = a;
    s.b = b;
    s.bus = aBus;
    g.segs.push_back( s );
}


/// Split segment i at p (a point inside it).
void splitSeg( Geo& g, size_t i, const Pt& p )
{
    if( g.segs[i].fixed || g.segs[i].moved )
        return;

    GSeg rest = g.segs[i];
    rest.item = -1;
    rest.changed = false;
    rest.a = p;
    g.segs[i].b = p;
    g.segs[i].changed = true;
    g.segs.push_back( rest );
}


/// Two wires that meet end to end in a straight line, with nothing else at the point: their
/// indices.
bool mergeable( const Geo& g, const Pt& x, size_t& aFirst, size_t& aSecond )
{
    for( const GAnchor& a : g.anchors )
    {
        if( same( a.p, x ) )
            return false;
    }

    if( hasJunction( g, x ) )
        return false;

    int found[2] = { -1, -1 };
    int n = 0;

    for( size_t i = 0; i < g.segs.size(); ++i )
    {
        const GSeg& s = g.segs[i];

        if( !alive( s ) )
            continue;

        if( inside( x, s.a, s.b ) )
            return false;

        if( same( x, s.a ) || same( x, s.b ) )
        {
            if( n < 2 )
                found[n] = (int) i;

            ++n;
        }
    }

    if( n != 2 )
        return false;

    const GSeg& s1 = g.segs[(size_t) found[0]];
    const GSeg& s2 = g.segs[(size_t) found[1]];

    if( s1.bus != s2.bus )
        return false;

    Pt      f1 = same( x, s1.a ) ? s1.b : s1.a;
    Pt      f2 = same( x, s2.a ) ? s2.b : s2.a;
    int64_t ax = iu( f1.x ) - iu( x.x ), ay = iu( f1.y ) - iu( x.y );
    int64_t bx = iu( f2.x ) - iu( x.x ), by = iu( f2.y ) - iu( x.y );

    if( ax * by - ay * bx != 0 || ax * bx + ay * by >= 0 )
        return false;

    aFirst = (size_t) found[0];
    aSecond = (size_t) found[1];
    return true;
}


/// Join two straight wires at x into one (KiCad's clean-up does the same) when the edit made
/// them meet there; a split the file already had stays.
void mergeAt( Geo& g, const Geo& aBefore, const Pt& x )
{
    size_t i1 = 0, i2 = 0;

    if( mergeable( aBefore, x, i1, i2 ) || !mergeable( g, x, i1, i2 ) )
        return;

    GSeg& s1 = g.segs[i1];
    GSeg& s2 = g.segs[i2];

    if( s1.fixed || s2.fixed || s1.moved || s2.moved )
        return;

    // keep the older file item (an edit followed by its opposite gives the file back)
    auto  older = []( const GSeg& a, const GSeg& b ) { return a.item >= 0 && ( b.item < 0 || a.item < b.item ); };
    GSeg* keep = older( s2, s1 ) ? &s2 : &s1;
    GSeg* drop = keep == &s1 ? &s2 : &s1;
    Pt    far = same( x, drop->a ) ? drop->b : drop->a;

    if( same( keep->a, x ) )
        keep->a = far;
    else
        keep->b = far;

    keep->changed = true;
    drop->dead = true;
}


/// A new or changed wire lying on top of another one of its kind adds nothing. (A wire that
/// ends on one of aPins only gives way to one that ends there too.)
void dropOverlaps( Geo& g, const std::vector<Pt>& aPins = {} )
{
    auto endsAt = [&]( const GSeg& s, const Pt& p ) { return same( s.a, p ) || same( s.b, p ); };

    for( size_t i = 0; i < g.segs.size(); ++i )
    {
        for( size_t j = 0; j < g.segs.size() && alive( g.segs[i] ); ++j )
        {
            GSeg&       s = g.segs[i];
            const GSeg& o = g.segs[j];

            if( i == j || !alive( o ) || o.bus != s.bus || s.moved || s.fixed || !( s.item < 0 || s.changed ) )
                continue;

            bool aOn = same( s.a, o.a ) || same( s.a, o.b ) || inside( s.a, o.a, o.b );
            bool bOn = same( s.b, o.a ) || same( s.b, o.b ) || inside( s.b, o.a, o.b );
            bool twin = ( same( s.a, o.a ) && same( s.b, o.b ) ) || ( same( s.a, o.b ) && same( s.b, o.a ) );

            bool held = std::any_of( aPins.begin(), aPins.end(),
                                     [&]( const Pt& p ) { return endsAt( s, p ) && !endsAt( o, p ); } );

            // of two equal wires the newer one goes
            if( aOn && bOn && !held && ( !twin || ( o.item >= 0 && ( s.item < 0 || o.item < s.item ) ) ) )
                s.dead = true;
        }
    }
}


void setWirePoints( Ctx& c, int aItem, const Pt& a, const Pt& b )
{
    ItemTree tree( c.body( aItem ) );
    N        pts = tree.root.find( "pts" );
    std::vector<N> xys;

    for( N x = pts ? pts.first() : N(); x; x = x.next() )
    {
        if( x.isList() && x.head() == "xy" )
            xys.push_back( x );
    }

    if( xys.size() < 2 )
        return;

    std::vector<Patch> p;
    setPoint( p, xys[0], a );
    setPoint( p, xys[1], b );
    c.replace( aItem, tree.body, p );
}


void setJunctionPos( Ctx& c, int aItem, const Pt& aPos )
{
    ItemTree tree( c.body( aItem ) );
    std::vector<Patch> p;

    if( N at = tree.root.find( "at" ) )
        setPoint( p, at, aPos );

    c.replace( aItem, tree.body, p );
}


/// Pairs (new, spare) by increasing distance, each used once.
std::vector<std::pair<size_t, size_t>> nearestPairs( size_t aNew, size_t aSpare,
                                                     const std::function<double( size_t, size_t )>& aDistance )
{
    std::vector<std::tuple<double, size_t, size_t>> all;

    for( size_t i = 0; i < aNew; ++i )
    {
        for( size_t k = 0; k < aSpare; ++k )
            all.emplace_back( aDistance( i, k ), i, k );
    }

    std::stable_sort( all.begin(), all.end(),
                      []( const auto& a, const auto& b ) { return std::get<0>( a ) < std::get<0>( b ); } );

    std::vector<bool>                      usedNew( aNew, false ), usedSpare( aSpare, false );
    std::vector<std::pair<size_t, size_t>> out;

    for( const auto& [d, i, k] : all )
    {
        if( usedNew[i] || usedSpare[k] )
            continue;

        usedNew[i] = usedSpare[k] = true;
        out.emplace_back( i, k );
    }

    return out;
}


double dist( const Pt& a, const Pt& b )
{
    return std::hypot( a.x - b.x, a.y - b.y );
}


/// Write the planned wires and junctions: changed items are patched, an item that went away
/// is reused for the nearest new one of its kind (so that an edit and its opposite give the
/// file back), the rest is removed or inserted.
void writeGeo( Ctx& c, Geo& g )
{
    for( GSeg& s : g.segs )
    {
        if( s.item >= 0 && !s.fixed && !s.moved && !s.dead && same( s.a, s.b ) )
            s.dead = true;
    }

    for( const GSeg& s : g.segs )
    {
        if( s.item >= 0 && !s.fixed && !s.moved && !s.dead && s.changed )
            setWirePoints( c, s.item, s.a, s.b );
    }

    for( int layer = 0; layer < 2; ++layer )
    {
        std::vector<const GSeg*> fresh, spare;

        for( const GSeg& s : g.segs )
        {
            if( ( s.bus ? 1 : 0 ) != layer )
                continue;

            if( s.item < 0 && alive( s ) )
                fresh.push_back( &s );
            else if( s.item >= 0 && !s.fixed && !s.moved && s.dead )
                spare.push_back( &s );
        }

        auto pairs = nearestPairs( fresh.size(), spare.size(),
                                   [&]( size_t i, size_t k )
                                   {
                                       const GSeg& n = *fresh[i];
                                       const GSeg& o = *spare[k];
                                       return std::min( dist( n.a, o.a0 ) + dist( n.b, o.b0 ),
                                                        dist( n.a, o.b0 ) + dist( n.b, o.a0 ) );
                                   } );
        std::vector<bool> reused( spare.size(), false ), placed( fresh.size(), false );

        for( const auto& [i, k] : pairs )
        {
            const GSeg& n = *fresh[i];
            const GSeg& o = *spare[k];

            // keep the old point order when the wire comes back the other way round
            if( dist( n.a, o.b0 ) + dist( n.b, o.a0 ) < dist( n.a, o.a0 ) + dist( n.b, o.b0 ) )
                setWirePoints( c, o.item, n.b, n.a );
            else
                setWirePoints( c, o.item, n.a, n.b );

            reused[k] = placed[i] = true;
        }

        for( size_t i = 0; i < fresh.size(); ++i )
        {
            if( !placed[i] )
                insertItem( c, wireText( c.style, fresh[i]->bus, fresh[i]->a, fresh[i]->b ) );
        }

        for( size_t k = 0; k < spare.size(); ++k )
        {
            if( !reused[k] )
                c.t.doc.Remove( c.file, spare[k]->item );
        }
    }

    std::vector<const GJunction*> fresh, spare;

    for( const GJunction& j : g.juncs )
    {
        if( j.item < 0 && !j.dead )
            fresh.push_back( &j );
        else if( j.item >= 0 && j.dead )
            spare.push_back( &j );
    }

    auto pairs = nearestPairs( fresh.size(), spare.size(),
                               [&]( size_t i, size_t k ) { return dist( fresh[i]->p, spare[k]->p0 ); } );
    std::vector<bool> reused( spare.size(), false ), placed( fresh.size(), false );

    for( const auto& [i, k] : pairs )
    {
        setJunctionPos( c, spare[k]->item, fresh[i]->p );
        reused[k] = placed[i] = true;
    }

    for( size_t i = 0; i < fresh.size(); ++i )
    {
        if( !placed[i] )
            insertItem( c, junctionText( c.style, fresh[i]->p ) );
    }

    for( size_t k = 0; k < spare.size(); ++k )
    {
        if( !reused[k] )
            c.t.doc.Remove( c.file, spare[k]->item );
    }
}


/// Junctions at the points an edit touched: drop the ones the edit made needless, join
/// straight wires, add the ones it made necessary. A point the edit did not change keeps what
/// the file has (a needless junction stays, a missing one is not added).
void fixJunctions( Geo& g, const Geo& aBefore, std::vector<Pt> aPoints, bool aAdd )
{
    dropOverlaps( g );

    std::sort( aPoints.begin(), aPoints.end(),
               []( const Pt& a, const Pt& b ) { return iu( a.x ) != iu( b.x ) ? iu( a.x ) < iu( b.x ) : iu( a.y ) < iu( b.y ); } );
    aPoints.erase( std::unique( aPoints.begin(), aPoints.end(), []( const Pt& a, const Pt& b ) { return same( a, b ); } ),
                   aPoints.end() );

    for( GJunction& j : g.juncs )
    {
        if( j.keep || j.dead )
            continue;

        for( const Pt& p : aPoints )
        {
            if( same( p, j.p ) && !junctionNeeded( g, p ) && junctionNeeded( aBefore, p ) )
                j.dead = true;
        }
    }

    for( const Pt& p : aPoints )
        mergeAt( g, aBefore, p );

    if( !aAdd )
        return;

    for( const Pt& p : aPoints )
    {
        if( junctionNeeded( g, p ) && !hasJunction( g, p ) && !junctionNeeded( aBefore, p ) )
        {
            GJunction j;
            j.p = p;
            g.juncs.push_back( j );
        }
    }
}

// ---- symbol orientation (KiCad SCH_SYMBOL::SetOrientation / GetOrientation) ------------------

/// New transform = old one modified by aTemp, as KiCad composes them.
SymTransform compose( const SymTransform& m, const SymTransform& t )
{
    SymTransform r;
    r.x1 = m.x1 * t.x1 + m.x2 * t.y1;
    r.y1 = m.y1 * t.x1 + m.y2 * t.y1;
    r.x2 = m.x1 * t.x2 + m.x2 * t.y2;
    r.y2 = m.y1 * t.x2 + m.y2 * t.y2;
    return r;
}


bool sameTransform( const SymTransform& a, const SymTransform& b )
{
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}


const SymTransform T_ID { 1, 0, 0, 1 };
const SymTransform T_CCW { 0, 1, -1, 0 };
const SymTransform T_CW { 0, -1, 1, 0 };
const SymTransform T_MX { 1, 0, 0, -1 };
const SymTransform T_MY { -1, 0, 0, 1 };

/// The angle and mirror KiCad writes for a transform (first match in GetOrientation's order).
void orientationOf( const SymTransform& aT, int& aAngle, char& aMirror )
{
    const SymTransform o0 = T_ID, o90 = compose( T_ID, T_CCW ), o180 = compose( o90, T_CCW ), o270 = compose( T_ID, T_CW );
    const struct
    {
        SymTransform t;
        int          angle;
        char         mirror;
    } order[] = { { o0, 0, 0 },
                  { o90, 90, 0 },
                  { o180, 180, 0 },
                  { o270, 270, 0 },
                  { compose( o0, T_MX ), 0, 'x' },
                  { compose( o90, T_MX ), 90, 'x' },
                  { compose( o270, T_MX ), 270, 'x' },
                  { compose( o0, T_MY ), 0, 'y' },
                  { compose( o90, T_MY ), 90, 'y' },
                  { compose( o180, T_MY ), 180, 'y' },
                  { compose( o270, T_MY ), 270, 'y' } };

    for( const auto& o : order )
    {
        if( sameTransform( o.t, aT ) )
        {
            aAngle = o.angle;
            aMirror = o.mirror;
            return;
        }
    }

    aAngle = 0;
    aMirror = 0;
}


/// Rotate or mirror a placed symbol about its position. Field positions are stored on the
/// sheet, so they turn with it (KiCad keeps them fixed relative to the unrotated symbol).
bool transformSymbol( Ctx& c, int aSymbol, const SymTransform& aTemp, const std::string& aLabel, std::string& err )
{
    const SchSymbol& s = c.sc.symbols[(size_t) aSymbol];
    int              di = c.docItem( s.docItem );

    if( di < 0 )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    ItemTree tree( c.body( di ) );
    N        at = tree.root.find( "at" );

    if( !at )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    const SymTransform& T = s.transform;
    const SymTransform  T2 = compose( T, aTemp );
    const SymTransform  inv { T.x1, T.x2, T.y1, T.y2 }; // orthogonal: inverse = transpose
    int                 angle = 0;
    char                mirror = 0;
    orientationOf( T2, angle, mirror );

    std::vector<Patch> p;
    setAngle( p, at, angle );
    N mir = tree.root.find( "mirror" );

    if( !mirror && mir )
    {
        p.push_back( { wsStart( tree.body, mir.begin() ), mir.end(), std::string() } );
    }
    else if( mirror && mir )
    {
        N tok = mir.arg( 0 );

        if( !tok || tok.text() != std::string( 1, mirror ) || mir.size() != 2 )
            p.push_back( { mir.begin(), mir.end(), std::string( "(mirror " ) + mirror + ")" } );
    }
    else if( mirror )
    {
        size_t      ws = wsStart( tree.body, at.begin() );
        std::string sep = tree.body.substr( ws, at.begin() - ws );
        p.push_back( { at.end(), at.end(), ( sep.empty() ? std::string( " " ) : sep ) + "(mirror " + mirror + ")" } );
    }

    tree.root.each( "property",
                    [&]( N prop )
                    {
                        N fat = prop.find( "at" );

                        if( !fat )
                            return;

                        Pt rel = inv.apply( Pt( fat.num( 0 ), fat.num( 1 ) ) - s.pos );
                        setPoint( p, fat, s.pos + T2.apply( rel ) );
                    } );

    c.t.doc.Begin( aLabel );
    c.replace( di, tree.body, p );
    c.t.doc.Commit();
    return true;
}

// ---- labels and texts ------------------------------------------------------------------------

int spinAngle( Spin s )
{
    return s == Spin::UP ? 90 : s == Spin::LEFT ? 180 : s == Spin::BOTTOM ? 270 : 0;
}


/// Turn or flip a label to a new spin; its fields go along (KiCad rotates or mirrors them
/// about the label's anchor).
bool respinLabel( Ctx& c, int aLabel, Spin aSpin, const std::function<void( const ItemTree&, N, std::vector<Patch>& )>& aFields,
                  const std::string& aLabelText, std::string& err )
{
    const SchLabel& L = c.sc.labels[(size_t) aLabel];
    int             di = c.docItem( L.docItem );

    if( di < 0 )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    ItemTree tree( c.body( di ) );
    N        at = tree.root.find( "at" );

    if( !at )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    std::vector<Patch> p;
    setAngle( p, at, spinAngle( aSpin ) );
    setHJustify( tree, tree.root, aSpin == Spin::LEFT || aSpin == Spin::BOTTOM ? "right" : "left", p );

    tree.root.each( "property", [&]( N prop ) { aFields( tree, prop, p ); } );

    c.t.doc.Begin( aLabelText );
    c.replace( di, tree.body, p );
    c.t.doc.Commit();
    return true;
}


bool isVertical( double aAngle )
{
    return ( (int) std::lround( aAngle ) % 180 + 180 ) % 180 == 90;
}


/// A schematic text: KiCad 7+ texts are horizontal or vertical (SCH_TEXT::Rotate90 and
/// MirrorSpinStyle).
bool changeText( Ctx& c, int aText, bool aRotate, bool aCcwOrVertical, std::string& err )
{
    const SchText& T = c.sc.texts[(size_t) aText];
    int            di = c.docItem( T.docItem );

    if( di < 0 )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    ItemTree tree( c.body( di ) );
    N        at = tree.root.find( "at" );
    bool     vertical = isVertical( T.angle );
    std::vector<Patch> p;

    if( aRotate )
    {
        bool ccw = aCcwOrVertical;

        if( at )
            setAngle( p, at, vertical ? 0 : 90 );

        if( ( !vertical && !ccw ) || ( vertical && ccw ) )
            flipHJustify( tree, tree.root, p );
    }
    else if( vertical == aCcwOrVertical )
    {
        flipHJustify( tree, tree.root, p );
    }

    c.t.doc.Begin( aRotate ? Tr( "Rotate" ) : Tr( "Mirror" ) );
    c.replace( di, tree.body, p );
    c.t.doc.Commit();
    return true;
}

// ---- symbol fields and references ------------------------------------------------------------

/// The (path ...) node of a symbol's "instances" that Schematic::Reference reads for a sheet.
N instancePath( N aSymbol, const std::string& aP6, const std::string& aP7, bool& aOnly )
{
    N   found, first;
    int count = 0;

    if( N inst = aSymbol.find( "instances" ) )
    {
        inst.each( "project",
                   [&]( N proj )
                   {
                       proj.each( "path",
                                  [&]( N path )
                                  {
                                      ++count;

                                      if( !first )
                                          first = path;

                                      std::string_view v = path.str( 0 );

                                      if( !found && ( v == aP7 || v == aP6 || ( aP6.empty() && v == "/" ) ) )
                                          found = path;
                                  } );
                   } );
    }

    aOnly = count == 1;
    return found ? found : ( count == 1 ? first : N() );
}


/// Live item of a file with the given head ("symbol_instances"), -1 if none.
int findItem( const SchDocument& aDoc, int aFile, const char* aHead )
{
    const Document& d = aDoc.Doc( (size_t) aFile );

    for( int i : d.LiveItems() )
    {
        if( headOf( d.Item( (size_t) i ).body ) == aHead )
            return i;
    }

    return -1;
}


/// "R12" -> "R", 12, 2 digits; "U?" -> "U", -1.
void splitRef( const std::string& aRef, std::string& aPrefix, long& aNumber, size_t& aDigits )
{
    size_t e = aRef.size();

    while( e > 0 && ( std::isdigit( (unsigned char) aRef[e - 1] ) || aRef[e - 1] == '?' ) )
        --e;

    aPrefix = aRef.substr( 0, e );
    std::string digits = aRef.substr( e );
    aDigits = digits.size();
    aNumber = -1;

    if( !digits.empty() && digits.find( '?' ) == std::string::npos && digits.size() < 10 )
        aNumber = std::atol( digits.c_str() );
}

} // anonymous namespace


// ================================================================================================
// Edits
// ================================================================================================

namespace
{

/// Would the new segments of a wire (aOwn[1..]; aOwn[0] is the wire itself, which keeps the
/// contacts it had) touch an item that is not theirs and so connect something new? Points in
/// aAllowed (the wire's own connections) do not count.
bool touchesOthers( const Geo& g, const std::vector<size_t>& aOwn, const std::vector<Pt>& aAllowed,
                    const Pt& aOldPin )
{
    auto own = [&]( size_t i ) { return std::find( aOwn.begin(), aOwn.end(), i ) != aOwn.end(); };
    auto allowed = [&]( const Pt& p )
    {
        return std::any_of( aAllowed.begin(), aAllowed.end(), [&]( const Pt& a ) { return same( a, p ); } );
    };

    std::vector<Pt> foreign;

    for( size_t i = 0; i < g.segs.size(); ++i )
    {
        if( !own( i ) && alive( g.segs[i] ) )
        {
            foreign.push_back( g.segs[i].a );
            foreign.push_back( g.segs[i].b );
        }
    }

    for( const GAnchor& a : g.anchors )
        foreign.push_back( a.p );

    // a junction on the old pin position is checked again after the move
    for( const GJunction& j : g.juncs )
    {
        if( !j.dead && !same( j.p, aOldPin ) )
            foreign.push_back( j.p );
    }

    for( size_t k = 1; k < aOwn.size(); ++k )
    {
        const GSeg& s = g.segs[aOwn[k]];

        if( !alive( s ) )
            continue;

        for( const Pt& q : foreign )
        {
            if( !allowed( q ) && inside( q, s.a, s.b ) )
                return true;
        }

        for( const Pt& e : { s.a, s.b } )
        {
            if( allowed( e ) )
                continue;

            for( const Pt& q : foreign )
            {
                if( same( q, e ) )
                    return true;
            }

            for( size_t j = 0; j < g.segs.size(); ++j )
            {
                if( !own( j ) && alive( g.segs[j] ) && inside( e, g.segs[j].a, g.segs[j].b ) )
                    return true;
            }
        }
    }

    return false;
}


/// Wire ends on moving pins follow a move along one axis (after KiCad's
/// SCH_MOVE_TOOL::orthoLineDrag). A wire along the move stretches. A wire across it moves
/// along when its other end is free, or drags the wire along the move that continues it;
/// otherwise it gets a bend next to the pin, one grid step further out for each next wire of a
/// row of pins so that the bends do not overlap. A bend that would touch anything else gives
/// way to a slanted wire.
void dragAxis( Geo& g, std::vector<Pt>& aPins, const Pt& aDelta, std::vector<Pt>& aTouched,
               std::map<int, std::vector<std::pair<Pt, Pt>>>& aFixed, std::map<int, Pt>& aLabels,
               const std::set<int>& aSelectedLabels )
{
    if( iu( aDelta.x ) == 0 && iu( aDelta.y ) == 0 )
        return;

    std::set<int> carried;

    // labels on a wire that moves as a whole (or on the part of it that moves) go along
    auto carry = [&]( const Pt a, const Pt b )
    {
        for( GAnchor& an : g.anchors )
        {
            if( an.label < 0 || aSelectedLabels.count( an.label ) || carried.count( an.label ) )
                continue;

            if( same( an.p, a ) || same( an.p, b ) || inside( an.p, a, b ) )
            {
                aTouched.push_back( an.p );
                an.p = roundIU( an.p + aDelta );
                aTouched.push_back( an.p );
                Pt& total = aLabels[an.label];
                total = total + aDelta;
                carried.insert( an.label );
            }
        }
    };

    const bool alongX = iu( aDelta.x ) != 0;
    auto       atPin = [&]( const Pt& p )
    {
        return std::any_of( aPins.begin(), aPins.end(), [&]( const Pt& a ) { return same( a, p ); } );
    };
    auto mv = [&]( const Pt& p ) { return roundIU( p + aDelta ); };

    struct Bend
    {
        size_t seg;
        bool   atA;
        double key;
    };

    std::vector<Bend> bends;
    const size_t      n = g.segs.size();

    // decisions look at the sheet as it is at the start of the pass
    const std::vector<GSeg>    before( g.segs.begin(), g.segs.end() );
    const std::vector<GAnchor> anchorsBefore = g.anchors;

    // a pin of another symbol (or a sheet pin) right on a moving pin keeps its connection
    // through a new wire (KiCad adds one too)
    for( const Pt& p : aPins )
    {
        for( const GAnchor& a : g.anchors )
        {
            if( ( a.kind == GAnchor::PIN || a.kind == GAnchor::SHEET_PIN ) && !a.moving && same( a.p, p ) )
            {
                GSeg w;
                w.a = p;
                w.b = mv( p );
                g.segs.push_back( w );
                aTouched.push_back( w.a );
                aTouched.push_back( w.b );
                break;
            }
        }
    }

    for( size_t i = 0; i < n; ++i )
    {
        GSeg& s = g.segs[i];

        if( s.dead || s.moved )
            continue;

        const bool atA = atPin( s.a ), atB = atPin( s.b );

        if( !atA && !atB )
            continue;

        aTouched.push_back( s.a );
        aTouched.push_back( s.b );

        if( s.fixed )
        {
            for( Pt* e : { &s.a, &s.b } )
            {
                if( !atPin( *e ) )
                    continue;

                auto& moves = aFixed[s.item];
                auto  prev = std::find_if( moves.begin(), moves.end(),
                                           [&]( const std::pair<Pt, Pt>& m ) { return same( m.second, *e ); } );

                if( prev != moves.end() )
                    prev->second = mv( *e );
                else
                    moves.push_back( { *e, mv( *e ) } );

                *e = mv( *e );
            }

            continue;
        }

        s.changed = true;

        if( atA && atB )
        {
            carry( s.a, s.b );
            s.a = mv( s.a );
            s.b = mv( s.b );
            continue;
        }

        Pt&        P = atA ? s.a : s.b;
        Pt&        F = atA ? s.b : s.a;
        const bool horiz = iu( s.a.y ) == iu( s.b.y ), vert = iu( s.a.x ) == iu( s.b.x );

        if( ( horiz && vert ) || ( alongX ? horiz : vert ) || ( !horiz && !vert ) )
        {
            P = mv( P );
            aTouched.push_back( P );
            continue;
        }

        // across the move: what else is at the far end? (A wire with something on its middle,
        // other than labels that go along, stays where it is.)
        int  others = 0, partner = -1;
        bool busy = hasJunction( g, F );

        for( size_t j = 0; j < n; ++j )
        {
            if( j != i && !before[j].dead && ( same( before[j].a, F ) || same( before[j].b, F ) ) )
            {
                ++others;
                partner = (int) j;
            }

            if( j != i && alive( before[j] ) && ( inside( before[j].a, P, F ) || inside( before[j].b, P, F ) ) )
                busy = true;
        }

        for( const GAnchor& a : anchorsBefore )
            busy |= !a.moving && a.label < 0 && inside( a.p, P, F );

        for( const GAnchor& a : anchorsBefore )
            busy |= same( a.p, F );

        // a wire end that moves must not land on (or run over) anything new
        auto clear = [&]( const Pt& a, const Pt& b, const Pt& aOldA, const Pt& aOldB, size_t aSkip )
        {
            auto covered = [&]( const Pt& q )
            {
                return same( q, aOldA ) || same( q, aOldB ) || inside( q, aOldA, aOldB );
            };
            auto bad = [&]( const Pt& q )
            {
                return !covered( q ) && ( same( q, a ) || same( q, b ) || inside( q, a, b ) );
            };

            for( size_t j = 0; j < n; ++j )
            {
                const GSeg& o = before[j];

                if( j == i || j == aSkip || !alive( o ) )
                    continue;

                if( bad( o.a ) || bad( o.b ) || ( !covered( b ) && inside( b, o.a, o.b ) ) )
                    return false;
            }

            for( const GAnchor& an : anchorsBefore )
            {
                if( !an.moving && an.label < 0 && bad( an.p ) )
                    return false;
            }

            for( const Pt& pin : aPins )
            {
                if( !same( pin, P ) && bad( mv( pin ) ) )
                    return false;
            }

            for( const GJunction& jn : g.juncs )
            {
                if( !jn.dead && bad( jn.p ) )
                    return false;
            }

            return true;
        };

        if( others == 0 && !busy && clear( mv( P ), mv( F ), P, F, SIZE_MAX ) )
        {
            carry( P, F );
            P = mv( P );
            F = mv( F );
            aTouched.push_back( P );
            aTouched.push_back( F );
            continue;
        }

        if( others == 1 && !busy )
        {
            const GSeg& w0 = before[(size_t) partner];
            GSeg&       w = g.segs[(size_t) partner];
            const bool  wh = iu( w0.a.y ) == iu( w0.b.y ), wv = iu( w0.a.x ) == iu( w0.b.x );

            const Pt    wo = same( w0.a, F ) ? w0.b : w0.a; // the partner's other end

            if( !w0.fixed && !w0.moved && ( ( wh && wv ) || ( alongX ? wh : wv ) )
                && clear( mv( P ), mv( F ), P, F, (size_t) partner ) && clear( wo, mv( F ), wo, F, (size_t) partner ) )
            {
                Pt& wf = same( w0.a, F ) ? w.a : w.b;
                carry( P, F );
                wf = mv( wf );
                w.changed = true;
                P = mv( P );
                F = mv( F );
                aTouched.push_back( P );
                aTouched.push_back( F );
                continue;
            }
        }

        double sign = ( alongX ? aDelta.x : aDelta.y ) > 0 ? 1.0 : -1.0;
        bends.push_back( { i, atA, sign * ( alongX ? P.x : P.y ) } );
    }

    // the wire first in the direction of the move bends at the pin, the next one a step out
    std::stable_sort( bends.begin(), bends.end(), []( const Bend& a, const Bend& b ) { return a.key < b.key; } );

    struct Plan
    {
        size_t              seg;
        bool                atA;
        Pt                  P, F;
        int                 r = 0;
        std::vector<size_t> own;
    };

    // bend r grid steps from the pin; false (and a slanted wire) when the wire is too short
    auto place = [&]( Plan& plan, int r ) -> bool
    {
        for( size_t k = 1; k < plan.own.size(); ++k )
            g.segs[plan.own[k]].dead = true;

        plan.own.resize( 1 );
        plan.r = r;
        Pt&          end = plan.atA ? g.segs[plan.seg].a : g.segs[plan.seg].b;
        const Pt     dir = alongX ? Pt( 0, plan.F.y > plan.P.y ? 1 : -1 ) : Pt( plan.F.x > plan.P.x ? 1 : -1, 0 );
        const double len = alongX ? std::fabs( plan.F.y - plan.P.y ) : std::fabs( plan.F.x - plan.P.x );

        if( r < 0 || r * SCH_GRID >= len - 1e-6 )
        {
            end = mv( plan.P );
            return false;
        }

        const Pt Q = roundIU( plan.P + dir * ( r * SCH_GRID ) );
        end = Q;
        GSeg add;
        add.bus = g.segs[plan.seg].bus;
        add.a = Q;
        add.b = mv( Q );
        g.segs.push_back( add );
        plan.own.push_back( g.segs.size() - 1 );

        if( r > 0 )
        {
            add.a = mv( Q );
            add.b = mv( plan.P );
            g.segs.push_back( add );
            plan.own.push_back( g.segs.size() - 1 );
        }

        return true;
    };

    std::vector<Plan> plans;

    // bends already placed per column of pins and side: (pin coordinate along the move, step)
    std::map<std::pair<int64_t, int>, std::vector<std::pair<double, int>>> column;
    const double reach = std::fabs( alongX ? aDelta.x : aDelta.y ) + 1e-6;

    for( const Bend& b : bends )
    {
        const GSeg& s = g.segs[b.seg];
        Plan        plan;
        plan.seg = b.seg;
        plan.atA = b.atA;
        plan.P = b.atA ? s.a : s.b;
        plan.F = b.atA ? s.b : s.a;
        plan.own = { b.seg };
        int    side = alongX ? ( plan.F.y > plan.P.y ? 1 : -1 ) : ( plan.F.x > plan.P.x ? 1 : -1 );
        double at = alongX ? plan.P.x : plan.P.y;
        auto&  placed = column[{ iu( alongX ? plan.P.y : plan.P.x ), side }];
        int    r = 0;

        // one step further out than a bend this one could run into
        for( const auto& [pos, step] : placed )
        {
            if( std::fabs( pos - at ) <= reach )
                r = std::max( r, step + 1 );
        }

        place( plan, r );
        placed.emplace_back( at, plan.r );
        plans.push_back( plan );
    }

    // what moves with the selection is now one step further
    for( GAnchor& a : g.anchors )
    {
        if( a.moving )
            a.p = mv( a.p );
    }

    // Does the shortened wire leave something behind that was on the part next to the pin?
    auto losesContact = [&]( const Plan& plan )
    {
        const Pt Q = plan.atA ? g.segs[plan.seg].a : g.segs[plan.seg].b;
        const Pt from = plan.own.size() > 1 ? Q : plan.F; // a slanted wire leaves all of it

        if( same( from, plan.P ) )
            return false;

        for( const GAnchor& a : g.anchors )
        {
            if( !a.moving && a.label < 0 && inside( a.p, from, plan.P ) )
                return true;
        }

        for( size_t i = 0; i < g.segs.size(); ++i )
        {
            const GSeg& o = g.segs[i];

            if( i != plan.seg && alive( o ) && ( inside( o.a, from, plan.P ) || inside( o.b, from, plan.P ) ) )
                return true;
        }

        return false;
    };

    auto fits = [&]( const Plan& plan )
    {
        return plan.own.size() > 1 && !losesContact( plan )
               && !touchesOthers( g, plan.own, { mv( plan.P ), plan.F }, plan.P );
    };

    // a bend that touches something moves further out; else one at the pin, else a slanted
    // wire (if that keeps the contacts), else the bend at the pin all the same
    for( Plan& plan : plans )
    {
        const int first = plan.r;

        while( plan.own.size() > 1 && !fits( plan ) && !losesContact( plan ) )
            place( plan, plan.r + 1 );

        if( !fits( plan ) )
        {
            for( int r = 0; r < first && !fits( plan ); ++r )
                place( plan, r );
        }

        if( !fits( plan ) )
        {
            place( plan, -1 );

            if( losesContact( plan ) )
                place( plan, 0 );
        }

        Pt Q = plan.own.size() > 1 ? g.segs[plan.own[1]].a : plan.P;

        // labels between the bend and the pin are on the part that moves
        if( plan.own.size() > 2 )
        {
            for( GAnchor& an : g.anchors )
            {
                if( an.label >= 0 && !same( an.p, Q ) && ( same( an.p, plan.P ) || inside( an.p, Q, plan.P ) ) )
                    carry( an.p, an.p );
            }
        }

        for( const Pt& p : { plan.P, Q, mv( Q ), mv( plan.P ), plan.F } )
            aTouched.push_back( p );
    }

    for( Pt& p : aPins )
        p = mv( p );

    dropOverlaps( g, aPins );
}

} // anonymous namespace


bool SchMoveItems( SchEditTarget& t, const std::vector<SchItemRef>& aItems, double aDx, double aDy, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx c( t );
    Sel sel;

    if( !resolve( c, aItems, sel, true, err ) )
        return false;

    if( sel.empty() )
    {
        err = Tr( "Nothing selected" );
        return false;
    }

    const Pt d( roundIU( aDx ), roundIU( aDy ) );

    if( iu( d.x ) == 0 && iu( d.y ) == 0 )
    {
        err = Tr( "No change" );
        return false;
    }

    wholeItems( c, sel );
    const SchScreen& sc = c.sc;

    // pins of moved symbols and sheets drag what sits on them
    std::vector<Pt> anchors;

    for( int s : sel.symbols )
    {
        std::vector<Pt> tips = pinTips( c, s );
        anchors.insert( anchors.end(), tips.begin(), tips.end() );
    }

    for( int s : sel.sheets )
    {
        for( const SchSheetPin& pin : sc.sheets[(size_t) s].pins )
            anchors.push_back( roundIU( pin.pos ) );
    }

    auto atAnchor = [&]( const Pt& p )
    {
        return std::any_of( anchors.begin(), anchors.end(), [&]( const Pt& a ) { return same( a, p ); } );
    };

    std::vector<Pt> touched;

    for( size_t i = 0; i < sc.labels.size(); ++i )
    {
        if( !sel.labels.count( (int) i ) && atAnchor( sc.labels[i].pos ) )
            sel.labels.insert( (int) i );
    }

    for( size_t i = 0; i < sc.noConnects.size(); ++i )
    {
        if( !sel.noConnects.count( (int) i ) && atAnchor( sc.noConnects[i].pos ) )
            sel.noConnects.insert( (int) i );
    }

    for( const Pt& a : anchors )
    {
        touched.push_back( a );
        touched.push_back( roundIU( a + d ) );
    }

    for( int i : sel.lines )
    {
        for( const Pt& p : { sc.lines[(size_t) i].a, sc.lines[(size_t) i].b } )
        {
            touched.push_back( roundIU( p ) );
            touched.push_back( roundIU( p + d ) );
        }
    }

    for( int i : sel.labels )
    {
        touched.push_back( roundIU( sc.labels[(size_t) i].pos ) );
        touched.push_back( roundIU( sc.labels[(size_t) i].pos + d ) );
    }

    Geo g = buildGeo( c, sel, d, Sel() );

    // rubber band, one axis after the other (as KiCad splits a drag)
    std::map<int, std::vector<std::pair<Pt, Pt>>> fixedMoves; // wires with several points
    std::map<int, Pt>                             labelMoves; // labels carried by wires
    std::vector<Pt>                               pins = anchors;

    for( GAnchor& a : g.anchors )
    {
        if( a.moving )
            a.p = roundIU( a.p - d ); // the passes below move them step by step
    }

    dragAxis( g, pins, Pt( d.x, 0 ), touched, fixedMoves, labelMoves, sel.labels );
    dragAxis( g, pins, Pt( 0, d.y ), touched, fixedMoves, labelMoves, sel.labels );

    fixJunctions( g, buildGeo( c, Sel(), Pt(), Sel() ), touched, true );

    // KiCad connects wires only at their ends: a wire with a new junction on its middle is split
    for( size_t j = 0; j < g.juncs.size(); ++j )
    {
        const GJunction jn = g.juncs[j];

        if( jn.dead || ( jn.item >= 0 && same( jn.p, jn.p0 ) ) )
            continue;

        for( size_t i = 0; i < g.segs.size(); ++i )
        {
            if( alive( g.segs[i] ) && inside( jn.p, g.segs[i].a, g.segs[i].b ) )
                splitSeg( g, i, jn.p );
        }
    }

    std::string label = Tr( "Move" );

    if( sel.symbols.size() == 1 && sel.sheets.empty() && sel.lines.empty() )
        label = TrF( "Move {0}", { c.shownRef( *sel.symbols.begin() ) } );

    t.doc.Begin( label );

    for( int parse : selItems( c, sel ) )
    {
        int di = c.docItem( parse );

        if( di < 0 )
            continue;

        ItemTree           tree( c.body( di ) );
        std::vector<Patch> p;
        translateNodes( tree.root, d, p );
        c.replace( di, tree.body, p );
    }

    for( const auto& [label, delta] : labelMoves )
    {
        int di = c.docItem( sc.labels[(size_t) label].docItem );

        if( di < 0 || sel.labels.count( label ) )
            continue;

        ItemTree           tree( c.body( di ) );
        std::vector<Patch> p;
        translateNodes( tree.root, delta, p );
        c.replace( di, tree.body, p );
    }

    for( const auto& [item, moves] : fixedMoves )
    {
        if( item < 0 )
            continue;

        ItemTree           tree( c.body( item ) );
        std::vector<Patch> p;
        std::vector<N>     xys;
        N                  pts = tree.root.find( "pts" );

        for( N x = pts ? pts.first() : N(); x; x = x.next() )
        {
            if( !x.isList() || x.head() != "xy" )
                continue;

            for( const auto& [from, to] : moves )
            {
                if( same( Pt( x.num( 0 ), x.num( 1 ) ), from ) )
                {
                    setPoint( p, x, to );
                    break;
                }
            }
        }

        c.replace( item, tree.body, p );
    }

    writeGeo( c, g );
    t.doc.Commit();
    return true;
}


bool SchRotateItem( SchEditTarget& t, const SchItemRef& aItem, bool aCcw, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx c( t );
    Sel sel;

    if( !resolve( c, { aItem }, sel, true, err ) )
        return false;

    if( sel.symbols.size() == 1 )
    {
        int s = *sel.symbols.begin();
        return transformSymbol( c, s, aCcw ? T_CCW : T_CW, TrF( "Rotate {0}", { c.shownRef( s ) } ), err );
    }

    if( sel.labels.size() == 1 )
    {
        int             i = *sel.labels.begin();
        const SchLabel& L = c.sc.labels[(size_t) i];
        static const Spin ccwNext[] = { Spin::BOTTOM, Spin::LEFT, Spin::UP, Spin::RIGHT }; // from LEFT, UP, RIGHT, BOTTOM
        static const Spin cwNext[] = { Spin::UP, Spin::RIGHT, Spin::BOTTOM, Spin::LEFT };
        Spin next = ( aCcw ? ccwNext : cwNext )[(int) L.spin];

        auto fields = [&]( const ItemTree& tree, N prop, std::vector<Patch>& p )
        {
            N fat = prop.find( "at" );

            if( !fat )
                return;

            Pt   rel = Pt( fat.num( 0 ), fat.num( 1 ) ) - L.pos;
            Pt   r = aCcw ? Pt( rel.y, -rel.x ) : Pt( -rel.y, rel.x );
            bool vertical = isVertical( fat.num( 2 ) );
            setPoint( p, fat, L.pos + r );
            setAngle( p, fat, vertical ? 0 : 90 );

            if( ( !vertical && !aCcw ) || ( vertical && aCcw ) )
                flipHJustify( tree, prop, p );
        };

        return respinLabel( c, i, next, fields, Tr( "Rotate" ), err );
    }

    if( sel.texts.size() == 1 )
        return changeText( c, *sel.texts.begin(), true, aCcw, err );

    err = Tr( "This item cannot be rotated" );
    return false;
}


bool SchMirrorItem( SchEditTarget& t, const SchItemRef& aItem, bool aVertical, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx c( t );
    Sel sel;

    if( !resolve( c, { aItem }, sel, true, err ) )
        return false;

    if( sel.symbols.size() == 1 )
    {
        int s = *sel.symbols.begin();
        return transformSymbol( c, s, aVertical ? T_MX : T_MY, TrF( "Mirror {0}", { c.shownRef( s ) } ), err );
    }

    if( sel.labels.size() == 1 )
    {
        int             i = *sel.labels.begin();
        const SchLabel& L = c.sc.labels[(size_t) i];
        Spin            next = L.spin;

        if( aVertical && ( L.spin == Spin::UP || L.spin == Spin::BOTTOM ) )
            next = L.spin == Spin::UP ? Spin::BOTTOM : Spin::UP;
        else if( !aVertical && ( L.spin == Spin::LEFT || L.spin == Spin::RIGHT ) )
            next = L.spin == Spin::LEFT ? Spin::RIGHT : Spin::LEFT;

        auto fields = [&]( const ItemTree& tree, N prop, std::vector<Patch>& p )
        {
            N fat = prop.find( "at" );

            if( !fat )
                return;

            Pt q( fat.num( 0 ), fat.num( 1 ) );

            if( aVertical )
                q.y = 2 * L.pos.y - q.y;
            else
                q.x = 2 * L.pos.x - q.x;

            setPoint( p, fat, q );

            if( isVertical( fat.num( 2 ) ) == aVertical )
                flipHJustify( tree, prop, p );
        };

        return respinLabel( c, i, next, fields, Tr( "Mirror" ), err );
    }

    if( sel.texts.size() == 1 )
        return changeText( c, *sel.texts.begin(), false, aVertical, err );

    err = Tr( "This item cannot be mirrored" );
    return false;
}


bool SchDeleteItems( SchEditTarget& t, const std::vector<SchItemRef>& aItems, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx c( t );
    Sel gone;

    if( !resolve( c, aItems, gone, false, err ) )
        return false;

    if( gone.empty() )
    {
        err = Tr( "Nothing selected" );
        return false;
    }

    wholeItems( c, gone );
    const SchScreen& sc = c.sc;
    std::vector<Pt>  touched;

    for( int i : gone.lines )
    {
        touched.push_back( roundIU( sc.lines[(size_t) i].a ) );
        touched.push_back( roundIU( sc.lines[(size_t) i].b ) );
    }

    for( int s : gone.symbols )
    {
        std::vector<Pt> tips = pinTips( c, s );
        touched.insert( touched.end(), tips.begin(), tips.end() );
    }

    int flat = 0;

    for( size_t s = 0; s < sc.sheets.size(); ++s )
    {
        for( const SchSheetPin& pin : sc.sheets[s].pins )
        {
            if( gone.sheets.count( (int) s ) || gone.sheetPins.count( flat ) )
                touched.push_back( roundIU( pin.pos ) );

            ++flat;
        }
    }

    for( int i : gone.labels )
        touched.push_back( roundIU( sc.labels[(size_t) i].pos ) );

    for( int i : gone.entries )
    {
        touched.push_back( roundIU( sc.busEntries[(size_t) i].pos ) );
        touched.push_back( roundIU( sc.busEntries[(size_t) i].pos + sc.busEntries[(size_t) i].size ) );
    }

    Geo g = buildGeo( c, Sel(), Pt(), gone );
    fixJunctions( g, buildGeo( c, Sel(), Pt(), Sel() ), touched, false );

    std::string label = Tr( "Delete" );

    if( gone.symbols.size() == 1 && gone.lines.empty() && gone.sheets.empty() && gone.labels.empty() )
        label = TrF( "Delete {0}", { c.shownRef( *gone.symbols.begin() ) } );

    t.doc.Begin( label );
    std::set<int> removed;

    for( int parse : selItems( c, gone ) )
    {
        int di = c.docItem( parse );

        if( di >= 0 )
        {
            t.doc.Remove( c.file, di );
            removed.insert( parse );
        }
    }

    // sheet pins: remove their node from the sheet
    std::map<int, std::set<int>> pinsBySheet; // sheet -> pin indices

    for( int f : gone.sheetPins )
    {
        int sh = -1, pin = -1;

        if( sheetPinRef( sc, f, sh, pin ) && !removed.count( sc.sheets[(size_t) sh].docItem ) )
            pinsBySheet[sh].insert( pin );
    }

    for( const auto& [sh, pins] : pinsBySheet )
    {
        int di = c.docItem( sc.sheets[(size_t) sh].docItem );

        if( di < 0 )
            continue;

        ItemTree           tree( c.body( di ) );
        std::vector<Patch> p;
        int                k = 0;

        tree.root.each( "pin",
                        [&]( N pin )
                        {
                            if( pins.count( k++ ) )
                                p.push_back( { wsStart( tree.body, pin.begin() ), pin.end(), std::string() } );
                        } );

        c.replace( di, tree.body, p );
    }

    writeGeo( c, g );
    t.doc.Commit();
    return true;
}


bool SchSetField( SchEditTarget& t, int aSymbol, const std::string& aName, const std::string& aValue, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx c( t );

    if( aSymbol < 0 || aSymbol >= (int) c.sc.symbols.size() )
    {
        err = Tr( "Invalid symbol" );
        return false;
    }

    const SchSymbol& sym = c.sc.symbols[(size_t) aSymbol];
    int              di = c.docItem( sym.docItem );

    if( di < 0 )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    ItemTree           tree( c.body( di ) );
    std::vector<Patch> p;
    const std::string  want = lower( aName );
    const std::string  rootUuid = c.sch.screens.empty() ? std::string() : c.sch.screens[0].uuid;
    const std::string  p6 = c.node.path == "/" ? std::string() : c.node.path;
    const std::string  p7 = "/" + rootUuid + p6;
    const std::string  ref = c.shownRef( aSymbol );

    // KiCad 6 root files keep references (and per sheet values) of all symbols
    const int          rootFile = c.sch.screens.empty() ? -1 : c.sch.screens[0].sourceFile;
    const int          legacyItem = rootFile >= 0 ? findItem( c.t.doc, rootFile, "symbol_instances" ) : -1;
    std::unique_ptr<ItemTree> legacy;
    N                  legacyPath;
    std::vector<Patch> lp;

    if( legacyItem >= 0 )
    {
        legacy = std::make_unique<ItemTree>( c.t.doc.Doc( (size_t) rootFile ).Item( (size_t) legacyItem ).body );
        const std::string key = p6 + "/" + sym.uuid;

        legacy->root.each( "path",
                           [&]( N path )
                           {
                               if( !legacyPath && path.str( 0 ) == key )
                                   legacyPath = path;
                           } );
    }

    N prop;

    for( N ch = tree.root.arg( 0 ); ch; ch = ch.next() )
    {
        if( ch.isList() && ch.head() == "property" && lower( std::string( ch.str( 0 ) ) ) == want )
        {
            prop = ch;
            break;
        }
    }

    if( want == "reference" )
    {
        if( aValue.empty() )
        {
            err = Tr( "The reference cannot be empty" );
            return false;
        }

        std::string old;
        bool        done = false, only = false;

        if( N path = instancePath( tree.root, p6, p7, only ) )
        {
            if( N r = path.find( "reference" ) )
            {
                old = std::string( r.str( 0 ) );
                setString( p, r.arg( 0 ), aValue );
                done = true;
            }
        }

        if( !done && legacyPath )
        {
            if( N r = legacyPath.find( "reference" ) )
            {
                old = std::string( r.str( 0 ) );
                setString( lp, r.arg( 0 ), aValue );
                done = true;
            }
        }

        // the property mirrors the instance reference (or is the only place it is kept)
        if( prop && ( !done || std::string( prop.str( 1 ) ) == old ) )
            setString( p, prop.arg( 1 ), aValue );
        else if( !prop && !done )
        {
            err = TrF( "The symbol has no \"{0}\" field", { aName } );
            return false;
        }
    }
    else
    {
        if( !prop )
        {
            err = TrF( "The symbol has no \"{0}\" field", { aName } );
            return false;
        }

        setString( p, prop.arg( 1 ), aValue );

        // KiCad 6 per sheet value / footprint
        if( legacyPath && ( want == "value" || want == "footprint" ) )
        {
            N v = legacyPath.find( want.c_str() );

            if( v && !v.str( 0 ).empty() )
                setString( lp, v.arg( 0 ), aValue );
        }
    }

    if( p.empty() && lp.empty() )
        return true; // already set

    t.doc.Begin( ref + ": " + Tr( aName.c_str() ) + " = " + aValue );
    c.replace( di, tree.body, p );

    if( legacy && !lp.empty() )
        t.doc.Replace( rootFile, legacyItem, applyPatches( legacy->body, lp ) );

    t.doc.Commit();
    return true;
}


namespace
{

/// Every connection point of the sheet (for snapping).
std::vector<Pt> connectionPoints( const Ctx& c )
{
    Geo             g = buildGeo( c, Sel(), Pt(), Sel() );
    std::vector<Pt> out;

    for( const GSeg& s : g.segs )
    {
        out.push_back( s.a );
        out.push_back( s.b );
    }

    for( const GJunction& j : g.juncs )
        out.push_back( j.p );

    for( const GAnchor& a : g.anchors )
    {
        if( a.kind != GAnchor::NO_CONNECT )
            out.push_back( a.p );
    }

    return out;
}


bool nearestPoint( const std::vector<Pt>& aPoints, const Pt& p, double aRadius, Pt& aOut )
{
    double best = aRadius * aRadius;
    bool   hit = false;

    for( const Pt& q : aPoints )
    {
        double d2 = ( q.x - p.x ) * ( q.x - p.x ) + ( q.y - p.y ) * ( q.y - p.y );

        if( d2 <= best )
        {
            best = d2;
            aOut = q;
            hit = true;
        }
    }

    return hit;
}


/// A point given for a new item: an exact connection point stays, anything else goes to the grid.
Pt placePoint( const std::vector<Pt>& aConnections, const Pt& p )
{
    Pt hit;

    if( nearestPoint( aConnections, p, 0.001, hit ) )
        return hit;

    return gridPoint( p );
}

} // anonymous namespace


bool SchAddWire( SchEditTarget& t, const std::vector<Pt>& aPoints, bool aBus, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx             c( t );
    std::vector<Pt> conn = connectionPoints( c );
    std::vector<Pt> pts;

    for( const Pt& raw : aPoints )
    {
        Pt q = placePoint( conn, raw );

        if( pts.empty() || !same( pts.back(), q ) )
            pts.push_back( q );
    }

    // straight runs through a corner become one segment
    for( size_t i = 1; i + 1 < pts.size(); )
    {
        int64_t ax = iu( pts[i].x ) - iu( pts[i - 1].x ), ay = iu( pts[i].y ) - iu( pts[i - 1].y );
        int64_t bx = iu( pts[i + 1].x ) - iu( pts[i].x ), by = iu( pts[i + 1].y ) - iu( pts[i].y );

        if( ax * by - ay * bx == 0 && ax * bx + ay * by > 0 )
            pts.erase( pts.begin() + (std::ptrdiff_t) i );
        else
            ++i;
    }

    if( pts.size() < 2 )
    {
        err = Tr( "The wire is too short" );
        return false;
    }

    const Geo    before = buildGeo( c, Sel(), Pt(), Sel() );
    Geo          g = before;
    const size_t firstNew = g.segs.size();
    std::vector<Pt> touched( pts.begin(), pts.end() );

    for( size_t i = 0; i + 1 < pts.size(); ++i )
        addSeg( g, pts[i], pts[i + 1], aBus );

    // a new end on the middle of a wire of its kind splits that wire
    for( const Pt& v : pts )
    {
        for( size_t i = 0; i < firstNew; ++i )
        {
            if( alive( g.segs[i] ) && g.segs[i].bus == aBus && inside( v, g.segs[i].a, g.segs[i].b ) )
                splitSeg( g, i, v );
        }
    }

    // connection points the new wire passes over may need a junction
    std::vector<Pt> existing;

    for( size_t i = 0; i < firstNew; ++i )
    {
        existing.push_back( g.segs[i].a );
        existing.push_back( g.segs[i].b );
    }

    for( const GAnchor& a : g.anchors )
        existing.push_back( a.p );

    for( const Pt& q : existing )
    {
        for( size_t i = firstNew; i < g.segs.size(); ++i )
        {
            if( alive( g.segs[i] ) && inside( q, g.segs[i].a, g.segs[i].b ) )
                touched.push_back( q );
        }
    }

    fixJunctions( g, before, touched, true );

    // wires are broken at junctions (KiCad's clean-up)
    for( size_t j = 0; j < g.juncs.size(); ++j )
    {
        const GJunction jn = g.juncs[j];

        if( jn.dead )
            continue;

        for( size_t i = 0; i < g.segs.size(); ++i )
        {
            bool isNew = i >= firstNew || g.segs[i].item < 0;

            if( ( jn.item < 0 || isNew ) && alive( g.segs[i] ) && inside( jn.p, g.segs[i].a, g.segs[i].b ) )
                splitSeg( g, i, jn.p );
        }
    }

    t.doc.Begin( aBus ? Tr( "Add bus" ) : Tr( "Add wire" ) );
    writeGeo( c, g );
    t.doc.Commit();
    return true;
}


bool SchAddLabel( SchEditTarget& t, const Pt& aPos, const std::string& aText, SchLabelKind aKind, double aAngle,
                  std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx         c( t );
    std::string text;

    // net names cannot hold a slash or line breaks (KiCad escapes them)
    for( char ch : aText )
    {
        if( ch == '/' )
            text += "{slash}";
        else if( ch != '\n' && ch != '\r' )
            text += ch;
    }

    while( !text.empty() && std::isspace( (unsigned char) text.back() ) )
        text.pop_back();

    while( !text.empty() && std::isspace( (unsigned char) text.front() ) )
        text.erase( text.begin() );

    if( text.empty() )
    {
        err = Tr( "The label needs a name" );
        return false;
    }

    int angle = ( (int) std::lround( aAngle / 90.0 ) % 4 + 4 ) % 4 * 90;

    // text size of the labels already on the sheet
    double size = 0;
    const SchLabel::Kind kind = aKind == SchLabelKind::GLOBAL         ? SchLabel::GLOBAL
                                : aKind == SchLabelKind::HIERARCHICAL ? SchLabel::HIERARCHICAL
                                                                      : SchLabel::LOCAL;

    for( const SchLabel& L : c.sc.labels )
    {
        if( L.kind == kind && size == 0 )
            size = L.attr.sizeY;
    }

    if( size <= 0 && !c.sc.labels.empty() )
        size = c.sc.labels[0].attr.sizeY;

    if( size <= 0 || size > 50 )
        size = 1.27;

    Pt pos = placePoint( connectionPoints( c ), aPos );

    t.doc.Begin( Tr( "Add label" ) );
    insertItem( c, labelText( c.style, aKind, text, pos, angle, size ) );
    t.doc.Commit();
    return true;
}


bool SchAddNoConnect( SchEditTarget& t, const Pt& aPos, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx c( t );
    Pt  pos = placePoint( connectionPoints( c ), aPos );

    for( const SchNoConnect& nc : c.sc.noConnects )
    {
        if( same( nc.pos, pos ) )
        {
            err = Tr( "There is already a no-connect flag here" );
            return false;
        }
    }

    t.doc.Begin( Tr( "Add no-connect flag" ) );
    insertItem( c, noConnectText( c.style, pos ) );
    t.doc.Commit();
    return true;
}


bool SchAddJunction( SchEditTarget& t, const Pt& aPos, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx c( t );
    Pt  pos = placePoint( connectionPoints( c ), aPos );
    Geo g = buildGeo( c, Sel(), Pt(), Sel() );

    if( hasJunction( g, pos ) )
    {
        err = Tr( "There is already a junction here" );
        return false;
    }

    const size_t n = g.segs.size();

    for( size_t i = 0; i < n; ++i )
    {
        if( alive( g.segs[i] ) && inside( pos, g.segs[i].a, g.segs[i].b ) )
            splitSeg( g, i, pos );
    }

    GJunction j;
    j.p = pos;
    g.juncs.push_back( j );

    t.doc.Begin( Tr( "Add junction" ) );
    writeGeo( c, g );
    t.doc.Commit();
    return true;
}


bool SchDuplicateSymbol( SchEditTarget& t, int aSymbol, double aDx, double aDy, std::string& err )
{
    if( !validTarget( t, err ) )
        return false;

    Ctx c( t );

    if( aSymbol < 0 || aSymbol >= (int) c.sc.symbols.size() )
    {
        err = Tr( "Invalid symbol" );
        return false;
    }

    const SchSymbol& sym = c.sc.symbols[(size_t) aSymbol];
    int              di = c.docItem( sym.docItem );

    if( di < 0 )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    // references in use, by prefix
    std::map<std::string, std::set<long>> used;

    for( const SheetNode& node : c.sch.sheets )
    {
        if( node.screen < 0 )
            continue;

        for( const SchSymbol& s : c.sch.screens[(size_t) node.screen].symbols )
        {
            std::string prefix;
            long        n = -1;
            size_t      digits = 0;
            splitRef( c.sch.Reference( node, s ), prefix, n, digits );

            if( n >= 0 )
                used[prefix].insert( n );
        }
    }

    std::string prefix;
    long        n0 = -1;
    size_t      digits = 0;
    splitRef( c.sch.Reference( c.node, sym ), prefix, n0, digits );
    const bool pad = digits > 1 && n0 >= 0 && std::to_string( n0 ).size() < digits;

    auto nextRef = [&]() -> std::string
    {
        if( prefix.empty() || n0 < 0 )
            return prefix + "?";

        long n = 1;

        while( used[prefix].count( n ) )
            ++n;

        used[prefix].insert( n );
        std::string s = std::to_string( n );

        if( pad && s.size() < digits )
            s = std::string( digits - s.size(), '0' ) + s;

        return prefix + s;
    };

    ItemTree           tree( c.body( di ) );
    std::vector<Patch> p;
    translateNodes( tree.root, Pt( roundIU( aDx ), roundIU( aDy ) ), p );

    // new UUIDs for the symbol and its pins
    std::string newUuidText;

    std::function<void( N, bool )> uuids = [&]( N aNode, bool aTop )
    {
        for( N ch = aNode.first(); ch; ch = ch.next() )
        {
            if( !ch.isList() )
                continue;

            if( ch.head() == "uuid" && ch.arg( 0 ) )
            {
                std::string u = newUuid();
                p.push_back( { ch.arg( 0 ).begin(), ch.arg( 0 ).end(), ch.arg( 0 ).isString() ? QuoteString( u ) : u } );

                if( aTop )
                    newUuidText = u;
            }
            else if( ch.head() != "instances" )
            {
                uuids( ch, false );
            }
        }
    };

    uuids( tree.root, true );

    if( newUuidText.empty() )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    const std::string rootUuid = c.sch.screens[0].uuid;
    const std::string p6 = c.node.path == "/" ? std::string() : c.node.path;
    const std::string p7 = "/" + rootUuid + p6;
    const std::string newRef = nextRef();
    N                 prop;

    for( N ch = tree.root.arg( 0 ); ch; ch = ch.next() )
    {
        if( ch.isList() && ch.head() == "property" && ch.str( 0 ) == "Reference" )
            prop = ch;
    }

    if( prop )
        setString( p, prop.arg( 1 ), newRef );

    if( N inst = tree.root.find( "instances" ) )
    {
        // KiCad 7+: the copy's own instances, one reference per sheet path
        inst.each( "project",
                   [&]( N proj )
                   {
                       proj.each( "path",
                                  [&]( N path )
                                  {
                                      std::string_view v = path.str( 0 );
                                      bool here = v == p7 || v == p6 || ( p6.empty() && v == "/" );

                                      if( N r = path.find( "reference" ) )
                                          setString( p, r.arg( 0 ), here ? newRef : nextRef() );
                                  } );
                   } );
    }

    std::string copy = applyPatches( tree.body, p );
    const int   rootFile = c.sch.screens[0].sourceFile;
    const int   legacyItem = tree.root.find( "instances" ) ? -1 : findItem( c.t.doc, rootFile, "symbol_instances" );
    std::string legacyText;

    if( legacyItem >= 0 )
    {
        // KiCad 6: the root lists the copy for every sheet that shows this file
        ItemTree           lt( c.t.doc.Doc( (size_t) rootFile ).Item( (size_t) legacyItem ).body );
        std::vector<Patch> lp;

        for( const SheetNode& node : c.sch.sheets )
        {
            if( node.screen != c.node.screen )
                continue;

            const std::string np6 = node.path == "/" ? std::string() : node.path;
            const std::string key = np6 + "/" + sym.uuid;
            N                 entry;

            lt.root.each( "path",
                          [&]( N path )
                          {
                              if( !entry && path.str( 0 ) == key )
                                  entry = path;
                          } );

            if( !entry )
                continue;

            ItemTree           et( lt.body.substr( entry.begin(), entry.end() - entry.begin() ) );
            std::vector<Patch> ep;
            setString( ep, et.root.arg( 0 ), np6 + "/" + newUuidText );

            if( N r = et.root.find( "reference" ) )
                setString( ep, r.arg( 0 ), &node == &c.node ? newRef : nextRef() );

            size_t ws = wsStart( lt.body, entry.begin() );
            lp.push_back( { entry.end(), entry.end(), lt.body.substr( ws, entry.begin() - ws ) + applyPatches( et.body, ep ) } );
        }

        if( !lp.empty() )
            legacyText = applyPatches( lt.body, lp );
    }

    const Document& d = c.t.doc.Doc( (size_t) c.file );
    t.doc.Begin( TrF( "Duplicate {0}", { c.shownRef( aSymbol ) } ) );
    t.doc.InsertAfter( c.file, di, copy, lineWs( d.Item( (size_t) di ).ws, c.style.multiline ? "\t" : "  " ) );

    if( !legacyText.empty() )
        t.doc.Replace( rootFile, legacyItem, legacyText );

    t.doc.Commit();
    return true;
}


// ================================================================================================
// Helpers for drawing
// ================================================================================================

bool SchSnapPoint( const Schematic& aSch, int aSheet, const Pt& aPoint, double aRadius, Pt& aOut )
{
    if( aSheet < 0 || aSheet >= (int) aSch.sheets.size() || aSch.sheets[(size_t) aSheet].screen < 0 )
    {
        aOut = gridPoint( aPoint );
        return false;
    }

    const Schematic* sch = &aSch;
    const SheetNode& node = sch->sheets[(size_t) aSheet];
    const SchScreen& sc = sch->screens[(size_t) node.screen];
    std::vector<Pt>  pts;

    for( size_t s = 0; s < sc.symbols.size(); ++s )
    {
        const SchSymbol& sym = sc.symbols[s];
        const LibSymbol* lib = sch->Lib( sc, sym );

        if( !lib )
            continue;

        const int unit = sch->Unit( node, sym );

        for( const LibPin& pin : lib->pins )
        {
            if( pin.unit != 0 && unit != 0 && pin.unit != unit )
                continue;

            if( lib->bodyStyleCount > 1 && pin.bodyStyle != 0 && pin.bodyStyle != sym.bodyStyle )
                continue;

            pts.push_back( roundIU( sym.pos + sym.transform.apply( pin.pos ) ) );
        }
    }

    for( const SchLine& l : sc.lines )
    {
        if( l.kind != SchLine::NOTES )
        {
            pts.push_back( l.a );
            pts.push_back( l.b );
        }
    }

    for( const SchJunction& j : sc.junctions )
        pts.push_back( j.pos );

    for( const SchLabel& L : sc.labels )
        pts.push_back( L.pos );

    for( const SchSheet& sh : sc.sheets )
    {
        for( const SchSheetPin& pin : sh.pins )
            pts.push_back( pin.pos );
    }

    for( const SchBusEntry& e : sc.busEntries )
    {
        pts.push_back( e.pos );
        pts.push_back( e.pos + e.size );
    }

    if( nearestPoint( pts, aPoint, aRadius, aOut ) )
    {
        aOut = roundIU( aOut );
        return true;
    }

    aOut = gridPoint( aPoint );
    return false;
}


namespace
{

/// Ramer-Douglas-Peucker simplification.
void simplify( const std::vector<Pt>& aIn, size_t a, size_t b, double aEps, std::vector<bool>& aKeep )
{
    if( b <= a + 1 )
        return;

    const Pt& A = aIn[a];
    const Pt& B = aIn[b];
    double    dx = B.x - A.x, dy = B.y - A.y, len = std::hypot( dx, dy );
    double    best = -1;
    size_t    at = a;

    for( size_t i = a + 1; i < b; ++i )
    {
        double d = len < 1e-9 ? std::hypot( aIn[i].x - A.x, aIn[i].y - A.y )
                              : std::fabs( ( aIn[i].x - A.x ) * dy - ( aIn[i].y - A.y ) * dx ) / len;

        if( d > best )
        {
            best = d;
            at = i;
        }
    }

    if( best > aEps )
    {
        aKeep[at] = true;
        simplify( aIn, a, at, aEps, aKeep );
        simplify( aIn, at, b, aEps, aKeep );
    }
}

} // anonymous namespace


std::vector<Pt> SchWireFromStroke( const Schematic& aSch, int aSheet, const std::vector<Pt>& aStroke, double aRadius )
{
    std::vector<Pt> out;

    if( aStroke.size() < 2 )
        return out;

    Pt S, E;
    SchSnapPoint( aSch, aSheet, aStroke.front(), aRadius, S );
    SchSnapPoint( aSch, aSheet, aStroke.back(), aRadius, E );

    if( same( S, E ) )
        return out;

    double length = 0;

    for( size_t i = 1; i < aStroke.size(); ++i )
        length += std::hypot( aStroke[i].x - aStroke[i - 1].x, aStroke[i].y - aStroke[i - 1].y );

    // runs of the stroke along x or y; fewer when the stroke wiggles
    std::vector<bool> dirs; // true = horizontal
    std::vector<Pt>   corners;

    for( double eps = std::max( SCH_GRID, 0.05 * length );; eps *= 1.6 )
    {
        std::vector<bool> keep( aStroke.size(), false );
        keep.front() = keep.back() = true;
        simplify( aStroke, 0, aStroke.size() - 1, eps, keep );

        std::vector<Pt> v;

        for( size_t i = 0; i < aStroke.size(); ++i )
        {
            if( keep[i] )
                v.push_back( aStroke[i] );
        }

        dirs.clear();
        corners.clear();

        for( size_t i = 0; i + 1 < v.size(); ++i )
        {
            double dx = v[i + 1].x - v[i].x, dy = v[i + 1].y - v[i].y;

            if( std::hypot( dx, dy ) < SCH_GRID / 2 )
                continue;

            bool h = std::fabs( dx ) >= std::fabs( dy );

            if( dirs.empty() || dirs.back() != h )
            {
                if( !dirs.empty() )
                    corners.push_back( v[i] );

                dirs.push_back( h );
            }
        }

        if( dirs.size() <= 4 || eps > length )
            break;
    }

    if( dirs.empty() )
        dirs.push_back( std::fabs( E.x - S.x ) >= std::fabs( E.y - S.y ) );

    // one run only, but the ends are not in line: an L
    if( dirs.size() == 1 && iu( dirs[0] ? S.y : S.x ) != iu( dirs[0] ? E.y : E.x ) )
    {
        corners.push_back( dirs[0] ? Pt( E.x, S.y ) : Pt( S.x, E.y ) );
        dirs.push_back( !dirs[0] );
    }

    out.push_back( S );

    for( size_t j = 0; j + 1 < dirs.size(); ++j )
    {
        const Pt& prev = out.back();
        Pt        g = gridPoint( corners[j] );
        bool      last = j + 2 == dirs.size();

        // run j goes along x (or y) to the corner; the last corner lines up with the end
        if( dirs[j] )
            out.push_back( Pt( last ? E.x : g.x, prev.y ) );
        else
            out.push_back( Pt( prev.x, last ? E.y : g.y ) );
    }

    out.push_back( E );

    // drop zero length runs and straight joints
    std::vector<Pt> clean;

    for( const Pt& q : out )
    {
        if( clean.empty() || !same( clean.back(), q ) )
            clean.push_back( q );
    }

    for( size_t i = 1; i + 1 < clean.size(); )
    {
        int64_t ax = iu( clean[i].x ) - iu( clean[i - 1].x ), ay = iu( clean[i].y ) - iu( clean[i - 1].y );
        int64_t bx = iu( clean[i + 1].x ) - iu( clean[i].x ), by = iu( clean[i + 1].y ) - iu( clean[i].y );

        if( ax * by - ay * bx == 0 )
            clean.erase( clean.begin() + (std::ptrdiff_t) i );
        else
            ++i;
    }

    return clean.size() >= 2 ? clean : std::vector<Pt>();
}


std::vector<std::pair<std::string, std::string>> SchSymbolFields( const Schematic& aSch, int aSheet, int aSymbol )
{
    std::vector<std::pair<std::string, std::string>> out;

    if( aSheet < 0 || aSheet >= (int) aSch.sheets.size() )
        return out;

    const SheetNode& node = aSch.sheets[(size_t) aSheet];

    if( node.screen < 0 || aSymbol < 0 || aSymbol >= (int) aSch.screens[(size_t) node.screen].symbols.size() )
        return out;

    const SchSymbol& sym = aSch.screens[(size_t) node.screen].symbols[(size_t) aSymbol];
    out.push_back( { "Reference", aSch.Reference( node, sym ) } );

    for( const SchField& f : sym.fields )
    {
        if( f.name != "Reference" )
            out.push_back( { f.name, aSch.FieldValue( node, sym, f ) } );
    }

    return out;
}

} // namespace kp
