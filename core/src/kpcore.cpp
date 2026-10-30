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

        // zone fill outlines are only needed for meshing; free them
        for( kp::Zone& z : b->board.zones )
        {
            std::vector<kp::ZoneFill>().swap( z.fills );
        }

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


extern "C" const char* kp_core_version( void )
{
    return "0.1.0";
}
