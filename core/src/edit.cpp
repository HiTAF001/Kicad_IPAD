/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "edit.h"
#include "sexpr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kp
{

static constexpr int FIRST_FP_AFFINE_TRANSFORM = 20260616;

std::string FormatNumber( double v )
{
    if( std::fabs( v ) < 5e-7 )
        return "0";

    char buf[64];
    snprintf( buf, sizeof( buf ), "%.6f", v );
    std::string s( buf );

    while( !s.empty() && s.back() == '0' )
        s.pop_back();

    if( !s.empty() && s.back() == '.' )
        s.pop_back();

    if( s == "-0" )
        s = "0";

    return s;
}


std::string QuoteString( const std::string& t )
{
    std::string out = "\"";

    for( char c : t )
    {
        switch( c )
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }

    out += "\"";
    return out;
}


namespace
{

double normalizeAngle( double a )
{
    a = std::fmod( a, 360.0 );

    if( a < 0 )
        a += 360.0;

    if( std::fabs( a - 360.0 ) < 1e-9 || std::fabs( a ) < 1e-9 )
        a = 0.0;

    return a;
}

struct Patch
{
    size_t      begin;
    size_t      end;
    std::string text;
};

std::string applyPatches( const std::string& src, std::vector<Patch> patches )
{
    std::sort( patches.begin(), patches.end(),
               []( const Patch& a, const Patch& b ) { return a.begin < b.begin; } );

    std::string out;
    out.reserve( src.size() + 64 );
    size_t pos = 0;

    for( const Patch& p : patches )
    {
        if( p.begin < pos ) // overlapping patch: ignore (should not happen)
            continue;

        out.append( src, pos, p.begin - pos );
        out += p.text;
        pos = p.end;
    }

    out.append( src, pos, std::string::npos );
    return out;
}

/// A parsed copy of one document item, with byte offsets.
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

std::string atText( const char* aKey, double x, double y, const std::string& aAngle )
{
    std::string s = std::string( "(" ) + aKey + " " + FormatNumber( x ) + " " + FormatNumber( y );

    if( !aAngle.empty() )
        s += " " + aAngle;

    return s + ")";
}

/// Patch an "(at x y [angle] [extra...])" node keeping extra atoms (e.g. "unlocked").
Patch rewriteAt( const ItemTree& t, N at, const Pt* aNewPos, const double* aDeltaAngle )
{
    double      x = at.num( 0 ), y = at.num( 1 );
    N           a = at.arg( 2 );
    bool        hasAngle = a && a.isNumber();
    double      angle = hasAngle ? ParseNumber( a.text() ) : 0.0;
    std::string extra;

    for( N c = hasAngle ? a.next() : at.arg( 2 ); c; c = c.next() )
        extra += " " + std::string( t.body, c.begin(), c.end() - c.begin() );

    if( aNewPos )
    {
        x = aNewPos->x;
        y = aNewPos->y;
    }

    std::string angleText;

    if( aDeltaAngle )
    {
        double na = normalizeAngle( angle + *aDeltaAngle );

        if( na != 0.0 )
            angleText = FormatNumber( na );
    }
    else if( hasAngle )
    {
        angleText = std::string( a.text() );
    }

    std::string text = atText( "at", x, y, angleText );
    text.insert( text.size() - 1, extra );
    return { at.begin(), at.end(), text };
}

/// Collect all (xy x y) nodes below aNode.
void collectXY( N aNode, std::vector<N>& aOut )
{
    for( N c = aNode.first(); c; c = c.next() )
    {
        if( !c.isList() )
            continue;

        if( c.head() == "xy" )
            aOut.push_back( c );
        else
            collectXY( c, aOut );
    }
}

bool isLocked( N aNode )
{
    // "(footprint locked ...)", "(segment ... (locked yes))", "(via locked ...)"
    for( N c = aNode.arg( 0 ); c; c = c.next() )
    {
        if( c.isAtom() && !c.isString() && c.text() == "locked" )
            return true;

        if( c.isList() && c.head() == "locked" )
        {
            std::string_view v = c.str( 0 );
            return v.empty() || v == "yes" || v == "true";
        }
    }

    return false;
}

/// "keep upright" unless the text says "unlocked" (inside "at" in old files, a list in new ones).
bool textKeepsUpright( N aText )
{
    if( N at = aText.find( "at" ) )
    {
        for( N c = at.first(); c; c = c.next() )
        {
            if( c.isAtom() && !c.isString() && c.text() == "unlocked" )
                return false;
        }
    }

    if( N u = aText.find( "unlocked" ) )
    {
        std::string_view v = u.str( 0 );
        return !( v.empty() || v == "yes" || v == "true" );
    }

    return true;
}

/// Mirror left/right and top/bottom in the text's "(effects (justify ...))".
void flipJustify( N aText, std::vector<Patch>& aPatches )
{
    N eff = aText.find( "effects" );
    N just = eff ? eff.find( "justify" ) : N();

    if( !just )
        return;

    std::string text = "(justify";
    bool        changed = false;

    for( N c = just.first().next(); c; c = c.next() )
    {
        std::string w( c.text() );

        if( w == "left" )        { w = "right"; changed = true; }
        else if( w == "right" )  { w = "left"; changed = true; }
        else if( w == "top" )    { w = "bottom"; changed = true; }
        else if( w == "bottom" ) { w = "top"; changed = true; }

        text += " " + w;
    }

    if( changed )
        aPatches.push_back( { just.begin(), just.end(), text + ")" } );
}

int docIndex( const EditTarget& t, int aParseIndex )
{
    if( aParseIndex < 0 || aParseIndex >= (int) t.docMap.size() )
        return -1;

    return t.docMap[(size_t) aParseIndex];
}

const Footprint* footprintOf( const EditTarget& t, int aFp, std::string& err )
{
    if( aFp < 0 || aFp >= (int) t.board.footprints.size() )
    {
        err = "Geçersiz footprint";
        return nullptr;
    }

    return &t.board.footprints[(size_t) aFp];
}

} // anonymous namespace


bool MoveFootprint( EditTarget& t, int aFp, double dx, double dy, std::string& err )
{
    const Footprint* fp = footprintOf( t, aFp, err );

    if( !fp )
        return false;

    int di = docIndex( t, fp->docItem );

    if( di < 0 )
    {
        err = "Footprint dosyada bulunamadı";
        return false;
    }

    ItemTree tree( t.doc.Item( (size_t) di ).body );

    if( isLocked( tree.root ) )
    {
        err = ( fp->reference.empty() ? std::string( "Footprint" ) : fp->reference )
              + " kilitli. Kilidi KiCad'de açabilirsin.";
        return false;
    }

    std::vector<Patch> patches;

    if( N at = tree.root.find( "at" ) )
    {
        Pt np( at.num( 0 ) + dx, at.num( 1 ) + dy );
        patches.push_back( rewriteAt( tree, at, &np, nullptr ) );
    }

    if( N tr = tree.root.find( "transform" ) )
    {
        if( N tl = tr.find( "translate" ) )
        {
            patches.push_back( { tl.begin(), tl.end(),
                                 "(translate " + FormatNumber( tl.num( 0 ) + dx ) + " "
                                         + FormatNumber( tl.num( 1 ) + dy ) + ")" } );
        }
    }

    if( patches.empty() )
    {
        err = "Footprint konumu bulunamadı";
        return false;
    }

    // footprint zones are stored in board coordinates in pre-KiCad 11 files
    if( t.board.version < FIRST_FP_AFFINE_TRANSFORM )
    {
        tree.root.each( "zone",
                        [&]( N zone )
                        {
                            std::vector<N> xys;
                            collectXY( zone, xys );

                            for( N xy : xys )
                            {
                                patches.push_back( { xy.begin(), xy.end(),
                                                     "(xy " + FormatNumber( xy.num( 0 ) + dx ) + " "
                                                             + FormatNumber( xy.num( 1 ) + dy ) + ")" } );
                            }
                        } );
    }

    t.doc.Begin( "Taşı " + fp->reference );
    t.doc.Replace( di, applyPatches( tree.body, patches ) );
    t.doc.Commit();
    return true;
}


bool RotateFootprint( EditTarget& t, int aFp, double deg, std::string& err )
{
    const Footprint* fp = footprintOf( t, aFp, err );

    if( !fp )
        return false;

    int di = docIndex( t, fp->docItem );

    if( di < 0 )
    {
        err = "Footprint dosyada bulunamadı";
        return false;
    }

    ItemTree tree( t.doc.Item( (size_t) di ).body );

    if( isLocked( tree.root ) )
    {
        err = ( fp->reference.empty() ? std::string( "Footprint" ) : fp->reference )
              + " kilitli. Kilidi KiCad'de açabilirsin.";
        return false;
    }

    bool affine = t.board.version >= FIRST_FP_AFFINE_TRANSFORM;
    std::vector<Patch> patches;

    if( N at = tree.root.find( "at" ) )
        patches.push_back( rewriteAt( tree, at, nullptr, &deg ) );

    if( N tr = tree.root.find( "transform" ) )
    {
        if( N ro = tr.find( "rotate" ) )
        {
            patches.push_back( { ro.begin(), ro.end(),
                                 "(rotate " + FormatNumber( normalizeAngle( ro.num( 0 ) + deg ) ) + ")" } );
        }
        else
        {
            // insert a rotate node right after "(transform"
            size_t pos = tr.first().end();
            patches.push_back( { pos, pos, " (rotate " + FormatNumber( normalizeAngle( deg ) ) + ")" } );
        }
    }

    // pad orientations are stored in board frame
    tree.root.each( "pad",
                    [&]( N pad )
                    {
                        if( N at = pad.find( "at" ) )
                            patches.push_back( rewriteAt( tree, at, nullptr, &deg ) );
                    } );

    // Footprint texts. Pre-KiCad 11 files store their angle in board frame, so it turns with
    // the footprint; newer files store it relative to the footprint. Either way KiCad then
    // keeps "keep upright" texts readable: a board angle of 180..360 is turned by 180 and the
    // justification mirrored (PCB_TEXT::KeepUpright).
    double newOrientation = normalizeAngle( fp->angle + deg );

    for( const char* key : { "fp_text", "property" } )
    {
        tree.root.each( key,
                        [&]( N text )
                        {
                            N at = text.find( "at" );

                            if( !at )
                                return;

                            N    a = at.arg( 2 );
                            double stored = a && a.isNumber() ? ParseNumber( a.text() ) : 0.0;
                            double delta = affine ? 0.0 : deg;
                            double board = normalizeAngle( affine ? stored + newOrientation : stored + deg );

                            if( textKeepsUpright( text ) && board >= 180.0 - 1e-9 )
                            {
                                delta += 180.0;
                                flipJustify( text, patches );
                            }

                            if( delta != 0.0 )
                                patches.push_back( rewriteAt( tree, at, nullptr, &delta ) );
                        } );
    }

    if( !affine )
    {
        // and footprint zones are in board coordinates: rotate around the anchor
        tree.root.each( "zone",
                        [&]( N zone )
                        {
                            std::vector<N> xys;
                            collectXY( zone, xys );

                            for( N xy : xys )
                            {
                                Pt p = RotatePt( Pt( xy.num( 0 ), xy.num( 1 ) ), fp->pos, deg );
                                patches.push_back( { xy.begin(), xy.end(),
                                                     "(xy " + FormatNumber( p.x ) + " " + FormatNumber( p.y ) + ")" } );
                            }
                        } );
    }

    t.doc.Begin( "Döndür " + fp->reference );
    t.doc.Replace( di, applyPatches( tree.body, patches ) );
    t.doc.Commit();
    return true;
}


bool DeleteItem( EditTarget& t, ItemType type, int idx, std::string& err )
{
    int         parseIndex = -1;
    std::string label = "Sil";

    switch( type )
    {
    case ItemType::FOOTPRINT:
        if( idx >= 0 && idx < (int) t.board.footprints.size() )
        {
            parseIndex = t.board.footprints[(size_t) idx].docItem;
            label = "Sil " + t.board.footprints[(size_t) idx].reference;
        }

        break;

    case ItemType::TRACK:
        if( idx >= 0 && idx < (int) t.board.tracks.size() )
            parseIndex = t.board.tracks[(size_t) idx].docItem;

        label = "Pisti sil";
        break;

    case ItemType::ARC:
        if( idx >= 0 && idx < (int) t.board.arcs.size() )
            parseIndex = t.board.arcs[(size_t) idx].docItem;

        label = "Yay pisti sil";
        break;

    case ItemType::VIA:
        if( idx >= 0 && idx < (int) t.board.vias.size() )
            parseIndex = t.board.vias[(size_t) idx].docItem;

        label = "Via'yı sil";
        break;

    case ItemType::ZONE:
        if( idx >= 0 && idx < (int) t.board.zones.size() )
        {
            if( t.board.zones[(size_t) idx].footprint >= 0 )
            {
                err = "Footprint'e ait bölge tek başına silinemez";
                return false;
            }

            parseIndex = t.board.zones[(size_t) idx].docItem;
        }

        label = "Bölgeyi sil";
        break;

    case ItemType::SHAPE:
        if( idx >= 0 && idx < (int) t.board.shapes.size() )
        {
            if( t.board.shapes[(size_t) idx].footprint >= 0 )
            {
                err = "Footprint'e ait çizim tek başına silinemez";
                return false;
            }

            parseIndex = t.board.shapes[(size_t) idx].docItem;
        }

        label = "Çizimi sil";
        break;

    case ItemType::TEXT:
        if( idx >= 0 && idx < (int) t.board.texts.size() )
        {
            if( t.board.texts[(size_t) idx].footprint >= 0 )
            {
                err = "Footprint yazısı tek başına silinemez; footprint'i seçip alanı düzenleyebilirsin";
                return false;
            }

            parseIndex = t.board.texts[(size_t) idx].docItem;
        }

        label = "Yazıyı sil";
        break;

    case ItemType::PAD:
        err = "Pad tek başına silinemez; footprint'i silebilirsin";
        return false;

    default:
        err = "Bu öğe silinemez";
        return false;
    }

    int di = docIndex( t, parseIndex );

    if( di < 0 )
    {
        err = "Öğe dosyada bulunamadı";
        return false;
    }

    ItemTree tree( t.doc.Item( (size_t) di ).body );

    if( isLocked( tree.root ) )
    {
        err = "Öğe kilitli. Kilidi KiCad'de açabilirsin.";
        return false;
    }

    t.doc.Begin( label );
    t.doc.Remove( di );
    t.doc.Commit();
    return true;
}


bool SetFootprintFields( EditTarget& t, int aFp, const std::vector<std::pair<std::string, std::string>>& aFields,
                         std::string& err )
{
    const Footprint* fp = footprintOf( t, aFp, err );

    if( !fp )
        return false;

    int di = docIndex( t, fp->docItem );

    if( di < 0 )
    {
        err = "Footprint dosyada bulunamadı";
        return false;
    }

    auto lower = []( std::string s )
    {
        for( char& c : s )
            c = (char) tolower( (unsigned char) c );

        return s;
    };

    ItemTree           tree( t.doc.Item( (size_t) di ).body );
    std::vector<Patch> patches;
    std::string        label;

    for( const auto& [field, value] : aFields )
    {
        std::string want = lower( field );
        size_t      before = patches.size();

        for( N c = tree.root.arg( 0 ); c; c = c.next() )
        {
            if( !c.isList() )
                continue;

            std::string_view h = c.head();

            if( ( h == "property" || h == "fp_text" ) && lower( std::string( c.str( 0 ) ) ) == want )
            {
                N valueNode = c.arg( 1 );

                if( valueNode && valueNode.isAtom() )
                    patches.push_back( { valueNode.begin(), valueNode.end(), QuoteString( value ) } );
            }
        }

        if( patches.size() == before )
        {
            err = "Footprint'te \"" + field + "\" alanı yok";
            return false;
        }

        label += ( label.empty() ? "" : ", " ) + field + " = " + value;
    }

    if( patches.empty() )
    {
        err = "Değişiklik yok";
        return false;
    }

    t.doc.Begin( label );
    t.doc.Replace( di, applyPatches( tree.body, patches ) );
    t.doc.Commit();
    return true;
}


bool SetFootprintField( EditTarget& t, int aFp, const std::string& field, const std::string& value,
                        std::string& err )
{
    return SetFootprintFields( t, aFp, { { field, value } }, err );
}


bool SetTrackWidth( EditTarget& t, ItemType type, int idx, double width, std::string& err )
{
    if( width <= 0 || width > 100 )
    {
        err = "Geçersiz genişlik";
        return false;
    }

    int parseIndex = -1;

    if( type == ItemType::TRACK && idx >= 0 && idx < (int) t.board.tracks.size() )
        parseIndex = t.board.tracks[(size_t) idx].docItem;
    else if( type == ItemType::ARC && idx >= 0 && idx < (int) t.board.arcs.size() )
        parseIndex = t.board.arcs[(size_t) idx].docItem;

    int di = docIndex( t, parseIndex );

    if( di < 0 )
    {
        err = "Pist dosyada bulunamadı";
        return false;
    }

    ItemTree tree( t.doc.Item( (size_t) di ).body );

    if( isLocked( tree.root ) )
    {
        err = "Pist kilitli";
        return false;
    }

    N w = tree.root.find( "width" );

    if( !w )
    {
        err = "Pist genişliği bulunamadı";
        return false;
    }

    t.doc.Begin( "Pist genişliği " + FormatNumber( width ) + " mm" );
    t.doc.Replace( di, applyPatches( tree.body, { { w.begin(), w.end(), "(width " + FormatNumber( width ) + ")" } } ) );
    t.doc.Commit();
    return true;
}


bool SetViaSize( EditTarget& t, int aVia, double size, double drill, std::string& err )
{
    if( aVia < 0 || aVia >= (int) t.board.vias.size() )
    {
        err = "Geçersiz via";
        return false;
    }

    if( size <= 0 || drill <= 0 || drill >= size )
    {
        err = "Delik çapı via çapından küçük olmalı";
        return false;
    }

    int di = docIndex( t, t.board.vias[(size_t) aVia].docItem );

    if( di < 0 )
    {
        err = "Via dosyada bulunamadı";
        return false;
    }

    ItemTree tree( t.doc.Item( (size_t) di ).body );

    if( isLocked( tree.root ) )
    {
        err = "Via kilitli";
        return false;
    }

    std::vector<Patch> patches;

    if( N s = tree.root.find( "size" ) )
        patches.push_back( { s.begin(), s.end(), "(size " + FormatNumber( size ) + ")" } );

    if( N d = tree.root.find( "drill" ) )
    {
        patches.push_back( { d.begin(), d.end(), "(drill " + FormatNumber( drill ) + ")" } );
    }
    else if( N s = tree.root.find( "size" ) )
    {
        patches.push_back( { s.end(), s.end(), " (drill " + FormatNumber( drill ) + ")" } );
    }

    if( patches.empty() )
    {
        err = "Via boyutu bulunamadı";
        return false;
    }

    t.doc.Begin( "Via " + FormatNumber( size ) + "/" + FormatNumber( drill ) + " mm" );
    t.doc.Replace( di, applyPatches( tree.body, patches ) );
    t.doc.Commit();
    return true;
}

} // namespace kp
