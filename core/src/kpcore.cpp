/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "kpcore.h"
#include "i18n.h"

#include "board.h"
#include "mesh.h"
#include "pick.h"
#include "connectivity.h"
#include "drc.h"
#include "fab.h"
#include "document.h"
#include "edit.h"
#include "geom.h"
#include "route.h"
#include "sexpr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>

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
    std::unique_ptr<kp::CopperIndex> copper; // built on first use (routing, checks)

    const kp::CopperIndex& Copper()
    {
        if( !copper )
        {
            copper = std::make_unique<kp::CopperIndex>();
            copper->Build( board );
        }

        return *copper;
    }
};

struct kp_route
{
    kp::RouteResult    result;
    const kp_board*    board = nullptr;
    uint64_t           boardState = 0;
    std::vector<float> tracks, vias, conflicts, anchors;
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
        copyError( error, error_size, kp::Tr( "The file is empty" ) );
        return nullptr;
    }

    kp_board* b = new( std::nothrow ) kp_board();

    if( !b )
    {
        copyError( error, error_size, kp::Tr( "Out of memory" ) );
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
        copyError( error, error_size, kp::TrF( "The file could not be read: {0}", { e.what() } ) );
        delete b;
        return nullptr;
    }
    catch( const std::bad_alloc& )
    {
        copyError( error, error_size, kp::Tr( "Out of memory (the board is too large)" ) );
        delete b;
        return nullptr;
    }
    catch( const std::exception& e )
    {
        copyError( error, error_size, kp::TrF( "Error: {0}", { e.what() } ) );
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


extern "C" void kp_set_translations( const char* const* keys, const char* const* values, int32_t count )
{
    std::unordered_map<std::string, std::string> table;

    for( int32_t i = 0; keys && values && i < count; ++i )
    {
        if( keys[i] && values[i] )
            table[keys[i]] = values[i];
    }

    kp::SetTranslations( std::move( table ) );
}


extern "C" int32_t kp_board_footprint_pad_count( const kp_board* board, int32_t index )
{
    if( !board || index < 0 || index >= (int32_t) board->board.footprints.size() )
        return 0;

    return board->board.footprints[(size_t) index].padCount;
}


extern "C" int32_t kp_board_net_pad_count( const kp_board* board, int32_t net )
{
    if( !board || net <= 0 )
        return 0;

    int32_t n = 0;

    for( const kp::Pad& p : board->board.pads )
        n += p.net == net;

    return n;
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
        copyError( error, error_size, kp::Tr( "The file is empty" ) );
        return nullptr;
    }

    kp_doc* d = new( std::nothrow ) kp_doc();

    if( !d )
    {
        copyError( error, error_size, kp::Tr( "Out of memory" ) );
        return nullptr;
    }

    try
    {
        d->doc.Load( data, length );
    }
    catch( const std::exception& e )
    {
        copyError( error, error_size, kp::TrF( "The file could not be read: {0}", { e.what() } ) );
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
        copyError( error, error_size, kp::Tr( "No document" ) );
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
        copyError( error, error_size, kp::Tr( "No document" ) );
        return 0;
    }

    if( !kp_doc_board_current( doc, board ) )
    {
        copyError( error, error_size, kp::Tr( "The board view is not up to date yet; wait a moment and try again" ) );
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
        copyError( error, error_size, kp::TrF( "Edit error: {0}", { e.what() } ) );
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


extern "C" int32_t kp_board_net_find( const kp_board* board, const char* name )
{
    if( !board || !name )
        return -1;

    auto it = board->board.netByName.find( name );
    return it == board->board.netByName.end() ? -1 : it->second;
}


extern "C" int32_t kp_board_slot_layer( const kp_board* board, int32_t slot )
{
    if( !board || slot < 0 || slot >= (int32_t) board->mesh.slots.size() )
        return -1;

    return board->mesh.slots[(size_t) slot].layer;
}


extern "C" int32_t kp_board_layer_slot( const kp_board* board, int32_t layer )
{
    if( !board || layer < 0 )
        return -1;

    for( size_t i = 0; i < board->mesh.slots.size(); ++i )
    {
        if( board->mesh.slots[i].layer == layer )
            return (int32_t) i;
    }

    return -1;
}


extern "C" int32_t kp_edit_add_track( kp_doc* doc, const kp_board* board, double x1, double y1, double x2,
                                      double y2, double width, int32_t slot, int32_t net, char* error,
                                      size_t error_size )
{
    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err )
                    {
                        kp::NewTrack tr;
                        tr.start = kp::Pt( x1, y1 );
                        tr.end = kp::Pt( x2, y2 );
                        tr.width = width;
                        tr.layer = kp_board_slot_layer( board, slot );
                        tr.net = net;
                        return kp::AddItems( t, { tr }, {}, kp::Tr( "Add track" ), err );
                    } );
}


extern "C" int32_t kp_edit_add_via( kp_doc* doc, const kp_board* board, double x, double y, double size,
                                    double drill, int32_t net, char* error, size_t error_size )
{
    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err )
                    {
                        kp::NewVia v;
                        v.pos = kp::Pt( x, y );
                        v.size = size;
                        v.drill = drill;
                        v.net = net;
                        return kp::AddItems( t, {}, { v }, kp::Tr( "Add via" ), err );
                    } );
}


extern "C" kp_route* kp_route_build( kp_board* board, const float* xy, int32_t point_count, int32_t slot,
                                     double width, double tolerance, double snap_radius, double via_size,
                                     double via_drill, double clearance )
{
    if( !board || !xy || point_count < 1 )
        return nullptr;

    kp_route* r = new( std::nothrow ) kp_route();

    if( !r )
        return nullptr;

    try
    {
        kp::RouteRequest req;
        req.stroke.reserve( (size_t) point_count );

        for( int32_t i = 0; i < point_count; ++i )
            req.stroke.push_back( kp::Pt( xy[2 * i], xy[2 * i + 1] ) );

        req.layer = kp_board_slot_layer( board, slot );
        req.width = width;
        req.tolerance = tolerance;
        req.snapRadius = snap_radius;
        req.viaSize = via_size;
        req.viaDrill = via_drill;
        req.clearance = clearance;
        r->result = kp::RoutePencil( board->board, board->Copper(), req );
        r->board = board;
        r->boardState = board->docState;

        for( const kp::NewTrack& t : r->result.tracks )
            r->tracks.insert( r->tracks.end(), { (float) t.start.x, (float) t.start.y, (float) t.end.x, (float) t.end.y } );

        for( const kp::NewVia& v : r->result.vias )
            r->vias.insert( r->vias.end(), { (float) v.pos.x, (float) v.pos.y, (float) v.size } );

        for( const kp::RouteConflict& c : r->result.conflicts )
            r->conflicts.insert( r->conflicts.end(), { (float) c.pos.x, (float) c.pos.y, c.shorts ? 1.0f : 0.0f } );

        if( !r->result.tracks.empty() )
        {
            const kp::Pt& a = r->result.tracks.front().start;
            const kp::Pt& b = r->result.tracks.back().end;
            r->anchors = { (float) a.x, (float) a.y, (float) b.x, (float) b.y };
        }
    }
    catch( const std::exception& e )
    {
        r->result = kp::RouteResult();
        r->result.message = e.what();
    }

    return r;
}


extern "C" void kp_route_free( kp_route* route )
{
    delete route;
}


static kp::AnchorKind anchorKindOr( const kp::Anchor& a ) { return a.kind; }

extern "C" void kp_route_get_info( const kp_route* route, kp_route_info* out )
{
    if( !out )
        return;

    *out = kp_route_info {};

    if( !route )
        return;

    const kp::RouteResult& r = route->result;
    out->ok = r.ok ? 1 : 0;
    out->blocked = r.blocked ? 1 : 0;
    out->net = r.net;
    out->start_kind = (int32_t) anchorKindOr( r.start );
    out->end_kind = (int32_t) anchorKindOr( r.end );
    out->start_via = r.start.needsVia ? 1 : 0;
    out->end_via = r.end.needsVia ? 1 : 0;
    out->shorts = r.shorts ? 1 : 0;
    out->too_close = r.tooClose ? 1 : 0;
    out->track_count = (int32_t) r.tracks.size();
    out->via_count = (int32_t) r.vias.size();
    out->conflict_count = (int32_t) r.conflicts.size();

    for( const kp::NewTrack& t : r.tracks )
        out->length += std::hypot( t.end.x - t.start.x, t.end.y - t.start.y );
}


extern "C" const char* kp_route_message( const kp_route* route )
{
    return route ? route->result.message.c_str() : "";
}


static const float* floatsOf( const std::vector<float>& v, size_t* count )
{
    if( count )
        *count = v.size();

    return v.empty() ? nullptr : v.data();
}

extern "C" const float* kp_route_tracks( const kp_route* route, size_t* count )
{
    static const std::vector<float> none;
    return floatsOf( route ? route->tracks : none, count );
}

extern "C" const float* kp_route_vias( const kp_route* route, size_t* count )
{
    static const std::vector<float> none;
    return floatsOf( route ? route->vias : none, count );
}

extern "C" const float* kp_route_conflicts( const kp_route* route, size_t* count )
{
    static const std::vector<float> none;
    return floatsOf( route ? route->conflicts : none, count );
}

extern "C" const float* kp_route_anchors( const kp_route* route, size_t* count )
{
    static const std::vector<float> none;
    return floatsOf( route ? route->anchors : none, count );
}


extern "C" int32_t kp_edit_add_route( kp_doc* doc, const kp_board* board, const kp_route* route, char* error,
                                      size_t error_size )
{
    if( !route || !route->result.ok )
    {
        copyError( error, error_size, route ? route->result.message : std::string( kp::Tr( "No route" ) ) );
        return 0;
    }

    if( route->result.blocked )
    {
        copyError( error, error_size, route->result.message );
        return 0;
    }

    if( route->board != board || route->boardState != board->docState )
    {
        copyError( error, error_size, kp::Tr( "The board view is not up to date yet; wait a moment and try again" ) );
        return 0;
    }

    return runEdit( doc, board, error, error_size,
                    [&]( kp::EditTarget& t, std::string& err )
                    {
                        std::string label = route->result.net > 0
                                                    ? kp::TrF( "Draw track ({0})", { board->board.nets[(size_t) route->result.net] } )
                                                    : std::string( kp::Tr( "Draw track" ) );
                        return kp::AddItems( t, route->result.tracks, route->result.vias, label, err );
                    } );
}


struct kp_drc
{
    std::vector<kp::DrcItem> items;
    std::vector<kp_item>     refs;   // a, b per item
    std::vector<int32_t>     slots;  // layer slot per item
    std::vector<std::string> messages;
    std::string              note;
    int32_t                  errors = 0, warnings = 0, source = 0;
    double                   ms = 0;
};


static void fillItem( const kp::Board& b, const kp::DrcRef& r, kp_item& out )
{
    out = kp_item { 0, -1, -1, 0, -1 };

    if( r.type == kp::ItemType::NONE || r.index < 0 )
        return;

    out.type = (int32_t) r.type;
    out.index = r.index;

    switch( r.type )
    {
    case kp::ItemType::PAD:
        out.footprint = b.pads[(size_t) r.index].footprint;
        out.net = b.pads[(size_t) r.index].net;
        break;
    case kp::ItemType::VIA: out.net = b.vias[(size_t) r.index].net; break;
    case kp::ItemType::TRACK: out.net = b.tracks[(size_t) r.index].net; break;
    case kp::ItemType::ARC: out.net = b.arcs[(size_t) r.index].net; break;
    case kp::ItemType::ZONE:
        out.net = b.zones[(size_t) r.index].net;
        out.footprint = b.zones[(size_t) r.index].footprint;
        break;
    case kp::ItemType::SHAPE: out.footprint = b.shapes[(size_t) r.index].footprint; break;
    case kp::ItemType::FOOTPRINT: out.footprint = r.index; break;
    default: break;
    }
}


static kp_drc* runDrc( const kp_board* board, const char* project_json, size_t project_length, bool all );

extern "C" kp_drc* kp_board_drc( const kp_board* board, const char* project_json, size_t project_length )
{
    return runDrc( board, project_json, project_length, false );
}


extern "C" kp_drc* kp_board_drc_all( const kp_board* board, const char* project_json, size_t project_length )
{
    return runDrc( board, project_json, project_length, true );
}


static kp_drc* runDrc( const kp_board* board, const char* project_json, size_t project_length, bool all )
{
    if( !board )
        return nullptr;

    kp_drc* d = new( std::nothrow ) kp_drc();

    if( !d )
        return nullptr;

    try
    {
        auto             t0 = std::chrono::steady_clock::now();
        kp::DesignRules  rules = kp::ResolveRules( board->board, project_json, project_length, d->note );
        rules.reportAllTrackErrors = all;
        kp::Connectivity conn;
        kp::ComputeConnectivity( board->board, conn );
        kp::RunDrc( board->board, rules, conn, d->items );
        d->source = (int32_t) rules.source;

        for( const kp::DrcItem& it : d->items )
        {
            kp_item a, b;
            fillItem( board->board, it.a, a );
            fillItem( board->board, it.b, b );
            d->refs.push_back( a );
            d->refs.push_back( b );
            d->slots.push_back( it.layer >= 0 ? kp_board_layer_slot( board, it.layer ) : -1 );
            d->messages.push_back( kp::DrcMessage( board->board, it ) );
            ( it.severity == kp::Severity::ERROR ? d->errors : d->warnings )++;
        }

        d->ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
    }
    catch( const std::exception& e )
    {
        d->items.clear();
        d->messages.clear();
        d->note = e.what();
    }

    return d;
}


extern "C" void kp_drc_free( kp_drc* drc )
{
    delete drc;
}


extern "C" int32_t kp_drc_count( const kp_drc* drc )
{
    return drc ? (int32_t) drc->items.size() : 0;
}


extern "C" void kp_drc_get( const kp_drc* drc, int32_t index, kp_drc_item* out )
{
    if( !drc || !out || index < 0 || index >= (int32_t) drc->items.size() )
        return;

    const kp::DrcItem& it = drc->items[(size_t) index];
    *out = kp_drc_item {};
    out->code = (int32_t) it.code;
    out->severity = it.severity == kp::Severity::ERROR ? KP_SEVERITY_ERROR : KP_SEVERITY_WARNING;
    out->x = it.pos.x;
    out->y = it.pos.y;
    out->layer_slot = drc->slots[(size_t) index];
    out->a = drc->refs[(size_t) index * 2];
    out->b = drc->refs[(size_t) index * 2 + 1];
    out->actual = it.actual;
    out->required = it.required;
}


extern "C" const char* kp_drc_message( const kp_drc* drc, int32_t index )
{
    if( !drc || index < 0 || index >= (int32_t) drc->messages.size() )
        return "";

    return drc->messages[(size_t) index].c_str();
}


extern "C" int32_t kp_drc_error_count( const kp_drc* drc ) { return drc ? drc->errors : 0; }
extern "C" int32_t kp_drc_warning_count( const kp_drc* drc ) { return drc ? drc->warnings : 0; }
extern "C" int32_t kp_drc_rules_source( const kp_drc* drc ) { return drc ? drc->source : 0; }
extern "C" const char* kp_drc_note( const kp_drc* drc ) { return drc ? drc->note.c_str() : ""; }
extern "C" double kp_drc_ms( const kp_drc* drc ) { return drc ? drc->ms : 0.0; }
extern "C" int32_t kp_drc_code_count( void ) { return (int32_t) kp::DrcCode::COUNT; }

extern "C" const char* kp_drc_key( int32_t code )
{
    return code >= 0 && code < (int32_t) kp::DrcCode::COUNT ? kp::DrcKey( (kp::DrcCode) code ) : "";
}

extern "C" const char* kp_drc_title( int32_t code )
{
    return code >= 0 && code < (int32_t) kp::DrcCode::COUNT ? kp::Tr( kp::DrcTitle( (kp::DrcCode) code ) ) : "";
}


struct kp_fab
{
    std::vector<kp::FabFile> files;
    std::vector<std::string> warnings;
    std::string              zip, date;
    bool                     ok = false;
};


extern "C" kp_fab* kp_board_fab( const kp_board* board, const char* base_name, const char* date, int32_t use_aux_origin )
{
    if( !board )
        return nullptr;

    kp_fab* f = new( std::nothrow ) kp_fab();

    if( !f )
        return nullptr;

    try
    {
        kp::FabOptions o;

        if( base_name && *base_name )
            o.baseName = base_name;

        if( date )
            o.date = date;

        o.version = kp_core_version();
        o.useAuxOrigin = use_aux_origin != 0;
        f->date = o.date;
        f->ok = kp::WriteFabFiles( board->board, o, f->files, f->warnings );
    }
    catch( const std::exception& e )
    {
        f->ok = false;
        f->warnings.push_back( e.what() );
    }

    return f;
}


extern "C" void kp_fab_free( kp_fab* fab ) { delete fab; }
extern "C" int32_t kp_fab_ok( const kp_fab* fab ) { return fab && fab->ok ? 1 : 0; }
extern "C" int32_t kp_fab_file_count( const kp_fab* fab ) { return fab ? (int32_t) fab->files.size() : 0; }

extern "C" const char* kp_fab_file_name( const kp_fab* fab, int32_t i )
{
    return fab && i >= 0 && i < (int32_t) fab->files.size() ? fab->files[(size_t) i].name.c_str() : "";
}

extern "C" const char* kp_fab_file_kind( const kp_fab* fab, int32_t i )
{
    return fab && i >= 0 && i < (int32_t) fab->files.size() ? fab->files[(size_t) i].kind.c_str() : "";
}

extern "C" const char* kp_fab_file_data( const kp_fab* fab, int32_t i, size_t* length )
{
    if( !fab || i < 0 || i >= (int32_t) fab->files.size() )
    {
        if( length )
            *length = 0;

        return "";
    }

    if( length )
        *length = fab->files[(size_t) i].data.size();

    return fab->files[(size_t) i].data.data();
}

extern "C" const char* kp_fab_zip( kp_fab* fab, size_t* length )
{
    if( !fab )
    {
        if( length )
            *length = 0;

        return "";
    }

    if( fab->zip.empty() && !fab->files.empty() )
        fab->zip = kp::MakeZip( fab->files, fab->date );

    if( length )
        *length = fab->zip.size();

    return fab->zip.data();
}

extern "C" int32_t kp_fab_warning_count( const kp_fab* fab ) { return fab ? (int32_t) fab->warnings.size() : 0; }

extern "C" const char* kp_fab_warning( const kp_fab* fab, int32_t i )
{
    return fab && i >= 0 && i < (int32_t) fab->warnings.size() ? fab->warnings[(size_t) i].c_str() : "";
}


extern "C" const char* kp_core_version( void )
{
    return "0.5.0";
}
