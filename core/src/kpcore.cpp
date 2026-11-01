/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "kpcore.h"

#include "board.h"
#include "mesh.h"
#include "pick.h"
#include "connectivity.h"
#include "document.h"
#include "edit.h"
#include "sexpr.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>
#include <string>

struct kp_board
{
    kp::Board board;
    kp::Mesh  mesh;
    kp_stats  stats {};
    std::string strings[6];
    std::string infoScratch;
    std::vector<kp::ItemRange> rangeScratch;
    kp_item     rangeKey { -1, -1, -1, -1, -1 };
    int32_t     rangeWholeNet = -1;
    std::vector<float> airwires;   // x1,y1,x2,y2 per airwire
    std::vector<int>   docMap;     // parse index -> document item (boards built from a kp_doc)
    const void*        docOwner = nullptr;
    uint64_t           docState = 0;
};

struct kp_doc
{
    kp::Document doc;
    std::string  textScratch;
    std::string  labelScratch;
};

static void copyError( char* aDst, size_t aSize, const std::string& aMsg )
{
    if( !aDst || aSize == 0 )
        return;

    size_t n = std::min( aSize - 1, aMsg.size() );
    std::memcpy( aDst, aMsg.data(), n );
    aDst[n] = 0;
}


static kp_board* buildBoard( const char* data, size_t length, char* error, size_t error_size );

extern "C" kp_board* kp_board_load( const char* data, size_t length, char* error, size_t error_size )
{
    return buildBoard( data, length, error, error_size );
}


static kp_board* buildBoard( const char* data, size_t length, char* error, size_t error_size )
{
    if( !data || length == 0 )
    {
        copyError( error, error_size, "Dosya boş" );
        return nullptr;
    }

    kp_board* b = new( std::nothrow ) kp_board();

    if( !b )
    {
        copyError( error, error_size, "Bellek yetersiz" );
        return nullptr;
    }

    try
    {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();

        kp::LoadBoard( b->board, data, length );

        auto t1 = clock::now();

        kp::BuildMesh( b->board, b->mesh );

        auto t2 = clock::now();

        kp::Connectivity conn;
        kp::ComputeConnectivity( b->board, conn );
        b->airwires.reserve( conn.airwires.size() * 4 );

        for( const kp::Airwire& a : conn.airwires )
        {
            b->airwires.push_back( (float) a.a.x );
            b->airwires.push_back( (float) a.a.y );
            b->airwires.push_back( (float) a.b.x );
            b->airwires.push_back( (float) a.b.y );
        }

        auto t3 = clock::now();
        b->stats.unconnected = (int32_t) conn.airwires.size();
        b->stats.conn_ms = std::chrono::duration<double, std::milli>( t3 - t2 ).count();

        kp::BoardStats s = b->board.Stats();
        kp_stats&      o = b->stats;
        o.footprints = s.footprints;
        o.pads = s.pads;
        o.tracks = s.tracks;
        o.arcs = s.arcs;
        o.vias = s.vias;
        o.zones = s.zones;
        o.keepouts = s.keepouts;
        o.nets = s.nets;
        o.copper_layers = s.copperLayers;
        o.graphics = s.graphics;
        o.texts = s.texts;
        o.holes = s.holes;
        o.width_mm = s.widthMM;
        o.height_mm = s.heightMM;
        o.thickness_mm = b->board.thickness;
        o.file_version = b->board.version;
        o.warning_count = (int32_t) b->board.warnings.size();
        o.vertex_count = (int64_t) ( b->mesh.vertices.size() / 2 );
        o.triangle_count = (int64_t) ( b->mesh.indices.size() / 3 );
        o.parse_ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
        o.mesh_ms = std::chrono::duration<double, std::milli>( t2 - t1 ).count();

        auto tb = [&]( const char* k ) -> std::string
        {
            auto it = b->board.titleBlock.find( k );
            return it == b->board.titleBlock.end() ? std::string() : it->second;
        };

        b->strings[KP_STR_GENERATOR] = b->board.generator;
        b->strings[KP_STR_GENERATOR_VERSION] = b->board.generatorVersion;
        b->strings[KP_STR_TITLE] = tb( "TITLE" );
        b->strings[KP_STR_REVISION] = tb( "REVISION" );
        b->strings[KP_STR_COMPANY] = tb( "COMPANY" );
        b->strings[KP_STR_DATE] = tb( "ISSUE_DATE" );
    }
    catch( const kp::ParseError& e )
    {
        copyError( error, error_size, std::string( "Dosya okunamadı: " ) + e.what() );
        delete b;
        return nullptr;
    }
    catch( const std::bad_alloc& )
    {
        copyError( error, error_size, "Bellek yetersiz (kart çok büyük)" );
        delete b;
        return nullptr;
    }
    catch( const std::exception& e )
    {
        copyError( error, error_size, std::string( "Hata: " ) + e.what() );
        delete b;
        return nullptr;
    }

    return b;
}


extern "C" void kp_board_free( kp_board* board )
{
    delete board;
}


extern "C" void kp_board_stats( const kp_board* board, kp_stats* out )
{
    if( board && out )
        *out = board->stats;
}


extern "C" const char* kp_board_string( const kp_board* board, int32_t key )
{
    if( !board || key < 0 || key >= 6 )
        return "";

    return board->strings[key].c_str();
}


extern "C" const char* kp_board_warning( const kp_board* board, int32_t index )
{
    if( !board || index < 0 || index >= (int32_t) board->board.warnings.size() )
        return "";

    return board->board.warnings[(size_t) index].c_str();
}


extern "C" int32_t kp_board_slot_count( const kp_board* board )
{
    return board ? (int32_t) board->mesh.slots.size() : 0;
}


extern "C" void kp_board_slot_info( const kp_board* board, int32_t slot, kp_slot_info* out )
{
    if( !board || !out || slot < 0 || slot >= (int32_t) board->mesh.slots.size() )
        return;

    const kp::Slot& s = board->mesh.slots[(size_t) slot];
    out->kind = s.kind;
    out->side = s.side;
    out->sort_front = s.sortFront;
    out->sort_back = s.sortBack;
    out->r = s.color[0];
    out->g = s.color[1];
    out->b = s.color[2];
    out->a = s.color[3];
    out->default_visible = s.defaultVisible ? 1 : 0;
    out->index_count = s.indexCount;
    out->parent_slot = s.parentSlot;
    out->view_side = s.viewSide;
}


extern "C" const char* kp_board_slot_name( const kp_board* board, int32_t slot )
{
    if( !board || slot < 0 || slot >= (int32_t) board->mesh.slots.size() )
        return "";

    return board->mesh.slots[(size_t) slot].name.c_str();
}


extern "C" const char* kp_board_slot_display( const kp_board* board, int32_t slot )
{
    if( !board || slot < 0 || slot >= (int32_t) board->mesh.slots.size() )
        return "";

    return board->mesh.slots[(size_t) slot].display.c_str();
}


extern "C" int32_t kp_board_group_count( const kp_board* board )
{
    return board ? (int32_t) board->mesh.groups.size() : 0;
}


extern "C" void kp_board_group( const kp_board* board, int32_t index, kp_group* out )
{
    if( !board || !out || index < 0 || index >= (int32_t) board->mesh.groups.size() )
        return;

    const kp::DrawGroup& g = board->mesh.groups[(size_t) index];
    out->slot = g.slot;
    out->kind = (int32_t) g.kind;
    out->r = g.color[0];
    out->g = g.color[1];
    out->b = g.color[2];
    out->a = g.color[3];
    out->index_start = g.indexStart;
    out->index_count = g.indexCount;
    out->min_zoom = g.minZoom;
}


extern "C" const float* kp_board_vertices( const kp_board* board, size_t* count )
{
    if( count )
        *count = board ? board->mesh.vertices.size() : 0;

    return board && !board->mesh.vertices.empty() ? board->mesh.vertices.data() : nullptr;
}


extern "C" const uint32_t* kp_board_indices( const kp_board* board, size_t* count )
{
    if( count )
        *count = board ? board->mesh.indices.size() : 0;

    return board && !board->mesh.indices.empty() ? board->mesh.indices.data() : nullptr;
}


extern "C" void kp_board_release_mesh( kp_board* board )
{
    if( !board )
        return;

    std::vector<float>().swap( board->mesh.vertices );
    std::vector<uint32_t>().swap( board->mesh.indices );
}


extern "C" void kp_board_bounds( const kp_board* board, double* min_x, double* min_y, double* max_x,
                                 double* max_y )
{
    if( !board )
        return;

    const kp::BBox& b = board->mesh.bounds;

    if( min_x )
        *min_x = b.minX;
    if( min_y )
        *min_y = b.minY;
    if( max_x )
        *max_x = b.maxX;
    if( max_y )
        *max_y = b.maxY;
}


static kp::PickFilter makeFilter( const uint8_t* vis, int32_t count, uint32_t hidden, int32_t flipped )
{
    kp::PickFilter f;
    f.slotVisible = vis;
    f.slotCount = count;
    f.hiddenKinds = hidden;
    f.flipped = flipped != 0;
    return f;
}


extern "C" int32_t kp_board_pick( const kp_board* board, double x, double y, double tolerance,
                                  const uint8_t* slot_visible, int32_t slot_count, uint32_t hidden_kinds,
                                  int32_t flipped, kp_item* out, int32_t max_out )
{
    if( !board || !out || max_out <= 0 )
        return 0;

    std::vector<kp::PickHit> hits;
    kp::Pick( board->board, board->mesh, kp::Pt( x, y ), tolerance,
              makeFilter( slot_visible, slot_count, hidden_kinds, flipped ), hits );

    int32_t n = 0;

    for( const kp::PickHit& h : hits )
    {
        if( n >= max_out )
            break;

        out[n].type = (int32_t) h.type;
        out[n].index = h.index;
        out[n].footprint = h.footprint;
        out[n].net = h.net;
        out[n].layer = h.layer;
        ++n;
    }

    return n;
}


extern "C" int32_t kp_board_snap( const kp_board* board, double x, double y, double tolerance,
                                  const uint8_t* slot_visible, int32_t slot_count, double* out_x,
                                  double* out_y )
{
    if( !board )
        return 0;

    kp::Pt p;

    if( !kp::Snap( board->board, kp::Pt( x, y ), tolerance, makeFilter( slot_visible, slot_count, 0, 0 ), p ) )
        return 0;

    if( out_x )
        *out_x = p.x;
    if( out_y )
        *out_y = p.y;

    return 1;
}


extern "C" const char* kp_board_item_info( kp_board* board, const kp_item* item )
{
    if( !board || !item )
        return "";

    board->infoScratch = kp::ItemInfo( board->board, board->mesh, (kp::ItemType) item->type, item->index );
    return board->infoScratch.c_str();
}


extern "C" const char* kp_board_net_info( kp_board* board, int32_t net )
{
    if( !board )
        return "";

    board->infoScratch = kp::NetInfo( board->board, net );
    return board->infoScratch.c_str();
}


extern "C" int32_t kp_board_item_bounds( const kp_board* board, const kp_item* item, int32_t whole_net,
                                         double* min_x, double* min_y, double* max_x, double* max_y )
{
    if( !board || !item )
        return 0;

    kp::BBox b = whole_net ? kp::NetBounds( board->board, item->net )
                           : kp::ItemBounds( board->board, board->mesh, (kp::ItemType) item->type, item->index );

    if( !b.valid() )
        return 0;

    if( min_x )
        *min_x = b.minX;
    if( min_y )
        *min_y = b.minY;
    if( max_x )
        *max_x = b.maxX;
    if( max_y )
        *max_y = b.maxY;

    return 1;
}


extern "C" int32_t kp_board_item_ranges( kp_board* board, const kp_item* item, int32_t whole_net,
                                         kp_range* out, int32_t max_out )
{
    if( !board || !item )
        return 0;

    const kp_item& k = board->rangeKey;
    bool same = k.type == item->type && k.index == item->index && k.net == item->net
                && board->rangeWholeNet == whole_net;

    if( !same )
    {
        kp::HighlightRanges( board->board, board->mesh, (kp::ItemType) item->type, item->index,
                             whole_net != 0, item->net, board->rangeScratch );
        board->rangeKey = *item;
        board->rangeWholeNet = whole_net;
    }

    int32_t total = (int32_t) board->rangeScratch.size();

    if( out )
    {
        for( int32_t i = 0; i < total && i < max_out; ++i )
        {
            const kp::ItemRange& r = board->rangeScratch[(size_t) i];
            out[i].index_start = r.start;
            out[i].index_count = r.count;
            out[i].slot = r.slot;
            out[i].kind = r.kind;
        }
    }

    return total;
}


extern "C" int32_t kp_board_footprint_count( const kp_board* board )
{
    return board ? (int32_t) board->board.footprints.size() : 0;
}


extern "C" const char* kp_board_footprint_ref( const kp_board* board, int32_t index )
{
    if( !board || index < 0 || index >= (int32_t) board->board.footprints.size() )
        return "";

    return board->board.footprints[(size_t) index].reference.c_str();
}


extern "C" const char* kp_board_footprint_value( const kp_board* board, int32_t index )
{
    if( !board || index < 0 || index >= (int32_t) board->board.footprints.size() )
        return "";

    return board->board.footprints[(size_t) index].value.c_str();
}


extern "C" int32_t kp_board_footprint_back( const kp_board* board, int32_t index )
{
    if( !board || index < 0 || index >= (int32_t) board->board.footprints.size() )
        return 0;

    return board->board.footprints[(size_t) index].back ? 1 : 0;
}


extern "C" int32_t kp_board_net_count( const kp_board* board )
{
    return board ? (int32_t) board->board.nets.size() : 0;
}


extern "C" const char* kp_board_net_name( const kp_board* board, int32_t net )
{
    if( !board || net < 0 || net >= (int32_t) board->board.nets.size() )
        return "";

    return board->board.nets[(size_t) net].c_str();
}


extern "C" int32_t kp_board_footprint_pose( const kp_board* board, int32_t index, double* x, double* y,
                                            double* angle )
{
    if( !board || index < 0 || index >= (int32_t) board->board.footprints.size() )
        return 0;

    const kp::Footprint& f = board->board.footprints[(size_t) index];

    if( x )
        *x = f.pos.x;
    if( y )
        *y = f.pos.y;
    if( angle )
        *angle = f.angle;

    return 1;
}


extern "C" double kp_board_track_width( const kp_board* board, const kp_item* item )
{
    if( !board || !item )
        return 0.0;

    if( item->type == KP_ITEM_TRACK && item->index >= 0 && item->index < (int32_t) board->board.tracks.size() )
        return board->board.tracks[(size_t) item->index].width;

    if( item->type == KP_ITEM_ARC && item->index >= 0 && item->index < (int32_t) board->board.arcs.size() )
        return board->board.arcs[(size_t) item->index].width;

    return 0.0;
}


extern "C" int32_t kp_board_via_geometry( const kp_board* board, int32_t index, double* size, double* drill )
{
    if( !board || index < 0 || index >= (int32_t) board->board.vias.size() )
        return 0;

    const kp::Via& v = board->board.vias[(size_t) index];

    if( size )
        *size = v.size;
    if( drill )
        *drill = v.drill;

    return 1;
}


extern "C" int32_t kp_board_airwire_count( const kp_board* board )
{
    return board ? (int32_t) ( board->airwires.size() / 4 ) : 0;
}


extern "C" const float* kp_board_airwires( const kp_board* board, size_t* count )
{
    if( count )
        *count = board ? board->airwires.size() : 0;

    return board && !board->airwires.empty() ? board->airwires.data() : nullptr;
}


// ------------------------------------------------------------------------------------------
// Editable document
// ------------------------------------------------------------------------------------------

extern "C" kp_doc* kp_doc_create( const char* data, size_t length, char* error, size_t error_size )
{
    if( !data || length == 0 )
    {
        copyError( error, error_size, "Dosya boş" );
        return nullptr;
    }

    kp_doc* d = new( std::nothrow ) kp_doc();

    if( !d )
    {
        copyError( error, error_size, "Bellek yetersiz" );
        return nullptr;
    }

    try
    {
        d->doc.Load( data, length );
    }
    catch( const std::exception& e )
    {
        copyError( error, error_size, std::string( "Dosya okunamadı: " ) + e.what() );
        delete d;
        return nullptr;
    }

    return d;
}


extern "C" void kp_doc_free( kp_doc* doc )
{
    delete doc;
}


extern "C" kp_board* kp_board_load_doc( kp_doc* doc, char* error, size_t error_size )
{
    if( !doc )
    {
        copyError( error, error_size, "Belge yok" );
        return nullptr;
    }

    std::string text = doc->doc.Text();
    kp_board*   b = buildBoard( text.data(), text.size(), error, error_size );

    if( b )
    {
        b->docMap = doc->doc.LiveItems();
        b->docOwner = doc;
        b->docState = doc->doc.StateId();
    }

    return b;
}


extern "C" const char* kp_doc_text( kp_doc* doc, size_t* length )
{
    if( !doc )
    {
        if( length )
            *length = 0;

        return "";
    }

    doc->textScratch = doc->doc.Text();

    if( length )
        *length = doc->textScratch.size();

    return doc->textScratch.c_str();
}


extern "C" int32_t kp_doc_modified( const kp_doc* doc ) { return doc && doc->doc.Modified() ? 1 : 0; }
extern "C" void    kp_doc_mark_saved( kp_doc* doc ) { if( doc ) doc->doc.MarkSaved(); }
extern "C" int32_t kp_doc_can_undo( const kp_doc* doc ) { return doc && doc->doc.CanUndo() ? 1 : 0; }
extern "C" int32_t kp_doc_can_redo( const kp_doc* doc ) { return doc && doc->doc.CanRedo() ? 1 : 0; }
extern "C" int32_t kp_doc_undo( kp_doc* doc ) { return doc && doc->doc.Undo() ? 1 : 0; }
extern "C" int32_t kp_doc_redo( kp_doc* doc ) { return doc && doc->doc.Redo() ? 1 : 0; }


extern "C" const char* kp_doc_undo_label( kp_doc* doc )
{
    if( !doc )
        return "";

    doc->labelScratch = doc->doc.UndoLabel();
    return doc->labelScratch.c_str();
}


extern "C" const char* kp_doc_redo_label( kp_doc* doc )
{
    if( !doc )
        return "";

    doc->labelScratch = doc->doc.RedoLabel();
    return doc->labelScratch.c_str();
}


extern "C" int32_t kp_doc_board_current( const kp_doc* doc, const kp_board* board )
{
    return doc && board && board->docOwner == doc && board->docState == doc->doc.StateId() ? 1 : 0;
}


template <typename F>
static int32_t runEdit( kp_doc* doc, const kp_board* board, char* error, size_t error_size, F&& op )
{
    if( !doc || !board )
    {
        copyError( error, error_size, "Belge yok" );
        return 0;
    }

    if( !kp_doc_board_current( doc, board ) )
    {
        copyError( error, error_size, "Kart görünümü güncel değil, biraz bekleyip tekrar dene" );
        return 0;
    }

    try
    {
        kp::EditTarget t { doc->doc, board->board, board->docMap };
        std::string    err;

        if( !op( t, err ) )
        {
            copyError( error, error_size, err );
            return 0;
        }
    }
    catch( const std::exception& e )
    {
        doc->doc.Cancel();
        copyError( error, error_size, std::string( "Düzenleme hatası: " ) + e.what() );
        return 0;
    }

    return 1;
}


extern "C" int32_t kp_edit_move_footprint( kp_doc* doc, const kp_board* board, int32_t footprint, double dx,
                                           double dy, char* error, size_t error_size )
{
    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err ) { return kp::MoveFootprint( t, footprint, dx, dy, err ); } );
}


extern "C" int32_t kp_edit_rotate_footprint( kp_doc* doc, const kp_board* board, int32_t footprint,
                                             double degrees, char* error, size_t error_size )
{
    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err ) { return kp::RotateFootprint( t, footprint, degrees, err ); } );
}


extern "C" int32_t kp_edit_delete( kp_doc* doc, const kp_board* board, const kp_item* item, char* error,
                                   size_t error_size )
{
    if( !item )
        return 0;

    kp_item it = *item;
    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err )
                    { return kp::DeleteItem( t, (kp::ItemType) it.type, it.index, err ); } );
}


extern "C" int32_t kp_edit_set_field( kp_doc* doc, const kp_board* board, int32_t footprint, const char* field,
                                      const char* value, char* error, size_t error_size )
{
    std::string f = field ? field : "";
    std::string v = value ? value : "";
    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err ) { return kp::SetFootprintField( t, footprint, f, v, err ); } );
}


extern "C" int32_t kp_edit_set_ref_value( kp_doc* doc, const kp_board* board, int32_t footprint,
                                          const char* reference, const char* value, char* error,
                                          size_t error_size )
{
    std::vector<std::pair<std::string, std::string>> fields;

    if( reference )
        fields.emplace_back( "Reference", reference );

    if( value )
        fields.emplace_back( "Value", value );

    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err ) { return kp::SetFootprintFields( t, footprint, fields, err ); } );
}


extern "C" int32_t kp_edit_set_track_width( kp_doc* doc, const kp_board* board, const kp_item* item, double width,
                                            char* error, size_t error_size )
{
    if( !item )
        return 0;

    kp_item it = *item;
    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err )
                    { return kp::SetTrackWidth( t, (kp::ItemType) it.type, it.index, width, err ); } );
}


extern "C" int32_t kp_edit_set_via( kp_doc* doc, const kp_board* board, int32_t via, double size, double drill,
                                    char* error, size_t error_size )
{
    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err ) { return kp::SetViaSize( t, via, size, drill, err ); } );
}


extern "C" const char* kp_core_version( void )
{
    return "0.4.0";
}
