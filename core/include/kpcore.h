/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Plain C interface to the C++ core, imported into Swift through the bridging header.
 */
#ifndef KPCORE_H
#define KPCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kp_board kp_board;

/** Item kinds of draw groups. */
enum
{
    KP_KIND_PAD = 0,
    KP_KIND_VIA = 1,
    KP_KIND_TRACK = 2,
    KP_KIND_GRAPHIC = 3,
    KP_KIND_TEXT = 4,
    KP_KIND_ZONE = 5,
    KP_KIND_WALL_PTH = 6,
    KP_KIND_WALL_NPTH = 7,
    KP_KIND_WALL_VIA = 8,
    KP_KIND_HOLE = 9,
    KP_KIND_COUNT = 10
};

/** Layer slot kinds. */
enum
{
    KP_LAYER_COPPER = 0,
    KP_LAYER_SILK = 1,
    KP_LAYER_MASK = 2,
    KP_LAYER_PASTE = 3,
    KP_LAYER_ADHES = 4,
    KP_LAYER_FAB = 5,
    KP_LAYER_COURTYARD = 6,
    KP_LAYER_EDGE = 7,
    KP_LAYER_MARGIN = 8,
    KP_LAYER_USER = 9,
    KP_LAYER_VIRTUAL = 10
};

/** String keys for kp_board_string(). */
enum
{
    KP_STR_GENERATOR = 0,
    KP_STR_GENERATOR_VERSION = 1,
    KP_STR_TITLE = 2,
    KP_STR_REVISION = 3,
    KP_STR_COMPANY = 4,
    KP_STR_DATE = 5
};

typedef struct
{
    int32_t footprints;
    int32_t pads;
    int32_t tracks;
    int32_t arcs;
    int32_t vias;
    int32_t zones;
    int32_t keepouts;
    int32_t nets;
    int32_t copper_layers;
    int32_t graphics;
    int32_t texts;
    int32_t holes;
    double  width_mm;   /**< board outline size (0 if no Edge.Cuts) */
    double  height_mm;
    double  thickness_mm;
    int32_t file_version;
    int32_t warning_count;
    int64_t vertex_count;
    int64_t triangle_count;
    double  parse_ms;
    double  mesh_ms;
} kp_stats;

typedef struct
{
    int32_t  slot;        /**< layer slot index */
    int32_t  kind;        /**< KP_KIND_* */
    float    r, g, b, a;  /**< colour, straight alpha */
    uint32_t index_start; /**< first index in the index buffer */
    uint32_t index_count; /**< number of indices (multiple of 3) */
} kp_group;

typedef struct
{
    int32_t  kind;        /**< KP_LAYER_* */
    int32_t  side;        /**< -1 back, 0 none/inner, +1 front */
    int32_t  sort_front;  /**< draw order when looking at the top side */
    int32_t  sort_back;   /**< draw order when looking at the bottom side */
    float    r, g, b, a;
    int32_t  default_visible;
    uint32_t index_count; /**< 0 = nothing drawn on this slot */
} kp_slot_info;

/**
 * Parse a .kicad_pcb file from memory and build its render mesh.
 * Returns NULL on failure and writes a message into error (UTF-8, may be NULL).
 */
kp_board* kp_board_load( const char* data, size_t length, char* error, size_t error_size );

void kp_board_free( kp_board* board );

void        kp_board_stats( const kp_board* board, kp_stats* out );
const char* kp_board_string( const kp_board* board, int32_t key );
const char* kp_board_warning( const kp_board* board, int32_t index );

int32_t     kp_board_slot_count( const kp_board* board );
void        kp_board_slot_info( const kp_board* board, int32_t slot, kp_slot_info* out );
const char* kp_board_slot_name( const kp_board* board, int32_t slot );    /**< canonical */
const char* kp_board_slot_display( const kp_board* board, int32_t slot ); /**< user name */

int32_t kp_board_group_count( const kp_board* board );
void    kp_board_group( const kp_board* board, int32_t index, kp_group* out );

/** Interleaved x,y floats in millimetres; count receives the number of floats. */
const float*    kp_board_vertices( const kp_board* board, size_t* count );
const uint32_t* kp_board_indices( const kp_board* board, size_t* count );

/** Free the vertex/index arrays once they have been uploaded to the GPU. */
void kp_board_release_mesh( kp_board* board );

void kp_board_bounds( const kp_board* board, double* min_x, double* min_y, double* max_x,
                      double* max_y );

const char* kp_core_version( void );

#ifdef __cplusplus
}
#endif

#endif
