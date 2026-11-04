/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Wire changes (drag, splits, junctions) are planned on a small model of the sheet's
 * connections and then written as a difference: items that keep their place keep their bytes.
 * Coordinates are compared in KiCad's schematic internal units (100 nm).
 */
#include "sch_edit.h"
#include "edit.h"
#include "i18n.h"
#include "sexpr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>

namespace kp
{

namespace
{

constexpr double SCH_PI = 3.14159265358979323846;

// ---- numbers and points ----------------------------------------------------------------------

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


using PtKey = std::pair<int64_t, int64_t>;

PtKey key( const Pt& p )
{
    return { iu( p.x ), iu( p.y ) };
}


std::string num( double aMM )
{
    return FormatNumber( roundIU( aMM ) );
}


double dist( const Pt& a, const Pt& b )
{
    return std::hypot( a.x - b.x, a.y - b.y );
}


/// Is p on segment a-b, but not at its ends? (the connectivity's tolerance)
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


/// Direction from one point to another in tenths of a degree (KiCad SCH_LINE::GetAngleFrom).
int dirKey( const Pt& aFrom, const Pt& aTo )
{
    int a = (int) std::lround( std::atan2( (double) ( iu( aTo.y ) - iu( aFrom.y ) ),
                                           (double) ( iu( aTo.x ) - iu( aFrom.x ) ) ) * 1800.0 / SCH_PI );
    return a == -1800 ? 1800 : a;
}


bool horizontal( const Pt& a, const Pt& b )
{
    return iu( a.y ) == iu( b.y ) && iu( a.x ) != iu( b.x );
}


bool vertical( const Pt& a, const Pt& b )
{
    return iu( a.x ) == iu( b.x ) && iu( a.y ) != iu( b.y );
}


std::string newUuid()
{
    static std::mt19937_64 rng( std::random_device {}() ^ 0x6e65746c6f6f6dULL );
    uint64_t hi = rng(), lo = rng();
    hi = ( hi & 0xFFFFFFFFFFFF0FFFULL ) | 0x0000000000004000ULL; // version 4
    lo = ( lo & 0x3FFFFFFFFFFFFFFFULL ) | 0x8000000000000000ULL; // variant 1
    char buf[40];
    snprintf( buf, sizeof( buf ), "%08x-%04x-%04x-%04x-%012llx", (unsigned) ( hi >> 32 ),
              (unsigned) ( ( hi >> 16 ) & 0xFFFF ), (unsigned) ( hi & 0xFFFF ), (unsigned) ( lo >> 48 ),
              (unsigned long long) ( lo & 0xFFFFFFFFFFFFULL ) );
    return buf;
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


/// Angle (third atom) of an "(at x y angle)" node, added when the node has none.
void setAngle( std::vector<Patch>& aPatches, N aAt, int aAngle )
{
    N a = aAt.arg( 2 );

    if( a && a.isNumber() )
    {
        if( (int) std::lround( ParseNumber( a.text() ) ) != aAngle )
            aPatches.push_back( { a.begin(), a.end(), std::to_string( aAngle ) } );
    }
    else if( N y = aAt.arg( 1 ) )
    {
        aPatches.push_back( { y.end(), y.end(), " " + std::to_string( aAngle ) } );
    }
}


/// Replace a string atom if the text changes.
void setString( std::vector<Patch>& aPatches, N aAtom, const std::string& aValue )
{
    if( !aAtom || !aAtom.isAtom() || ( aAtom.isString() && std::string( aAtom.text() ) == aValue ) )
        return;

    aPatches.push_back( { aAtom.begin(), aAtom.end(), QuoteString( aValue ) } );
}


/// Remove a child node together with the whitespace in front of it.
void removeNode( std::vector<Patch>& aPatches, const ItemTree& t, N aNode )
{
    aPatches.push_back( { wsStart( t.body, aNode.begin() ), aNode.end(), std::string() } );
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


/// Horizontal justification of a text's effects: "left", "right" or "" (centred).
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


/// Set the horizontal justification of a text to "left", "right" or "" (centred).
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
                if( aWant.empty() )
                {
                    // centred: drop the word (or the whole node when it was the only one)
                    bool alone = just.size() == 2;

                    if( alone )
                        removeNode( aPatches, t, just );
                    else
                        aPatches.push_back( { wsStart( t.body, c.begin() ), c.end(), std::string() } );
                }
                else if( c.text() != aWant )
                {
                    aPatches.push_back( { c.begin(), c.end(), aWant } );
                }

                return;
            }
        }

        if( !aWant.empty() )
            aPatches.push_back( { just.first().end(), just.first().end(), " " + aWant } );

        return;
    }

    if( aWant.empty() )
        return;

    // no justify yet: add one after the font, separated like it
    N           font = eff.find( "font" );
    size_t      at = font ? font.end() : eff.first().end();
    size_t      ws = font ? wsStart( t.body, font.begin() ) : at;
    std::string sep = font ? t.body.substr( ws, font.begin() - ws ) : std::string( " " );

    if( sep.empty() )
        sep = " ";

    aPatches.push_back( { at, at, sep + "(justify " + aWant + ")" } );
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

// ---- writing new items -----------------------------------------------------------------------

/// How the file's KiCad version writes schematic items.
struct Style
{
    int         version = 0;
    bool        multiline = false;   ///< KiCad 8+: one token per line, tab indented, quoted UUIDs
    bool        strokeColor = false; ///< KiCad 6 writes a stroke colour
    bool        labelAutoplaced = false; ///< KiCad 7 marks labels "(fields_autoplaced)"
    std::string indent;              ///< indentation of top-level items
    std::string unit;                ///< one more level

    std::string in( int aLevel ) const
    {
        std::string s = indent;

        for( int i = 0; i < aLevel; ++i )
            s += unit;

        return s;
    }

    std::string uuid() const
    {
        return multiline ? "(uuid \"" + newUuid() + "\")" : "(uuid " + newUuid() + ")";
    }
};

std::string xyText( const char* aKey, const Pt& p )
{
    return std::string( "(" ) + aKey + " " + num( p.x ) + " " + num( p.y ) + ")";
}


std::string wireText( const Style& st, bool aBus, const Pt& a, const Pt& b )
{
    const std::string head = aBus ? "bus" : "wire";

    if( st.multiline )
    {
        return "(" + head + "\n" + st.in( 1 ) + "(pts\n" + st.in( 2 ) + xyText( "xy", a ) + " " + xyText( "xy", b ) + "\n"
               + st.in( 1 ) + ")\n" + st.in( 1 ) + "(stroke\n" + st.in( 2 ) + "(width 0)\n" + st.in( 2 )
               + "(type default)\n" + st.in( 1 ) + ")\n" + st.in( 1 ) + st.uuid() + "\n" + st.in( 0 ) + ")";
    }

    return "(" + head + " (pts " + xyText( "xy", a ) + " " + xyText( "xy", b ) + ")\n" + st.in( 1 )
           + "(stroke (width 0) (type default)" + ( st.strokeColor ? " (color 0 0 0 0)" : "" ) + ")\n" + st.in( 1 )
           + st.uuid() + "\n" + st.in( 0 ) + ")";
}


std::string junctionText( const Style& st, const Pt& p )
{
    if( st.multiline )
    {
        return "(junction\n" + st.in( 1 ) + xyText( "at", p ) + "\n" + st.in( 1 ) + "(diameter 0)\n" + st.in( 1 )
               + "(color 0 0 0 0)\n" + st.in( 1 ) + st.uuid() + "\n" + st.in( 0 ) + ")";
    }

    return "(junction " + xyText( "at", p ) + " (diameter 0) (color 0 0 0 0)\n" + st.in( 1 ) + st.uuid() + "\n"
           + st.in( 0 ) + ")";
}


std::string noConnectText( const Style& st, const Pt& p )
{
    if( st.multiline )
        return "(no_connect\n" + st.in( 1 ) + xyText( "at", p ) + "\n" + st.in( 1 ) + st.uuid() + "\n" + st.in( 0 ) + ")";

    return "(no_connect " + xyText( "at", p ) + " " + st.uuid() + ")";
}


std::string labelText( const Style& st, SchLabelKind aKind, const std::string& aText, const Pt& p, int aAngle,
                       double aSize )
{
    const char* head = aKind == SchLabelKind::GLOBAL         ? "global_label"
                       : aKind == SchLabelKind::HIERARCHICAL ? "hierarchical_label"
                                                             : "label";
    std::string just = aAngle == 180 || aAngle == 270 ? "right" : "left";
    std::string at = "(at " + num( p.x ) + " " + num( p.y ) + " " + std::to_string( aAngle ) + ")";
    std::string size = "(size " + num( aSize ) + " " + num( aSize ) + ")";

    if( aKind == SchLabelKind::LOCAL )
        just += " bottom";

    if( st.multiline )
    {
        std::string s = std::string( "(" ) + head + " " + QuoteString( aText ) + "\n";

        if( aKind != SchLabelKind::LOCAL )
            s += st.in( 1 ) + "(shape input)\n";

        s += st.in( 1 ) + at + "\n";

        if( aKind == SchLabelKind::GLOBAL )
            s += st.in( 1 ) + "(fields_autoplaced yes)\n";

        s += st.in( 1 ) + "(effects\n" + st.in( 2 ) + "(font\n" + st.in( 3 ) + size + "\n" + st.in( 2 ) + ")\n" + st.in( 2 )
             + "(justify " + just + ")\n" + st.in( 1 ) + ")\n" + st.in( 1 ) + st.uuid() + "\n" + st.in( 0 ) + ")";
        return s;
    }

    bool autoplaced = aKind == SchLabelKind::GLOBAL || st.labelAutoplaced;
    return std::string( "(" ) + head + " " + QuoteString( aText ) + ( aKind != SchLabelKind::LOCAL ? " (shape input)" : "" )
           + " " + at + ( autoplaced ? " (fields_autoplaced)" : "" ) + "\n" + st.in( 1 ) + "(effects (font " + size
           + ") (justify " + just + "))\n" + st.in( 1 ) + st.uuid() + "\n" + st.in( 0 ) + ")";
}

// ---- the sheet being edited ------------------------------------------------------------------

/// Items of a sheet picked by the user, sorted by kind (indices into the screen's lists).
struct Sel
{
    std::set<int> symbols, sheets, labels, junctions, noConnects, entries, texts, graphics, images, lines, sheetPins;

    bool empty() const
    {
        return symbols.empty() && sheets.empty() && labels.empty() && junctions.empty() && noConnects.empty()
               && entries.empty() && texts.empty() && graphics.empty() && images.empty() && lines.empty()
               && sheetPins.empty();
    }

    size_t count() const
    {
        return symbols.size() + sheets.size() + labels.size() + junctions.size() + noConnects.size() + entries.size()
               + texts.size() + graphics.size() + images.size() + lines.size() + sheetPins.size();
    }
};

class Ctx
{
public:
    explicit Ctx( SchEditTarget& aT ) :
            t( aT ),
            sch( aT.sch )
    {
    }

    /// Check the target and prepare; false (with a message) if the sheet cannot be edited.
    bool Init( std::string& err )
    {
        if( t.sheet < 0 || t.sheet >= (int) sch.sheets.size() )
        {
            err = Tr( "Not a schematic" );
            return false;
        }

        node = &sch.sheets[(size_t) t.sheet];

        if( node->screen < 0 || node->screen >= (int) sch.screens.size() )
        {
            err = Tr( "Not a schematic" );
            return false;
        }

        sc = &sch.screens[(size_t) node->screen];
        file = sc->file;

        if( file < 0 || file >= (int) t.doc.FileCount() || !t.doc.FileEditable( (size_t) file ) )
        {
            err = Tr( "Item not found in the file" );
            return false;
        }

        items = t.doc.Doc( (size_t) file ).LiveItems();
        style = styleFor( file, sc->version );
        return true;
    }

    /// Document item of a parsed element (-1 if unknown).
    int docItem( int aOrdinal ) const
    {
        return aOrdinal >= 0 && aOrdinal < (int) items.size() ? items[(size_t) aOrdinal] : -1;
    }

    const std::string& body( int aDocItem ) const { return t.doc.Doc( (size_t) file ).Item( (size_t) aDocItem ).body; }

    void begin( const std::string& aLabel ) { t.doc.Begin( aLabel ); }
    void commit() { t.doc.Commit(); }

    void replace( int aDocItem, const std::string& aOld, const std::vector<Patch>& aPatches, int aFile = -1 )
    {
        if( aPatches.empty() )
            return;

        std::string n = applyPatches( aOld, aPatches );

        if( n != aOld )
            t.doc.Edit( aFile < 0 ? file : aFile ).Replace( aDocItem, n );
    }

    void remove( int aDocItem ) { t.doc.Edit( file ).Remove( aDocItem ); }

    /// Insert a new item where KiCad keeps its kind: after the last item of the same kind, else
    /// in front of the closing sections (sheet and symbol instances, embedded files).
    void insert( const std::string& aBody )
    {
        const Document&   d = t.doc.Doc( (size_t) file );
        const std::string head = headOf( aBody );
        int               lastSame = -1, firstTail = -1, last = -1;

        for( int i : d.LiveItems() )
        {
            std::string h = headOf( d.Item( (size_t) i ).body );

            if( firstTail < 0
                && ( h == "sheet_instances" || h == "symbol_instances" || h == "embedded_fonts" || h == "embedded_files" ) )
            {
                firstTail = i;
            }

            if( h == head && firstTail < 0 )
                lastSame = i;

            last = i;
        }

        const std::string ws = "\n" + style.indent;
        std::vector<int>  live = d.LiveItems();
        Document&         e = t.doc.Edit( file );

        if( lastSame >= 0 )
        {
            // after lastSame = in front of the item that follows it
            auto it = std::find( live.begin(), live.end(), lastSame );

            if( it + 1 != live.end() )
                e.InsertBefore( *( it + 1 ), aBody, ws );
            else
                e.Append( aBody, ws );
        }
        else if( firstTail >= 0 )
        {
            e.InsertBefore( firstTail, aBody, ws );
        }
        else
        {
            e.Append( aBody, last >= 0 ? ws : "\n" + style.indent );
        }
    }

    std::string shownRef( int aSymbol ) const { return SchematicShownReference( sch, *node, sc->symbols[(size_t) aSymbol] ); }

    /// Unit of a symbol on this sheet (pins of other units are not there).
    int unitOf( int aSymbol ) const { return sch.Unit( *node, sc->symbols[(size_t) aSymbol] ); }

    SchEditTarget&     t;
    const Schematic&   sch;
    const SheetNode*   node = nullptr;
    const SchScreen*   sc = nullptr;
    int                file = -1;
    std::vector<int>   items;
    Style              style;

private:
    Style styleFor( int aFile, int aVersion ) const
    {
        Style st;
        st.version = aVersion;
        st.multiline = aVersion >= 20231120;
        st.strokeColor = aVersion <= 20211123;
        st.labelAutoplaced = aVersion > 20211123 && aVersion < 20231120;
        st.unit = st.multiline ? "\t" : "  ";
        st.indent = st.unit;

        // indentation of the file's own items
        const Document& d = t.doc.Doc( (size_t) aFile );

        for( int i : d.LiveItems() )
        {
            const std::string& ws = d.Item( (size_t) i ).ws;
            size_t             nl = ws.rfind( '\n' );

            if( nl != std::string::npos )
            {
                std::string ind = ws.substr( nl + 1 );

                if( ind.find_first_not_of( " \t" ) == std::string::npos )
                {
                    st.indent = ind;

                    if( !ind.empty() )
                        st.unit = ind[0] == '\t' ? "\t" : "  ";
                }

                break;
            }
        }

        return st;
    }
};

bool checkIndex( int i, size_t n, std::string& err )
{
    if( i >= 0 && i < (int) n )
        return true;

    err = Tr( "Invalid item" );
    return false;
}


int pinOwner( const Ctx& c, int aFlatPin )
{
    std::vector<int> base = SchematicPinBase( c.sch, *c.sc );

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


/// Sort picked items into a selection. A pin stands for its symbol (and a sheet pin for its
/// sheet) when aPinIsOwner; otherwise a sheet pin is an item of its own and a symbol pin an
/// error.
bool resolve( const Ctx& c, const std::vector<SchItemRef>& aItems, Sel& sel, bool aPinIsOwner, std::string& err )
{
    const SchScreen& sc = *c.sc;

    for( const SchItemRef& r : aItems )
    {
        const int i = r.index;

        switch( r.type )
        {
        case ItemType::SCH_SYMBOL:
            if( !checkIndex( i, sc.symbols.size(), err ) )
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

            const int s = pinOwner( c, i );

            if( !checkIndex( s, sc.symbols.size(), err ) )
                return false;

            sel.symbols.insert( s );
            break;
        }

        case ItemType::SCH_WIRE:
            if( !checkIndex( i, sc.lines.size(), err ) )
                return false;

            sel.lines.insert( i );
            break;

        case ItemType::SCH_JUNCTION:
            if( !checkIndex( i, sc.junctions.size(), err ) )
                return false;

            sel.junctions.insert( i );
            break;

        case ItemType::SCH_LABEL:
            if( !checkIndex( i, sc.labels.size(), err ) )
                return false;

            sel.labels.insert( i );
            break;

        case ItemType::SCH_SHEET:
            if( !checkIndex( i, sc.sheets.size(), err ) )
                return false;

            sel.sheets.insert( i );
            break;

        case ItemType::SCH_SHEET_PIN:
        {
            int sh = -1, pin = -1;

            if( !sheetPinRef( sc, i, sh, pin ) )
                return checkIndex( -1, 0, err );

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

            return checkIndex( -1, 0, err );
        }

        default:
            err = Tr( "Invalid item" );
            return false;
        }
    }

    // a wire with several points (and a table) is one file item: take all of it
    std::set<int> lineItems, graphicItems;

    for( int i : sel.lines )
        lineItems.insert( sc.lines[(size_t) i].item );

    for( int i : sel.graphics )
        graphicItems.insert( sc.graphics[(size_t) i].item );

    for( size_t i = 0; i < sc.lines.size(); ++i )
    {
        if( lineItems.count( sc.lines[i].item ) )
            sel.lines.insert( (int) i );
    }

    for( size_t i = 0; i < sc.graphics.size(); ++i )
    {
        if( graphicItems.count( sc.graphics[i].item ) )
            sel.graphics.insert( (int) i );
    }

    return true;
}


/// Connection points of the pins a symbol has on this sheet.
std::vector<Pt> pinTips( const Ctx& c, int aSymbol )
{
    std::vector<Pt>  out;
    const SchSymbol& s = c.sc->symbols[(size_t) aSymbol];
    const LibSymbol* lib = c.sch.Lib( *c.sc, s );

    if( !lib )
        return out;

    const int unit = c.unitOf( aSymbol );

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
    return !m.empty() && !( m.size() == 1 && m[0] == aText );
}

// ---- connection model ------------------------------------------------------------------------

struct GSeg
{
    Pt   a, b;
    Pt   a0, b0;          ///< as in the file
    bool bus = false;
    int  line = -1;       ///< SchScreen::lines index, -1 = new
    int  item = -1;       ///< document item, -1 = new
    bool multi = false;   ///< part of a wire item with more than two points
    bool dead = false;
};

struct GJunction
{
    Pt   p, p0;
    int  index = -1; ///< SchScreen::junctions index, -1 = new
    int  item = -1;  ///< document item, -1 = new
    bool keep = false; ///< placed or moved by the user: not removed here
    bool dead = false;
};

struct GAnchor
{
    enum Kind { PIN, SHEET_PIN, LABEL, BUS_LABEL, PORT_LABEL, ENTRY, NO_CONNECT };

    Pt   p;
    Kind kind;
    int  owner = -1; ///< symbol, sheet, label, bus entry or no-connect index
};

struct Geo
{
    std::vector<GSeg>      segs;
    std::vector<GJunction> juncs;
    std::vector<GAnchor>   anchors;
};

bool alive( const GSeg& s )
{
    return !s.dead && !same( s.a, s.b );
}


Geo buildGeo( const Ctx& c )
{
    Geo              g;
    const SchScreen& sc = *c.sc;
    std::map<int, int> perItem;

    for( const SchLine& l : sc.lines )
        perItem[l.item]++;

    for( size_t i = 0; i < sc.lines.size(); ++i )
    {
        const SchLine& l = sc.lines[i];

        if( l.kind == SchLine::NOTES )
            continue;

        GSeg s;
        s.a = s.a0 = roundIU( l.a );
        s.b = s.b0 = roundIU( l.b );
        s.bus = l.kind == SchLine::BUS;
        s.line = (int) i;
        s.item = c.docItem( l.item );
        s.multi = perItem[l.item] > 1;
        g.segs.push_back( s );
    }

    for( size_t i = 0; i < sc.junctions.size(); ++i )
    {
        GJunction j;
        j.p = j.p0 = roundIU( sc.junctions[i].pos );
        j.index = (int) i;
        j.item = c.docItem( sc.junctions[i].item );
        g.juncs.push_back( j );
    }

    for( size_t s = 0; s < sc.symbols.size(); ++s )
    {
        for( const Pt& p : pinTips( c, (int) s ) )
            g.anchors.push_back( { p, GAnchor::PIN, (int) s } );
    }

    for( size_t s = 0; s < sc.sheets.size(); ++s )
    {
        for( const SchSheetPin& pin : sc.sheets[s].pins )
            g.anchors.push_back( { roundIU( pin.pos ), GAnchor::SHEET_PIN, (int) s } );
    }

    for( size_t i = 0; i < sc.labels.size(); ++i )
    {
        const SchLabel& L = sc.labels[i];

        if( L.kind == SchLabel::DIRECTIVE )
            continue;

        GAnchor::Kind k = L.kind != SchLabel::LOCAL ? GAnchor::PORT_LABEL
                          : isBusLabel( L.text ) ? GAnchor::BUS_LABEL
                                                 : GAnchor::LABEL;
        g.anchors.push_back( { roundIU( L.pos ), k, (int) i } );
    }

    for( size_t i = 0; i < sc.busEntries.size(); ++i )
    {
        const SchBusEntry& e = sc.busEntries[i];
        g.anchors.push_back( { roundIU( e.pos ), GAnchor::ENTRY, (int) i } );
        g.anchors.push_back( { roundIU( e.pos + e.size ), GAnchor::ENTRY, (int) i } );
    }

    for( size_t i = 0; i < sc.noConnects.size(); ++i )
        g.anchors.push_back( { roundIU( sc.noConnects[i].pos ), GAnchor::NO_CONNECT, (int) i } );

    return g;
}


/// KiCad's rule (JUNCTION_HELPERS::AnalyzePoint): a junction dot belongs where items leave a
/// point in three or more directions on the wire layer (or on the bus layer). Each symbol and
/// sheet with a pin there, and each bus entry, counts as a direction of its own; a wire passing
/// through counts twice when something ends on it there.
bool junctionNeeded( const Geo& g, const Pt& p )
{
    std::set<int>            exits[2];
    bool                     brk[2] = { false, false };
    std::vector<const GSeg*> mids[2];
    int                      unique = 100000, entryBusAngles = 0, busEnds = 0;
    bool                     hasEntry = false;
    std::set<std::pair<int, int>> owners; // (kind, owner) of pins and sheet pins counted

    for( const GSeg& s : g.segs )
    {
        if( !alive( s ) )
            continue;

        const int L = s.bus ? 1 : 0;

        if( same( p, s.a ) || same( p, s.b ) )
        {
            exits[L].insert( dirKey( p, same( p, s.a ) ? s.b : s.a ) );
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
            if( owners.insert( { (int) a.kind, a.owner } ).second )
                exits[0].insert( unique++ );

            brk[0] = true;
            break;

        case GAnchor::LABEL:
        case GAnchor::PORT_LABEL: brk[0] = true; break;
        case GAnchor::BUS_LABEL: brk[1] = true; break;

        case GAnchor::ENTRY:
            exits[1].insert( unique++ );
            exits[0].insert( unique++ );
            brk[0] = brk[1] = true;
            ++entryBusAngles;
            hasEntry = true;
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
            exits[L].insert( dirKey( p, s->a ) );
            exits[L].insert( dirKey( p, s->b ) );
        }
    }

    if( exits[0].size() < 3 && exits[1].size() < 3 )
        return false;

    if( !hasEntry )
        return true;

    bool toWires = exits[0].size() > 2 && exits[1].size() == 1;
    bool toBuses = (int) exits[1].size() - entryBusAngles >= 3 && busEnds >= 1;
    return toWires || toBuses;
}


GJunction* junctionAt( Geo& g, const Pt& p )
{
    for( GJunction& j : g.juncs )
    {
        if( !j.dead && same( j.p, p ) )
            return &j;
    }

    return nullptr;
}


/// Split the wires passing through p (KiCad breaks wires at junctions).
void splitAt( Geo& g, const Pt& p )
{
    const size_t n = g.segs.size();

    for( size_t i = 0; i < n; ++i )
    {
        if( !alive( g.segs[i] ) || g.segs[i].multi || !inside( p, g.segs[i].a, g.segs[i].b ) )
            continue;

        GSeg rest = g.segs[i];
        rest.line = -1;
        rest.item = -1;
        rest.a = p;
        rest.a0 = rest.b0 = Pt();
        g.segs[i].b = p;
        g.segs.push_back( rest );
    }
}


void addJunction( Geo& g, const Pt& p, bool aKeep )
{
    GJunction j;
    j.p = p;
    j.keep = aKeep;
    g.juncs.push_back( j );
    splitAt( g, p );
}


/// Junctions at the points an edit touched: add the ones that became necessary, drop the ones
/// that became needless, and split the wires running through a junction (KiCad breaks them;
/// KiCad 7 does not join a wire to a junction in its middle). Other points keep what the file
/// has.
void fixJunctions( Geo& g, std::vector<Pt> aPoints, bool aAdd, bool aRemove )
{
    std::sort( aPoints.begin(), aPoints.end(), []( const Pt& a, const Pt& b ) { return key( a ) < key( b ); } );
    aPoints.erase( std::unique( aPoints.begin(), aPoints.end(), []( const Pt& a, const Pt& b ) { return same( a, b ); } ),
                   aPoints.end() );

    for( const Pt& p : aPoints )
    {
        const bool need = junctionNeeded( g, p );
        GJunction* j = junctionAt( g, p );

        if( need && !j && aAdd )
            addJunction( g, p, false );
        else if( !need && j && !j->keep && aRemove )
            j->dead = true;
        else if( j )
            splitAt( g, p );
    }
}


/// Connection points of the sheet (pins, sheet pins, labels, entries, junctions, wire ends).
std::vector<Pt> connectionPoints( const Geo& g )
{
    std::vector<Pt> out;

    for( const GAnchor& a : g.anchors )
        out.push_back( a.p );

    for( const GJunction& j : g.juncs )
    {
        if( !j.dead )
            out.push_back( j.p );
    }

    for( const GSeg& s : g.segs )
    {
        if( alive( s ) )
        {
            out.push_back( s.a );
            out.push_back( s.b );
        }
    }

    return out;
}


/// The points of the sheet lying inside segment a-b (KiCad SCH_SCREEN::GetNeededJunctions
/// checks them when a wire is added or moved).
void pointsOn( const Geo& g, const Pt& a, const Pt& b, std::vector<Pt>& aOut )
{
    for( const Pt& p : connectionPoints( g ) )
    {
        if( inside( p, a, b ) )
            aOut.push_back( p );
    }
}


/// Set the points of a wire item (the xy nodes of its segments).
void writeWireItem( Ctx& c, int aItem, const std::map<int, Pt>& aPoints )
{
    ItemTree       tree( c.body( aItem ) );
    N              pts = tree.root.find( "pts" );
    std::vector<N> xys;

    for( N x = pts ? pts.first() : N(); x; x = x.next() )
    {
        if( x.isList() && x.head() == "xy" )
            xys.push_back( x );
    }

    std::vector<Patch> p;

    for( const auto& [index, pos] : aPoints )
    {
        if( index >= 0 && index < (int) xys.size() )
            setPoint( p, xys[(size_t) index], pos );
    }

    c.replace( aItem, tree.body, p );
}


/// Write the planned wires and junctions: changed items are patched, the rest is removed or
/// inserted.
void writeGeo( Ctx& c, const Geo& g )
{
    // wire items: new point positions per item, or removal
    std::map<int, std::map<int, Pt>> points;
    std::map<int, bool>              anyAlive;

    for( const GSeg& s : g.segs )
    {
        if( s.item < 0 )
            continue;

        bool live = s.multi ? !s.dead : alive( s );
        anyAlive[s.item] = anyAlive[s.item] || live;

        if( !live )
            continue;

        const int first = c.sc->lines[(size_t) s.line].point;

        if( !same( s.a, s.a0 ) )
            points[s.item][first] = s.a;

        if( !same( s.b, s.b0 ) )
            points[s.item][first + 1] = s.b;
    }

    for( const auto& [item, live] : anyAlive )
    {
        if( !live )
            c.remove( item );
        else if( points.count( item ) )
            writeWireItem( c, item, points[item] );
    }

    for( const GSeg& s : g.segs )
    {
        if( s.item < 0 && alive( s ) )
            c.insert( wireText( c.style, s.bus, s.a, s.b ) );
    }

    for( const GJunction& j : g.juncs )
    {
        if( j.item >= 0 && j.dead )
        {
            c.remove( j.item );
        }
        else if( j.item >= 0 && !same( j.p, j.p0 ) )
        {
            ItemTree           tree( c.body( j.item ) );
            std::vector<Patch> p;

            if( N at = tree.root.find( "at" ) )
                setPoint( p, at, j.p );

            c.replace( j.item, tree.body, p );
        }
        else if( j.item < 0 && !j.dead )
        {
            c.insert( junctionText( c.style, j.p ) );
        }
    }
}


/// Translate a whole file item.
void translateItem( Ctx& c, int aOrdinal, const Pt& aDelta, std::set<int>& aDone )
{
    const int di = c.docItem( aOrdinal );

    if( di < 0 || !aDone.insert( di ).second )
        return;

    ItemTree           tree( c.body( di ) );
    std::vector<Patch> p;
    translateNodes( tree.root, aDelta, p );
    c.replace( di, tree.body, p );
}

// ---- symbol orientation (KiCad SCH_SYMBOL::SetOrientation / GetOrientation) ------------------

/// KiCad composes an incremental transform after the current one: new = temp * old.
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
    const SymTransform o90 = compose( T_ID, T_CCW ), o180 = compose( o90, T_CCW ), o270 = compose( T_ID, T_CW );
    const struct
    {
        SymTransform t;
        int          angle;
        char         mirror;
    } order[] = { { T_ID, 0, 0 },
                  { o90, 90, 0 },
                  { o180, 180, 0 },
                  { o270, 270, 0 },
                  { compose( T_ID, T_MX ), 0, 'x' },
                  { compose( o90, T_MX ), 90, 'x' },
                  { compose( o270, T_MX ), 270, 'x' },
                  { compose( T_ID, T_MY ), 0, 'y' },
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


/// Rotate or mirror a placed symbol about its anchor (KiCad SCH_SYMBOL::SetOrientation). Field
/// positions are stored as drawn, so they turn with the symbol (KiCad keeps them fixed in the
/// symbol's frame); their angles and justification stay.
void transformSymbol( Ctx& c, int aSymbol, const SymTransform& aTemp, bool aClearAutoplaced )
{
    const SchSymbol& s = c.sc->symbols[(size_t) aSymbol];
    const int        di = c.docItem( s.item );
    ItemTree         tree( c.body( di ) );
    N                at = tree.root.find( "at" );

    if( !at )
        return;

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
        removeNode( p, tree, mir );
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

    if( aClearAutoplaced )
    {
        if( N fa = tree.root.find( "fields_autoplaced" ) )
            removeNode( p, tree, fa );
    }

    const Pt pos = roundIU( s.pos );

    tree.root.each( "property",
                    [&]( N prop )
                    {
                        N fat = prop.find( "at" );

                        if( !fat )
                            return;

                        Pt rel = inv.apply( roundIU( Pt( fat.num( 0 ), fat.num( 1 ) ) ) - pos );
                        setPoint( p, fat, pos + T2.apply( rel ) );
                    } );

    c.replace( di, tree.body, p );
}


/// The same symbol with another transform (pins only matter here).
std::vector<Pt> transformedTips( const Ctx& c, int aSymbol, const SymTransform& aT )
{
    SchSymbol s = c.sc->symbols[(size_t) aSymbol];
    s.transform = aT;
    std::vector<Pt>  out;
    const LibSymbol* lib = c.sch.Lib( *c.sc, s );

    if( !lib )
        return out;

    const int unit = c.unitOf( aSymbol );

    for( const LibPin& pin : lib->pins )
    {
        if( pin.unit != 0 && unit != 0 && pin.unit != unit )
            continue;

        if( lib->bodyStyleCount > 1 && pin.bodyStyle != 0 && pin.bodyStyle != s.bodyStyle )
            continue;

        out.push_back( roundIU( s.pos + aT.apply( pin.pos ) ) );
    }

    return out;
}

// ---- labels ----------------------------------------------------------------------------------

int spinAngle( Spin s )
{
    return s == Spin::UP ? 90 : s == Spin::LEFT ? 180 : s == Spin::BOTTOM ? 270 : 0;
}


bool isVerticalAngle( double aAngle )
{
    return ( (int) std::lround( aAngle ) % 180 + 180 ) % 180 == 90;
}


/// Give a label a new spin (angle and justification as KiCad writes them) and move or turn its
/// fields with aField.
template <typename F>
void respinLabel( Ctx& c, int aLabel, Spin aSpin, F&& aField )
{
    const SchLabel& L = c.sc->labels[(size_t) aLabel];
    const int       di = c.docItem( L.item );
    ItemTree        tree( c.body( di ) );
    std::vector<Patch> p;

    if( N at = tree.root.find( "at" ) )
        setAngle( p, at, spinAngle( aSpin ) );

    setHJustify( tree, tree.root, aSpin == Spin::LEFT || aSpin == Spin::BOTTOM ? "right" : "left", p );

    tree.root.each( "property", [&]( N prop ) { aField( tree, prop, p ); } );
    c.replace( di, tree.body, p );
}


// ---- references ------------------------------------------------------------------------------

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


std::string describe( const Ctx& c, const Sel& sel )
{
    if( sel.count() != 1 )
        return std::string();

    if( !sel.symbols.empty() )
        return c.shownRef( *sel.symbols.begin() );

    if( !sel.labels.empty() )
        return c.sc->labels[(size_t) *sel.labels.begin()].text;

    if( !sel.sheets.empty() )
        return c.sc->sheets[(size_t) *sel.sheets.begin()].name;

    if( !sel.lines.empty() )
        return Tr( "Wire" );

    if( !sel.junctions.empty() )
        return Tr( "Junction" );

    if( !sel.noConnects.empty() )
        return Tr( "No-connect flag" );

    if( !sel.entries.empty() )
        return Tr( "Bus entry" );

    if( !sel.texts.empty() )
        return Tr( "Text" );

    if( !sel.images.empty() )
        return Tr( "Image" );

    return Tr( "Graphic" );
}


template <typename F>
bool runEdit( SchEditTarget& t, std::string& err, F&& aBody )
{
    try
    {
        if( !aBody() )
        {
            t.doc.Cancel();
            return false;
        }
    }
    catch( const std::exception& e )
    {
        t.doc.Cancel();
        err = TrF( "Edit error: {0}", { e.what() } );
        return false;
    }

    return true;
}

} // anonymous namespace


// ================================================================================================
// Edits
// ================================================================================================

namespace
{

/// Move the free end of wire i from aOld to aNew; a horizontal or vertical wire that would turn
/// slanted gets a bend (the part at the fixed end keeps its direction; the other way round if
/// that one would touch something; slanted if both would). Returns the bend points.
void dragEnd( Geo& g, size_t i, bool aEndA, const Pt& aNew, std::vector<Pt>& aBends )
{
    GSeg&    s = g.segs[i];
    const Pt F = aEndA ? s.b : s.a;
    const Pt M = aEndA ? s.a : s.b;
    const bool h = horizontal( F, M ), v = vertical( F, M );

    auto set = [&]( const Pt& aEnd )
    {
        if( aEndA )
            g.segs[i].a = aEnd;
        else
            g.segs[i].b = aEnd;
    };

    if( s.multi || ( !h && !v ) || ( h && iu( aNew.y ) == iu( F.y ) ) || ( v && iu( aNew.x ) == iu( F.x ) ) )
    {
        set( aNew );
        return;
    }

    // a bend at E: F-E keeps the wire's direction (option A) or turns at F (option B)
    const Pt candidates[2] = { h ? Pt( aNew.x, F.y ) : Pt( F.x, aNew.y ), h ? Pt( F.x, aNew.y ) : Pt( aNew.x, F.y ) };

    // would the new pieces touch anything that is not theirs?
    auto touches = [&]( const Pt& E )
    {
        std::vector<Pt> mine = { F, aNew, M };

        auto own = [&]( const Pt& q )
        {
            return std::any_of( mine.begin(), mine.end(), [&]( const Pt& m ) { return same( m, q ); } );
        };

        std::vector<Pt> foreign;

        for( size_t k = 0; k < g.segs.size(); ++k )
        {
            if( k != i && alive( g.segs[k] ) )
            {
                foreign.push_back( g.segs[k].a );
                foreign.push_back( g.segs[k].b );

                if( !same( E, F ) && !same( E, aNew ) && ( same( E, g.segs[k].a ) || same( E, g.segs[k].b )
                                                           || inside( E, g.segs[k].a, g.segs[k].b ) ) )
                {
                    return true;
                }
            }
        }

        for( const GAnchor& a : g.anchors )
            foreign.push_back( a.p );

        for( const GJunction& j : g.juncs )
        {
            if( !j.dead )
                foreign.push_back( j.p );
        }

        for( const Pt& q : foreign )
        {
            if( own( q ) )
                continue;

            if( same( q, E ) || inside( q, F, E ) || inside( q, E, aNew ) )
                return true;
        }

        return false;
    };

    for( const Pt& E : candidates )
    {
        if( touches( E ) )
            continue;

        if( same( E, F ) )
        {
            set( aNew );
        }
        else
        {
            set( E );
            GSeg piece;
            piece.a = E;
            piece.b = aNew;
            piece.bus = s.bus;
            g.segs.push_back( piece );
            aBends.push_back( E );
        }

        return;
    }

    set( aNew );
}

} // anonymous namespace


bool SchMoveItems( SchEditTarget& t, const std::vector<SchItemRef>& aItems, double aDx, double aDy, bool aDrag,
                   std::string& err )
{
    Ctx c( t );
    Sel sel;

    if( !c.Init( err ) || !resolve( c, aItems, sel, true, err ) )
        return false;

    const Pt d = roundIU( Pt( aDx, aDy ) );

    if( sel.empty() )
    {
        err = Tr( "Invalid item" );
        return false;
    }

    if( iu( d.x ) == 0 && iu( d.y ) == 0 )
    {
        err = Tr( "No change" );
        return false;
    }

    return runEdit( t, err, [&]()
    {
        const SchScreen& sc = *c.sc;
        const std::string what = describe( c, sel );
        if( what.empty() )
            c.begin( aDrag ? Tr( "Drag" ) : Tr( "Move" ) );
        else
            c.begin( aDrag ? TrF( "Drag {0}", { what } ) : TrF( "Move {0}", { what } ) );

        Geo             g = buildGeo( c );
        std::set<int>   done;
        std::vector<Pt> moving; // connection points that move (old positions)

        auto movePt = [&]( const Pt& p ) { return roundIU( p + d ); };

        // whole items
        for( int s : sel.symbols )
            translateItem( c, sc.symbols[(size_t) s].item, d, done );

        for( int s : sel.sheets )
            translateItem( c, sc.sheets[(size_t) s].item, d, done );

        for( int i : sel.labels )
            translateItem( c, sc.labels[(size_t) i].item, d, done );

        for( int i : sel.noConnects )
            translateItem( c, sc.noConnects[(size_t) i].item, d, done );

        for( int i : sel.entries )
            translateItem( c, sc.busEntries[(size_t) i].item, d, done );

        for( int i : sel.texts )
            translateItem( c, sc.texts[(size_t) i].item, d, done );

        for( int i : sel.graphics )
            translateItem( c, sc.graphics[(size_t) i].item, d, done );

        for( int i : sel.images )
            translateItem( c, sc.images[(size_t) i].item, d, done );

        // graphic lines are drawn as lines but written as polylines: move them as items
        for( int i : sel.lines )
        {
            if( sc.lines[(size_t) i].kind == SchLine::NOTES )
                translateItem( c, sc.lines[(size_t) i].item, d, done );
        }

        // the connection model follows
        for( GAnchor& a : g.anchors )
        {
            bool m = false;

            switch( a.kind )
            {
            case GAnchor::PIN: m = sel.symbols.count( a.owner ) > 0; break;
            case GAnchor::SHEET_PIN: m = sel.sheets.count( a.owner ) > 0; break;
            case GAnchor::LABEL:
            case GAnchor::BUS_LABEL:
            case GAnchor::PORT_LABEL: m = sel.labels.count( a.owner ) > 0; break;
            case GAnchor::ENTRY: m = sel.entries.count( a.owner ) > 0; break;
            case GAnchor::NO_CONNECT: m = sel.noConnects.count( a.owner ) > 0; break;
            }

            if( m )
            {
                if( a.kind != GAnchor::NO_CONNECT )
                    moving.push_back( a.p );

                a.p = movePt( a.p );
            }
        }

        for( GSeg& s : g.segs )
        {
            if( s.line >= 0 && sel.lines.count( s.line ) )
            {
                moving.push_back( s.a );
                moving.push_back( s.b );
                s.a = movePt( s.a );
                s.b = movePt( s.b );
            }
        }

        for( GJunction& j : g.juncs )
        {
            if( sel.junctions.count( j.index ) )
            {
                moving.push_back( j.p );
                j.p = movePt( j.p );
                j.keep = true;
            }
        }

        std::vector<Pt> touched = moving;

        for( const Pt& p : moving )
            touched.push_back( movePt( p ) );

        std::vector<Pt> bends;

        if( aDrag )
        {
            std::set<PtKey> m;

            for( const Pt& p : moving )
                m.insert( key( p ) );

            // junctions, labels and flags sitting on a moving point go along
            for( GJunction& j : g.juncs )
            {
                if( !sel.junctions.count( j.index ) && m.count( key( j.p0 ) ) )
                    j.p = movePt( j.p0 );
            }

            for( GAnchor& a : g.anchors )
            {
                bool label = a.kind == GAnchor::LABEL || a.kind == GAnchor::BUS_LABEL || a.kind == GAnchor::PORT_LABEL;

                if( ( label && !sel.labels.count( a.owner ) ) || ( a.kind == GAnchor::NO_CONNECT && !sel.noConnects.count( a.owner ) ) )
                {
                    if( m.count( key( a.p ) ) )
                    {
                        translateItem( c, label ? sc.labels[(size_t) a.owner].item : sc.noConnects[(size_t) a.owner].item, d, done );
                        a.p = movePt( a.p );
                    }
                }
            }

            // wire ends: whole wires first, then single ends (bends see the final places)
            std::vector<std::pair<size_t, bool>> ends;

            for( size_t i = 0; i < g.segs.size(); ++i )
            {
                GSeg& s = g.segs[i];

                if( s.line < 0 || sel.lines.count( s.line ) )
                    continue;

                const bool ea = m.count( key( s.a0 ) ) > 0, eb = m.count( key( s.b0 ) ) > 0;

                if( ea && eb )
                {
                    s.a = movePt( s.a0 );
                    s.b = movePt( s.b0 );
                }
                else if( ea || eb )
                {
                    ends.emplace_back( i, ea );
                }
            }

            for( const auto& [i, endA] : ends )
            {
                const Pt old = endA ? g.segs[i].a : g.segs[i].b;
                dragEnd( g, i, endA, movePt( old ), bends );
                touched.push_back( g.segs[i].a );
                touched.push_back( g.segs[i].b );
            }
        }

        touched.insert( touched.end(), bends.begin(), bends.end() );

        // points of the sheet that moved wires now run over
        for( const GSeg& s : g.segs )
        {
            if( alive( s ) && ( s.line < 0 || !same( s.a, s.a0 ) || !same( s.b, s.b0 ) ) )
            {
                touched.push_back( s.a );
                touched.push_back( s.b );
                pointsOn( g, s.a, s.b, touched );
            }
        }

        // and moved pins / labels / sheet pins landing on wires
        fixJunctions( g, touched, true, true );
        writeGeo( c, g );
        c.commit();
        return true;
    } );
}


bool SchRotateItem( SchEditTarget& t, const SchItemRef& aItem, std::string& err )
{
    Ctx c( t );
    Sel sel;

    if( !c.Init( err ) || !resolve( c, { aItem }, sel, true, err ) )
        return false;

    if( sel.symbols.size() == 1 && sel.count() == 1 )
    {
        const int s = *sel.symbols.begin();

        return runEdit( t, err, [&]()
        {
            const SchSymbol& sym = c.sc->symbols[(size_t) s];
            c.begin( TrF( "Rotate {0}", { c.shownRef( s ) } ) );

            Geo             g = buildGeo( c );
            std::vector<Pt> before = pinTips( c, s );
            std::vector<Pt> after = transformedTips( c, s, compose( sym.transform, T_CCW ) );
            transformSymbol( c, s, T_CCW, false );

            std::vector<Pt> touched = before;
            size_t          k = 0;

            for( GAnchor& a : g.anchors )
            {
                if( a.kind == GAnchor::PIN && a.owner == s && k < after.size() )
                    a.p = after[k++];
            }

            touched.insert( touched.end(), after.begin(), after.end() );
            fixJunctions( g, touched, true, true );
            writeGeo( c, g );
            c.commit();
            return true;
        } );
    }

    if( sel.labels.size() == 1 && sel.count() == 1 )
    {
        const int i = *sel.labels.begin();

        return runEdit( t, err, [&]()
        {
            const SchLabel& L = c.sc->labels[(size_t) i];
            const Spin      next = L.spin == Spin::RIGHT ? Spin::UP
                                   : L.spin == Spin::UP  ? Spin::LEFT
                                   : L.spin == Spin::LEFT ? Spin::BOTTOM
                                                          : Spin::RIGHT;
            c.begin( TrF( "Rotate {0}", { L.text } ) );
            const Pt pos = roundIU( L.pos );

            // fields turn about the label (KiCad SCH_FIELD::Rotate)
            respinLabel( c, i, next,
                         [&]( const ItemTree& tree, N prop, std::vector<Patch>& p )
                         {
                             N at = prop.find( "at" );

                             if( !at )
                                 return;

                             const bool  vert = isVerticalAngle( at.num( 2 ) );
                             std::string just = hJustify( prop );

                             if( vert && !just.empty() )
                                 setHJustify( tree, prop, just == "left" ? "right" : "left", p );

                             Pt rel = roundIU( Pt( at.num( 0 ), at.num( 1 ) ) ) - pos;
                             setPoint( p, at, pos + Pt( rel.y, -rel.x ) );
                             setAngle( p, at, vert ? 0 : 90 );
                         } );

            c.commit();
            return true;
        } );
    }

    err = Tr( "Only symbols and labels can be rotated" );
    return false;
}


bool SchMirrorItem( SchEditTarget& t, const SchItemRef& aItem, bool aMirrorX, std::string& err )
{
    Ctx c( t );
    Sel sel;

    if( !c.Init( err ) || !resolve( c, { aItem }, sel, true, err ) )
        return false;

    if( sel.symbols.size() == 1 && sel.count() == 1 )
    {
        const int s = *sel.symbols.begin();

        return runEdit( t, err, [&]()
        {
            const SchSymbol&   sym = c.sc->symbols[(size_t) s];
            const SymTransform temp = aMirrorX ? T_MX : T_MY;
            c.begin( TrF( "Mirror {0}", { c.shownRef( s ) } ) );

            Geo             g = buildGeo( c );
            std::vector<Pt> before = pinTips( c, s );
            std::vector<Pt> after = transformedTips( c, s, compose( sym.transform, temp ) );

            // KiCad drops the autoplaced flag of a mirrored symbol
            transformSymbol( c, s, temp, true );

            size_t k = 0;

            for( GAnchor& a : g.anchors )
            {
                if( a.kind == GAnchor::PIN && a.owner == s && k < after.size() )
                    a.p = after[k++];
            }

            std::vector<Pt> touched = before;
            touched.insert( touched.end(), after.begin(), after.end() );
            fixJunctions( g, touched, true, true );
            writeGeo( c, g );
            c.commit();
            return true;
        } );
    }

    if( sel.labels.size() == 1 && sel.count() == 1 )
    {
        const int i = *sel.labels.begin();

        return runEdit( t, err, [&]()
        {
            // KiCad SCH_LABEL_BASE::MirrorSpinStyle: only a label across the mirror axis flips
            const SchLabel& L = c.sc->labels[(size_t) i];
            const bool      leftRight = !aMirrorX;
            Spin            next = L.spin;

            if( leftRight && ( L.spin == Spin::LEFT || L.spin == Spin::RIGHT ) )
                next = L.spin == Spin::LEFT ? Spin::RIGHT : Spin::LEFT;
            else if( !leftRight && ( L.spin == Spin::UP || L.spin == Spin::BOTTOM ) )
                next = L.spin == Spin::UP ? Spin::BOTTOM : Spin::UP;

            c.begin( TrF( "Mirror {0}", { L.text } ) );
            const Pt pos = roundIU( L.pos );

            respinLabel( c, i, next,
                         [&]( const ItemTree& tree, N prop, std::vector<Patch>& p )
                         {
                             N at = prop.find( "at" );

                             if( !at )
                                 return;

                             const bool vert = isVerticalAngle( at.num( 2 ) );

                             if( ( leftRight && !vert ) || ( !leftRight && vert ) )
                             {
                                 std::string just = hJustify( prop );
                                 setHJustify( tree, prop, just == "right" ? "left" : "right", p );
                             }

                             Pt q = roundIU( Pt( at.num( 0 ), at.num( 1 ) ) );

                             if( leftRight )
                                 q.x = 2 * pos.x - q.x;
                             else
                                 q.y = 2 * pos.y - q.y;

                             setPoint( p, at, q );
                         } );

            c.commit();
            return true;
        } );
    }

    err = Tr( "Only symbols and labels can be mirrored" );
    return false;
}


bool SchDeleteItems( SchEditTarget& t, const std::vector<SchItemRef>& aItems, std::string& err )
{
    Ctx c( t );
    Sel sel;

    if( !c.Init( err ) || !resolve( c, aItems, sel, false, err ) )
        return false;

    if( sel.empty() )
    {
        err = Tr( "Invalid item" );
        return false;
    }

    return runEdit( t, err, [&]()
    {
        const SchScreen&  sc = *c.sc;
        const std::string what = describe( c, sel );
        c.begin( what.empty() ? std::string( Tr( "Delete" ) ) : TrF( "Delete {0}", { what } ) );

        Geo             g = buildGeo( c );
        std::set<int>   gone; // document items
        std::vector<Pt> touched;

        auto drop = [&]( int aOrdinal )
        {
            const int di = c.docItem( aOrdinal );

            if( di >= 0 && gone.insert( di ).second )
                c.remove( di );
        };

        for( int s : sel.symbols )
            drop( sc.symbols[(size_t) s].item );

        for( int s : sel.sheets )
            drop( sc.sheets[(size_t) s].item );

        for( int i : sel.labels )
            drop( sc.labels[(size_t) i].item );

        for( int i : sel.noConnects )
            drop( sc.noConnects[(size_t) i].item );

        for( int i : sel.entries )
            drop( sc.busEntries[(size_t) i].item );

        for( int i : sel.texts )
            drop( sc.texts[(size_t) i].item );

        for( int i : sel.graphics )
            drop( sc.graphics[(size_t) i].item );

        for( int i : sel.images )
            drop( sc.images[(size_t) i].item );

        for( int i : sel.lines )
        {
            if( sc.lines[(size_t) i].kind == SchLine::NOTES )
                drop( sc.lines[(size_t) i].item );
        }

        // sheet pins: remove the pin from its sheet
        std::map<int, std::vector<int>> pinsOf;

        for( int flat : sel.sheetPins )
        {
            int sh = -1, pin = -1;

            if( sheetPinRef( sc, flat, sh, pin ) && !sel.sheets.count( sh ) )
                pinsOf[sh].push_back( pin );
        }

        for( const auto& [sh, pins] : pinsOf )
        {
            const int di = c.docItem( sc.sheets[(size_t) sh].item );

            if( di < 0 )
                continue;

            ItemTree           tree( c.body( di ) );
            std::vector<Patch> p;
            int                k = 0;

            tree.root.each( "pin",
                            [&]( N pinNode )
                            {
                                if( std::find( pins.begin(), pins.end(), k++ ) != pins.end() )
                                    removeNode( p, tree, pinNode );
                            } );

            c.replace( di, tree.body, p );
        }

        // the connection model without the deleted items
        g.anchors.erase( std::remove_if( g.anchors.begin(), g.anchors.end(),
                                         [&]( const GAnchor& a )
                                         {
                                             bool del = false;

                                             switch( a.kind )
                                             {
                                             case GAnchor::PIN: del = sel.symbols.count( a.owner ) > 0; break;
                                             case GAnchor::SHEET_PIN: del = sel.sheets.count( a.owner ) > 0; break;
                                             case GAnchor::LABEL:
                                             case GAnchor::BUS_LABEL:
                                             case GAnchor::PORT_LABEL: del = sel.labels.count( a.owner ) > 0; break;
                                             case GAnchor::ENTRY: del = sel.entries.count( a.owner ) > 0; break;
                                             case GAnchor::NO_CONNECT: del = sel.noConnects.count( a.owner ) > 0; break;
                                             }

                                             if( del )
                                                 touched.push_back( a.p );

                                             return del;
                                         } ),
                         g.anchors.end() );

        for( int flat : sel.sheetPins )
        {
            int sh = -1, pin = -1;

            if( sheetPinRef( sc, flat, sh, pin ) )
            {
                Pt p = roundIU( sc.sheets[(size_t) sh].pins[(size_t) pin].pos );
                touched.push_back( p );

                for( auto it = g.anchors.begin(); it != g.anchors.end(); ++it )
                {
                    if( it->kind == GAnchor::SHEET_PIN && it->owner == sh && same( it->p, p ) )
                    {
                        g.anchors.erase( it );
                        break;
                    }
                }
            }
        }

        for( GSeg& s : g.segs )
        {
            if( s.line >= 0 && sel.lines.count( s.line ) )
            {
                s.dead = true;
                touched.push_back( s.a );
                touched.push_back( s.b );
            }
        }

        for( GJunction& j : g.juncs )
        {
            if( sel.junctions.count( j.index ) )
                j.dead = true;
        }

        // junctions that only the deleted items needed go too
        fixJunctions( g, touched, false, true );
        writeGeo( c, g );

        // KiCad 6: the root file lists the references of every symbol
        const int rootFile = c.sch.screens.empty() ? -1 : c.sch.screens[0].file;

        if( !sel.symbols.empty() && rootFile >= 0 && t.doc.FileEditable( (size_t) rootFile ) )
        {
            const int si = findItem( t.doc, rootFile, "symbol_instances" );

            if( si >= 0 )
            {
                ItemTree           tree( t.doc.Doc( (size_t) rootFile ).Item( (size_t) si ).body );
                std::vector<Patch> p;
                std::set<std::string> tails;

                for( int s : sel.symbols )
                    tails.insert( "/" + sc.symbols[(size_t) s].uuid );

                tree.root.each( "path",
                                [&]( N path )
                                {
                                    std::string v( path.str( 0 ) );
                                    size_t      slash = v.rfind( '/' );

                                    if( slash != std::string::npos && tails.count( v.substr( slash ) ) )
                                        removeNode( p, tree, path );
                                } );

                c.replace( si, tree.body, p, rootFile );
            }
        }

        c.commit();
        return true;
    } );
}


bool SchSetField( SchEditTarget& t, int aSymbol, const std::string& aName, const std::string& aValue,
                  std::string& err )
{
    Ctx c( t );

    if( !c.Init( err ) || !checkIndex( aSymbol, c.sc->symbols.size(), err ) )
        return false;

    const SchSymbol& sym = c.sc->symbols[(size_t) aSymbol];
    const int        di = c.docItem( sym.item );

    if( di < 0 )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    ItemTree tree( c.body( di ) );
    N        prop;

    tree.root.each( "property",
                    [&]( N p )
                    {
                        if( !prop && p.str( 0 ) == aName )
                            prop = p;
                    } );

    if( !prop )
    {
        err = TrF( "The symbol has no \"{0}\" field", { aName } );
        return false;
    }

    const std::string p6 = c.node->path == "/" ? std::string() : c.node->path;
    const std::string p7 = "/" + c.sch.screens[0].uuid + p6;
    const bool        isRef = aName == "Reference";
    const bool        isValue = aName == "Value", isFootprint = aName == "Footprint";

    std::string current = isRef ? c.sch.Reference( *c.node, sym ) : std::string( prop.str( 1 ) );

    if( !isRef )
    {
        for( const SchField& f : sym.fields )
        {
            if( f.name == aName )
                current = c.sch.FieldValue( *c.node, sym, f );
        }
    }

    if( current == aValue && std::string( prop.str( 1 ) ) == aValue )
    {
        err = Tr( "No change" );
        return false;
    }

    return runEdit( t, err, [&]()
    {
        c.begin( TrF( "Change {0} of {1}", { Tr( aName.c_str() ), c.shownRef( aSymbol ) } ) );
        std::vector<Patch> p;
        setString( p, prop.arg( 1 ), aValue );

        // KiCad 7+: the reference of this sheet's instance (or of the only one)
        if( isRef )
        {
            std::vector<N> paths;

            if( N inst = tree.root.find( "instances" ) )
            {
                inst.each( "project", [&]( N proj ) { proj.each( "path", [&]( N path ) { paths.push_back( path ); } ); } );
            }

            bool found = false;

            for( N path : paths )
            {
                std::string_view v = path.str( 0 );

                if( v == p7 || v == p6 || ( p6.empty() && v == "/" ) )
                {
                    if( N r = path.find( "reference" ) )
                    {
                        setString( p, r.arg( 0 ), aValue );
                        found = true;
                    }
                }
            }

            if( !found && paths.size() == 1 )
            {
                if( N r = paths[0].find( "reference" ) )
                    setString( p, r.arg( 0 ), aValue );
            }
        }

        c.replace( di, tree.body, p );

        // KiCad 6: per-instance reference, value and footprint in the root file
        const int rootFile = c.sch.screens[0].file;

        if( ( isRef || isValue || isFootprint ) && rootFile >= 0 && t.doc.FileEditable( (size_t) rootFile ) )
        {
            const int si = findItem( t.doc, rootFile, "symbol_instances" );

            if( si >= 0 )
            {
                ItemTree           rt( t.doc.Doc( (size_t) rootFile ).Item( (size_t) si ).body );
                std::vector<Patch> rp;
                const std::string  want = p6 + "/" + sym.uuid;
                const char*        tok = isRef ? "reference" : isValue ? "value" : "footprint";

                rt.root.each( "path",
                              [&]( N path )
                              {
                                  if( path.str( 0 ) != want )
                                      return;

                                  if( N v = path.find( tok ) )
                                  {
                                      if( isRef || !v.str( 0 ).empty() )
                                          setString( rp, v.arg( 0 ), aValue );
                                  }
                              } );

                c.replace( si, rt.body, rp, rootFile );
            }
        }

        c.commit();
        return true;
    } );
}


bool SchSetLabelText( SchEditTarget& t, int aLabel, const std::string& aText, std::string& err )
{
    Ctx c( t );

    if( !c.Init( err ) || !checkIndex( aLabel, c.sc->labels.size(), err ) )
        return false;

    if( aText.empty() )
    {
        err = Tr( "The label needs a name" );
        return false;
    }

    const SchLabel& L = c.sc->labels[(size_t) aLabel];

    if( L.text == aText )
    {
        err = Tr( "No change" );
        return false;
    }

    const int di = c.docItem( L.item );

    if( di < 0 )
    {
        err = Tr( "Item not found in the file" );
        return false;
    }

    return runEdit( t, err, [&]()
    {
        c.begin( TrF( "Rename label {0}", { L.text } ) );
        ItemTree           tree( c.body( di ) );
        std::vector<Patch> p;
        setString( p, tree.root.arg( 0 ), aText );
        c.replace( di, tree.body, p );
        c.commit();
        return true;
    } );
}


bool SchAddWire( SchEditTarget& t, const std::vector<Pt>& aPoints, bool aBus, std::string& err )
{
    Ctx c( t );

    if( !c.Init( err ) )
        return false;

    std::vector<Pt> pts;

    for( const Pt& p : aPoints )
    {
        Pt q = roundIU( p );

        if( pts.empty() || !same( pts.back(), q ) )
            pts.push_back( q );
    }

    // drop points in the middle of a straight run
    for( size_t i = 1; i + 1 < pts.size(); )
    {
        const Pt& a = pts[i - 1];
        const Pt& b = pts[i];
        const Pt& n = pts[i + 1];
        const double cross = ( b.x - a.x ) * ( n.y - b.y ) - ( b.y - a.y ) * ( n.x - b.x );
        const double dot = ( b.x - a.x ) * ( n.x - b.x ) + ( b.y - a.y ) * ( n.y - b.y );

        if( std::fabs( cross ) < 1e-12 && dot > 0 )
            pts.erase( pts.begin() + (std::ptrdiff_t) i );
        else
            ++i;
    }

    if( pts.size() < 2 )
    {
        err = Tr( "A wire needs two different points" );
        return false;
    }

    return runEdit( t, err, [&]()
    {
        c.begin( aBus ? Tr( "Add bus" ) : Tr( "Add wire" ) );
        Geo             g = buildGeo( c );
        std::vector<Pt> touched;

        for( size_t i = 0; i + 1 < pts.size(); ++i )
        {
            // existing connection points along the new piece
            pointsOn( g, pts[i], pts[i + 1], touched );
            touched.push_back( pts[i] );
            touched.push_back( pts[i + 1] );

            GSeg s;
            s.a = pts[i];
            s.b = pts[i + 1];
            s.bus = aBus;
            g.segs.push_back( s );
        }

        fixJunctions( g, touched, true, false );
        writeGeo( c, g );
        c.commit();
        return true;
    } );
}


bool SchAddLabel( SchEditTarget& t, const Pt& aPos, const std::string& aText, SchLabelKind aKind, int aAngle,
                  std::string& err )
{
    Ctx c( t );

    if( !c.Init( err ) )
        return false;

    if( aText.empty() )
    {
        err = Tr( "The label needs a name" );
        return false;
    }

    aAngle = ( ( aAngle % 360 ) + 360 ) % 360;
    aAngle = ( aAngle + 45 ) / 90 * 90 % 360;

    return runEdit( t, err, [&]()
    {
        c.begin( aKind == SchLabelKind::GLOBAL         ? Tr( "Add global label" )
                 : aKind == SchLabelKind::HIERARCHICAL ? Tr( "Add hierarchical label" )
                                                       : Tr( "Add label" ) );

        // the text size the file's labels use (KiCad takes the project default)
        std::map<int64_t, int> sizes;

        for( const SchLabel& L : c.sc->labels )
            sizes[iu( L.attr.sizeY )]++;

        double size = 1.27;
        int    best = 0;

        for( const auto& [s, n] : sizes )
        {
            if( n > best && s > 0 )
            {
                best = n;
                size = (double) s / 1e4;
            }
        }

        const Pt p = roundIU( aPos );
        Geo      g = buildGeo( c );
        c.insert( labelText( c.style, aKind, aText, p, aAngle, size ) );

        // a label on crossing wires joins them: KiCad wants a junction there
        GAnchor a { p, aKind == SchLabelKind::LOCAL ? ( isBusLabel( aText ) ? GAnchor::BUS_LABEL : GAnchor::LABEL ) : GAnchor::PORT_LABEL, -1 };
        g.anchors.push_back( a );
        fixJunctions( g, { p }, true, false );
        writeGeo( c, g );
        c.commit();
        return true;
    } );
}


bool SchAddNoConnect( SchEditTarget& t, const Pt& aPos, std::string& err )
{
    Ctx c( t );

    if( !c.Init( err ) )
        return false;

    const Pt p = roundIU( aPos );

    for( const SchNoConnect& n : c.sc->noConnects )
    {
        if( same( roundIU( n.pos ), p ) )
        {
            err = Tr( "No change" );
            return false;
        }
    }

    return runEdit( t, err, [&]()
    {
        c.begin( Tr( "Add no-connect flag" ) );
        c.insert( noConnectText( c.style, p ) );
        c.commit();
        return true;
    } );
}


bool SchAddJunction( SchEditTarget& t, const Pt& aPos, std::string& err )
{
    Ctx c( t );

    if( !c.Init( err ) )
        return false;

    const Pt p = roundIU( aPos );

    for( const SchJunction& j : c.sc->junctions )
    {
        if( same( roundIU( j.pos ), p ) )
        {
            err = Tr( "No change" );
            return false;
        }
    }

    return runEdit( t, err, [&]()
    {
        c.begin( Tr( "Add junction" ) );
        Geo g = buildGeo( c );
        addJunction( g, p, true );
        writeGeo( c, g );
        c.commit();
        return true;
    } );
}


// ================================================================================================
// Pencil routing
// ================================================================================================

namespace
{

struct Snap
{
    Pt        p;
    SchAnchor kind = SchAnchor::NONE;
};

double segDist( const Pt& p, const Pt& a, const Pt& b, Pt* aFoot = nullptr )
{
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    double       u = len2 > 0 ? ( ( p.x - a.x ) * dx + ( p.y - a.y ) * dy ) / len2 : 0.0;
    u = std::max( 0.0, std::min( 1.0, u ) );
    Pt f( a.x + u * dx, a.y + u * dy );

    if( aFoot )
        *aFoot = f;

    return dist( p, f );
}


double gridSnap( double v, double aGrid )
{
    return roundIU( std::round( v / aGrid ) * aGrid );
}


Snap snapEnd( const Ctx& c, const Geo& g, const Pt& p, double aGrid, double aRadius )
{
    Snap   best;
    double bestD = aRadius;
    int    bestRank = 99;

    auto offer = [&]( const Pt& q, SchAnchor k, int aRank )
    {
        double dd = dist( p, q );

        if( dd < bestD - 1e-9 || ( dd <= bestD + 1e-9 && aRank < bestRank ) )
        {
            if( dd <= aRadius )
            {
                best = { q, k };
                bestD = dd;
                bestRank = aRank;
            }
        }
    };

    for( const GAnchor& a : g.anchors )
    {
        switch( a.kind )
        {
        case GAnchor::PIN: offer( a.p, SchAnchor::PIN, 0 ); break;
        case GAnchor::SHEET_PIN: offer( a.p, SchAnchor::SHEET_PIN, 1 ); break;
        case GAnchor::LABEL:
        case GAnchor::BUS_LABEL:
        case GAnchor::PORT_LABEL: offer( a.p, SchAnchor::LABEL, 3 ); break;
        case GAnchor::ENTRY: offer( a.p, SchAnchor::BUS_ENTRY, 4 ); break;
        case GAnchor::NO_CONNECT: break;
        }
    }

    for( const GJunction& j : g.juncs )
        offer( j.p, SchAnchor::JUNCTION, 1 );

    for( const GSeg& s : g.segs )
    {
        if( !alive( s ) || s.bus )
            continue;

        offer( s.a, SchAnchor::WIRE_END, 2 );
        offer( s.b, SchAnchor::WIRE_END, 2 );
    }

    if( best.kind != SchAnchor::NONE )
        return best;

    // a point on a wire: on the grid along it where it runs straight
    double wireD = aRadius;

    for( const GSeg& s : g.segs )
    {
        if( !alive( s ) || s.bus )
            continue;

        Pt     foot;
        double dd = segDist( p, s.a, s.b, &foot );

        if( dd > wireD )
            continue;

        Pt q = roundIU( foot );

        if( horizontal( s.a, s.b ) )
        {
            double x = gridSnap( foot.x, aGrid );
            double lo = std::min( s.a.x, s.b.x ), hi = std::max( s.a.x, s.b.x );

            if( x > lo + 1e-9 && x < hi - 1e-9 )
                q = Pt( x, s.a.y );
        }
        else if( vertical( s.a, s.b ) )
        {
            double y = gridSnap( foot.y, aGrid );
            double lo = std::min( s.a.y, s.b.y ), hi = std::max( s.a.y, s.b.y );

            if( y > lo + 1e-9 && y < hi - 1e-9 )
                q = Pt( s.a.x, y );
        }

        wireD = dd;
        best = { q, inside( q, s.a, s.b ) ? SchAnchor::WIRE : SchAnchor::WIRE_END };
    }

    if( best.kind != SchAnchor::NONE )
        return best;

    return { Pt( gridSnap( p.x, aGrid ), gridSnap( p.y, aGrid ) ), SchAnchor::NONE };
}


double polyDist( const Pt& p, const std::vector<Pt>& aPoly )
{
    double best = 1e300;

    for( size_t i = 0; i + 1 < aPoly.size(); ++i )
        best = std::min( best, segDist( p, aPoly[i], aPoly[i + 1] ) );

    return best;
}


double median( std::vector<double> v )
{
    if( v.empty() )
        return 0.0;

    std::sort( v.begin(), v.end() );
    return v[v.size() / 2];
}

} // anonymous namespace


SchRoute SchRouteStroke( const Schematic& aSch, int aSheet, const std::vector<Pt>& aStroke, double aGrid,
                         double aSnapRadius )
{
    SchRoute route;

    if( aStroke.size() < 2 || aSheet < 0 || aSheet >= (int) aSch.sheets.size() )
        return route;

    if( aGrid <= 0 )
        aGrid = SCH_GRID;

    // a read-only view of the sheet (no document needed)
    SchDocument   dummy;
    SchEditTarget t { dummy, aSch, aSheet };
    Ctx           c( t );
    c.node = &aSch.sheets[(size_t) aSheet];

    if( c.node->screen < 0 || c.node->screen >= (int) aSch.screens.size() )
        return route;

    c.sc = &aSch.screens[(size_t) c.node->screen];
    const Geo g = buildGeo( c );

    const Snap s = snapEnd( c, g, aStroke.front(), aGrid, aSnapRadius );
    const Snap e = snapEnd( c, g, aStroke.back(), aGrid, aSnapRadius );
    route.start = s.kind;
    route.end = e.kind;
    const Pt S = s.p, E = e.p;

    if( same( S, E ) )
        return route;

    if( iu( S.x ) == iu( E.x ) || iu( S.y ) == iu( E.y ) )
    {
        route.points = { S, E };
        return route;
    }

    // samples along the stroke
    std::vector<Pt> samples;

    for( size_t i = 0; i + 1 < aStroke.size(); ++i )
    {
        const double len = dist( aStroke[i], aStroke[i + 1] );
        const int    n = std::max( 1, (int) std::ceil( len / ( aGrid / 2 ) ) );

        for( int k = 0; k < n; ++k )
            samples.push_back( aStroke[i] + ( aStroke[i + 1] - aStroke[i] ) * ( (double) k / n ) );
    }

    samples.push_back( aStroke.back() );

    std::vector<std::vector<Pt>> candidates;
    candidates.push_back( { S, Pt( E.x, S.y ), E } ); // horizontal first
    candidates.push_back( { S, Pt( S.x, E.y ), E } ); // vertical first

    // two bends: the middle run where the stroke crosses between the ends
    std::vector<double> xs, ys;

    for( const Pt& p : samples )
    {
        if( p.y > std::min( S.y, E.y ) && p.y < std::max( S.y, E.y ) )
            xs.push_back( p.x );

        if( p.x > std::min( S.x, E.x ) && p.x < std::max( S.x, E.x ) )
            ys.push_back( p.y );
    }

    if( !xs.empty() )
    {
        double m = gridSnap( median( xs ), aGrid );

        if( iu( m ) != iu( S.x ) && iu( m ) != iu( E.x ) )
            candidates.push_back( { S, Pt( m, S.y ), Pt( m, E.y ), E } );
    }

    if( !ys.empty() )
    {
        double m = gridSnap( median( ys ), aGrid );

        if( iu( m ) != iu( S.y ) && iu( m ) != iu( E.y ) )
            candidates.push_back( { S, Pt( S.x, m ), Pt( E.x, m ), E } );
    }

    double bestScore = 1e300;

    for( const std::vector<Pt>& cand : candidates )
    {
        double sum = 0.0;

        for( const Pt& p : samples )
            sum += polyDist( p, cand );

        // a bend more must earn its place
        const double score = sum / (double) samples.size() + 0.25 * aGrid * (double) ( cand.size() - 2 );

        if( score < bestScore )
        {
            bestScore = score;
            route.points = cand;
        }
    }

    return route;
}

} // namespace kp
