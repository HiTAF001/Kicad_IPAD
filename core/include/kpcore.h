/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
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
    KP_KIND_LABEL0 = 10, /**< pad number / net name labels, 5 size buckets (10..14) */
    KP_KIND_LABEL4 = 14,
    KP_KIND_COUNT = 15
};

/** Selectable item types. */
enum
{
    KP_ITEM_NONE = 0,
    KP_ITEM_PAD = 1,
    KP_ITEM_VIA = 2,
    KP_ITEM_TRACK = 3,
    KP_ITEM_ARC = 4,
    KP_ITEM_FOOTPRINT = 5,
    KP_ITEM_ZONE = 6,
    KP_ITEM_SHAPE = 7,
    KP_ITEM_TEXT = 8,
    KP_ITEM_SCH_SYMBOL = 9,
    KP_ITEM_SCH_PIN = 10,
    KP_ITEM_SCH_WIRE = 11,
    KP_ITEM_SCH_JUNCTION = 12,
    KP_ITEM_SCH_LABEL = 13,
    KP_ITEM_SCH_SHEET = 14,
    KP_ITEM_SCH_NOTE = 15,       /**< text, graphic, image, no-connect flag, bus entry */
    KP_ITEM_SCH_SHEET_PIN = 16
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
    int32_t unconnected; /**< airwires (missing connections) */
    double  conn_ms;
} kp_stats;

typedef struct
{
    int32_t  slot;        /**< layer slot index */
    int32_t  kind;        /**< KP_KIND_* */
    float    r, g, b, a;  /**< colour, straight alpha */
    uint32_t index_start; /**< first index in the index buffer */
    uint32_t index_count; /**< number of indices (multiple of 3) */
    float    min_zoom;    /**< draw only when the zoom (pixels per mm) is at least this */
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
    int32_t  parent_slot; /**< label slots: visible only together with this slot (-1 = none) */
    int32_t  view_side;   /**< +1 only in top view, -1 only in bottom view, 0 always */
} kp_slot_info;

typedef struct
{
    int32_t type;      /**< KP_ITEM_* */
    int32_t index;     /**< index within its type */
    int32_t footprint; /**< owning footprint or -1 */
    int32_t net;       /**< net index, 0 = none */
    int32_t layer;     /**< layer slot the item was hit on, or -1 */
} kp_item;

typedef struct
{
    uint32_t index_start;
    uint32_t index_count;
    int32_t  slot;
    int32_t  kind;
} kp_range;

/**
 * Parse a .kicad_pcb file from memory and build its render mesh.
 * Returns NULL on failure and writes a message into error (UTF-8, may be NULL).
 */
kp_board* kp_board_load( const char* data, size_t length, char* error, size_t error_size );

/**
 * Fabrication files: Gerber (RS-274X / X2), Excellon drill files and zip archives of them.
 * Collect the files of one board with kp_fab_input_add, then build a view-only board whose
 * layers follow the files (X2 file function, KiCad / Protel / Eagle file names).
 */
typedef struct kp_fab_input kp_fab_input;

int32_t       kp_data_is_fab( const char* data, size_t length ); /**< 1 = Gerber, drill or zip */
kp_fab_input* kp_fab_input_new( void );
void          kp_fab_input_add( kp_fab_input* input, const char* name, const char* data, size_t length );
void          kp_fab_input_free( kp_fab_input* input );
kp_board*     kp_board_load_fab( const kp_fab_input* input, char* error, size_t error_size );
int32_t       kp_board_is_fab( const kp_board* board ); /**< 1 = built from fabrication files */

enum
{
    KP_KIND_PCB = 0,
    KP_KIND_GERBER = 1,
    KP_KIND_SCHEMATIC = 2
};

int32_t kp_board_kind( const kp_board* board ); /**< KP_KIND_* */
void    kp_board_background( const kp_board* board, float* rgba ); /**< canvas colour, 4 floats */

/**
 * Schematics (.kicad_sch, KiCad 6 .. 10). Collect the root file and its sub-sheet files with
 * kp_fab_input_add (names relative to the project folder), then load one sheet of the
 * hierarchy. Other sheets of the same schematic are opened without parsing again.
 */
typedef struct
{
    int32_t parent;      /**< sheet index, -1 for the root */
    int32_t sheet_item;  /**< index of the sheet box in the parent sheet */
    int32_t child_count;
    int32_t depth;
} kp_sheet_info;

int32_t     kp_data_is_schematic( const char* data, size_t length );
kp_board*   kp_board_load_sch( const kp_fab_input* files, int32_t root, int32_t sheet, char* error,
                               size_t error_size );
kp_board*   kp_board_sch_open_sheet( const kp_board* board, int32_t sheet, char* error, size_t error_size );
int32_t     kp_sch_sheet_count( const kp_board* board );
int32_t     kp_sch_current_sheet( const kp_board* board );
void        kp_sch_sheet_info( const kp_board* board, int32_t sheet, kp_sheet_info* out );
const char* kp_sch_sheet_name( const kp_board* board, int32_t sheet ); /**< "" for the root */
const char* kp_sch_sheet_page( const kp_board* board, int32_t sheet );
const char* kp_sch_sheet_file( const kp_board* board, int32_t sheet );
int32_t     kp_sch_sheet_for_item( const kp_board* board, int32_t sheet_item ); /**< child sheet, -1 */
int32_t     kp_sch_missing_count( const kp_board* board );
const char* kp_sch_missing_file( const kp_board* board, int32_t index );
/** Netlist of the whole schematic as text: "net name<TAB>ref:pin ref:pin\n" per net. */
const char* kp_sch_netlist( kp_board* board );

/*
 * On a schematic board the board functions describe the open sheet: kp_board_pick /
 * item_info / item_ranges / item_bounds work on its items (KP_ITEM_SCH_*), "footprints" are
 * its symbols and nets (1..count-1) are the nets of the whole hierarchy.
 */

/** Sheets on which a net has items; returns their number, writes at most max_out. */
int32_t kp_sch_net_sheets( const kp_board* board, int32_t net, int32_t* out, int32_t max_out );

/** Symbols of the whole hierarchy (for search): sheet and index within it, shown reference
 * ("U1A") and value. The strings stay valid while the board lives. */
int32_t kp_sch_symbol_count( const kp_board* board );
int32_t kp_sch_symbol_entry( const kp_board* board, int32_t i, int32_t* sheet, int32_t* index,
                             const char** reference, const char** value );

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

/**
 * Items under (x, y) in millimetres, best first. slot_visible holds one byte per slot
 * (non-zero = visible); hidden_kinds has bit (1 << KP_KIND_x) set for hidden item kinds.
 * Returns the number of items written (at most max_out).
 */
int32_t kp_board_pick( const kp_board* board, double x, double y, double tolerance,
                       const uint8_t* slot_visible, int32_t slot_count, uint32_t hidden_kinds,
                       int32_t flipped, kp_item* out, int32_t max_out );

/** Centre of the nearest visible pad/via within tolerance. Returns 1 if found. */
int32_t kp_board_snap( const kp_board* board, double x, double y, double tolerance,
                       const uint8_t* slot_visible, int32_t slot_count, double* out_x, double* out_y );

/**
 * Description as "Title\nKey\tValue\n...". The pointer stays valid until the next call of
 * kp_board_item_info / kp_board_net_info on this board.
 */
const char* kp_board_item_info( kp_board* board, const kp_item* item );
const char* kp_board_net_info( kp_board* board, int32_t net );

/** Bounds of an item (or of a whole net when whole_net != 0). Returns 0 if empty. */
int32_t kp_board_item_bounds( const kp_board* board, const kp_item* item, int32_t whole_net,
                              double* min_x, double* min_y, double* max_x, double* max_y );

/**
 * Triangle ranges (in the index buffer) to highlight an item (a footprint: all its parts) or,
 * when whole_net != 0, every copper item of item->net. Returns the total number of ranges;
 * at most max_out are written (pass out = NULL to query the count).
 */
int32_t kp_board_item_ranges( kp_board* board, const kp_item* item, int32_t whole_net,
                              kp_range* out, int32_t max_out );

int32_t     kp_board_footprint_count( const kp_board* board );
const char* kp_board_footprint_ref( const kp_board* board, int32_t index );
const char* kp_board_footprint_value( const kp_board* board, int32_t index );
int32_t     kp_board_footprint_back( const kp_board* board, int32_t index );
int32_t     kp_board_net_count( const kp_board* board );
const char* kp_board_net_name( const kp_board* board, int32_t net );

int32_t kp_board_footprint_pose( const kp_board* board, int32_t index, double* x, double* y, double* angle );

/**
 * Translations for the core's messages and descriptions: pairs English -> current language.
 * Call once before loading boards (not concurrently with loading).
 */
void kp_set_translations( const char* const* keys, const char* const* values, int32_t count );

int32_t kp_board_footprint_pad_count( const kp_board* board, int32_t index );
int32_t kp_board_net_pad_count( const kp_board* board, int32_t net );

/** Width of a track or arc item (mm), 0 for other items. */
double  kp_board_track_width( const kp_board* board, const kp_item* item );
/** Via diameter and drill (mm). Returns 0 for a bad index. */
int32_t kp_board_via_geometry( const kp_board* board, int32_t index, double* size, double* drill );

/** Ratsnest: airwires as x1,y1,x2,y2 floats (count receives the number of floats). */
int32_t      kp_board_airwire_count( const kp_board* board );
const float* kp_board_airwires( const kp_board* board, size_t* count );

/* ------------------------------------------------------------------------------------------
 * Editing. A kp_doc holds the file text (lossless: unedited items are written back byte for
 * byte) with undo/redo. Boards are built from it with kp_board_load_doc; edits take the doc
 * and the board built from its current state, change only the doc, and the caller then
 * builds a fresh board. All functions return 1 on success, 0 on failure (message in error).
 * ------------------------------------------------------------------------------------------ */
typedef struct kp_doc kp_doc;

kp_doc*     kp_doc_create( const char* data, size_t length, char* error, size_t error_size );
void        kp_doc_free( kp_doc* doc );
kp_board*   kp_board_load_doc( kp_doc* doc, char* error, size_t error_size );
const char* kp_doc_text( kp_doc* doc, size_t* length ); /**< valid until the next call */
int32_t     kp_doc_modified( const kp_doc* doc );
void        kp_doc_mark_saved( kp_doc* doc );
int32_t     kp_doc_can_undo( const kp_doc* doc );
int32_t     kp_doc_can_redo( const kp_doc* doc );
const char* kp_doc_undo_label( kp_doc* doc );
const char* kp_doc_redo_label( kp_doc* doc );
int32_t     kp_doc_undo( kp_doc* doc );
int32_t     kp_doc_redo( kp_doc* doc );
int32_t     kp_doc_board_current( const kp_doc* doc, const kp_board* board );

int32_t kp_edit_move_footprint( kp_doc* doc, const kp_board* board, int32_t footprint, double dx, double dy,
                                char* error, size_t error_size );
int32_t kp_edit_rotate_footprint( kp_doc* doc, const kp_board* board, int32_t footprint, double degrees,
                                  char* error, size_t error_size );
int32_t kp_edit_delete( kp_doc* doc, const kp_board* board, const kp_item* item, char* error, size_t error_size );
int32_t kp_edit_set_field( kp_doc* doc, const kp_board* board, int32_t footprint, const char* field,
                           const char* value, char* error, size_t error_size );
/** Reference and/or value (NULL = leave unchanged) in one undo step. */
int32_t kp_edit_set_ref_value( kp_doc* doc, const kp_board* board, int32_t footprint, const char* reference,
                               const char* value, char* error, size_t error_size );
int32_t kp_edit_set_track_width( kp_doc* doc, const kp_board* board, const kp_item* item, double width,
                                 char* error, size_t error_size );
int32_t kp_edit_set_via( kp_doc* doc, const kp_board* board, int32_t via, double size, double drill,
                         char* error, size_t error_size );

/** Add one track on a copper layer slot (net = net index, 0 = none). */
int32_t kp_edit_add_track( kp_doc* doc, const kp_board* board, double x1, double y1, double x2, double y2,
                           double width, int32_t slot, int32_t net, char* error, size_t error_size );
/** Add a through via. */
int32_t kp_edit_add_via( kp_doc* doc, const kp_board* board, double x, double y, double size, double drill,
                         int32_t net, char* error, size_t error_size );

/** Net index by name, or -1. */
int32_t kp_board_net_find( const kp_board* board, const char* name );
/** Board layer of a slot (-1 for virtual slots) and the slot of a board layer (-1 if none). */
int32_t kp_board_slot_layer( const kp_board* board, int32_t slot );
int32_t kp_board_layer_slot( const kp_board* board, int32_t layer );

/* ------------------------------------------------------------------------------------------
 * Pencil routing. A route is built from a stroke on a board, shown as a preview, and committed
 * to the document that board was built from.
 * ------------------------------------------------------------------------------------------ */
typedef struct kp_route kp_route;

enum
{
    KP_ANCHOR_NONE = 0,
    KP_ANCHOR_PAD = 1,
    KP_ANCHOR_VIA = 2,
    KP_ANCHOR_TRACK_END = 3,
    KP_ANCHOR_TRACK_MID = 4
};

typedef struct
{
    int32_t ok;             /**< geometry was produced */
    int32_t blocked;        /**< must not be committed (see kp_route_message) */
    int32_t net;
    int32_t start_kind;     /**< KP_ANCHOR_* */
    int32_t end_kind;
    int32_t start_via;      /**< a via is added at the start / end (layer change) */
    int32_t end_via;
    int32_t shorts;         /**< touches copper of another net */
    int32_t too_close;      /**< closer than the clearance to another net */
    int32_t track_count;
    int32_t via_count;
    int32_t conflict_count;
    double  length;         /**< total track length, mm */
} kp_route_info;

/** xy = x0,y0,x1,y1,... in mm. Distances in mm. */
kp_route* kp_route_build( kp_board* board, const float* xy, int32_t point_count, int32_t slot, double width,
                          double tolerance, double snap_radius, double via_size, double via_drill,
                          double clearance );
void         kp_route_free( kp_route* route );
void         kp_route_get_info( const kp_route* route, kp_route_info* out );
const char*  kp_route_message( const kp_route* route ); /**< empty if none */
const float* kp_route_tracks( const kp_route* route, size_t* count );    /**< x1,y1,x2,y2 per track */
const float* kp_route_vias( const kp_route* route, size_t* count );      /**< x,y,diameter per via */
const float* kp_route_conflicts( const kp_route* route, size_t* count ); /**< x,y,shorts(0/1) each */
const float* kp_route_anchors( const kp_route* route, size_t* count );   /**< x,y of start and end */
int32_t      kp_edit_add_route( kp_doc* doc, const kp_board* board, const kp_route* route, char* error,
                                size_t error_size );

/* ------------------------------------------------------------------------------------------
 * Design rule check.
 * ------------------------------------------------------------------------------------------ */
typedef struct kp_drc kp_drc;

enum
{
    KP_SEVERITY_ERROR = 0,
    KP_SEVERITY_WARNING = 1
};

enum
{
    KP_RULES_DEFAULTS = 0, /**< KiCad's defaults */
    KP_RULES_BOARD = 1,    /**< from the board file (KiCad 4/5) */
    KP_RULES_PROJECT = 2   /**< from the .kicad_pro */
};

typedef struct
{
    int32_t code;     /**< check, see kp_drc_key() */
    int32_t severity; /**< KP_SEVERITY_* */
    double  x, y;     /**< marker position, mm */
    int32_t layer_slot; /**< slot of the layer, -1 if not layer specific */
    kp_item a, b;     /**< items involved (type KP_ITEM_NONE when absent) */
    double  actual;   /**< measured value in mm, -1 = n/a */
    double  required; /**< rule value in mm, -1 = n/a */
} kp_drc_item;

/** Run the checks. project_json: the .kicad_pro text next to the board, or NULL. */
kp_drc*     kp_board_drc( const kp_board* board, const char* project_json, size_t project_length );
/** Same, listing every problem of each track instead of the first one per track and layer. */
kp_drc*     kp_board_drc_all( const kp_board* board, const char* project_json, size_t project_length );
void        kp_drc_free( kp_drc* drc );
int32_t     kp_drc_count( const kp_drc* drc );
void        kp_drc_get( const kp_drc* drc, int32_t index, kp_drc_item* out );
const char* kp_drc_message( const kp_drc* drc, int32_t index ); /**< translated one-line description */
int32_t     kp_drc_error_count( const kp_drc* drc );
int32_t     kp_drc_warning_count( const kp_drc* drc );
int32_t     kp_drc_rules_source( const kp_drc* drc ); /**< KP_RULES_* */
const char* kp_drc_note( const kp_drc* drc );         /**< e.g. why the project file was not used */
double      kp_drc_ms( const kp_drc* drc );           /**< run time */
int32_t     kp_drc_code_count( void );
const char* kp_drc_key( int32_t code );               /**< KiCad name, e.g. "clearance" */
const char* kp_drc_title( int32_t code );             /**< translated title */

/* ------------------------------------------------------------------------------------------
 * Fabrication outputs: Gerbers, drill files, BOM and placement, and a zip of them.
 * ------------------------------------------------------------------------------------------ */
typedef struct kp_fab kp_fab;

/** date: ISO 8601 (written into the files), may be NULL. */
kp_fab*     kp_board_fab( const kp_board* board, const char* base_name, const char* date, int32_t use_aux_origin );
void        kp_fab_free( kp_fab* fab );
int32_t     kp_fab_ok( const kp_fab* fab );
int32_t     kp_fab_file_count( const kp_fab* fab );
const char* kp_fab_file_name( const kp_fab* fab, int32_t index );
const char* kp_fab_file_kind( const kp_fab* fab, int32_t index ); /**< gerber, drill, bom, positions */
const char* kp_fab_file_data( const kp_fab* fab, int32_t index, size_t* length );
const char* kp_fab_zip( kp_fab* fab, size_t* length );           /**< all files, built on first call */
int32_t     kp_fab_warning_count( const kp_fab* fab );
const char* kp_fab_warning( const kp_fab* fab, int32_t index );

const char* kp_core_version( void );

#ifdef __cplusplus
}
#endif

#endif
