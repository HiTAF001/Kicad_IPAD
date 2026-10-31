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
};

static void copyError( char* aDst, size_t aSize, const std::string& aMsg )
{
    if( !aDst || aSize == 0 )
        return;

    size_t n = std::min( aSize - 1, aMsg.size() );
    std::memcpy( aDst, aMsg.data(), n );
    aDst[n] = 0;
}


extern "C" kp_board* kp_board_load( const char* data, size_t length, char* error, size_t error_size )
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


extern "C" const char* kp_core_version( void )
{
    return "0.2.0";
}
